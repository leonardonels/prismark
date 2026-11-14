/*
 * Throughput modes.
 *
 *   ST burst      single-core speed on short interactive tasks: K3 (small
 *                 input), K4, K5, K6; pre-warmed; one series per core type
 *   ST sustained  single-core speed at thermal steady state: K1, K2, K12
 *   MC threaded   scaling of multithreaded software: K1, K2, K12, K13 (and K1x) at
 *                 n = 1, 2, 4, ..., all CPUs, each run sustained
 *   MC instances  memory latency under contention: K7 while n - 1 other
 *                 copies chase memory on the other CPUs
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

#define MAX_STEPS 32
#define BURST_WARMUP_JOBS 2

static const char *const BURST_KERNELS[] = {"K3", "K4", "K5", "K6"};
static const char *const SUSTAINED_KERNELS[] = {"K1", "K2", "K12"};
static const char *const THREADED_KERNELS[] = {"K1", "K2", "K12", "K13"};

#define COUNT(a) ((int)(sizeof(a) / sizeof *(a)))

static void set_tk_fields(pmk_result *r, const pmk_tk *tk, const pmk_kernels *k, const char *size) {
  r->variant = tk->variant;
  r->k = k;
  r->size = size;
}

/* ---------- ST burst ---------- */

typedef struct burst_job {
  const pmk_tk *tk;
  int type, cpu;
  void *inst, *scratch;
  size_t res; /* index of its series in c->res (the array moves as series are added) */
} burst_job;

/* One session of a short-task series: two unscored jobs to bring the clock and the caches up, then the timed ones. */
static int burst_session(pmk_ctx *c, burst_job *j, int round, int rounds) {
  const pmk_tk *tk = j->tk;
  if (ctx_can_place(c) && pal_pin_self(j->cpu) && round == 0)
    ctx_emit(c, PMK_EV_WARNING, "st_burst", tk->id, 0, 0, "could not pin to CPU %d", j->cpu);
  pmk_result *r = &c->res[j->res];
  size_t from = r->n;
  result_session(r);
  for (int i = 0; i < BURST_WARMUP_JOBS; i++) {
    uint64_t h = wl_job(tk, j->inst, j->scratch);
    if (round == 0) r->checksum = h, r->checksum_ok = 1;
    else if (h != r->checksum) r->checksum_ok = 0;
  }
  while (!ctx_session_done(c, r, from, rounds, c->cfg.max_reps)) {
    if (ctx_cancelled()) return PMK_ERR_CANCELLED;
    int obs = pal_current_cpu();
    uint64_t t0 = pal_now_raw_ns();
    uint64_t h = wl_job(tk, j->inst, j->scratch);
    uint64_t t1 = pal_now_raw_ns();
    if (h != r->checksum) r->checksum_ok = 0;
    if (result_push(r, (double)(t1 - t0), NAN, obs)) return PMK_ERR_NOMEM;
  }
  return PMK_OK;
}

int mode_st_burst_steps(const pmk_ctx *c) {
  int n = 0;
  for (int i = 0; i < COUNT(BURST_KERNELS); i++)
    if (ctx_kernel_selected(c, BURST_KERNELS[i]) && pmk_find_tk(c->k, BURST_KERNELS[i], NULL)) n++;
  return ctx_ntypes(c) * n;
}

/* About 20 jobs of tens of milliseconds, plus the input. */
double mode_st_burst_est(const pmk_ctx *c) { return 1.5 * mode_st_burst_steps(c); }

