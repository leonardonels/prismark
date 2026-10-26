/*
 * Statistics for the throughput modes: per-series medians with bootstrap
 * CIs, steady-state throughput and R_throttle, scaling S(n), E(n), p(n), and
 * the same-kernel ratios R_build and R_resp of the realistic
 * cold bursts. Each ratio pairs series of one kernel only. And the stress
 * test: the score (the 10th minute), the first and last minute, and how the
 * speed held up between them.
 *
 * Copyright 2026 The Prismark Authors. Apache-2.0.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "buf.h"
#include "ctx.h"
#include "stats.h"

#define INITIAL_WINDOWS 3 /* windows after the warm-up window that form perf_initial */

typedef struct tp_row {
  const pmk_result *r;
  pmk_ci med;      /* median of the samples (ns for times, unit for windows) */
  pmk_ci perf;     /* throughput in the kernel's unit: steady windows, or work / median time */
  pmk_ci initial;  /* windowed: perf over the first windows after warm-up */
  pmk_ci throttle; /* windowed: R_throttle = perf_steady / perf_initial */
  pmk_ci job;      /* windowed: median job time, ns */
} tp_row;

typedef struct scale_row {
  const char *kernel;
  int n;
  pmk_ci S, E, p;
} scale_row;

typedef struct ratio_row {
  const char *name; /* "R_build", "R_resp" */
  const char *kernel, *variant;
  int n;            /* threads, or 0 */
  const char *core_type;
  pmk_ci r;
} ratio_row;

typedef struct sum_row { /* checksum agreement per kernel, variant, input size and tier */
  const char *kernel, *variant, *size, *tier;
  uint64_t input_hash, checksum;
  int consistent, series;
} sum_row;

typedef struct stress_row {
  const pmk_result *r;
  pmk_ci score;       /* median window speed over minute 9-10, NaN when the part ran less than 10 minutes */
  pmk_ci first, end;  /* the first minute (after its first window) and the last minute */
  pmk_ci hold_up;     /* end / first */
  double temp_max, mhz, watts;
} stress_row;

struct pmk_analysis_tp {
  stress_row *st;
  size_t nst;
  tp_row *rows;
  size_t nrows;
  scale_row *sc;
  size_t nsc;
  ratio_row *ra;
  size_t nra;
  sum_row *sums;
  size_t nsums;
};

static int is_tp_mode(const pmk_result *r) {
  return !strcmp(r->mode, "st_burst") || !strcmp(r->mode, "st_sustained") || !strcmp(r->mode, "mc_threaded") ||
         !strcmp(r->mode, "mc_instances") || (!strcmp(r->mode, "cold_burst") && strcmp(r->kernel, "K9"));
}

static int same_str(const char *a, const char *b) { return a == b || (a && b && !strcmp(a, b)); }

static const double *steady_set(const pmk_result *r, size_t *n) {
  *n = r->n - r->steady_from;
  return r->samples + r->steady_from;
}

/* Median of a per-window record over the scored windows, leaving out unknown values (NaN, or 0 for the clock). */
static double steady_median(const pmk_result *r, const pmk_dvec *d) {
  size_t end = d->n < r->n ? d->n : r->n, m = 0;
  double *x = end > r->steady_from ? malloc((end - r->steady_from) * sizeof *x) : NULL;
  for (size_t i = r->steady_from; x && i < end; i++)
    if (isfinite(d->v[i]) && d->v[i] > 0) x[m++] = d->v[i];
  double v = m ? pmk_median(x, m) : NAN;
  free(x);
  return v;
}

static const double *initial_set(const pmk_result *r, size_t *n) {
  size_t from = r->n > 1 + INITIAL_WINDOWS ? 1 : 0;
  *n = r->n - from < INITIAL_WINDOWS ? r->n - from : INITIAL_WINDOWS;
  return r->samples + from;
}

/* Windows [from, to) of r that end within seconds (lo, hi] of the run. */
static size_t windows_in(const pmk_result *r, double lo, double hi, size_t *from) {
  double w = r->window_ms / 1e3;
  size_t a = (size_t)ceil(lo / w - 1e-9), b = (size_t)floor(hi / w + 1e-9);
  if (b > r->n) b = r->n;
  *from = a;
  return b > a ? b - a : 0;
}

