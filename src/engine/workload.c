/*
 * Throughput-kernel execution shared by the ST and MC modes: single jobs on
 * the calling thread, and sustained runs on pinned worker threads with
 * windowed throughput: a fixed untimed warm-up, then a fixed measurement.
 *
 * Threaded runs share each job: workers take tasks from a common counter and
 * the leader (worker 0) starts the next job only when every task of the
 * current one is done, as a renderer finishes a frame before the next.
 *
 * Copyright 2026 The Prismark Authors. Apache-2.0.
 */
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ctx.h"
#include "stats.h"

/* K1 tasks are whole translation units (up to ~25 s): longer windows, and a measurement three times longer. */
#define SLOW_KERNEL_WINDOW_FACTOR 5

void *wl_create(pmk_ctx *c, const pmk_tk *tk, int size, const char *mode) {
  char err[256] = "";
  pmk_tk_args a = {size, c->cfg.k1_data, err, sizeof err};
  void *inst = tk->create(&a);
  if (!inst) ctx_unavailable(c, tk->id, tk->variant, mode, err[0] ? err : "kernel could not be created");
  return inst;
}

uint64_t wl_job(const pmk_tk *tk, const void *inst, void *scratch) {
  uint64_t acc = 0;
  size_t n = tk->ntasks(inst);
  for (size_t i = 0; i < n; i++) acc = pmk_job_combine(acc, i, tk->task(inst, scratch, i));
  return acc;
}

int wl_size_for(const pmk_ctx *c, const pmk_tk *tk) {
  return c->cfg.quick_inputs && !strcmp(tk->id, "K1") ? PMK_SIZE_BURST : PMK_SIZE_FULL;
}

double wl_window_ms(const pmk_ctx *c, const pmk_tk *tk) {
  return !strcmp(tk->id, "K1") ? c->cfg.window_ms * SLOW_KERNEL_WINDOW_FACTOR : c->cfg.window_ms;
}

/* ---------- sustained runs ---------- */

typedef struct shared {
  pmk_ctx *c;
  const sus_spec *s;
  const pmk_tk *tk;
  uint64_t t0_raw, win_ns;
  size_t maxwin;
  _Atomic uint64_t *work; /* nthreads x maxwin work units, by completion window */
  atomic_int stop;
  atomic_int cpu0;        /* CPU of worker 0, refreshed per task */
  atomic_int failed;

  /* threaded job control */
  pal_lock *lock;
  uint64_t gen;           /* job generation, under lock */
  int stopping;           /* under lock */
  size_t ntasks;
  atomic_size_t next, done;
  _Atomic uint64_t job_sum;

  /* job records, written by worker 0 */
  pmk_dvec job_ns;
  atomic_size_t njobs;
  uint64_t checksum;
  int checksum_set, checksum_bad;
} shared;

typedef struct worker {
  shared *sh;
  int index;
  int cpu;
  pal_thread *th;
} worker;

/*
 * Credits a finished task's work to the windows it ran in, in proportion to its time in each: a long task
 * (a K1 translation unit takes up to ~25 s) would otherwise put all its work into the window it ends in, leaving
 * empty windows before it. Windows are read only after the run, when every task has been credited.
 */
static void account(shared *sh, int w, uint64_t t_start, uint64_t work) {
  uint64_t t_end = pal_now_raw_ns();
  uint64_t a = t_start > sh->t0_raw ? t_start - sh->t0_raw : 0, b = t_end - sh->t0_raw;
  _Atomic uint64_t *row = &sh->work[(size_t)w * sh->maxwin];
  if (b <= a) b = a + 1;
  uint64_t first = a / sh->win_ns, last = b / sh->win_ns, given = 0;
  for (uint64_t idx = first; idx <= last && idx < sh->maxwin; idx++) {
    uint64_t lo = idx * sh->win_ns > a ? idx * sh->win_ns : a, hi = (idx + 1) * sh->win_ns < b ? (idx + 1) * sh->win_ns : b;
    uint64_t part = idx == last ? work - given : (uint64_t)((double)work * (double)(hi - lo) / (double)(b - a));
    given += part;
    atomic_fetch_add_explicit(&row[idx], part, memory_order_relaxed);
  }
}

