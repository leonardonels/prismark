/* Internal run context shared by the runner, the modes and the analysis.
 * Copyright 2026 The Prismark Authors. Apache-2.0. */
#ifndef PMK_CTX_H
#define PMK_CTX_H

#include <stddef.h>
#include <stdint.h>

#include "kernels.h"
#include "pal.h"
#include "json.h"
#include "prismark/prismark.h"
#include "rng.h"

typedef struct pmk_dvec {
  double *v;
  size_t n, cap;
} pmk_dvec;

int dvec_push(pmk_dvec *d, double x);
void dvec_free(pmk_dvec *d);

/* One measurement series: every repetition of one kernel under one condition. */
typedef struct pmk_result {
  const char *kernel, *variant, *mode, *unit;
  const pmk_kernels *k; /* tier the kernel ran from */
  int type;    /* core type index of the requested CPU (first CPU for multi-core series) */
  int cpu;     /* requested CPU; -1 for multi-core series */
  /* cold burst */
  int warm;
  double w_ms;
  uint64_t w_cycles;
  uint64_t iters;      /* K9 iterations: W_cycles / c */
  /* periodic */
  const char *condition;
  double period_ms;
  int loaders;
  uint64_t overruns;
  /* throughput kernels */
  const char *size;    /* "burst" or "full" */
  const char *purpose; /* "isa_uplift" for the ISA-uplift series, else NULL */
  int nthreads;        /* threads (threaded) or copies (K7 instances); 0 for single-core modes */
  int windowed;        /* samples are throughput per window, not times */
  double window_ms;
  pmk_dvec temps;      /* CPU temperature at the end of each window */
  pmk_dvec mhz;        /* average clock of the series' CPUs at the end of each window, 0 if unknown */
  pmk_dvec power;      /* package power over each window in W, NaN if unknown */
  int clamped;         /* measured windows below the platform-clamp threshold (see wl_sustained) */
  double lowest_mhz;   /* lowest window clock among them, NaN if none */
  pmk_dvec job_ns;     /* duration of each complete job */
  double job_work;     /* work units per job (burst series), for throughput = work / time */
  double work_fraction; /* K1x: share of the whole snapshot's compile work its build covers (0 = unknown) */
  uint64_t input_hash, checksum;
  int checksum_ok;     /* every job of the series produced the same checksum */
  int steady;          /* steady state reached (9.3) */
  size_t steady_from;  /* first window of the steady-state set */
  double tau_s;        /* thermal time constant fitted during the run, NaN if unknown */
  double elapsed_s;
  /* K7 latency */
  uint64_t ws_bytes;
  /* data, in repetition order */
  double *samples;
  double *wake;        /* cold burst: deadline -> thread running, ns; else NaN */
  int32_t *observed;   /* CPU read at each repetition or window */
  size_t n, cap;
} pmk_result;

typedef struct pmk_calib {
  int valid;
  int cpu;
  const char *method;       /* "perf" (measured) or "assumed" (1 cycle per add) */
  double cycles_per_iter;   /* c */
  double cycles_spread_rel; /* (max - min) / median over the calibration runs; NaN if assumed */
  double ns_per_iter;
  double implied_mhz;
} pmk_calib;

typedef struct pmk_analysis pmk_analysis;
typedef struct pmk_analysis_tp pmk_analysis_tp;

/* Internal return code: a quiet mode saw keyboard or mouse input and was skipped. */
#define PMK_SKIP_INPUT (-100)

typedef struct pmk_ctx {
  pmk_config cfg;
  pmk_progress_fn cb;
  void *user;
  pmk_rng rng;      /* drives the experiment (gaps, orders) */
  pmk_rng stat_rng; /* drives bootstrap resampling, so it never perturbs the experiment */
  uint64_t seed;
  pmk_machine m;
  const pmk_kernels *k;    /* baseline tier */
  const pmk_kernels *kmax; /* loaded max-level tier, or NULL */
  char kmax_status[256];   /* why kmax is NULL, or which level was selected */
  pmk_result *res;
  size_t nres, capres;
  pmk_calib calib[PMK_MAX_TYPES];
  pmk_jw avail; /* JSON array of {kernel, mode, reason} for everything that could not run */
  pmk_jw k1x;   /* JSON object describing the K1x build environment */
  double idle_temp;
  int quiet;             /* a mode that needs an idle machine is running */
  uint64_t quiet_since;  /* pal_now_ns() when it started */
  pmk_jw skipped;        /* JSON array of modes skipped because of input */
  pmk_analysis *an;
  pmk_analysis_tp *antp;
  int oom;
  uint32_t run_step, run_steps; /* position in the whole run: steps begun, steps planned (0 if not planned) */
} pmk_ctx;

pmk_result *ctx_new_result(pmk_ctx *c, const char *kernel, const char *mode, const char *unit);
int result_push(pmk_result *r, double sample, double wake, int cpu);
void result_free(pmk_result *r);

int ctx_cancelled(void);
/* PMK_ERR_CANCELLED, PMK_SKIP_INPUT (input during a quiet mode) or PMK_OK. Call between repetitions. */
int ctx_interrupt(pmk_ctx *c);
void ctx_emit(pmk_ctx *c, pmk_event_kind kind, const char *phase, const char *kernel, uint32_t step,
              uint32_t steps, const char *fmt, ...) __attribute__((format(printf, 7, 8)));
