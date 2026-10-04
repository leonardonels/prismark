/*
 * Comparison of two runs and the transparent profiles (spec 4 and 9.5).
 *
 * Every ratio r = perf(A) / perf(B) pairs two series of the same kernel,
 * mode, tier and parameters with the same input hash, and is recomputed from
 * the raw samples: both sample sets are resampled independently and the
 * ratio of medians forms the bootstrap distribution. Two machines differ on a
 * kernel only when the 95 % CI of r excludes 1.
 *
 * A profile declares kernels and time fractions alpha_i and reports
 *   H = (sum_i alpha_i / r_i)^-1   and   sigma = sqrt(sum_i alpha_i (ln r_i - mean)^2)
 * from jointly resampled r_i, alongside every r_i. A profile with a missing
 * kernel is not reported; weights are never renormalised. Tail profiles
 * (Realtime) report quantiles side by side and are never averaged.
 *
 * Copyright 2026 The Prismark Authors. Apache-2.0.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "buf.h"
#include "jparse.h"
#include "json.h"
#include "prismark/prismark.h"
#include "rng.h"
#include "stats.h"

#define B PMK_BOOT_B
#define MAX_SERIES 4096
#define MAX_ITEMS 16

static const char DEFAULT_PROFILES[] =
    "{\"schema\":\"prismark-profiles/1\",\"profiles\":[\n"
    " {\"name\":\"Daily\",\"kind\":\"speed\",\"items\":[\n"
    "  {\"kernel\":\"K4\",\"mode\":\"cold_burst\",\"start\":\"cold\",\"alpha\":0.25},\n"
    "  {\"kernel\":\"K6\",\"mode\":\"cold_burst\",\"start\":\"cold\",\"alpha\":0.25},\n"
    "  {\"kernel\":\"K3\",\"mode\":\"st_burst\",\"alpha\":0.25},\n"
    "  {\"kernel\":\"K5\",\"mode\":\"st_burst\",\"alpha\":0.25}]},\n"
    " {\"name\":\"Dev\",\"kind\":\"speed\",\"items\":[\n"
    "  {\"kernel\":\"K1\",\"mode\":\"st_sustained\",\"alpha\":0.3333333333},\n"
    "  {\"kernel\":\"K1\",\"mode\":\"mc_threaded\",\"n\":\"max\",\"alpha\":0.3333333333},\n"
    "  {\"kernel\":\"K1x\",\"mode\":\"mc_threaded\",\"n\":\"max\",\"alpha\":0.3333333334}]},\n"
    " {\"name\":\"Render\",\"kind\":\"single\",\"items\":[\n"
    "  {\"kernel\":\"K2\",\"mode\":\"mc_threaded\",\"n\":\"max\",\"alpha\":1}]},\n"
    " {\"name\":\"Realtime\",\"kind\":\"tail\",\"items\":[\n"
    "  {\"kernel\":\"K10\",\"mode\":\"periodic\"}]}\n"
    "]}\n";

char *pmk_default_profiles(void) {
  char *s = malloc(sizeof DEFAULT_PROFILES);
  if (s) memcpy(s, DEFAULT_PROFILES, sizeof DEFAULT_PROFILES);
  return s;
}

/* ---------- series ---------- */

typedef struct series {
  const jv *r;
  const char *kernel, *variant, *mode, *tier, *purpose, *core, *size, *start, *condition, *input_hash;
  int n;            /* threads or instances; 1 for single-core */
  double ws;        /* K7 working set */
  double w_ms;      /* K9 W */
  int higher_better;
  double *x;        /* comparable samples: steady windows (higher better) or times (lower better) */
  size_t nx;
} series;

typedef struct run {
  jdoc *doc;
  const jv *root;
  series *s;
  size_t ns;
  int max_n_threaded, max_n_instances;
  const char *fast_core; /* name of core type 0 */
} run;

static const char *str_or(const jv *v, const char *key, const char *def) { return jv_str(jv_get(v, key), def); }