int mode_st_burst(pmk_ctx *c) {
  const pmk_tk *list[COUNT(BURST_KERNELS)];
  int n = 0;
  for (int i = 0; i < COUNT(BURST_KERNELS); i++) {
    if (!ctx_kernel_selected(c, BURST_KERNELS[i])) continue;
    const pmk_tk *tk = pmk_find_tk(c->k, BURST_KERNELS[i], NULL);
    if (tk) list[n++] = tk;
    else ctx_unavailable(c, BURST_KERNELS[i], NULL, "st_burst", "not built");
  }
  int ntypes = ctx_ntypes(c);
  burst_job jobs[COUNT(BURST_KERNELS) * PMK_MAX_TYPES];
  int nj = 0, rc = PMK_OK;
  /* Every test's input once, on the CPU it will run on; the inputs stay for all the rounds. */
  for (int t = 0; t < ntypes && rc == PMK_OK; t++)
    for (int i = 0; i < n && rc == PMK_OK; i++) {
      burst_job *j = &jobs[nj];
      j->tk = list[i];
      j->type = c->cfg.cpu >= 0 ? ctx_type_of_cpu(c, c->cfg.cpu) : t;
      j->cpu = ctx_cpu_for_type(c, j->type);
      if (ctx_can_place(c)) pal_pin_self(j->cpu);
      j->inst = wl_create(c, j->tk, PMK_SIZE_BURST, "st_burst");
      if (!j->inst) continue;
      j->scratch = j->tk->scratch_new ? j->tk->scratch_new(j->inst) : NULL;
      pmk_result *r = j->tk->scratch_new && !j->scratch ? NULL : ctx_new_result(c, j->tk->id, "st_burst", "ns");
      if (!r) {
        if (j->scratch && j->tk->scratch_free) j->tk->scratch_free(j->scratch);
        j->tk->destroy(j->inst);
        rc = PMK_ERR_NOMEM;
        break;
      }
      set_tk_fields(r, j->tk, c->k, "burst");
      r->type = j->type;
      r->cpu = j->cpu;
      r->input_hash = j->tk->input_hash(j->inst);
      for (size_t k = 0; k < j->tk->ntasks(j->inst); k++) r->job_work += (double)j->tk->task_work(j->inst, k);
      j->res = c->nres - 1;
      nj++;
    }
  /* Rounds: a session of every test, in a new random order each round (ctx.h, PMK_SESSIONS). */
  int rounds = ctx_sessions(c), order[COUNT(BURST_KERNELS) * PMK_MAX_TYPES];
  for (int round = 0; round < rounds && rc == PMK_OK; round++) {
    for (int i = 0; i < nj; i++) order[i] = i;
    ctx_shuffle(&c->rng, order, nj);
    for (int i = 0; i < nj && rc == PMK_OK; i++) {
      burst_job *j = &jobs[order[i]];
      if (round == 0)
        ctx_emit(c, PMK_EV_PHASE, "st_burst", j->tk->id, (uint32_t)i + 1, (uint32_t)nj, "%s on core %s (CPU %d)", j->tk->name,
                 c->m.type_names[j->type], j->cpu);
      rc = burst_session(c, j, round, rounds);
    }
    if (rc == PMK_OK && round + 1 < rounds)
      ctx_emit(c, PMK_EV_INFO, "st_burst", NULL, 0, 0, "short tasks: round %d of %d", round + 2, rounds);
  }
  for (int i = 0; i < nj; i++) {
    if (jobs[i].scratch && jobs[i].tk->scratch_free) jobs[i].tk->scratch_free(jobs[i].scratch);
    jobs[i].tk->destroy(jobs[i].inst);
  }
  return rc;
}

/* ---------- sustained series ---------- */

/*
 * A mode's warm-up: cfg.warmup_s of 3D rendering (K2) at the mode's full load, so its series start with the
 * machine as hot (and as slow) as sustained use makes it. The same load whatever tests are selected, so every
 * run warms up identically. It is recorded as a series with purpose "warmup", never scored: its R_throttle
 * (last windows against the first) is how much slower the machine gets once hot.
 */
static int mode_warm_up(pmk_ctx *c, const char *mode, const int *cpus, int nthreads) {
  if (!(c->cfg.warmup_s > 0)) return PMK_OK;
  const pmk_tk *tk = pmk_find_tk(c->k, "K2", NULL);
  if (!tk) return PMK_OK;
  void *inst = wl_create(c, tk, PMK_SIZE_FULL, mode);
  if (!inst) return PMK_OK;
  ctx_emit(c, PMK_EV_PHASE, mode, NULL, 0, 0, "warming up: %.0f s of full load on %d thread%s (not scored)",
           c->cfg.warmup_s, nthreads, nthreads == 1 ? "" : "s");
  int rc = PMK_ERR_NOMEM;
  pmk_result *r = ctx_new_result(c, tk->id, mode, tk->unit);
  if (r) {
    set_tk_fields(r, tk, c->k, "full");
    r->purpose = "warmup";
    r->type = ctx_type_of_cpu(c, cpus[0]);
    r->cpu = nthreads == 1 ? cpus[0] : -1;
    sus_spec s = {.tk = tk, .inst = inst, .nthreads = nthreads, .cpus = cpus, .warm_s = c->cfg.warmup_s, .warm_only = 1};
    rc = wl_sustained(c, &s, r);
  }
  tk->destroy(inst);
  return rc;
}