/* "failed" (a wrong result), "too_hot" (ended at the temperature limit) or "stable". */
static const char *stress_verdict(const pmk_result *r) {
  if (r->errors || r->wrong_tasks) return "failed";
  if (r->stop_reason && !strcmp(r->stop_reason, "temperature")) return "too_hot";
  return "stable";
}

static void compute_stress(pmk_ctx *c, stress_row *row) {
  const pmk_result *r = row->r;
  pmk_rng *rng = &c->stat_rng;
  double w = r->window_ms / 1e3, ran = (double)r->n * w;
  size_t from, n;
  pmk_ci none = {NAN, NAN, NAN};
  row->score = none;
  if (ran >= PMK_STRESS_SCORE_TO_S - 1e-6 && (n = windows_in(r, PMK_STRESS_SCORE_FROM_S, PMK_STRESS_SCORE_TO_S, &from)))
    row->score = pmk_boot_median(r->samples + from, n, PMK_BOOT_B, rng);
  /* The first window holds the start-up (threads starting, the first job), so the first minute begins after it. */
  n = windows_in(r, w, 60, &from);
  if (!n) n = r->n, from = 0;
  row->first = pmk_boot_median(r->samples + from, n, PMK_BOOT_B, rng);
  size_t last = r->n < (size_t)(60 / w + 0.5) ? r->n : (size_t)(60 / w + 0.5);
  row->end = pmk_boot_median(r->samples + r->n - last, last, PMK_BOOT_B, rng);
  row->hold_up = pmk_boot_median_ratio(r->samples + r->n - last, last, r->samples + from, n, PMK_BOOT_B, rng);
  row->temp_max = NAN;
  for (size_t i = 0; i < r->temps.n; i++)
    if (isfinite(r->temps.v[i]) && !(row->temp_max >= r->temps.v[i])) row->temp_max = r->temps.v[i];
  row->mhz = steady_median(r, &r->mhz);
  row->watts = steady_median(r, &r->power);
}

static pmk_ci invert_time(pmk_ci t, double work, double unit_div) {
  /* throughput = work / time: decreasing in time, so the bounds swap */
  double k = work / unit_div * 1e9;
  return (pmk_ci){k / t.est, k / t.hi, k / t.lo};
}

static void compute_row(pmk_ctx *c, tp_row *row) {
  const pmk_result *r = row->r;
  pmk_rng *rng = &c->stat_rng;
  row->med = pmk_boot_median(r->samples, r->n, PMK_BOOT_B, rng);
  row->perf = row->initial = row->throttle = row->job = (pmk_ci){NAN, NAN, NAN};
  if (r->windowed) {
    size_t ns, ni;
    const double *s = steady_set(r, &ns), *i = initial_set(r, &ni);
    row->perf = pmk_boot_median(s, ns, PMK_BOOT_B, rng);
    row->initial = pmk_boot_median(i, ni, PMK_BOOT_B, rng);
    row->throttle = pmk_boot_median_ratio(s, ns, i, ni, PMK_BOOT_B, rng);
    row->job = pmk_boot_median(r->job_ns.v, r->job_ns.n, PMK_BOOT_B, rng);
  } else if (r->job_work > 0) {
    const pmk_tk *tk = pmk_find_tk(r->k, r->kernel, r->variant);
    if (tk) row->perf = invert_time(row->med, r->job_work, tk->unit_div);
  }
}

static const pmk_result *find(const pmk_ctx *c, const char *kernel, const char *mode, const char *tier, int n,
                              const char *purpose, const char *variant) {
  for (size_t i = 0; i < c->nres; i++) {
    const pmk_result *r = &c->res[i];
    if (r->n == 0 || strcmp(r->kernel, kernel) || strcmp(r->mode, mode)) continue;
    if (tier && strcmp(r->k->tier, tier)) continue;
    if (n >= 0 && r->nthreads != n) continue;
    if (!same_str(r->purpose, purpose) || !same_str(r->variant, variant)) continue;
    return r;
  }
  return NULL;
}

static void add_ratio(pmk_analysis_tp *a, const char *name, const pmk_result *r, int n, const char *core, pmk_ci v) {
  ratio_row *x = &a->ra[a->nra++];
  x->name = name;
  x->kernel = r->kernel;
  x->variant = r->variant;
  x->n = n;
  x->core_type = core;
  x->r = v;
}

