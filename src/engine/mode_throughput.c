/*
 * Throughput modes.
 *
 *   ST burst      single-core speed on short interactive tasks: K3 (small
 *                 input), K4, K5, K6; pre-warmed; one series per core type
 *   ST sustained  single-core speed at thermal steady state: K1, K2; plus the
 *                 ISA-uplift series (K2, K3, K4, K8 at both tiers)
 *   MC threaded   scaling of multithreaded software: K1, K2 (and K1x) at
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
static const char *const SUSTAINED_KERNELS[] = {"K1", "K2"};
static const char *const THREADED_KERNELS[] = {"K1", "K2"};
static const struct {
  const char *id, *variant;
} UPLIFT_KERNELS[] = {{"K2", NULL}, {"K3", NULL}, {"K4", NULL}, {"K8", "fp32"}, {"K8", "int8"}};

#define COUNT(a) ((int)(sizeof(a) / sizeof *(a)))

static void set_tk_fields(pmk_result *r, const pmk_tk *tk, const pmk_kernels *k, const char *size) {
  r->variant = tk->variant;
  r->k = k;
  r->size = size;
}

/* ---------- ST burst ---------- */

static int burst_series(pmk_ctx *c, const pmk_tk *tk, int type, uint32_t step, uint32_t steps) {
  int cpu = ctx_cpu_for_type(c, type);
  if (ctx_can_place(c) && pal_pin_self(cpu))
    ctx_emit(c, PMK_EV_WARNING, "st_burst", tk->id, 0, 0, "could not pin to CPU %d", cpu);
  void *inst = wl_create(c, tk, PMK_SIZE_BURST, "st_burst");
  if (!inst) return PMK_OK;
  void *scratch = tk->scratch_new ? tk->scratch_new(inst) : NULL;
  if (tk->scratch_new && !scratch) {
    tk->destroy(inst);
    return PMK_ERR_NOMEM;
  }
  ctx_emit(c, PMK_EV_PHASE, "st_burst", tk->id, step, steps, "%s on core %s (CPU %d)", tk->name,
           c->m.type_names[type], cpu);
  pmk_result *r = ctx_new_result(c, tk->id, "st_burst", "ns");
  int rc = r ? PMK_OK : PMK_ERR_NOMEM;
  if (r) {
    set_tk_fields(r, tk, c->k, "burst");
    r->type = type;
    r->cpu = cpu;
    r->input_hash = tk->input_hash(inst);
    for (size_t t = 0; t < tk->ntasks(inst); t++) r->job_work += (double)tk->task_work(inst, t);
    for (int i = 0; i < BURST_WARMUP_JOBS; i++) r->checksum = wl_job(tk, inst, scratch);
    r->checksum_ok = 1;
    while (!ctx_precise_enough(c, r, c->cfg.max_reps)) {
      if (ctx_cancelled()) {
        rc = PMK_ERR_CANCELLED;
        break;
      }
      int obs = pal_current_cpu();
      uint64_t t0 = pal_now_raw_ns();
      uint64_t h = wl_job(tk, inst, scratch);
      uint64_t t1 = pal_now_raw_ns();
      if (h != r->checksum) r->checksum_ok = 0;
      if (result_push(r, (double)(t1 - t0), NAN, obs)) {
        rc = PMK_ERR_NOMEM;
        break;
      }
    }
  }
  if (scratch && tk->scratch_free) tk->scratch_free(scratch);
  tk->destroy(inst);
  return rc;
}

int mode_st_burst(pmk_ctx *c) {
  const pmk_tk *list[COUNT(BURST_KERNELS)];
  int n = 0;
  for (int i = 0; i < COUNT(BURST_KERNELS); i++) {
    if (!ctx_kernel_selected(c, BURST_KERNELS[i])) continue;
    const pmk_tk *tk = pmk_find_tk(c->k, BURST_KERNELS[i], NULL);
    if (tk) list[n++] = tk;
    else ctx_unavailable(c, BURST_KERNELS[i], NULL, "st_burst", "not built");
  }
  int ntypes = c->cfg.cpu >= 0 ? 1 : c->m.ntypes;
  int order[COUNT(BURST_KERNELS) * PMK_MAX_TYPES], nj = 0;
  for (int t = 0; t < ntypes; t++)
    for (int i = 0; i < n; i++) order[nj++] = t * n + i;
  ctx_shuffle(&c->rng, order, nj);
  int rc = PMK_OK;
  for (int j = 0; j < nj && rc == PMK_OK; j++) {
    int type = c->cfg.cpu >= 0 ? ctx_type_of_cpu(c, c->cfg.cpu) : order[j] / n;
    rc = burst_series(c, list[order[j] % n], type, (uint32_t)j + 1, (uint32_t)nj);
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
    sus_spec s = {tk, inst, nthreads, cpus, c->cfg.warmup_s, 1};
    rc = wl_sustained(c, &s, r);
  }
  tk->destroy(inst);
  return rc;
}

