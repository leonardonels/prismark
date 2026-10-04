/*
 * Throughput-kernel execution shared by the ST and MC modes: single jobs on
 * the calling thread, and sustained runs on pinned worker threads with
 * windowed throughput and steady-state detection.
 *
 * Threaded runs share each job: workers take tasks from a common counter and
 * the leader (worker 0) starts the next job only when every task of the
 * current one is done, as a renderer finishes a frame before the next.
 * Instance runs give every worker its own copy of the inputs and let it run
 * jobs back to back, with no coordination at all.
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

#define STEADY_K 10  /* windows in the steady-state set and the slope test */
#define SLOW_KERNEL_WINDOW_FACTOR 20

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
  /* K1 tasks are whole translation units (seconds each); a window must hold many of them. */
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
  const void *inst; /* instances mode: this copy's inputs */
  pal_thread *th;
} worker;

static void account(shared *sh, int w, uint64_t work) {
  uint64_t idx = (pal_now_raw_ns() - sh->t0_raw) / sh->win_ns;
  if (idx < sh->maxwin) atomic_fetch_add_explicit(&sh->work[(size_t)w * sh->maxwin + idx], work, memory_order_relaxed);
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
    /* On cancel the remaining tasks are counted as done without running, so the job ends quickly. */
    if (!ctx_cancelled()) {
      uint64_t h = sh->tk->task(inst, scratch, i);
      atomic_fetch_add(&sh->job_sum, pmk_job_combine(0, i, h));
      account(sh, w, sh->tk->task_work(inst, i));
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
      if (!ctx_cancelled()) record_job(sh, t1 - t0, atomic_load(&sh->job_sum));
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

static void *instance_main(void *arg) {
  worker *wk = arg;
  shared *sh = wk->sh;
  if (wk->cpu >= 0) pal_pin_self(wk->cpu);
  const void *inst = wk->inst;
  void *scratch = inst && sh->tk->scratch_new ? sh->tk->scratch_new(inst) : NULL;
  if (!inst || (sh->tk->scratch_new && !scratch)) {
    atomic_store(&sh->failed, 1);
    atomic_store(&sh->stop, 1);
  } else {
    size_t n = sh->tk->ntasks(inst);
    while (!atomic_load(&sh->stop) && !ctx_cancelled()) {
      uint64_t t0 = pal_now_raw_ns(), acc = 0;
      size_t i;
      for (i = 0; i < n && !atomic_load_explicit(&sh->stop, memory_order_relaxed); i++) {
        if (wk->index == 0) atomic_store_explicit(&sh->cpu0, pal_current_cpu(), memory_order_relaxed);
        acc = pmk_job_combine(acc, i, sh->tk->task(inst, scratch, i));
        account(sh, wk->index, sh->tk->task_work(inst, i));
      }
      if (i == n && wk->index == 0) record_job(sh, pal_now_raw_ns() - t0, acc);
    }
  }
  if (scratch && sh->tk->scratch_free) sh->tk->scratch_free(scratch);
  return NULL;
}

/* Decides, after each window, whether the run has reached steady state (spec 9.3). */
static int steady_now(pmk_ctx *c, pmk_result *r, double elapsed_s, double win_s) {
  size_t nw = r->n;
  if (nw < 1 + STEADY_K) return 0;
  const double *perf = r->samples + nw - STEADY_K;
  int flat = pmk_slope_flat(perf, STEADY_K, NULL);
  /* Temperature trace from window 0; tau is refitted every window. */
  double tau = NAN;
  if (r->temps.n >= 5) {
    double *t = malloc(r->temps.n * sizeof *t);
    if (t) {
      for (size_t i = 0; i < r->temps.n; i++) t[i] = (double)(i + 1) * win_s;
      tau = pmk_fit_tau(t, r->temps.v, r->temps.n, NULL);
      free(t);
    }
  }
  r->tau_s = tau;
  (void)c;
  return flat && (!isfinite(tau) || elapsed_s > 5 * tau);
}

int wl_sustained(pmk_ctx *c, const sus_spec *s, pmk_result *r) {
  const pmk_tk *tk = s->tk;
  double win_ms = wl_window_ms(c, tk);
  double min_s = c->cfg.sustained_min_s, max_s = c->cfg.sustained_max_s;
  if (max_s < min_s) max_s = min_s;
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
  /* Room for the longest run plus slack for a long final job. */
  sh->maxwin = (size_t)(max_s * 1e3 / win_ms) * 4 + 64;
  sh->work = calloc((size_t)s->nthreads * sh->maxwin, sizeof *sh->work);
  sh->lock = pal_lock_new();
  sh->ntasks = tk->ntasks(s->inst);
  atomic_store(&sh->cpu0, -1);
  int rc = PMK_OK;
  if (!sh->work || !sh->lock) rc = PMK_ERR_NOMEM;

  /* Instances: copy 0 uses the given inputs, every other copy gets its own, created before timing starts. */
  for (int i = 0; i < s->nthreads && rc == PMK_OK; i++) {
    wk[i].inst = s->inst;
    if (s->instances && i > 0 && !(wk[i].inst = wl_create(c, tk, wl_size_for(c, tk), r->mode))) rc = PMK_ERR_SYSTEM;
  }

  int started = 0;
  if (rc == PMK_OK) {
    pal_unpin_self(); /* the monitor (this thread) runs wherever the OS has room */
    uint64_t t0_mono = pal_now_ns();
    sh->t0_raw = pal_now_raw_ns();
    for (int i = 0; i < s->nthreads; i++) {
      wk[i].sh = sh;
      wk[i].index = i;
      wk[i].cpu = s->cpus && ctx_can_place(c) ? s->cpus[i] : -1;
      wk[i].th = pal_thread_start(s->instances ? instance_main : threaded_main, &wk[i]);
      if (!wk[i].th) {
        atomic_store(&sh->stop, 1);
        rc = PMK_ERR_SYSTEM;
        break;
      }
      started++;
    }

    /* Monitor: close each window shortly after its end, then decide whether to stop. */
    double win_s = win_ms / 1e3;
    uint64_t last_event = t0_mono;
    for (size_t w = 0; rc == PMK_OK && w + 1 < sh->maxwin; w++) {
      pal_sleep_until_ns(t0_mono + (uint64_t)(w + 1) * sh->win_ns + sh->win_ns / 10);
      uint64_t sum = 0;
      for (int i = 0; i < s->nthreads; i++)
        sum += atomic_load_explicit(&sh->work[(size_t)i * sh->maxwin + w], memory_order_relaxed);
      if (result_push(r, (double)sum / win_s / tk->unit_div, NAN, atomic_load(&sh->cpu0)) ||
          dvec_push(&r->temps, pal_cpu_temp_c())) {
        rc = PMK_ERR_NOMEM;
        break;
      }
      double elapsed = (double)(w + 1) * win_s;
      size_t njobs = atomic_load(&sh->njobs);
      if (ctx_cancelled()) {
        rc = PMK_ERR_CANCELLED;
        break;
      }
      if (atomic_load(&sh->failed)) {
        rc = PMK_ERR_NOMEM;
        break;
      }
      int enough_jobs = njobs >= (size_t)(s->min_jobs > 0 ? s->min_jobs : 1);
      int steady = elapsed >= min_s && enough_jobs && steady_now(c, r, elapsed, win_s);
      if (pal_now_ns() - last_event > 10000000000ull) {
        last_event = pal_now_ns();
        ctx_emit(c, PMK_EV_INFO, r->mode, tk->id, 0, 0, "%s n=%d: %.0f s, %.4g %s (last window)%s", r->mode,
                 s->nthreads, elapsed, r->samples[r->n - 1], tk->unit, steady ? ", steady" : "");
      }
      if (steady) {
        r->steady = 1;
        break;
      }
      if (elapsed >= max_s && enough_jobs) break;
    }
    atomic_store(&sh->stop, 1);
    for (int i = 0; i < started; i++) pal_thread_join(wk[i].th);
    r->elapsed_s = (double)r->n * win_s;
  }

  r->steady_from = r->n > STEADY_K ? r->n - STEADY_K : (r->n > 1 ? 1 : 0);
  if (!isfinite(r->tau_s) && r->temps.n >= 5) steady_now(c, r, r->elapsed_s, win_ms / 1e3);
  r->job_ns = sh->job_ns;
  r->checksum = sh->checksum;
  r->checksum_ok = sh->checksum_set && !sh->checksum_bad;
  if (rc == PMK_OK && sh->checksum_bad)
    ctx_emit(c, PMK_EV_WARNING, r->mode, tk->id, 0, 0, "%s: jobs produced different checksums", tk->id);
  for (int i = 1; i < s->nthreads; i++)
    if (s->instances && wk[i].inst) tk->destroy((void *)wk[i].inst);
  pal_lock_free(sh->lock);
  free(sh->work);
  free(sh);
  free(wk);
  return rc;
}