static void add_checksum(pmk_analysis_tp *a, const pmk_result *r) {
  /* A fixed-length series of a slow kernel (K1: one job is a whole build) may finish no job, so it has no job
     checksum; `prismark checksums` verifies those kernels' output instead. */
  if (!r->input_hash || (r->windowed && !r->job_ns.n)) return;
  for (size_t i = 0; i < a->nsums; i++) {
    sum_row *s = &a->sums[i];
    if (strcmp(s->kernel, r->kernel) || !same_str(s->variant, r->variant) || !same_str(s->size, r->size) ||
        strcmp(s->tier, r->k->tier))
      continue;
    s->series++;
    if (!r->checksum_ok || r->checksum != s->checksum || r->input_hash != s->input_hash) s->consistent = 0;
    return;
  }
  sum_row *s = &a->sums[a->nsums++];
  *s = (sum_row){r->kernel, r->variant, r->size, r->k->tier, r->input_hash, r->checksum, r->checksum_ok, 1};
}

/* Presentation order: by mode, kernel, tier, working set and thread count (runs are in random order). */
static int mode_rank(const char *m) {
  static const char *const order[] = {"st_burst", "st_sustained", "mc_threaded", "mc_instances", "cold_burst"};
  for (int i = 0; i < 5; i++)
    if (!strcmp(m, order[i])) return i;
  return 5;
}

static int cmp_str(const char *a, const char *b) { return strcmp(a ? a : "", b ? b : ""); }

static int cmp_rows(const void *pa, const void *pb) {
  const pmk_result *a = ((const tp_row *)pa)->r, *b = ((const tp_row *)pb)->r;
  int d;
  if ((d = mode_rank(a->mode) - mode_rank(b->mode))) return d;
  if ((d = (a->purpose != NULL) - (b->purpose != NULL))) return d;
  if ((d = (!strcmp(a->kernel, "K7")) - (!strcmp(b->kernel, "K7")))) return d;
  if ((d = (!strcmp(a->kernel, "K1x")) - (!strcmp(b->kernel, "K1x")))) return d;
  if ((d = cmp_str(a->kernel, b->kernel))) return d;
  if ((d = cmp_str(a->variant, b->variant))) return d;
  if ((d = cmp_str(a->k->tier, b->k->tier))) return d; /* "baseline" < "max" */
  if (a->ws_bytes != b->ws_bytes) return a->ws_bytes < b->ws_bytes ? -1 : 1;
  if ((d = a->nthreads - b->nthreads)) return d;
  if ((d = a->type - b->type)) return d;
  return b->warm - a->warm;
}

static int cmp_scale(const void *pa, const void *pb) {
  const scale_row *a = pa, *b = pb;
  int d = strcmp(a->kernel, b->kernel);
  return d ? d : a->n - b->n;
}

static int cmp_ratio(const void *pa, const void *pb) {
  const ratio_row *a = pa, *b = pb;
  int d;
  if ((d = strcmp(a->name, b->name))) return d;
  if ((d = strcmp(a->kernel, b->kernel))) return d;
  if ((d = cmp_str(a->variant, b->variant))) return d;
  if ((d = a->n - b->n)) return d;
  return cmp_str(a->core_type, b->core_type);
}

