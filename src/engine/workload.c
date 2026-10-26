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
  uint64_t checksum, checksum_bad; /* checksum_bad: jobs whose checksum differed from the reference */
  uint64_t expected;               /* the known-good job checksum (pmk_tk.expected), 0 if unknown */
  int checksum_set;

  /* Wrong results by task: each task's checksum against its own from the first job, so the CPU that computed a
     wrong one is known. A task index is never run twice at once (a job starts when the last one is done). */
  uint64_t *ref;
  unsigned char *ref_set;
  _Atomic uint64_t wrong_tasks;
  _Atomic uint64_t *wrong_by_cpu; /* by CPU id, ncpu_ids entries */
  int ncpu_ids;
  _Atomic uint64_t first_wrong_ns; /* since t0_raw, plus 1 (0: none yet) */
  atomic_int first_wrong_cpu;
  uint64_t fault_after;            /* PRISMARK_STRESS_FAULT=CPU:SECONDS, tests only: one wrong task then, 0 if unset */
  int fault_cpu;
  atomic_int fault_done;
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

/* The first wrong result: when (since the start) and on which CPU (-1 if not known). */
static void note_wrong(shared *sh, int cpu) {
  uint64_t none = 0, t = pal_now_raw_ns() - sh->t0_raw + 1;
  if (atomic_compare_exchange_strong(&sh->first_wrong_ns, &none, t)) atomic_store(&sh->first_wrong_cpu, cpu);
}