static int load_series(run *R, char *err, size_t errlen) {
  const jv *results = jv_get(R->root, "results");
  size_t n = jv_len(results);
  R->s = calloc(n ? n : 1, sizeof *R->s);
  if (!R->s) return snprintf(err, errlen, "out of memory"), -1;
  R->fast_core = "P";
  const jv *cores = jv_path(R->root, "machine.cpu.cores");
  if (jv_len(cores)) R->fast_core = str_or(&cores->items[0], "type", "P");
  for (size_t i = 0; i < n; i++) {
    const jv *r = &results->items[i];
    const jv *params = jv_get(r, "params"), *samples = jv_get(r, "samples");
    /* Files from before ABI 4 may hold series measured with changed power settings (the removed
     * cold-burst grid); only those measured as configured are comparable. */
    const char *cell = str_or(params, "grid_cell", NULL);
    if (cell && strcmp(cell, "as_shipped")) continue;
    series *s = &R->s[R->ns];
    memset(s, 0, sizeof *s);
    s->r = r;
    s->kernel = str_or(r, "kernel", "");
    s->variant = str_or(r, "variant", NULL);
    s->mode = str_or(r, "mode", "");
    s->tier = str_or(r, "tier", "baseline");
    s->purpose = str_or(params, "purpose", NULL);
    s->core = jv_str(jv_path(r, "core.requested"), "");
    s->size = str_or(params, "size", NULL);
    s->start = str_or(params, "start", NULL);
    s->condition = str_or(params, "condition", NULL);
    s->input_hash = str_or(r, "input_hash", NULL);
    s->n = (int)jv_num(jv_get(params, "threads"), jv_num(jv_get(params, "instances"), 1));
    s->ws = jv_num(jv_get(params, "working_set_bytes"), 0);
    s->w_ms = jv_num(jv_get(params, "W_ms"), 0);
    size_t ns = jv_len(samples);
    if (!ns) continue;
    size_t from = 0;
    if (jv_get(params, "window_ms")) {
      s->higher_better = 1;
      from = (size_t)jv_num(jv_path(r, "steady.from_window"), 0);
      if (from >= ns) from = 0;
    } else if (!strcmp(s->mode, "periodic")) {
      s->higher_better = 0;
    } else if (strcmp(str_or(r, "unit", ""), "ns")) {
      continue; /* unknown unit: not comparable */
    }
    s->nx = ns - from;
    s->x = malloc(s->nx * sizeof *s->x);
    if (!s->x) return snprintf(err, errlen, "out of memory"), -1;
    for (size_t k = 0; k < s->nx; k++) s->x[k] = jv_num(&samples->items[from + k], NAN);
    if (!strcmp(s->mode, "mc_threaded") && s->n > R->max_n_threaded) R->max_n_threaded = s->n;
    if (!strcmp(s->mode, "mc_instances") && s->n > R->max_n_instances) R->max_n_instances = s->n;
    R->ns++;
  }
  return 0;
}

static int eq(const char *a, const char *b) { return a == b || (a && b && !strcmp(a, b)); }

/* Same series in another run: kernel, mode, tier and all parameters; n may be remapped for "max". */
static const series *match(const run *R, const series *s, int n) {
  for (size_t i = 0; i < R->ns; i++) {
    const series *t = &R->s[i];
    if (!eq(t->kernel, s->kernel) || !eq(t->variant, s->variant) || !eq(t->mode, s->mode) || !eq(t->tier, s->tier) ||
        !eq(t->purpose, s->purpose) || !eq(t->core, s->core) || !eq(t->size, s->size) ||
        !eq(t->start, s->start) || !eq(t->condition, s->condition) || t->n != n || t->ws != s->ws ||
        t->w_ms != s->w_ms)
      continue;
    return t;
  }
  return NULL;
}

/* r = perf(A) / perf(B): ratio of windows, or inverse ratio of times. */
static int ratio_reps(const series *a, const series *b, pmk_rng *rng, double *est, double *reps) {
  if (a->higher_better) {
    *est = pmk_median(a->x, a->nx) / pmk_median(b->x, b->nx);
    return pmk_boot_ratio_reps(a->x, a->nx, b->x, b->nx, B, rng, reps);
  }
  *est = pmk_median(b->x, b->nx) / pmk_median(a->x, a->nx);
  return pmk_boot_ratio_reps(b->x, b->nx, a->x, a->nx, B, rng, reps);
}