void analysis_tp_compute(pmk_ctx *c) {
  pmk_analysis_tp *a = calloc(1, sizeof *a);
  if (!a) return;
  c->antp = a;
  size_t cap = c->nres ? c->nres : 1;
  a->rows = calloc(cap, sizeof *a->rows);
  a->sc = calloc(cap, sizeof *a->sc);
  a->ra = calloc(cap * 2, sizeof *a->ra);
  a->sums = calloc(cap, sizeof *a->sums);
  a->st = calloc(cap, sizeof *a->st);
  if (!a->rows || !a->sc || !a->ra || !a->sums || !a->st) return;
  pmk_rng *rng = &c->stat_rng;

  for (size_t i = 0; i < c->nres; i++)
    if (!strcmp(c->res[i].mode, "stress") && c->res[i].n) {
      a->st[a->nst].r = &c->res[i];
      compute_stress(c, &a->st[a->nst++]);
    }

  for (size_t i = 0; i < c->nres; i++) {
    const pmk_result *r = &c->res[i];
    if (!is_tp_mode(r) && strcmp(r->kernel, "K1x")) continue;
    add_checksum(a, r);
    if (r->n == 0) continue;
    tp_row *row = &a->rows[a->nrows++];
    row->r = r;
    compute_row(c, row);
  }

  /* Scaling against n = 1 of the same kernel (MC threaded). */
  for (size_t i = 0; i < c->nres; i++) {
    const pmk_result *r = &c->res[i];
    if (strcmp(r->mode, "mc_threaded") || r->n == 0 || r->purpose) continue; /* not the warm-up */
    const pmk_result *one = find(c, r->kernel, "mc_threaded", NULL, 1, NULL, NULL);
    if (!one) continue;
    scale_row *s = &a->sc[a->nsc++];
    s->kernel = r->kernel;
    s->n = r->nthreads;
    if (r->windowed) {
      size_t n1, nn;
      const double *x1 = steady_set(one, &n1), *xn = steady_set(r, &nn);
      s->S = pmk_boot_median_ratio(xn, nn, x1, n1, PMK_BOOT_B, rng); /* perf_n / perf_1 = T_1 / T_n */
    } else {
      s->S = pmk_boot_median_ratio(one->samples, one->n, r->samples, r->n, PMK_BOOT_B, rng);
    }
    double n = (double)s->n;
    s->E = (pmk_ci){s->S.est / n, s->S.lo / n, s->S.hi / n};
    if (s->n > 1) {
      double k = 1.0 / (1.0 - 1.0 / n); /* p is increasing in S */
      s->p = (pmk_ci){(1 - 1 / s->S.est) * k, (1 - 1 / s->S.lo) * k, (1 - 1 / s->S.hi) * k};
    } else {
      s->p = (pmk_ci){NAN, NAN, NAN};
    }
  }

  for (size_t i = 0; i < c->nres; i++) {
    const pmk_result *r = &c->res[i];
    if (r->n == 0) continue;
    const char *core = c->m.type_names[r->type];
    /* R_build(n) = T_K1x(n) / T_K1(n) for the same share of the compile work: K1's measured windows are in
       whole builds per hour, so each gives T_K1 = work_fraction * 1 h / perf for the units K1x builds. */
    if (!strcmp(r->mode, "mc_threaded") && !strcmp(r->kernel, "K1x") && r->work_fraction > 0) {
      const pmk_result *k1 = find(c, "K1", "mc_threaded", NULL, r->nthreads, NULL, NULL);
      if (k1 && r->size && k1->size && !strcmp(r->size, k1->size)) { /* same run kind (quick or full) */
        size_t nk;
        const double *w = steady_set(k1, &nk);
        double *t = malloc((nk ? nk : 1) * sizeof *t);
        size_t m = 0;
        for (size_t j = 0; t && j < nk; j++)
          if (w[j] > 0) t[m++] = r->work_fraction * 3600e9 / w[j];
        if (m) add_ratio(a, "R_build", r, r->nthreads, core, pmk_boot_median_ratio(r->samples, r->n, t, m, PMK_BOOT_B, rng));
        free(t);
      }
    }
    /* R_resp of the realistic cold bursts: t_warm / t_cold. */
    if (!strcmp(r->mode, "cold_burst") && strcmp(r->kernel, "K9") && !r->warm) {
      for (size_t j = 0; j < c->nres; j++) {
        const pmk_result *w = &c->res[j];
        if (w->n && w->warm && w->type == r->type && !strcmp(w->mode, "cold_burst") && !strcmp(w->kernel, r->kernel))
          add_ratio(a, "R_resp", r, 0, core, pmk_boot_median_ratio(w->samples, w->n, r->samples, r->n, PMK_BOOT_B, rng));
      }
    }
  }
  qsort(a->rows, a->nrows, sizeof *a->rows, cmp_rows);
  qsort(a->sc, a->nsc, sizeof *a->sc, cmp_scale);
  qsort(a->ra, a->nra, sizeof *a->ra, cmp_ratio);
}