static void record_job(shared *sh, uint64_t ns, uint64_t sum) {
  /* Against the known-good checksum where the kernel has one, else against the first job. */
  if (!sh->checksum_set) {
    sh->checksum = sh->expected ? sh->expected : sum;
    sh->checksum_set = 1;
  }
  if (sum != sh->checksum) {
    sh->checksum_bad++;
    note_wrong(sh, -1);
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
      if (sh->ref && sh->fault_after && pal_current_cpu() == sh->fault_cpu &&
          pal_now_raw_ns() - sh->t0_raw > sh->fault_after && !atomic_exchange(&sh->fault_done, 1))
        h ^= 1; /* PRISMARK_STRESS_FAULT: one wrong result, for testing the checks */
      if (sh->ref) {
        if (!sh->ref_set[i]) {
          sh->ref[i] = h;
          sh->ref_set[i] = 1;
        } else if (h != sh->ref[i]) {
          int cpu = pal_current_cpu();
          atomic_fetch_add(&sh->wrong_tasks, 1);
          if (cpu >= 0 && cpu < sh->ncpu_ids) atomic_fetch_add(&sh->wrong_by_cpu[cpu], 1);
          note_wrong(sh, cpu);
        }
      }
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

/*
 * Clock domains: CPUs of one core type in one last-level cache domain run at one clock (a CCX on AMD, a core type
 * on hybrid processors). On machines with several, every series records the clock of each, so a chart can follow
 * a domain across thread counts: cpu_dom[j] is machine CPU j's domain and dom[i] thread i's, and the domains no
 * thread runs on are marked idle in r. r->nclock_dom stays 0 on machines with a single domain.
 */
static void clock_domains(const pmk_ctx *c, const sus_spec *s, pmk_result *r, int *cpu_dom, int *dom) {
  const pmk_machine *m = &c->m;
  int key[PMK_MAX_CLOCK_DOMAINS], n = 0, many_llc[PMK_MAX_TYPES] = {0}, llc_of_type[PMK_MAX_TYPES];
  for (int t = 0; t < PMK_MAX_TYPES; t++) llc_of_type[t] = -1;
  for (int i = 0; i < m->ncpu; i++) { /* a core type spanning several cache domains names them */
    int t = m->cpus[i].type;
    if (llc_of_type[t] < 0) llc_of_type[t] = m->cpus[i].llc;
    else if (llc_of_type[t] != m->cpus[i].llc) many_llc[t] = 1;
  }
  for (int j = 0; j < m->ncpu; j++) {
    const pmk_cpu *cpu = &m->cpus[j];
    int k = cpu->type * 4096 + (many_llc[cpu->type] ? cpu->llc : 0), d = 0;
    while (d < n && key[d] != k) d++;
    if (d == n && n < PMK_MAX_CLOCK_DOMAINS) {
      key[n++] = k;
      const char *type = m->ntypes > 1 ? m->type_names[cpu->type] : "";
      if (many_llc[cpu->type])
        snprintf(r->clock_dom[d], sizeof r->clock_dom[d], "%s%s%s%d", type, *type ? " " : "",
                 strstr(m->model, "AMD") ? "CCX" : "L3 ", cpu->llc);
      else snprintf(r->clock_dom[d], sizeof r->clock_dom[d], "%s", type);
    }
    cpu_dom[j] = d < n ? d : n - 1;
  }
  /* In the order of the domains' keys: by core type, then by cache domain. */
  for (int a = 0; a < n; a++)
    for (int b = a + 1; b < n; b++)
      if (key[b] < key[a]) {
        int t = key[a];
        key[a] = key[b], key[b] = t;
        char name[16];
        memcpy(name, r->clock_dom[a], sizeof name);
        memcpy(r->clock_dom[a], r->clock_dom[b], sizeof name);
        memcpy(r->clock_dom[b], name, sizeof name);
        for (int j = 0; j < m->ncpu; j++) cpu_dom[j] = cpu_dom[j] == a ? b : cpu_dom[j] == b ? a : cpu_dom[j];
      }
  for (int d = 0; d < n; d++) r->clock_idle[d] = 1;
  for (int i = 0; i < s->nthreads; i++) {
    int j = i % m->ncpu;
    if (s->cpus)
      for (int k = 0; k < m->ncpu; k++)
        if (m->cpus[k].id == s->cpus[i]) j = k;
    dom[i] = cpu_dom[j];
    r->clock_idle[dom[i]] = 0;
  }
  r->nclock_dom = n > 1 ? n : 0;
}

int wl_sustained(pmk_ctx *c, const sus_spec *s, pmk_result *r) {
  const pmk_tk *tk = s->tk;
  double win_ms = s->window_ms > 0 ? s->window_ms : wl_window_ms(c, tk), win_s = win_ms / 1e3;
  double measure_s = s->measure_s > 0 ? s->measure_s
                                      : c->cfg.measure_s * (!strcmp(tk->id, "K1") ? SLOW_KERNEL_MEASURE_FACTOR : 1);
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
  int rc = PMK_OK;
  sh->c = c;
  sh->s = s;
  sh->tk = tk;
  sh->win_ns = (uint64_t)(win_ms * 1e6);
  /* Room for the run plus the tasks still running at its end, which finish (and are credited) after it. */
  sh->maxwin = total_w * 2 + 64;
  sh->work = calloc((size_t)s->nthreads * sh->maxwin, sizeof *sh->work);
  sh->lock = pal_lock_new();
  sh->ntasks = tk->ntasks(s->inst);
  sh->expected = tk->expected ? tk->expected(s->inst) : 0;
  if (s->check_tasks) {
    for (int i = 0; i < c->m.ncpu; i++)
      if (c->m.cpus[i].id >= sh->ncpu_ids) sh->ncpu_ids = c->m.cpus[i].id + 1;
    sh->ref = calloc(sh->ntasks, sizeof *sh->ref);
    sh->ref_set = calloc(sh->ntasks, 1);
    sh->wrong_by_cpu = calloc((size_t)sh->ncpu_ids, sizeof *sh->wrong_by_cpu);
    if (!sh->ref || !sh->ref_set || !sh->wrong_by_cpu) rc = PMK_ERR_NOMEM;
    const char *fault = getenv("PRISMARK_STRESS_FAULT");
    double after;
    if (fault && sscanf(fault, "%d:%lf", &sh->fault_cpu, &after) == 2 && after > 0)
      sh->fault_after = (uint64_t)(after * 1e9);
  }
  atomic_store(&sh->cpu0, -1);
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
    int *cpu_of = calloc(total_w, sizeof *cpu_of), *dom = calloc((size_t)s->nthreads, sizeof *dom),
        *cpu_dom = calloc((size_t)c->m.ncpu, sizeof *cpu_dom);
    if (!cpu_of || !dom || !cpu_dom) rc = PMK_ERR_NOMEM;
    else clock_domains(c, s, r, cpu_dom, dom);
    uint64_t last_event = t0_mono;
    pal_power pw;
    pal_power_start(&pw);
    size_t w = 0;
    for (; rc == PMK_OK && w < total_w; w++) {
      pal_sleep_until_ns(t0_mono + (uint64_t)(w + 1) * sh->win_ns);
      cpu_of[w] = atomic_load(&sh->cpu0);
      double khz = 0, dkhz[PMK_MAX_CLOCK_DOMAINS] = {0};
      int nk = 0, dn[PMK_MAX_CLOCK_DOMAINS] = {0};
      for (int i = 0; i < s->nthreads; i++) {
        int k = pal_cpu_cur_khz(s->cpus ? s->cpus[i] : c->m.cpus[i % c->m.ncpu].id);
        if (k > 0) khz += k, nk++, dkhz[dom[i]] += k, dn[dom[i]]++;
      }
      double temp = pal_cpu_temp_c();
      if (dvec_push(&r->temps, temp) || dvec_push(&r->mhz, nk ? khz / nk / 1000 : 0) ||
          dvec_push(&r->power, pal_power_read(&pw)))
        rc = PMK_ERR_NOMEM;
      /* An idle domain's clock: the average of all its CPUs. */
      for (int j = 0; j < c->m.ncpu && r->nclock_dom; j++)
        if (r->clock_idle[cpu_dom[j]]) {
          int k = pal_cpu_cur_khz(c->m.cpus[j].id);
          if (k > 0) dkhz[cpu_dom[j]] += k, dn[cpu_dom[j]]++;
        }
      for (int d = 0; d < r->nclock_dom; d++)
        if (dvec_push(&r->mhz_dom[d], dn[d] ? dkhz[d] / dn[d] / 1000 : 0)) rc = PMK_ERR_NOMEM;
      /* Stress test: a wrong result, or the temperature limit, ends it there (this window counts: it ran whole). */
      int wrong = atomic_load(&sh->first_wrong_ns) != 0;
      if (rc == PMK_OK && ((s->stop_on_error && wrong) || (s->max_temp_c > 0 && temp >= s->max_temp_c))) {
        atomic_store(&sh->stop, 1);
        r->stopped = 1;
        r->stop_reason = s->stop_on_error && wrong ? "error" : "temperature";
        w++;
        break;
      }
      if (ctx_cancelled() && s->open_ended && w + 1 > warm_w) {
        /* The end of a run without a set length: the windows before this one are the measurement. This one is
           left out, as the workers stopped when the cancel arrived during it. They stop before the cancel is
           cleared, so the job cut short is not recorded either. */
        atomic_store(&sh->stop, 1);
        ctx_uncancel();
        r->stopped = 1;
        r->stop_reason = "user";
        break;
      }
      if (ctx_cancelled()) rc = PMK_ERR_CANCELLED;
      if (atomic_load(&sh->failed)) rc = PMK_ERR_NOMEM;
      if (rc == PMK_OK && pal_now_ns() - last_event > 10000000000ull) {
        last_event = pal_now_ns();
        double t = (double)(w + 1) * win_s;
        if (s->open_ended)
          ctx_emit(c, PMK_EV_INFO, r->mode, tk->id, 0, 0, "%d thread%s: %.0f s (until stopped)", s->nthreads,
                   s->nthreads == 1 ? "" : "s", t);
        else
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
    free(dom);
    free(cpu_dom);
    r->elapsed_s = (double)r->n * win_s;
  }

  /* The scored set is the measurement after the warm-up ("steady" in the result format); for a warm-up series,
     its last 10 windows (the hot end, against the first windows for R_throttle). */
  if (s->warm_only) r->steady_from = r->n > 13 ? r->n - 10 : r->n / 2;
  else r->steady_from = r->n > warm_w ? warm_w : 0;
  r->steady = rc == PMK_OK && (r->n == total_w || r->stopped) && !s->warm_only;
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
  r->checksum_ok = sh->checksum_set && !sh->checksum_bad && !atomic_load(&sh->wrong_tasks);
  r->errors = sh->checksum_bad;
  r->wrong_tasks = atomic_load(&sh->wrong_tasks);
  uint64_t fw = atomic_load(&sh->first_wrong_ns);
  r->first_wrong_s = fw ? (double)(fw - 1) * 1e-9 : NAN;
  r->first_wrong_cpu = fw ? atomic_load(&sh->first_wrong_cpu) : -1;
  for (int i = 0; sh->wrong_by_cpu && i < sh->ncpu_ids; i++)
    if (atomic_load(&sh->wrong_by_cpu[i]) && r->nwrong_cpu < PMK_MAX_WRONG_CPUS) {
      r->wrong_cpu[r->nwrong_cpu] = i;
      r->wrong_cpu_n[r->nwrong_cpu++] = atomic_load(&sh->wrong_by_cpu[i]);
    }
  if (!r->stop_reason && s->check_tasks) r->stop_reason = "time";
  if (rc == PMK_OK && sh->checksum_bad)
    ctx_emit(c, PMK_EV_WARNING, r->mode, tk->id, 0, 0, "%s: %llu of %zu jobs produced a different checksum", tk->id,
             (unsigned long long)sh->checksum_bad, atomic_load(&sh->njobs));
  pal_lock_free(sh->lock);
  free(sh->work);
  free(sh->ref);
  free(sh->ref_set);
  free(sh->wrong_by_cpu);
  free(sh);
  free(wk);
  return rc;
}