static void ci_json(pmk_jw *w, const char *key, pmk_ci ci) {
  jw_obj_begin(w, key);
  jw_num(w, "est", ci.est);
  jw_num(w, "lo", ci.lo);
  jw_num(w, "hi", ci.hi);
  jw_obj_end(w);
}

static void free_run(run *R) {
  for (size_t i = 0; i < R->ns; i++) free(R->s[i].x);
  free(R->s);
  jdoc_free(R->doc);
}

/*
 * Capabilities that affect measurements must agree: pinning decides where single-core work runs and
 * perf counters how the K9 loop is calibrated. The power telemetry source never affects a measurement.
 */
static int caps_mismatch(const run *a, const run *b, pmk_buf *why) {
  static const char *const keys[] = {"pinning", "perf_counters"};
  int bad = 0;
  for (size_t i = 0; i < sizeof keys / sizeof *keys; i++) {
    char path[64];
    snprintf(path, sizeof path, "machine.capabilities.%s", keys[i]);
    int x = jv_bool(jv_path(a->root, path), -1), y = jv_bool(jv_path(b->root, path), -1);
    if (x != y) {
      buf_printf(why, "capability %s differs (A: %s, B: %s)\n", keys[i], x == 1 ? "yes" : x == 0 ? "no" : "?",
                 y == 1 ? "yes" : y == 0 ? "no" : "?");
      bad = 1;
    }
  }
  return bad;
}

static void tail_json(pmk_jw *w, const char *key, const series *s) {
  jw_obj_begin(w, key);
  jw_int(w, "n", (int64_t)s->nx);
  jw_num(w, "p50_ns", pmk_quantile(s->x, s->nx, 0.5));
  jw_num(w, "p99_ns", pmk_quantile(s->x, s->nx, 0.99));
  jw_num(w, "p999_ns", pmk_quantile(s->x, s->nx, 0.999));
  jw_num(w, "max_ns", pmk_max(s->x, s->nx));
  jw_obj_end(w);
}

static void series_label(char *out, size_t n, const series *s) {
  snprintf(out, n, "%s%s%s %s%s%s n=%d%s%s", s->kernel, s->variant ? " " : "", s->variant ? s->variant : "", s->mode,
           s->start ? " " : "", s->start ? s->start : "", s->n, s->purpose ? " isa_uplift/" : " ",
           s->purpose ? s->tier : s->core);
}

/* ---------- profiles ---------- */

typedef struct pitem {
  const series *a, *b;
  double alpha, est;
  double *reps;
} pitem;

/* Finds the series a profile item names in run R: baseline tier, fastest core type, no ISA-uplift purpose. */
static const series *find_item(const run *R, const jv *item) {
  const char *kernel = str_or(item, "kernel", ""), *mode = str_or(item, "mode", "");
  const char *start = str_or(item, "start", NULL), *tier = str_or(item, "tier", "baseline");
  const char *core = str_or(item, "core_type", R->fast_core);
  const jv *nv = jv_get(item, "n");
  int n = 1;
  if (nv && nv->t == JV_STR && !strcmp(nv->str, "max"))
    n = !strcmp(mode, "mc_instances") ? R->max_n_instances : R->max_n_threaded;
  else if (nv)
    n = (int)jv_num(nv, 1);
  for (size_t i = 0; i < R->ns; i++) {
    const series *s = &R->s[i];
    if (strcmp(s->kernel, kernel) || strcmp(s->mode, mode) || strcmp(s->tier, tier) || s->purpose) continue;
    if (start && !eq(s->start, start)) continue;
    if (strcmp(s->mode, "periodic") && s->n != n) continue;
    if (s->n == 1 && core && *s->core && strcmp(s->core, core)) continue;
    return s;
  }
  return NULL;
}