void analysis_tp_free(pmk_ctx *c) {
  if (!c->antp) return;
  free(c->antp->rows);
  free(c->antp->sc);
  free(c->antp->ra);
  free(c->antp->sums);
  free(c->antp->st);
  free(c->antp);
  c->antp = NULL;
}

/* ---------- JSON ---------- */

static void ci_json(pmk_jw *w, const char *key, pmk_ci ci) {
  jw_obj_begin(w, key);
  jw_num(w, "est", ci.est);
  jw_num(w, "lo", ci.lo);
  jw_num(w, "hi", ci.hi);
  jw_obj_end(w);
}

static const char *unit_of(const pmk_result *r) {
  const pmk_tk *tk = pmk_find_tk(r->k, r->kernel, r->variant);
  return tk ? tk->unit : r->unit;
}

void analysis_tp_json(pmk_ctx *c, pmk_jw *w) {
  const pmk_analysis_tp *a = c->antp;
  char hx[19];
  jw_arr_begin(w, "series");
  for (size_t i = 0; a && i < a->nrows; i++) {
    const tp_row *row = &a->rows[i];
    const pmk_result *r = row->r;
    jw_obj_begin(w, NULL);
    jw_str(w, "kernel", r->kernel);
    jw_str(w, "variant", r->variant);
    jw_str(w, "mode", r->mode);
    jw_str(w, "tier", r->k->tier);
    jw_str(w, "purpose", r->purpose);
    jw_str(w, "core_type", c->m.type_names[r->type]);
    jw_int(w, "cpu", r->cpu);
    jw_int(w, "n", r->nthreads ? r->nthreads : 1);
    if (r->isa) jw_str(w, "vector_isa", r->isa);
    if (r->ws_bytes) jw_int(w, "working_set_bytes", (int64_t)r->ws_bytes);
    if (!strcmp(r->mode, "cold_burst")) jw_str(w, "start", r->warm ? "warm" : "cold");
    jw_int(w, "samples", (int64_t)r->n);
    if (r->windowed) {
      jw_str(w, "unit", unit_of(r));
      ci_json(w, "perf_steady", row->perf);
      ci_json(w, "perf_initial", row->initial);
      ci_json(w, "R_throttle", row->throttle);
      ci_json(w, "job_ns", row->job);
      jw_bool(w, "steady_reached", r->steady);
      jw_num(w, "tau_s", r->tau_s);
      jw_num(w, "elapsed_s", r->elapsed_s);
      jw_num(w, "perf_ci_rel", (row->perf.hi - row->perf.lo) / 2 / row->perf.est);
      jw_int(w, "clamped_windows", r->clamped);
      jw_num(w, "clamped_lowest_mhz", r->lowest_mhz);
      jw_num(w, "mhz", steady_median(r, &r->mhz));
      if (r->nclock_dom) {
        jw_obj_begin(w, "mhz_by_domain");
        for (int i = 0; i < r->nclock_dom; i++) jw_num(w, r->clock_dom[i], steady_median(r, &r->mhz_dom[i]));
        jw_obj_end(w);
        jw_arr_begin(w, "idle_domains");
        for (int i = 0; i < r->nclock_dom; i++)
          if (r->clock_idle[i]) jw_str(w, NULL, r->clock_dom[i]);
        jw_arr_end(w);
      }
      jw_num(w, "power_w", steady_median(r, &r->power));
    } else {
      jw_str(w, "unit", r->unit);
      ci_json(w, "median", row->med);
      jw_num(w, "median_ci_rel", (row->med.hi - row->med.lo) / 2 / row->med.est);
      if (isfinite(row->perf.est)) {
        jw_str(w, "perf_unit", unit_of(r));
        ci_json(w, "perf", row->perf);
      }
    }
    jw_obj_end(w);
  }
  jw_arr_end(w);

  jw_arr_begin(w, "scaling");
  for (size_t i = 0; a && i < a->nsc; i++) {
    const scale_row *s = &a->sc[i];
    jw_obj_begin(w, NULL);
    jw_str(w, "kernel", s->kernel);
    jw_int(w, "n", s->n);
    ci_json(w, "S", s->S);
    ci_json(w, "E", s->E);
    ci_json(w, "p", s->p);
    jw_obj_end(w);
  }
  jw_arr_end(w);

  jw_arr_begin(w, "ratios");
  for (size_t i = 0; a && i < a->nra; i++) {
    const ratio_row *x = &a->ra[i];
    jw_obj_begin(w, NULL);
    jw_str(w, "name", x->name);
    jw_str(w, "kernel", x->kernel);
    jw_str(w, "variant", x->variant);
    if (x->n) jw_int(w, "n", x->n);
    jw_str(w, "core_type", x->core_type);
    ci_json(w, "value", x->r);
    jw_obj_end(w);
  }
  jw_arr_end(w);

  jw_arr_begin(w, "stress");
  for (size_t i = 0; a && i < a->nst; i++) {
    const stress_row *x = &a->st[i];
    const pmk_result *r = x->r;
    jw_obj_begin(w, NULL);
    jw_str(w, "part", r->part);
    jw_str(w, "kernel", r->kernel);
    jw_str(w, "vector_isa", r->isa);
    jw_str(w, "unit", unit_of(r));
    jw_str(w, "core_type", c->m.type_names[r->type]);
    jw_int(w, "n", r->nthreads ? r->nthreads : 1);
    jw_num(w, "duration_s", (double)r->n * r->window_ms / 1e3);
    jw_bool(w, "stopped", r->stopped);
    if (isfinite(x->score.est)) ci_json(w, "score", x->score);
    else jw_raw(w, "score", "null");
    ci_json(w, "first_minute", x->first);
    ci_json(w, "last_minute", x->end);
    ci_json(w, "hold_up", x->hold_up);
    jw_num(w, "temp_max_c", x->temp_max);
    jw_num(w, "mhz", x->mhz);
    jw_num(w, "power_w", x->watts);
    jw_int(w, "clamped_windows", r->clamped);
    jw_int(w, "jobs", (int64_t)r->job_ns.n);
    jw_int(w, "checksum_errors", (int64_t)r->errors);
    jw_str(w, "verdict", stress_verdict(r));
    jw_str(w, "stop_reason", r->stop_reason);
    jw_int(w, "wrong_tasks", (int64_t)r->wrong_tasks);
    jw_num(w, "first_error_s", r->first_wrong_s);
    jw_int(w, "first_error_cpu", r->first_wrong_cpu);
    jw_obj_begin(w, "wrong_by_cpu");
    for (int k = 0; k < r->nwrong_cpu; k++) {
      char key[16];
      snprintf(key, sizeof key, "%d", r->wrong_cpu[k]);
      jw_int(w, key, (int64_t)r->wrong_cpu_n[k]);
    }
    jw_obj_end(w);
    jw_obj_end(w);
  }
  jw_arr_end(w);

  jw_arr_begin(w, "checksums");
  for (size_t i = 0; a && i < a->nsums; i++) {
    const sum_row *s = &a->sums[i];
    jw_obj_begin(w, NULL);
    jw_str(w, "kernel", s->kernel);
    jw_str(w, "variant", s->variant);
    jw_str(w, "size", s->size);
    jw_str(w, "tier", s->tier);
    snprintf(hx, sizeof hx, "%016llx", (unsigned long long)s->input_hash);
    jw_str(w, "input_hash", hx);
    snprintf(hx, sizeof hx, "%016llx", (unsigned long long)s->checksum);
    jw_str(w, "checksum", hx);
    jw_bool(w, "consistent", s->consistent);
    jw_int(w, "series", s->series);
    jw_obj_end(w);
  }
  jw_arr_end(w);
}