/* One sustained run of tk at the given tier on nthreads CPUs: a short settle, then the measurement. */
static int sustained_series(pmk_ctx *c, const char *mode, const pmk_tk *tk, const pmk_kernels *k,
                            const char *purpose, int type, const int *cpus, int nthreads, const void *inst) {
  ctx_cooldown(c, mode);
  pmk_result *r = ctx_new_result(c, tk->id, mode, tk->unit);
  if (!r) return PMK_ERR_NOMEM;
  set_tk_fields(r, tk, k, wl_size_for(c, tk) == PMK_SIZE_BURST ? "burst" : "full");
  r->purpose = purpose;
  r->type = type;
  r->cpu = nthreads == 1 ? cpus[0] : -1;
  sus_spec s = {tk, inst, nthreads, cpus, c->cfg.settle_s, 0};
  return wl_sustained(c, &s, r);
}

static int st_sustained_kernel(pmk_ctx *c, const pmk_tk *tk, const pmk_kernels *k, const char *purpose, int type,
                               uint32_t step, uint32_t steps) {
  int cpu = ctx_cpu_for_type(c, type);
  void *inst = wl_create(c, tk, wl_size_for(c, tk), purpose ? "isa_uplift" : "st_sustained");
  if (!inst) return PMK_OK;
  ctx_emit(c, PMK_EV_PHASE, "st_sustained", tk->id, step, steps, "%s%s, tier %s (%s), core %s (CPU %d)", tk->name,
           purpose ? " for ISA uplift" : "", k->tier, k->level, c->m.type_names[type], cpu);
  int rc = sustained_series(c, "st_sustained", tk, k, purpose, type, &cpu, 1, inst);
  tk->destroy(inst);
  return rc;
}

int mode_st_sustained(pmk_ctx *c) {
  typedef struct job {
    const pmk_tk *tk;
    const pmk_kernels *k;
    const char *purpose;
    int type;
  } job;
  job jobs[64];
  int nj = 0;
  int ntypes = c->cfg.cpu >= 0 ? 1 : c->m.ntypes;
  for (int t = 0; t < ntypes; t++)
    for (int i = 0; i < COUNT(SUSTAINED_KERNELS); i++) {
      if (!ctx_kernel_selected(c, SUSTAINED_KERNELS[i])) continue;
      const pmk_tk *tk = pmk_find_tk(c->k, SUSTAINED_KERNELS[i], NULL);
      int type = c->cfg.cpu >= 0 ? ctx_type_of_cpu(c, c->cfg.cpu) : t;
      if (tk) jobs[nj++] = (job){tk, c->k, NULL, type};
      else if (t == 0) ctx_unavailable(c, SUSTAINED_KERNELS[i], NULL, "st_sustained", "not built");
    }

  /* ISA uplift: the same kernels at both tiers on the fastest core type. */
  if (c->cfg.isa_uplift) {
    int type = c->cfg.cpu >= 0 ? ctx_type_of_cpu(c, c->cfg.cpu) : 0;
    for (int i = 0; i < COUNT(UPLIFT_KERNELS); i++) {
      if (!ctx_kernel_selected(c, UPLIFT_KERNELS[i].id)) continue;
      const pmk_tk *base = pmk_find_tk(c->k, UPLIFT_KERNELS[i].id, UPLIFT_KERNELS[i].variant);
      const pmk_tk *max = c->kmax ? pmk_find_tk(c->kmax, UPLIFT_KERNELS[i].id, UPLIFT_KERNELS[i].variant) : NULL;
      if (!base || !max) {
        ctx_unavailable(c, UPLIFT_KERNELS[i].id, UPLIFT_KERNELS[i].variant, "isa_uplift",
                        c->kmax ? "kernel missing from a tier" : c->kmax_status);
        continue;
      }
      jobs[nj++] = (job){base, c->k, "isa_uplift", type};
      jobs[nj++] = (job){max, c->kmax, "isa_uplift", type};
    }
  }

  int order[64];
  for (int i = 0; i < nj; i++) order[i] = i;
  ctx_shuffle(&c->rng, order, nj);
  int rc = PMK_OK;
  if (nj > 0) {
    int cpu = ctx_cpu_for_type(c, jobs[order[0]].type);
    rc = mode_warm_up(c, "st_sustained", &cpu, 1);
  }
  for (int i = 0; i < nj && rc == PMK_OK; i++) {
    const job *j = &jobs[order[i]];
    rc = st_sustained_kernel(c, j->tk, j->k, j->purpose, j->type, (uint32_t)i + 1, (uint32_t)nj);
  }
  return rc;
}