/* A new sustained series; its index in c->res, or -1 (out of memory). */
static long sustained_result(pmk_ctx *c, const char *mode, const pmk_tk *tk, int type, const int *cpus, int nthreads) {
  pmk_result *r = ctx_new_result(c, tk->id, mode, tk->unit);
  if (!r) return -1;
  set_tk_fields(r, tk, c->k, wl_size_for(c, tk) == PMK_SIZE_BURST ? "burst" : "full");
  r->type = type;
  r->cpu = nthreads == 1 ? cpus[0] : -1;
  r->lowest_mhz = NAN;
  return (long)(c->nres - 1);
}

/*
 * One session of a sustained series: the settle and the measurement, each a share of the series' (a session
 * settles for at least one window, which holds the threads' start), added to the series as a session.
 */
static int sustained_session(pmk_ctx *c, const char *mode, const pmk_tk *tk, const int *cpus, int nthreads,
                             const void *inst, size_t res, int rounds) {
  pmk_result v = {0};
  v.kernel = tk->id, v.mode = mode, v.unit = tk->unit, v.k = c->k;
  double total = c->cfg.measure_s * (!strcmp(tk->id, "K1") ? SLOW_KERNEL_MEASURE_FACTOR : 1);
  double settle = c->cfg.settle_s / rounds, win_s = wl_window_ms(c, tk) / 1e3;
  if (rounds > 1 && settle < win_s) settle = win_s;
  sus_spec s = {.tk = tk, .inst = inst, .nthreads = nthreads, .cpus = cpus, .warm_s = settle, .measure_s = total / rounds};
  int rc = wl_sustained(c, &s, &v);
  if (rc == PMK_OK) rc = wl_merge_session(&c->res[res], &v);
  result_free(&v);
  return rc;
}

int mode_st_sustained_steps(const pmk_ctx *c) {
  int n = 0;
  for (int i = 0; i < COUNT(SUSTAINED_KERNELS); i++)
    if (ctx_kernel_selected(c, SUSTAINED_KERNELS[i]) && pmk_find_tk(c->k, SUSTAINED_KERNELS[i], NULL)) n++;
  return n * ctx_ntypes(c);
}

/* One series of a kernel: its sessions' settles and measurements, and about a second to build the input. */
static double series_est(const pmk_ctx *c, const char *id) {
  int rounds = ctx_sessions(c);
  double win_s = c->cfg.window_ms / 1e3 * (!strcmp(id, "K1") ? 5 : 1), settle = c->cfg.settle_s / rounds;
  if (rounds > 1 && settle < win_s) settle = win_s;
  return rounds * settle + c->cfg.measure_s * (!strcmp(id, "K1") ? SLOW_KERNEL_MEASURE_FACTOR : 1) + 1;
}

double mode_st_sustained_est(const pmk_ctx *c) {
  int steps = mode_st_sustained_steps(c);
  if (!steps) return 0;
  double s = c->cfg.warmup_s + series_est(c, "K2") * steps;
  if (ctx_kernel_selected(c, "K1") && pmk_find_tk(c->k, "K1", NULL))
    s += (series_est(c, "K1") - series_est(c, "K2")) * ctx_ntypes(c);
  return s;
}