static void profile(pmk_jw *w, pmk_buf *t, const jv *p, const run *A, const run *Bn, pmk_rng *rng) {
  const char *name = str_or(p, "name", "?"), *kind = str_or(p, "kind", "speed");
  const jv *items = jv_get(p, "items");
  size_t ni = jv_len(items);
  jw_obj_begin(w, NULL);
  jw_str(w, "name", name);
  jw_str(w, "kind", kind);

  if (!strcmp(kind, "tail")) {
    buf_printf(t, "\n%s (tail latency, never averaged)\n", name);
    jw_arr_begin(w, "conditions");
    int shown = 0;
    for (size_t i = 0; i < A->ns; i++) {
      const series *a = &A->s[i];
      if (strcmp(a->mode, "periodic")) continue;
      const series *b = match(Bn, a, a->n);
      if (!b) continue;
      jw_obj_begin(w, NULL);
      jw_str(w, "condition", a->condition);
      tail_json(w, "a", a);
      tail_json(w, "b", b);
      jw_obj_end(w);
      buf_printf(t, "  %-7s p99.9 %8.1f us vs %8.1f us   max %8.1f us vs %8.1f us\n", a->condition,
                 pmk_quantile(a->x, a->nx, 0.999) / 1e3, pmk_quantile(b->x, b->nx, 0.999) / 1e3,
                 pmk_max(a->x, a->nx) / 1e3, pmk_max(b->x, b->nx) / 1e3);
      shown++;
    }
    jw_arr_end(w);
    if (!shown) buf_printf(t, "  not reported: no periodic results in both runs\n");
    jw_str(w, "status", shown ? "ok" : "not reported: no periodic results in both runs");
    jw_obj_end(w);
    return;
  }

  pitem it[MAX_ITEMS];
  memset(it, 0, sizeof it);
  const char *missing = NULL;
  char why[160] = "";
  double asum = 0;
  if (ni == 0 || ni > MAX_ITEMS) missing = "profile has no items or too many";
  for (size_t i = 0; i < ni && !missing; i++) {
    const jv *item = &items->items[i];
    it[i].alpha = jv_num(jv_get(item, "alpha"), NAN);
    asum += it[i].alpha;
    it[i].a = find_item(A, item);
    it[i].b = find_item(Bn, item);
    if (!it[i].a || !it[i].b) {
      snprintf(why, sizeof why, "%s %s missing in %s", str_or(item, "kernel", "?"), str_or(item, "mode", "?"),
               !it[i].a && !it[i].b ? "both runs" : !it[i].a ? "run A" : "run B");
      missing = why;
    } else if (it[i].a->input_hash && it[i].b->input_hash && strcmp(it[i].a->input_hash, it[i].b->input_hash)) {
      snprintf(why, sizeof why, "%s inputs differ between the runs", it[i].a->kernel);
      missing = why;
    }
  }
  if (!missing && !(fabs(asum - 1) < 1e-6)) missing = "weights alpha do not sum to 1";

  double *H = NULL, *S = NULL;
  for (size_t i = 0; i < ni && !missing; i++) {
    it[i].reps = malloc(B * sizeof(double));
    if (!it[i].reps || ratio_reps(it[i].a, it[i].b, rng, &it[i].est, it[i].reps)) missing = "out of memory";
  }
  if (!missing) {
    H = malloc(B * sizeof *H);
    S = malloc(B * sizeof *S);
    if (!H || !S) missing = "out of memory";
  }

  if (missing) {
    char st[200];
    snprintf(st, sizeof st, "not reported: %s", missing);
    jw_str(w, "status", st);
    buf_printf(t, "\n%s: %s\n", name, st);
  } else {
    /* Joint bootstrap distribution of H and sigma: replicate b uses replicate b of every r_i. */
    for (int b = 0; b < B; b++) {
      double inv = 0, mean = 0, var = 0;
      for (size_t i = 0; i < ni; i++) {
        inv += it[i].alpha / it[i].reps[b];
        mean += it[i].alpha * log(it[i].reps[b]);
      }
      for (size_t i = 0; i < ni; i++) var += it[i].alpha * (log(it[i].reps[b]) - mean) * (log(it[i].reps[b]) - mean);
      H[b] = 1 / inv;
      S[b] = sqrt(var);
    }
    jw_str(w, "status", "ok");
    double inv = 0, mean = 0, var = 0;
    for (size_t i = 0; i < ni; i++) {
      inv += it[i].alpha / it[i].est;
      mean += it[i].alpha * log(it[i].est);
    }
    for (size_t i = 0; i < ni; i++) var += it[i].alpha * (log(it[i].est) - mean) * (log(it[i].est) - mean);
    pmk_ci hci = pmk_ci_from_reps(1 / inv, H, B), sci = pmk_ci_from_reps(sqrt(var), S, B);
    ci_json(w, "H", hci);
    ci_json(w, "sigma_ln_r", sci);
    buf_printf(t, "\n%s: H = %.3f [%.3f, %.3f]   sigma(ln r) = %.3f [%.3f, %.3f]%s\n", name, hci.est, hci.lo, hci.hi,
               sci.est, sci.lo, sci.hi, ni == 1 ? "  (single kernel, no aggregate)" : "");
    jw_arr_begin(w, "items");
    for (size_t i = 0; i < ni; i++) {
      pmk_ci ci = pmk_ci_from_reps(it[i].est, it[i].reps, B);
      char lab[160];
      series_label(lab, sizeof lab, it[i].a);
      jw_obj_begin(w, NULL);
      jw_str(w, "kernel", it[i].a->kernel);
      jw_str(w, "mode", it[i].a->mode);
      jw_int(w, "n_a", it[i].a->n);
      jw_int(w, "n_b", it[i].b->n);
      jw_num(w, "alpha", it[i].alpha);
      ci_json(w, "r", ci);
      jw_obj_end(w);
      buf_printf(t, "    alpha %.3f  r = %.3f [%.3f, %.3f]  %s\n", it[i].alpha, ci.est, ci.lo, ci.hi, lab);
    }
    jw_arr_end(w);
  }
  for (size_t i = 0; i < ni && i < MAX_ITEMS; i++) free(it[i].reps);
  free(H);
  free(S);
  jw_obj_end(w);
}