/* ---------- text ---------- */

static const char *fmt_time(char *out, size_t n, double ns) {
  if (!isfinite(ns)) snprintf(out, n, "n/a");
  else if (ns >= 1e9) snprintf(out, n, "%.3g s", ns / 1e9);
  else if (ns >= 1e6) snprintf(out, n, "%.3g ms", ns / 1e6);
  else if (ns >= 1e3) snprintf(out, n, "%.3g us", ns / 1e3);
  else snprintf(out, n, "%.3g ns", ns);
  return out;
}

/* The app's name for the kernel and its variant, e.g. "3D rendering". */
static const char *label(char *out, size_t n, const pmk_result *r) {
  snprintf(out, n, "%s%s%s", pmk_display_name(r->kernel), r->variant ? " " : "", r->variant ? r->variant : "");
  return out;
}

void analysis_tp_text(pmk_ctx *c, pmk_buf *b) {
  const pmk_analysis_tp *a = c->antp;
  if (!a) return;
  char x[32], y[32], z[32], l[64];
  const char *mode = NULL;
  int table = -1;
  for (size_t i = 0; i < a->nrows; i++) {
    const tp_row *row = &a->rows[i];
    const pmk_result *r = row->r;
    if (!strcmp(r->mode, "cold_burst")) continue; /* shown with the ratios */
    int t = !strcmp(r->kernel, "K7") ? 1 : !strcmp(r->kernel, "K1x") ? 2 : 0;
    if (!mode || strcmp(mode, r->mode) || t != table) {
      mode = r->mode;
      table = t;
      if (!strcmp(mode, "st_burst"))
        buf_printf(b, "\nOne core, short task: time per job, median [95%% CI]\n  %-24s %-4s %-24s %14s\n", "test", "core",
                   "time", "throughput");
      else if (!strcmp(r->kernel, "K7"))
        buf_printf(b, "\nAll cores, each on its own: Memory delay, ns per load under contention\n  %10s %6s %24s\n", "set",
                   "copies", "median [95% CI]");
      else if (!strcmp(r->kernel, "K1x"))
        buf_printf(b, "\nAll cores, working together: Full software build\n  %6s %24s\n", "n", "time [95% CI]");
      else
        buf_printf(b, "\n%s: steady-state throughput, R_throttle = steady / initial\n  %-24s %-8s %4s %-28s %-22s %s\n",
                   !strcmp(mode, "st_sustained")  ? "One core, after warming up"
                   : !strcmp(mode, "mc_threaded") ? "All cores, working together"
                                                  : "All cores, each on its own",
                   "test", "tier", "n", "perf [95% CI]", "R_throttle", "steady");
    }
    if (!strcmp(r->mode, "st_burst")) {
      buf_printf(b, "  %-24s %-4s %-8s [%s, %s]  %8.4g %s\n", label(l, sizeof l, r), c->m.type_names[r->type],
                 fmt_time(x, sizeof x, row->med.est), fmt_time(y, sizeof y, row->med.lo),
                 fmt_time(z, sizeof z, row->med.hi), row->perf.est, unit_of(r));
    } else if (!strcmp(r->kernel, "K7")) {
      buf_printf(b, "  %7llu KiB %6d %8.3g [%.3g, %.3g]\n", (unsigned long long)(r->ws_bytes >> 10), r->nthreads,
                 row->med.est, row->med.lo, row->med.hi);
    } else if (!strcmp(r->kernel, "K1x")) {
      buf_printf(b, "  %6d %8s [%s, %s]\n", r->nthreads, fmt_time(x, sizeof x, row->med.est),
                 fmt_time(y, sizeof y, row->med.lo), fmt_time(z, sizeof z, row->med.hi));
    } else {
      char perf[64], thr[48];
      snprintf(perf, sizeof perf, "%.4g [%.4g, %.4g] %s", row->perf.est, row->perf.lo, row->perf.hi, unit_of(r));
      snprintf(thr, sizeof thr, "%.3f [%.3f, %.3f]", row->throttle.est, row->throttle.lo, row->throttle.hi);
      buf_printf(b, "  %-24s %-8s %4d %-28s %-22s %s", label(l, sizeof l, r), r->k->tier,
                 r->nthreads ? r->nthreads : 1, perf, thr, r->steady ? "yes" : "no");
      if (isfinite(r->tau_s)) buf_printf(b, " (tau %.0f s, %.0f s run)", r->tau_s, r->elapsed_s);
      else buf_printf(b, " (%.0f s run)", r->elapsed_s);
      if (r->purpose) buf_printf(b, "  [%s]", r->purpose);
      buf_printf(b, "\n");
    }
  }

  if (a->nsc) {
    buf_printf(b, "\nScaling (all cores, working together): S(n) = T1/Tn, E(n) = S/n, p(n) = parallel fraction\n  %-24s %4s %-22s %-8s %s\n",
               "test", "n", "S [95% CI]", "E", "p [95% CI]");
    for (size_t i = 0; i < a->nsc; i++) {
      const scale_row *s = &a->sc[i];
      snprintf(l, sizeof l, "%s", pmk_display_name(s->kernel));
      buf_printf(b, "  %-24s %4d %6.3f [%.3f, %.3f]   %.3f    ", l, s->n, s->S.est, s->S.lo, s->S.hi, s->E.est);
      if (isfinite(s->p.est)) buf_printf(b, "%.3f [%.3f, %.3f]\n", s->p.est, s->p.lo, s->p.hi);
      else buf_printf(b, "-\n");
    }
  }
  if (a->nra) {
    buf_printf(b, "\nSame-kernel ratios\n");
    for (size_t i = 0; i < a->nra; i++) {
      const ratio_row *r = &a->ra[i];
      snprintf(l, sizeof l, "%s%s%s", pmk_display_name(r->kernel), r->variant ? " " : "",
               r->variant ? r->variant : "");
      buf_printf(b, "  %-8s %-26s", r->name, l);
      if (r->n) buf_printf(b, " n=%-3d", r->n);
      else buf_printf(b, " %-5s", r->core_type);
      buf_printf(b, " %.3f [%.3f, %.3f]\n", r->r.est, r->r.lo, r->r.hi);
    }
  }
  if (a->nst) {
    buf_printf(b, "\nStress test: score = speed in the 10th minute; hold-up = last minute / first minute\n");
    for (size_t i = 0; i < a->nst; i++) {
      const stress_row *x = &a->st[i];
      const pmk_result *r = x->r;
      double ran = (double)r->n * r->window_ms / 1e3;
      const char *v = stress_verdict(r);
      if (!strcmp(v, "failed")) {
        buf_printf(b, "  %s: FAILED: wrong results", r->part);
        if (isfinite(r->first_wrong_s)) buf_printf(b, ", the first after %d:%02d", (int)r->first_wrong_s / 60, (int)r->first_wrong_s % 60);
        if (r->first_wrong_cpu >= 0) buf_printf(b, " on CPU %d", r->first_wrong_cpu);
        buf_printf(b, " (%llu wrong task%s, %llu wrong job%s)", (unsigned long long)r->wrong_tasks,
                   r->wrong_tasks == 1 ? "" : "s", (unsigned long long)r->errors, r->errors == 1 ? "" : "s");
        for (int k = 0; k < r->nwrong_cpu; k++) buf_printf(b, "%s CPU %d: %llu", k ? "," : ";", r->wrong_cpu[k], (unsigned long long)r->wrong_cpu_n[k]);
        buf_printf(b, "\n");
      } else if (!strcmp(v, "too_hot")) {
        buf_printf(b, "  %s: STOPPED at the temperature limit after %d:%02d, no wrong results\n", r->part, (int)ran / 60, (int)ran % 60);
      } else {
        buf_printf(b, "  %s: stable for %d:%02d, no wrong results%s\n", r->part, (int)ran / 60, (int)ran % 60,
                   r->stopped ? " (stopped by you)" : "");
      }
      char score[64], dur[32];
      if (isfinite(x->score.est))
        snprintf(score, sizeof score, "%.4g [%.4g, %.4g]", x->score.est, x->score.lo, x->score.hi);
      else snprintf(score, sizeof score, "no score (under 10 min)");
      double s = (double)r->n * r->window_ms / 1e3;
      if (s < 120) snprintf(dur, sizeof dur, "%.0f s%s", s, r->stopped ? ", stopped" : "");
      else snprintf(dur, sizeof dur, "%.0f min%s", s / 60, r->stopped ? ", stopped" : "");
      buf_printf(b, "  %-10s %-12s %3d thr  %-28s %s  hold-up %.3f", r->part, pmk_display_name(r->kernel),
                 r->nthreads ? r->nthreads : 1, score, unit_of(r), x->hold_up.est);
      if (isfinite(x->temp_max)) buf_printf(b, "  max %.0f C", x->temp_max);
      buf_printf(b, "  (%s)\n", dur);
    }
  }
  int bad = 0;
  for (size_t i = 0; i < a->nsums; i++) bad += !a->sums[i].consistent;
  if (a->nsums)
    buf_printf(b, "\nChecksums: %zu kernel/tier/input combinations, %s\n", a->nsums,
               bad ? "SOME INCONSISTENT (see analysis.checksums)" : "all consistent");
}