int mode_st_sustained(pmk_ctx *c) {
  typedef struct job {
    const pmk_tk *tk;
    int type, cpu;
    void *inst;
    long res;
  } job;
  job jobs[64];
  int nj = 0;
  int ntypes = ctx_ntypes(c);
  for (int t = 0; t < ntypes; t++)
    for (int i = 0; i < COUNT(SUSTAINED_KERNELS); i++) {
      if (!ctx_kernel_selected(c, SUSTAINED_KERNELS[i])) continue;
      const pmk_tk *tk = pmk_find_tk(c->k, SUSTAINED_KERNELS[i], NULL);
      int type = c->cfg.cpu >= 0 ? ctx_type_of_cpu(c, c->cfg.cpu) : t;
      if (tk) jobs[nj++] = (job){tk, type, ctx_cpu_for_type(c, type), NULL, -1};
      else if (t == 0) ctx_unavailable(c, SUSTAINED_KERNELS[i], NULL, "st_sustained", "not built");
    }
  /* The inputs first, so nothing is built between the warm-up and the series, or between two sessions. */
  for (int i = 0; i < nj; i++) jobs[i].inst = wl_create(c, jobs[i].tk, wl_size_for(c, jobs[i].tk), "st_sustained");

  int order[64];
  for (int i = 0; i < nj; i++) order[i] = i;
  ctx_shuffle(&c->rng, order, nj);
  int rc = PMK_OK;
  if (nj > 0) rc = mode_warm_up(c, "st_sustained", &jobs[order[0]].cpu, 1);
  /* Rounds: a session of every test on the hot core, in a new random order each round (ctx.h, PMK_SESSIONS). */
  int rounds = ctx_sessions(c);
  for (int round = 0; round < rounds && rc == PMK_OK; round++) {
    if (round > 0) ctx_shuffle(&c->rng, order, nj);
    for (int i = 0; i < nj && rc == PMK_OK; i++) {
      job *j = &jobs[order[i]];
      if (!j->inst) continue;
      if (round == 0) {
        ctx_emit(c, PMK_EV_PHASE, "st_sustained", j->tk->id, (uint32_t)i + 1, (uint32_t)nj, "%s, core %s (CPU %d)",
                 j->tk->name, c->m.type_names[j->type], j->cpu);
        ctx_cooldown(c, "st_sustained");
        if ((j->res = sustained_result(c, "st_sustained", j->tk, j->type, &j->cpu, 1)) < 0) rc = PMK_ERR_NOMEM;
      }
      if (rc == PMK_OK) rc = sustained_session(c, "st_sustained", j->tk, &j->cpu, 1, j->inst, (size_t)j->res, rounds);
    }
    if (rc == PMK_OK && round + 1 < rounds)
      ctx_emit(c, PMK_EV_INFO, "st_sustained", NULL, 0, 0, "one core: round %d of %d", round + 2, rounds);
  }
  for (int i = 0; i < nj; i++)
    if (jobs[i].inst) jobs[i].tk->destroy(jobs[i].inst);
  return rc;
}

/* ---------- MC threaded ---------- */

int mode_mc_threaded_steps(const pmk_ctx *c) {
  int steps[MAX_STEPS], nsteps = ctx_thread_steps(c, steps, MAX_STEPS), n = 0;
  for (int i = 0; i < COUNT(THREADED_KERNELS); i++)
    if (ctx_kernel_selected(c, THREADED_KERNELS[i]) && pmk_find_tk(c->k, THREADED_KERNELS[i], NULL)) n += nsteps;
  /* K1x: one build per repetition at the largest n, when a snapshot is given */
  if (ctx_kernel_selected(c, "K1x") && c->cfg.k1_data && *c->cfg.k1_data) n += c->cfg.k1x_reps > 0 ? c->cfg.k1x_reps : 3;
  return n;
}

double mode_mc_threaded_est(const pmk_ctx *c) {
  if (!mode_mc_threaded_steps(c)) return 0;
  int steps[MAX_STEPS], nsteps = ctx_thread_steps(c, steps, MAX_STEPS);
  double s = c->cfg.warmup_s;
  for (int i = 0; i < COUNT(THREADED_KERNELS); i++)
    if (ctx_kernel_selected(c, THREADED_KERNELS[i]) && pmk_find_tk(c->k, THREADED_KERNELS[i], NULL))
      s += series_est(c, THREADED_KERNELS[i]) * nsteps;
  /* A full build takes from about 20 s on a fast desktop to a few minutes on a small laptop. */
  if (ctx_kernel_selected(c, "K1x") && c->cfg.k1_data && *c->cfg.k1_data)
    s += (c->cfg.quick_inputs ? 20 : 60) * (c->cfg.k1x_reps > 0 ? c->cfg.k1x_reps : 3);
  return s;
}