static void record_job(shared *sh, uint64_t ns, uint64_t sum) {
  if (!sh->checksum_set) {
    sh->checksum = sum;
    sh->checksum_set = 1;
  } else if (sum != sh->checksum) {
    sh->checksum_bad = 1;
  }
  if (dvec_push(&sh->job_ns, (double)ns)) atomic_store(&sh->failed, 1);
  atomic_fetch_add(&sh->njobs, 1);
}

/* Takes and runs tasks of the current threaded job until none are left. */
static void work_tasks(shared *sh, int w, const void *inst, void *scratch) {
  size_t i;
  while ((i = atomic_fetch_add(&sh->next, 1)) < sh->ntasks) {
    if (w == 0) atomic_store_explicit(&sh->cpu0, pal_current_cpu(), memory_order_relaxed);
    /* On cancel, or once the run has stopped (its windows are closed, so nothing more is measured), the remaining
       tasks are counted as done without running: the job ends quickly instead of finishing a long build silently. */
    if (!ctx_cancelled() && !atomic_load_explicit(&sh->stop, memory_order_relaxed)) {
      uint64_t t_start = pal_now_raw_ns();
      uint64_t h = sh->tk->task(inst, scratch, i);
      atomic_fetch_add(&sh->job_sum, pmk_job_combine(0, i, h));
      account(sh, w, t_start, sh->tk->task_work(inst, i));
    }
    if (atomic_fetch_add(&sh->done, 1) + 1 == sh->ntasks) {
      pal_lock_acquire(sh->lock);
      pal_lock_broadcast(sh->lock);
      pal_lock_release(sh->lock);
    }
  }
}

static void *threaded_main(void *arg) {
  worker *wk = arg;
  shared *sh = wk->sh;
  const void *inst = sh->s->inst;
  if (wk->cpu >= 0) pal_pin_self(wk->cpu);
  void *scratch = sh->tk->scratch_new ? sh->tk->scratch_new(inst) : NULL;
  if (sh->tk->scratch_new && !scratch) {
    atomic_store(&sh->failed, 1);
    atomic_store(&sh->stop, 1);
  }

  if (wk->index == 0) {
    while (!atomic_load(&sh->stop) && !ctx_cancelled()) {
      pal_lock_acquire(sh->lock);
      /* done before next: a worker can only obtain a task index after both are reset. */
      atomic_store(&sh->done, 0);
      atomic_store(&sh->job_sum, 0);
      atomic_store(&sh->next, 0);
      sh->gen++;
      pal_lock_broadcast(sh->lock);
      pal_lock_release(sh->lock);
      uint64_t t0 = pal_now_raw_ns();
      if (scratch || !sh->tk->scratch_new) work_tasks(sh, 0, inst, scratch);
      pal_lock_acquire(sh->lock);
      while (atomic_load(&sh->done) < sh->ntasks) pal_lock_wait(sh->lock);
      pal_lock_release(sh->lock);
      uint64_t t1 = pal_now_raw_ns();
      /* A job cut short by stop or cancel is incomplete: its time and checksum are not recorded. */
      if (!ctx_cancelled() && !atomic_load(&sh->stop)) record_job(sh, t1 - t0, atomic_load(&sh->job_sum));
    }
    pal_lock_acquire(sh->lock);
    sh->stopping = 1;
    pal_lock_broadcast(sh->lock);
    pal_lock_release(sh->lock);
  } else {
    uint64_t seen = 0;
    for (;;) {
      pal_lock_acquire(sh->lock);
      while (sh->gen == seen && !sh->stopping) pal_lock_wait(sh->lock);
      int stopping = sh->stopping;
      seen = sh->gen;
      pal_lock_release(sh->lock);
      if (stopping) break;
      if (scratch || !sh->tk->scratch_new) work_tasks(sh, wk->index, inst, scratch);
    }
  }
  if (scratch && sh->tk->scratch_free) sh->tk->scratch_free(scratch);
  return NULL;
}