/* Records that a kernel could not run in a mode, and why. */
void ctx_unavailable(pmk_ctx *c, const char *kernel, const char *variant, const char *mode, const char *reason);
/* True when this platform offers the kernel and the kernel filter (cfg.kernels) selects it. */
int ctx_kernel_selected(const pmk_ctx *c, const char *id);
/* Waits for the CPU to cool to the idle temperature (if cfg.cooldown and a sensor exists). */
void ctx_cooldown(pmk_ctx *c, const char *phase);

/* Threads can be placed: pinned to a CPU, or (QoS-only platforms) steered to its core type. */
static inline int ctx_can_place(const pmk_ctx *c) { return c->m.caps.pinning || c->m.caps.qos_only; }
/* Picks the CPU for single-core work of the given type. */
int ctx_cpu_for_type(const pmk_ctx *c, int type);
int ctx_type_of_cpu(const pmk_ctx *c, int cpu);
/* CPUs in the order multi-core modes add threads: fastest type first, one thread per physical
 * core before SMT siblings. Returns the count; out holds c->m.ncpu entries. */
int ctx_cpu_order(const pmk_ctx *c, int *out);
/* Thread counts 1, 2, 4, ... up to the CPU count (capped by cfg.max_threads), which is always included. */
int ctx_thread_steps(const pmk_ctx *c, int *out, int max);
/* Clang at -O2 on the snapshot's largest unit peaks at 375 MB (median 164 MB; measured per unit, Clang 19); K1
   measured 436 MB on one thread and 719 MB on two. K1 and K1x plan 450 MB per thread: 3.6 GB at 8 threads. */
#define PMK_COMPILE_MEM_PER_THREAD (450ull << 20)
/* False (and the thread count recorded as unavailable) when n threads of per_thread bytes do not fit now. */
int ctx_memory_allows(pmk_ctx *c, const char *kernel, int n, uint64_t per_thread);
/* True once the median CI half-width is within 1% or max_reps is reached. */
int ctx_precise_enough(pmk_ctx *c, const pmk_result *r, int max_reps);
void ctx_shuffle(pmk_rng *r, int *v, int n);

/* ---------- throughput-kernel helpers (workload.c) ---------- */

/* Creates a kernel instance; on failure records it as unavailable for the mode and returns NULL. */
void *wl_create(pmk_ctx *c, const pmk_tk *tk, int size, const char *mode);
/* Runs one job on the calling thread; returns its checksum. */
uint64_t wl_job(const pmk_tk *tk, const void *inst, void *scratch);
/* Input size for sustained runs: full, except the compile kernels in quick runs (cfg.quick_inputs). */
int wl_size_for(const pmk_ctx *c, const pmk_tk *tk);
/* Window length for a kernel's sustained runs: long tasks (K1) need longer windows. */
double wl_window_ms(const pmk_ctx *c, const pmk_tk *tk);

typedef struct sus_spec {
  const pmk_tk *tk;
  const void *inst;   /* the kernel instance whose job the threads share */
  int nthreads;       /* threads sharing each job */
  const int *cpus;    /* nthreads CPUs to pin to, or NULL */
  double warm_s;      /* untimed seconds before the measurement: the mode's warm-up, or a short settle */
  int warm_only;      /* run warm_s only, measure nothing (a mode's warm-up at full load) */
} sus_spec;

/*
 * Sustained run: jobs back to back for warm_s seconds (recorded, not scored), then cfg.measure_s (three times
 * that for K1). Fills r with the per-window throughput, temperatures and job times; steady_from marks the first
 * measured window.
 */
int wl_sustained(pmk_ctx *c, const sus_spec *s, pmk_result *r);

/* ---------- modes ---------- */

int mode_cold_burst(pmk_ctx *c);
int mode_periodic(pmk_ctx *c);
int mode_st_burst(pmk_ctx *c);
int mode_st_sustained(pmk_ctx *c);
int mode_mc_threaded(pmk_ctx *c);
int mode_mc_instances(pmk_ctx *c);
/* Steps each mode will report (its PHASE events with a step number), counted before anything runs, for the
   run-wide position. Steps that turn out impossible (not enough memory, a failed build) are not run. */
int mode_cold_burst_steps(const pmk_ctx *c);
int mode_periodic_steps(const pmk_ctx *c);
int mode_st_burst_steps(const pmk_ctx *c);
int mode_st_sustained_steps(const pmk_ctx *c);
int mode_mc_threaded_steps(const pmk_ctx *c);
int mode_mc_instances_steps(const pmk_ctx *c);
/* K1x: full build of the prepared snapshot at n jobs; part of MC threaded. */
int k1x_run(pmk_ctx *c, const int *steps, int nsteps, const int *order);

/* Statistics over the collected results, computed once and rendered as JSON and text. */
void analysis_compute(pmk_ctx *c);
void analysis_json(pmk_ctx *c, pmk_jw *w, const char *key);
char *analysis_text(pmk_ctx *c, const char *run_id, const char *note);
void analysis_free(pmk_ctx *c);

/* Throughput modes: per-kernel statistics and derived ratios (analysis_tp.c). */
void analysis_tp_compute(pmk_ctx *c);
void analysis_tp_json(pmk_ctx *c, pmk_jw *w);
void analysis_tp_text(pmk_ctx *c, pmk_buf *b);
void analysis_tp_free(pmk_ctx *c);

#endif