int mode_mc_threaded(pmk_ctx *c) {
  int *cpus = malloc((size_t)c->m.ncpu * sizeof *cpus);
  if (!cpus) return PMK_ERR_NOMEM;
  ctx_cpu_order(c, cpus);
  int steps[MAX_STEPS];
  int nsteps = ctx_thread_steps(c, steps, MAX_STEPS);

  int rc = mode_warm_up(c, "mc_threaded", cpus, steps[nsteps - 1]); /* at the largest n: every thread */
  int korder[COUNT(THREADED_KERNELS) + 1];
  int nk = COUNT(THREADED_KERNELS) + 1; /* the last entry is K1x */
  for (int i = 0; i < nk; i++) korder[i] = i;
  ctx_shuffle(&c->rng, korder, nk);
  for (int ki = 0; ki < nk && rc == PMK_OK; ki++) {
    int sorder[MAX_STEPS];
    for (int i = 0; i < nsteps; i++) sorder[i] = i;
    ctx_shuffle(&c->rng, sorder, nsteps);
    if (korder[ki] == COUNT(THREADED_KERNELS)) {
      /* Full builds are minutes each: only at the largest n (all threads), where R_build is reported. */
      if (ctx_kernel_selected(c, "K1x")) {
        int largest = 0;
        rc = k1x_run(c, &steps[nsteps - 1], 1, &largest);
      }
      continue;
    }
    const char *id = THREADED_KERNELS[korder[ki]];
    if (!ctx_kernel_selected(c, id)) continue;
    const pmk_tk *tk = pmk_find_tk(c->k, id, NULL);
    if (!tk) {
      ctx_unavailable(c, id, NULL, "mc_threaded", "not built");
      continue;
    }
    if (!strcmp(tk->id, "K13")) { /* its matrix must fit in memory that is free: swapping would be measured */
      uint64_t avail = pal_mem_available();
      if (avail && avail < PMK_SOLVER_MEM) {
        ctx_unavailable(c, id, NULL, "mc_threaded", "not enough memory: its matrix needs about 1 GB free");
        continue;
      }
    }
    void *inst = wl_create(c, tk, wl_size_for(c, tk), "mc_threaded");
    if (!inst) continue;
    /* Rounds over the thread counts of this test, in a new random order each round (ctx.h, PMK_SESSIONS): the
       other thread counts are the time between a series' sessions, and the heat one count leaves for the next
       falls on a different one each round. */
    long res[MAX_STEPS];
    int rounds = ctx_sessions(c);
    for (int i = 0; i < nsteps; i++) res[i] = -1;
    for (int round = 0; round < rounds && rc == PMK_OK; round++) {
      if (round > 0) ctx_shuffle(&c->rng, sorder, nsteps);
      for (int si = 0; si < nsteps && rc == PMK_OK; si++) {
        int n = steps[sorder[si]];
        if (round == 0) {
          if (!strcmp(tk->id, "K1") && !ctx_memory_allows(c, tk->id, n, PMK_COMPILE_MEM_PER_THREAD)) continue;
          ctx_emit(c, PMK_EV_PHASE, "mc_threaded", tk->id, (uint32_t)si + 1, (uint32_t)nsteps, "%s, %d thread%s",
                   tk->name, n, n == 1 ? "" : "s");
          ctx_cooldown(c, "mc_threaded");
          if ((res[sorder[si]] = sustained_result(c, "mc_threaded", tk, ctx_type_of_cpu(c, cpus[0]), cpus, n)) < 0)
            rc = PMK_ERR_NOMEM;
        }
        if (rc == PMK_OK && res[sorder[si]] >= 0)
          rc = sustained_session(c, "mc_threaded", tk, cpus, n, inst, (size_t)res[sorder[si]], rounds);
      }
    }
    tk->destroy(inst);
  }
  free(cpus);
  return rc;
}