/* ---------- MC threaded ---------- */

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
    void *inst = wl_create(c, tk, wl_size_for(c, tk), "mc_threaded");
    if (!inst) continue;
    for (int si = 0; si < nsteps && rc == PMK_OK; si++) {
      int n = steps[sorder[si]];
      if (!strcmp(tk->id, "K1") && !ctx_memory_allows(c, tk->id, n, PMK_COMPILE_MEM_PER_THREAD)) continue;
      ctx_emit(c, PMK_EV_PHASE, "mc_threaded", tk->id, (uint32_t)si + 1, (uint32_t)nsteps, "%s, %d thread%s",
               tk->name, n, n == 1 ? "" : "s");
      rc = sustained_series(c, "mc_threaded", tk, c->k, NULL, ctx_type_of_cpu(c, cpus[0]), cpus, n, inst);
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

static volatile uint64_t g_sink;

static void *chaser_main(void *arg) {
  chaser *ch = arg;
  if (ch->cpu >= 0) pal_pin_self(ch->cpu);
  uint64_t p = ch->start;
  while (!atomic_load_explicit(ch->stop, memory_order_relaxed)) p = ch->c->k->k7_chase(ch->nodes, p, 1u << 12);
  g_sink = p;
  return NULL;
}

/*
 * K7 latency under contention: copy 0 measures ns per dependent load while
 * n - 1 copies chase the same (read-only) ring from other random starts on
 * the other CPUs. Each working-set size gets its own ring.
 */
static int k7_series(pmk_ctx *c, const int *cpus, const int *steps, int nsteps) {
  if (!ctx_kernel_selected(c, "K7")) return PMK_OK;
  enum { NSIZES = 12 };
  uint64_t sizes[NSIZES];
  int ns = 0;
  uint64_t top = c->m.llc_bytes ? c->m.llc_bytes * 4 : 256ull << 20;
  if (top < (64ull << 20)) top = 64ull << 20;
  for (uint64_t s = 16ull << 10; s <= top && ns < NSIZES; s *= 4) sizes[ns++] = s;
  if (sizes[ns - 1] < top && ns < NSIZES) sizes[ns++] = top;

  int sorder[NSIZES];
  for (int i = 0; i < ns; i++) sorder[i] = i;
  ctx_shuffle(&c->rng, sorder, ns);
  int rc = PMK_OK;
  for (int si = 0; si < ns && rc == PMK_OK; si++) {
    uint64_t ws = sizes[sorder[si]];
    size_t nn = (size_t)(ws / sizeof(pmk_k7_node));
    pmk_k7_node *nodes = pal_aligned_alloc(64, nn * sizeof *nodes);
    if (!nodes) return PMK_ERR_NOMEM;
    c->k->k7_build(nodes, nn, 0x6b37 + ws); /* fixed seed: identical rings on every host */
    /* Steps per repetition: about 2 M loads, at least 4 passes over the ring. */
    uint64_t chase_steps = nn * 4 > (2u << 20) ? nn * 4 : (2u << 20);

    int norder[MAX_STEPS];
    for (int i = 0; i < nsteps; i++) norder[i] = i;
    ctx_shuffle(&c->rng, norder, nsteps);
    for (int ni = 0; ni < nsteps && rc == PMK_OK; ni++) {
      int n = steps[norder[ni]];
      ctx_emit(c, PMK_EV_PHASE, "mc_instances", "K7", (uint32_t)(si * nsteps + ni + 1), (uint32_t)(ns * nsteps),
               "pointer chase, %llu KiB, %d cop%s", (unsigned long long)(ws >> 10), n, n == 1 ? "y" : "ies");
      atomic_int stop = 0;
      chaser *ch = calloc((size_t)n, sizeof *ch);
      if (!ch) {
        rc = PMK_ERR_NOMEM;
        break;
      }
      int started = 0;
      for (int i = 1; i < n; i++) {
        ch[i] = (chaser){c, nodes, pmk_rng_below(&c->rng, nn), ctx_can_place(c) ? cpus[i] : -1, &stop, NULL};
        ch[i].th = pal_thread_start(chaser_main, &ch[i]);
        if (ch[i].th) started++;
      }
      if (ctx_can_place(c)) pal_pin_self(cpus[0]);
      pmk_result *r = ctx_new_result(c, "K7", "mc_instances", "ns");
      if (!r) rc = PMK_ERR_NOMEM;
      else {
        r->k = c->k;
        r->type = ctx_type_of_cpu(c, cpus[0]);
        r->cpu = cpus[0];
        r->nthreads = started + 1;
        r->ws_bytes = ws;
        r->size = "full";
        uint64_t p = pmk_rng_below(&c->rng, nn);
        p = c->k->k7_chase(nodes, p, nn); /* warm the ring into whatever cache holds it */
        while (!ctx_precise_enough(c, r, c->cfg.max_reps)) {
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
        g_sink = p;
      }
      atomic_store(&stop, 1);
      for (int i = 1; i < n; i++)
        if (ch[i].th) pal_thread_join(ch[i].th);
      free(ch);
    }
    pal_aligned_free(nodes);
  }
  return rc;
}

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