int wl_sustained(pmk_ctx *c, const sus_spec *s, pmk_result *r) {
  const pmk_tk *tk = s->tk;
  double win_ms = wl_window_ms(c, tk), win_s = win_ms / 1e3;
  double measure_s = c->cfg.measure_s * (!strcmp(tk->id, "K1") ? SLOW_KERNEL_MEASURE_FACTOR : 1);
  /* Whole windows: the warm-up ones are kept (they show how much slower the machine gets when hot) but not
     scored; the measured ones follow. */
  size_t warm_w = (size_t)ceil(s->warm_s / win_s - 1e-9), meas_w = (size_t)ceil(measure_s / win_s - 1e-9);
  if (meas_w < 1) meas_w = 1;
  if (s->warm_only) meas_w = 0;
  size_t total_w = warm_w + meas_w;
  if (total_w < 1) total_w = 1;
  r->windowed = 1;
  r->window_ms = win_ms;
  r->nthreads = s->nthreads;
  r->tau_s = NAN;
  r->input_hash = tk->input_hash(s->inst);

  shared *sh = calloc(1, sizeof *sh);
  worker *wk = calloc((size_t)s->nthreads, sizeof *wk);
  if (!sh || !wk) {
    free(sh);
    free(wk);
    return PMK_ERR_NOMEM;
  }
  sh->c = c;
  sh->s = s;
  sh->tk = tk;
  sh->win_ns = (uint64_t)(win_ms * 1e6);
  /* Room for the run plus the tasks still running at its end, which finish (and are credited) after it. */
  sh->maxwin = total_w * 2 + 64;
  sh->work = calloc((size_t)s->nthreads * sh->maxwin, sizeof *sh->work);
  sh->lock = pal_lock_new();
  sh->ntasks = tk->ntasks(s->inst);
  atomic_store(&sh->cpu0, -1);
  int rc = PMK_OK;
  if (!sh->work || !sh->lock) rc = PMK_ERR_NOMEM;

  int started = 0;
  if (rc == PMK_OK) {
    pal_unpin_self(); /* the monitor (this thread) runs wherever the OS has room */
    uint64_t t0_mono = pal_now_ns();
    sh->t0_raw = pal_now_raw_ns();
    for (int i = 0; i < s->nthreads; i++) {
      wk[i].sh = sh;
      wk[i].index = i;
      wk[i].cpu = s->cpus && ctx_can_place(c) ? s->cpus[i] : -1;
      wk[i].th = pal_thread_start(threaded_main, &wk[i]);
      if (!wk[i].th) {
        atomic_store(&sh->stop, 1);
        rc = PMK_ERR_SYSTEM;
        break;
      }
      started++;
    }

    /* Monitor: a fixed number of windows. It records the temperature, clock, package power and the core of
       worker 0 per window;
       the throughput of each window is read after the run, once every task has been credited. */
    int *cpu_of = calloc(total_w, sizeof *cpu_of);
    if (!cpu_of) rc = PMK_ERR_NOMEM;
    uint64_t last_event = t0_mono;
    pal_power pw;
    pal_power_start(&pw);
    size_t w = 0;
    for (; rc == PMK_OK && w < total_w; w++) {
      pal_sleep_until_ns(t0_mono + (uint64_t)(w + 1) * sh->win_ns);
      cpu_of[w] = atomic_load(&sh->cpu0);
      double khz = 0;
      int nk = 0;
      for (int i = 0; i < s->nthreads; i++) {
        int k = pal_cpu_cur_khz(s->cpus ? s->cpus[i] : c->m.cpus[i % c->m.ncpu].id);
        if (k > 0) khz += k, nk++;
      }
      if (dvec_push(&r->temps, pal_cpu_temp_c()) || dvec_push(&r->mhz, nk ? khz / nk / 1000 : 0) ||
          dvec_push(&r->power, pal_power_read(&pw)))
        rc = PMK_ERR_NOMEM;
      if (ctx_cancelled()) rc = PMK_ERR_CANCELLED;
      if (atomic_load(&sh->failed)) rc = PMK_ERR_NOMEM;
      if (rc == PMK_OK && pal_now_ns() - last_event > 10000000000ull) {
        last_event = pal_now_ns();
        double t = (double)(w + 1) * win_s;
        ctx_emit(c, PMK_EV_INFO, r->mode, tk->id, 0, 0, "%d thread%s: %.0f s of %.0f (%s)", s->nthreads,
                 s->nthreads == 1 ? "" : "s", t,
                 (double)total_w * win_s, w + 1 <= warm_w ? "warming up, not scored" : "measuring");
      }
    }
    atomic_store(&sh->stop, 1);
    for (int i = 0; i < started; i++) pal_thread_join(wk[i].th);
    for (size_t i = 0; i < w && rc != PMK_ERR_NOMEM; i++) {
      uint64_t sum = 0;
      for (int t = 0; t < s->nthreads; t++)
        sum += atomic_load_explicit(&sh->work[(size_t)t * sh->maxwin + i], memory_order_relaxed);
      if (result_push(r, (double)sum / win_s / tk->unit_div, NAN, cpu_of[i])) rc = PMK_ERR_NOMEM;
    }
    free(cpu_of);
    r->elapsed_s = (double)r->n * win_s;
  }

  /* The scored set is the measurement after the warm-up ("steady" in the result format); for a warm-up series,
     its last 10 windows (the hot end, against the first windows for R_throttle). */
  if (s->warm_only) r->steady_from = r->n > 13 ? r->n - 10 : r->n / 2;
  else r->steady_from = r->n > warm_w ? warm_w : 0;
  r->steady = rc == PMK_OK && r->n == total_w && !s->warm_only;
  /*
   * Platform clamp: a clock below the CPU's own hardware minimum (or, where that is unknown, below 15 % of its
   * maximum, under the base clock any sustained load settles at) is forced from outside the processor, e.g. by
   * firmware (BD PROCHOT on some laptops), not by the CPU's own thermal or power limits. It is counted over the
   * scored windows and reported; the score stays as measured.
   */
  const pmk_cpu *cpu0 = NULL;
  for (int i = 0; i < c->m.ncpu; i++)
    if (c->m.cpus[i].id == (s->cpus ? s->cpus[0] : c->m.cpus[0].id)) cpu0 = &c->m.cpus[i];
  double limit = cpu0 && cpu0->min_khz > 0 ? 0.9 * cpu0->min_khz / 1000 : cpu0 && cpu0->max_khz > 0 ? 0.15 * cpu0->max_khz / 1000 : 0;
  r->lowest_mhz = NAN;
  for (size_t i = r->steady_from; limit > 0 && i < r->mhz.n && i < r->n; i++)
    if (r->mhz.v[i] > 0 && r->mhz.v[i] < limit) {
      r->clamped++;
      if (!(r->lowest_mhz <= r->mhz.v[i])) r->lowest_mhz = r->mhz.v[i];
    }
  if (r->clamped)
    ctx_emit(c, PMK_EV_WARNING, r->mode, tk->id, 0, 0,
             "%s n=%d: the platform throttled the CPU to %.0f MHz (below its %.0f MHz minimum) in %d of %zu scored "
             "windows; the score includes it",
             tk->id, s->nthreads, r->lowest_mhz, limit / 0.9, r->clamped, r->n - r->steady_from);
  r->job_ns = sh->job_ns;
  r->checksum = sh->checksum;
  r->checksum_ok = sh->checksum_set && !sh->checksum_bad;
  if (rc == PMK_OK && sh->checksum_bad)
    ctx_emit(c, PMK_EV_WARNING, r->mode, tk->id, 0, 0, "%s: jobs produced different checksums", tk->id);
  pal_lock_free(sh->lock);
  free(sh->work);
  free(sh);
  free(wk);
  return rc;
}