/* ---------- MC instances ---------- */

typedef struct chaser {
  const pmk_ctx *c;
  const pmk_k7_node *nodes;
  uint64_t start;
  int cpu;
  atomic_int *stop;
  pal_thread *th;
} chaser;

/* Written by several threads at once (after their timed loops), so atomic: a plain store would be a data race. */
static _Atomic uint64_t g_sink;

static void *chaser_main(void *arg) {
  chaser *ch = arg;
  if (ch->cpu >= 0) pal_pin_self(ch->cpu);
  else if (ch->c->m.caps.qos_only) pal_unpin_self();
  uint64_t p = ch->start;
  while (!atomic_load_explicit(ch->stop, memory_order_relaxed)) p = ch->c->k->k7_chase(ch->nodes, p, 1u << 12);
  atomic_store_explicit(&g_sink, p, memory_order_relaxed);
  return NULL;
}

/*
 * K7 latency under contention: copy 0 measures ns per dependent load while
 * n - 1 copies chase the same (read-only) ring from other random starts on
 * the other CPUs. Each working-set size gets its own ring.
 */
enum { K7_NSIZES = 12 };

/*
 * Working-set sizes: 16 KiB upwards in steps of 4 to 256 MiB, the size the headline result is read at, the same
 * on every machine (PMK_K7_HEADLINE_BYTES is on that grid); a machine whose last-level cache is larger than a
 * quarter of that also gets 4x its cache, so the curve still ends in main memory.
 */
#define PMK_K7_HEADLINE_BYTES (256ull << 20)
static int k7_sizes(const pmk_ctx *c, uint64_t sizes[K7_NSIZES]) {
  int ns = 0;
  uint64_t top = c->m.llc_bytes * 4;
  if (top < PMK_K7_HEADLINE_BYTES) top = PMK_K7_HEADLINE_BYTES;
  for (uint64_t s = 16ull << 10; s <= top && ns < K7_NSIZES; s *= 4) sizes[ns++] = s;
  if (sizes[ns - 1] < top && ns < K7_NSIZES) sizes[ns++] = top;
  return ns;
}