/* ---------- entry point ---------- */

static int load_run(run *R, const char *json, const char *name, pmk_buf *why) {
  char err[160];
  memset(R, 0, sizeof *R);
  R->doc = jdoc_parse(json, err, sizeof err);
  if (!R->doc) {
    buf_printf(why, "%s: not valid JSON (%s)\n", name, err);
    return -1;
  }
  R->root = jdoc_root(R->doc);
  if (!eq(str_or(R->root, "schema", NULL), "prismark/1")) {
    buf_printf(why, "%s: not a prismark/1 result document\n", name);
    return -1;
  }
  if (load_series(R, err, sizeof err)) {
    buf_printf(why, "%s: %s\n", name, err);
    return -1;
  }
  return 0;
}

int pmk_compare(const char *a_json, const char *b_json, const char *profiles_json, char **report_json,
                char **report_text) {
  if (report_json) *report_json = NULL;
  if (report_text) *report_text = NULL;
  if (!a_json || !b_json) return PMK_ERR_INVALID;
  pmk_buf t = {0};
  run A, Bn;
  memset(&Bn, 0, sizeof Bn);
  jdoc *pdoc = NULL;
  int rc = PMK_OK;
  if (load_run(&A, a_json, "A", &t) | load_run(&Bn, b_json, "B", &t)) rc = PMK_ERR_INVALID;
  char err[160];
  if (rc == PMK_OK) {
    pdoc = jdoc_parse(profiles_json ? profiles_json : DEFAULT_PROFILES, err, sizeof err);
    if (!pdoc) {
      buf_printf(&t, "profiles: not valid JSON (%s)\n", err);
      rc = PMK_ERR_INVALID;
    }
  }
  if (rc == PMK_OK && caps_mismatch(&A, &Bn, &t)) {
    buf_printf(&t, "The runs were measured under different capabilities and are not compared.\n");
    rc = PMK_ERR_INVALID;
  }
  if (rc != PMK_OK) {
    if (report_text) *report_text = buf_take(&t);
    else buf_free(&t);
    free_run(&A);
    free_run(&Bn);
    jdoc_free(pdoc);
    return rc;
  }

  pmk_rng rng;
  pmk_rng_seed(&rng, 0xc0a1e5ce);
  pmk_jw w = {0};
  const char *ma = jv_str(jv_path(A.root, "machine.cpu.model"), "?"), *mb = jv_str(jv_path(Bn.root, "machine.cpu.model"), "?");
  jw_obj_begin(&w, NULL);
  jw_str(&w, "schema", "prismark-compare/1");
  jw_obj_begin(&w, "a");
  jw_str(&w, "run_id", jv_str(jv_get(A.root, "run_id"), NULL));
  jw_str(&w, "model", ma);
  jw_obj_end(&w);
  jw_obj_begin(&w, "b");
  jw_str(&w, "run_id", jv_str(jv_get(Bn.root, "run_id"), NULL));
  jw_str(&w, "model", mb);
  jw_obj_end(&w);
  buf_printf(&t, "Prismark comparison  r = perf(A) / perf(B), median [95%% CI]\n  A: %s\n  B: %s\n", ma, mb);

  /* Per-kernel ratios over every matching series. */
  jw_arr_begin(&w, "kernels");
  double *reps = malloc(B * sizeof *reps);
  size_t matched = 0, skipped_inputs = 0;
  buf_printf(&t, "\nPer kernel (* = CI excludes 1)\n");
  for (size_t i = 0; reps && i < A.ns; i++) {
    const series *a = &A.s[i];
    if (!strcmp(a->mode, "periodic")) continue;
    const series *b = match(&Bn, a, a->n);
    if (!b) continue;
    if (a->input_hash && b->input_hash && strcmp(a->input_hash, b->input_hash)) {
      skipped_inputs++;
      continue;
    }
    double est;
    if (ratio_reps(a, b, &rng, &est, reps)) continue;
    pmk_ci ci = pmk_ci_from_reps(est, reps, B);
    int differ = ci.lo > 1 || ci.hi < 1;
    char lab[160];
    series_label(lab, sizeof lab, a);
    jw_obj_begin(&w, NULL);
    jw_str(&w, "kernel", a->kernel);
    jw_str(&w, "variant", a->variant);
    jw_str(&w, "mode", a->mode);
    jw_str(&w, "tier", a->tier);
    jw_str(&w, "purpose", a->purpose);
    jw_str(&w, "core_type", a->core);
    jw_int(&w, "n", a->n);
    jw_str(&w, "start", a->start);
    if (a->ws > 0) jw_num(&w, "working_set_bytes", a->ws);
    if (a->w_ms > 0) jw_num(&w, "W_ms", a->w_ms);
    ci_json(&w, "r", ci);
    jw_bool(&w, "different", differ);
    jw_obj_end(&w);
    buf_printf(&t, "  %-48s %7.3f [%.3f, %.3f]%s\n", lab, ci.est, ci.lo, ci.hi, differ ? " *" : "");
    matched++;
  }
  jw_arr_end(&w);
  free(reps);
  if (!matched) buf_printf(&t, "  no series in common\n");
  if (skipped_inputs) buf_printf(&t, "  %zu series skipped: different input hashes\n", skipped_inputs);

  jw_arr_begin(&w, "profiles");
  const jv *profiles = jv_get(jdoc_root(pdoc), "profiles");
  for (size_t i = 0; i < jv_len(profiles); i++) profile(&w, &t, &profiles->items[i], &A, &Bn, &rng);
  jw_arr_end(&w);
  jw_obj_end(&w);

  if (report_json) *report_json = buf_take(&w.b);
  else buf_free(&w.b);
  if (report_text) *report_text = buf_take(&t);
  else buf_free(&t);
  free_run(&A);
  free_run(&Bn);
  jdoc_free(pdoc);
  return PMK_OK;
}