static int k7_series(pmk_ctx *c, const int *cpus, const int *steps, int nsteps) {
  if (!ctx_kernel_selected(c, "K7")) return PMK_OK;
  enum { NSIZES = K7_NSIZES };
  uint64_t sizes[NSIZES];
  int ns = k7_sizes(c, sizes);

  int sorder[NSIZES];
  for (int i = 0; i < ns; i++) sorder[i] = i;
  ctx_shuffle(&c->rng, sorder, ns);
  int rc = PMK_OK;
  for (int si = 0; si < ns && rc == PMK_OK; si++) {
    uint64_t ws = sizes[sorder[si]];
    uint64_t avail = pal_mem_available();
    if (avail && ws > avail / 2) { /* a small device: the ring would be swapped, or the app ended */
      char variant[32];
      snprintf(variant, sizeof variant, "%llu KiB", (unsigned long long)(ws >> 10));
      ctx_unavailable(c, "K7", variant, "mc_instances", "not enough free memory for this working set");
      continue;
    }
    size_t nn = (size_t)(ws / sizeof(pmk_k7_node));
    pmk_k7_node *nodes = pal_aligned_alloc(64, nn * sizeof *nodes);
    if (!nodes) return PMK_ERR_NOMEM;
    c->k->k7_build(nodes, nn, 0x6b37 + ws); /* fixed seed: identical rings on every host */
    /* Steps per repetition: 4 passes over the ring, at least 2 M loads and at most 8 M (two passes over the
       largest fixed ring; more only lengthens the run). */
    uint64_t chase_steps = nn * 4 > (2u << 20) ? nn * 4 : (2u << 20);
    if (chase_steps > (8u << 20) && chase_steps > nn) chase_steps = nn > (8u << 20) ? nn : (8u << 20);

    /* Rounds over the copy counts of this working set, in a new random order each round (ctx.h, PMK_SESSIONS). */
    int norder[MAX_STEPS], rounds = ctx_sessions(c);
    long res[MAX_STEPS];
    for (int i = 0; i < nsteps; i++) norder[i] = i, res[i] = -1;
    for (int round = 0; round < rounds && rc == PMK_OK; round++) {
      ctx_shuffle(&c->rng, norder, nsteps);
      for (int ni = 0; ni < nsteps && rc == PMK_OK; ni++) {
        int n = steps[norder[ni]];
        if (round == 0)
          ctx_emit(c, PMK_EV_PHASE, "mc_instances", "K7", (uint32_t)(si * nsteps + ni + 1), (uint32_t)(ns * nsteps),
                   "pointer chase, %llu KiB, %d cop%s", (unsigned long long)(ws >> 10), n, n == 1 ? "y" : "ies");
        atomic_int stop = 0;
        chaser *ch = calloc((size_t)n, sizeof *ch);
        if (!ch) {
          rc = PMK_ERR_NOMEM;
          break;
        }
        int started = 0, place = ctx_can_place_set(c, cpus, n);
        for (int i = 1; i < n; i++) {
          ch[i] = (chaser){c, nodes, pmk_rng_below(&c->rng, nn), place ? cpus[i] : -1, &stop, NULL};
          ch[i].th = pal_thread_start(chaser_main, &ch[i]);
          if (ch[i].th) started++;
        }
        if (place) pal_pin_self(cpus[0]);
        else if (c->m.caps.qos_only) pal_unpin_self();
        if (round == 0) {
          pmk_result *nr = ctx_new_result(c, "K7", "mc_instances", "ns");
          if (!nr) rc = PMK_ERR_NOMEM;
          else {
            nr->k = c->k;
            nr->type = ctx_type_of_cpu(c, cpus[0]);
            nr->cpu = cpus[0];
            nr->unsteered = !place && c->m.caps.qos_only;
            nr->nthreads = started + 1;
            nr->ws_bytes = ws;
            nr->size = "full";
            res[norder[ni]] = (long)(c->nres - 1);
          }
        }
        if (rc == PMK_OK && res[norder[ni]] >= 0) {
          pmk_result *r = &c->res[res[norder[ni]]];
          size_t from = r->n;
          result_session(r);
          uint64_t p = pmk_rng_below(&c->rng, nn);
          p = c->k->k7_chase(nodes, p, nn); /* warm the ring into whatever cache holds it */
          while (!ctx_session_done(c, r, from, rounds, c->cfg.max_reps)) {
            if (ctx_cancelled()) {
              rc = PMK_ERR_CANCELLED;
              break;
            }
            int obs = pal_current_cpu();
            uint64_t t0 = pal_now_raw_ns();
            p = c->k->k7_chase(nodes, p, chase_steps);
            uint64_t t1 = pal_now_raw_ns();
            if (result_push(r, (double)(t1 - t0) / (double)chase_steps, NAN, obs)) {
              rc = PMK_ERR_NOMEM;
              break;
            }
          }
          atomic_store_explicit(&g_sink, p, memory_order_relaxed);
        }
        atomic_store(&stop, 1);
        for (int i = 1; i < n; i++)
          if (ch[i].th) pal_thread_join(ch[i].th);
        free(ch);
      }
    }
    pal_aligned_free(nodes);
  }
  return rc;
}

int mode_mc_instances_steps(const pmk_ctx *c) {
  if (!ctx_kernel_selected(c, "K7")) return 0;
  uint64_t sizes[K7_NSIZES];
  int steps[MAX_STEPS];
  return k7_sizes(c, sizes) * ctx_thread_steps(c, steps, MAX_STEPS);
}

/* About 20 repetitions of 2 M loads at ~100 ns each, more for the small sets. */
double mode_mc_instances_est(const pmk_ctx *c) { return 2.0 * mode_mc_instances_steps(c); }

int mode_mc_instances(pmk_ctx *c) {
  int *cpus = malloc((size_t)c->m.ncpu * sizeof *cpus);
  if (!cpus) return PMK_ERR_NOMEM;
  ctx_cpu_order(c, cpus);
  int steps[MAX_STEPS];
  int nsteps = ctx_thread_steps(c, steps, MAX_STEPS);
  int rc = k7_series(c, cpus, steps, nsteps);
  free(cpus);
  return rc;
}
