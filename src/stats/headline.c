/*
 * Headline comparison: the results the desktop app shows (apps/gui/metrics.cpp, same keys and the same
 * reading rules), taken from two documents and set side by side. A document is a result (prismark/1) or a
 * reference system (prismark-reference/1, references/README.md), so a run can be compared with a
 * reference that holds only typed-in values.
 *
 * Copyright 2026 The Prismark Authors. Apache-2.0.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "buf.h"
#include "jparse.h"
#include "prismark/prismark.h"

typedef enum { THROUGHPUT, K1X, MEDIAN, RRESP, K7, PERIODIC, UPLIFT } how;

static const struct metric {
  const char *key, *group, *name, *unit;
  int higher_better;
  how how;
  const char *kernel, *mode, *arg; /* MEDIAN: start; UPLIFT: variant; K7: "all"; PERIODIC: condition */
  double k;                        /* MEDIAN: scale; RRESP: W in ms */
} METRICS[] = {
    {"mc_k2", "All cores", "3D rendering on all cores", "M samples/s", 1, THROUGHPUT, "K2", "mc_threaded", NULL, 0},
    {"mc_k1", "All cores", "Compiling code on all cores", "builds/h", 1, THROUGHPUT, "K1", "mc_threaded", NULL, 0},
    {"mc_k1x", "All cores", "Full software build", "s", 0, K1X, "K1x", NULL, NULL, 0},
    {"st_k2", "One core", "3D rendering on one core", "M samples/s", 1, THROUGHPUT, "K2", "st_sustained", NULL, 0},
    {"st_k1", "One core", "Compiling code on one core", "builds/h", 1, THROUGHPUT, "K1", "st_sustained", NULL, 0},
    {"b_k3", "One core", "Compressing a file", "ms", 0, MEDIAN, "K3", "st_burst", NULL, 1e-6},
    {"b_k4", "One core", "Opening a photo", "ms", 0, MEDIAN, "K4", "st_burst", NULL, 1e-6},
    {"b_k5", "One core", "Reading structured data (JSON)", "ms", 0, MEDIAN, "K5", "st_burst", NULL, 1e-6},
    {"b_k6", "One core", "Starting a small script", "ms", 0, MEDIAN, "K6", "st_burst", NULL, 1e-6},
    {"r_1ms", "Responsiveness", "Reacting from rest: a 1 ms task", "%", 1, RRESP, "K9", NULL, NULL, 1},
    {"r_10ms", "Responsiveness", "Reacting from rest: a 10 ms task", "%", 1, RRESP, "K9", NULL, NULL, 10},
    {"c_k4", "Responsiveness", "Opening a photo from rest", "ms", 0, MEDIAN, "K4", "cold_burst", "cold", 1e-6},
    {"c_k6", "Responsiveness", "Starting a small script from rest", "ms", 0, MEDIAN, "K6", "cold_burst", "cold", 1e-6},
    {"m_lat1", "Memory & timing", "Memory delay", "ns", 0, K7, "K7", NULL, NULL, 0},
    {"m_latn", "Memory & timing", "Memory delay while every core uses memory", "ns", 0, K7, "K7", NULL, "all", 0},
    {"j_idle", "Memory & timing", "Timer punctuality, idle (worst 1 in 1000)", "us", 0, PERIODIC, "K10", NULL, "idle", 0},
    {"j_load", "Memory & timing", "Timer punctuality, other cores busy", "us", 0, PERIODIC, "K10", NULL, "loaded", 0},
    {"u_k2", "New instructions", "3D rendering: gain", "x", 1, UPLIFT, "K2", NULL, NULL, 0},
    {"u_k3", "New instructions", "Compression: gain", "x", 1, UPLIFT, "K3", NULL, NULL, 0},
    {"u_k4", "New instructions", "Opening a photo: gain", "x", 1, UPLIFT, "K4", NULL, NULL, 0},
    {"u_k8f", "New instructions", "Matrix maths, decimal numbers: gain", "x", 1, UPLIFT, "K8", NULL, "fp32", 0},
    {"u_k8i", "New instructions", "Matrix maths, small integers: gain", "x", 1, UPLIFT, "K8", NULL, "int8", 0},
};
#define NMETRICS (sizeof METRICS / sizeof *METRICS)

typedef struct value {
  int ok;
  double est, lo, hi; /* lo, hi NaN when there is no interval */
  int fewer_threads;  /* an all-cores value measured on fewer threads than the machine has */
} value;

typedef struct doc {
  jdoc *d;
  const jv *root;
  int reference; /* prismark-reference/1 */
  value v[NMETRICS];
} doc;

static int is(const jv *o, const char *key, const char *want) {
  const char *s = jv_str(jv_get(o, key), NULL);
  return want ? s && !strcmp(s, want) : !s || !*s;
}

static value ci(const jv *o, double k) {
  value v = {0, NAN, NAN, NAN, 0};
  const jv *est = jv_get(o, "est");
  if (!est || est->t != JV_NUM) return v;
  v.ok = 1;
  v.est = est->num * k;
  v.lo = jv_num(jv_get(o, "lo"), NAN) * k;
  v.hi = jv_num(jv_get(o, "hi"), NAN) * k;
  return v;
}

static int fast(const jv *r) { return is(r, "core_type", "P"); }

/* The value the desktop app shows for metric m, read from a result document (metrics.cpp). */
static value read_result(const jv *root, const struct metric *m) {
  value none = {0, NAN, NAN, NAN, 0};
  const jv *an = jv_get(root, "analysis");
  const jv *series = jv_get(an, "series");
  size_t ns = jv_len(series);
  int ncpu = (int)jv_len(jv_path(root, "machine.cpu.cores"));
  const jv *best = NULL;
  switch (m->how) {
    case THROUGHPUT:
    case K1X:
      for (size_t i = 0; i < ns; i++) {
        const jv *r = &series->items[i];
        if (!is(r, "kernel", m->kernel)) continue;
        if (m->how == THROUGHPUT &&
            (!is(r, "mode", m->mode) || !is(r, "tier", "baseline") || !is(r, "purpose", NULL)))
          continue;
        if (m->how == THROUGHPUT && !strcmp(m->mode, "st_sustained")) {
          if (fast(r)) {
            best = r;
            break;
          }
        } else if (!best || jv_num(jv_get(r, "n"), 0) > jv_num(jv_get(best, "n"), 0)) {
          best = r;
        }
      }
      if (!best) return none;
      {
        value v = m->how == K1X ? ci(jv_get(best, "median"), 1e-9) : ci(jv_get(best, "perf_steady"), 1);
        int n = (int)jv_num(jv_get(best, "n"), 0);
        v.fewer_threads = (m->how == K1X || !strcmp(m->mode, "mc_threaded")) && ncpu > 0 && n > 0 && n < ncpu;
        return v;
      }
    case MEDIAN:
      for (size_t i = 0; i < ns; i++) {
        const jv *r = &series->items[i];
        if (is(r, "kernel", m->kernel) && is(r, "mode", m->mode) && fast(r) && (!m->arg || is(r, "start", m->arg)))
          return ci(jv_get(r, "median"), m->k);
      }
      return none;
    case RRESP: {
      const jv *cb = jv_get(an, "cold_burst");
      for (size_t i = 0; i < jv_len(cb); i++) {
        const jv *r = &cb->items[i];
        const char *cell = jv_str(jv_get(r, "cell"), NULL);
        if (jv_num(jv_get(r, "W_ms"), NAN) == m->k && fast(r) && (!cell || !strcmp(cell, "as_shipped")))
          return ci(jv_get(r, "R_resp"), 100); /* a percentage of busy-core speed */
      }
      return none;
    }
    case K7: {
      double ws = 0;
      for (size_t i = 0; i < ns; i++)
        if (is(&series->items[i], "kernel", "K7")) ws = fmax(ws, jv_num(jv_get(&series->items[i], "working_set_bytes"), 0));
      for (size_t i = 0; i < ns; i++) {
        const jv *r = &series->items[i];
        if (!is(r, "kernel", "K7") || jv_num(jv_get(r, "working_set_bytes"), 0) != ws) continue;
        double n = jv_num(jv_get(r, "n"), 0);
        if (m->arg ? !best || n > jv_num(jv_get(best, "n"), 0) : n == 1) best = r;
      }
      return best ? ci(jv_get(best, "median"), 1) : none;
    }
    case PERIODIC: {
      const jv *pd = jv_get(an, "periodic");
      for (size_t i = 0; i < jv_len(pd); i++) {
        const jv *r = &pd->items[i];
        const jv *p = jv_get(r, "p999_ns");
        if (is(r, "condition", m->arg) && p && p->t == JV_NUM) return (value){1, p->num / 1e3, NAN, NAN, 0};
      }
      return none;
    }
    case UPLIFT: {
      const jv *ra = jv_get(an, "ratios");
      for (size_t i = 0; i < jv_len(ra); i++) {
        const jv *r = &ra->items[i];
        if (is(r, "name", "U_ISA") && is(r, "kernel", m->kernel) && is(r, "variant", m->arg))
          return ci(jv_get(r, "value"), 1);
      }
      return none;
    }
  }
  return none;
}

/* A reference system's typed-in value: "value", and "uncertainty" as half the 95 % range over the value. */
static value read_reference(const jv *root, const struct metric *m) {
  value v = {0, NAN, NAN, NAN, 0};
  const jv *e = jv_get(jv_get(root, "results"), m->key);
  const jv *val = jv_get(e, "value");
  if (!val || val->t != JV_NUM) return v;
  v.ok = 1;
  v.est = val->num;
  double u = jv_num(jv_get(e, "uncertainty"), NAN);
  if (isfinite(u)) {
    v.lo = v.est * (1 - u);
    v.hi = v.est * (1 + u);
  }
  return v;
}

static int load(doc *d, const char *json, const char *which, pmk_buf *t) {
  memset(d, 0, sizeof *d);
  char err[160];
  if (!json || !(d->d = jdoc_parse(json, err, sizeof err))) {
    buf_printf(t, "%s: not valid JSON (%s)\n", which, json ? err : "missing");
    return -1;
  }
  d->root = jdoc_root(d->d);
  const char *schema = jv_str(jv_get(d->root, "schema"), "");
  d->reference = !strcmp(schema, "prismark-reference/1");
  if (!d->reference && strcmp(schema, "prismark/1")) {
    buf_printf(t, "%s: not a Prismark result or reference system (schema \"%s\")\n", which, schema);
    return -1;
  }
  for (size_t i = 0; i < NMETRICS; i++)
    d->v[i] = d->reference ? read_reference(d->root, &METRICS[i]) : read_result(d->root, &METRICS[i]);
  return 0;
}

static void describe(pmk_buf *t, const char *which, const doc *d) {
  if (d->reference) {
    int ph = jv_bool(jv_get(d->root, "placeholder"), 0);
    buf_printf(t, "  %s: %s  (reference system%s)\n", which, jv_str(jv_get(d->root, "name"), "?"),
               ph ? ", PLACEHOLDER: example values, not a measurement" : "");
    return;
  }
  /* The run id and the power source tell apart two runs of the same processor. */
  const char *when = jv_str(jv_get(d->root, "started_utc"), "");
  const jv *ac = jv_path(d->root, "state.start.ac_online");
  buf_printf(t, "  %s: %s  (%s run, %.10s%s%s, run %.8s)\n", which, jv_str(jv_path(d->root, "machine.cpu.model"), "?"),
             jv_bool(jv_path(d->root, "config.quick"), 0) ? "quick" : "full", when,
             ac && ac->t == JV_BOOL ? (jv_bool(ac, 1) ? ", plugged in" : ", on battery") : "",
             jv_bool(jv_get(d->root, "complete"), 1) ? "" : ", incomplete", jv_str(jv_get(d->root, "run_id"), "?"));
}

static const char *fmt(char *out, size_t n, double v) {
  double a = fabs(v);
  snprintf(out, n, a >= 100 ? "%.0f" : a >= 10 ? "%.1f" : a >= 1 ? "%.2f" : "%.3f", v);
  return out;
}

int pmk_compare_headline(const char *a_json, const char *b_json, char **report_text) {
  if (report_text) *report_text = NULL;
  pmk_buf t = {0};
  doc A, B;
  int bad = load(&A, a_json, "A", &t);
  bad |= load(&B, b_json, "B", &t);
  if (bad) {
    jdoc_free(A.d);
    jdoc_free(B.d);
    if (report_text) *report_text = buf_take(&t);
    else buf_free(&t);
    return PMK_ERR_INVALID;
  }

  buf_printf(&t, "Prismark headline results, A against B\n");
  describe(&t, "A", &A);
  describe(&t, "B", &B);
  buf_printf(&t, "\n  %-44s %-12s %10s %10s   %s\n", "", "unit", "A", "B", "A against B");
  const char *group = NULL;
  int shown = 0, no_interval = 0, fewer = 0, better = 0, worse = 0, any_points = 0, only[2] = {0, 0};
  pmk_buf missing = {0};
  for (size_t i = 0; i < NMETRICS; i++) {
    const struct metric *m = &METRICS[i];
    const value *a = &A.v[i], *b = &B.v[i];
    if (!a->ok || !b->ok || a->est <= 0 || b->est <= 0) {
      if (a->ok || b->ok) {
        only[!a->ok]++;
        buf_printf(&missing, "%s%s (%s only)", missing.len ? ", " : "", m->name, a->ok ? "A" : "B");
      }
      continue;
    }
    if (!group || strcmp(group, m->group)) buf_printf(&t, "%s\n", group = m->group);
    /* r > 1: A is better, whichever direction the unit goes */
    double r = m->higher_better ? a->est / b->est : b->est / a->est;
    int intervals = isfinite(a->lo) && isfinite(a->hi) && isfinite(b->lo) && isfinite(b->hi);
    int overlap = intervals && a->lo <= b->hi && b->lo <= a->hi;
    char x[32], y[32], verdict[48];
    /* A percentage is compared by its difference in points: 47 % against 86 % is 38 points, not "1.80x". */
    int points = !strcmp(m->unit, "%");
    any_points |= points;
    double d = m->higher_better ? a->est - b->est : b->est - a->est;
    if (overlap) snprintf(verdict, sizeof verdict, "same");
    else if (points) snprintf(verdict, sizeof verdict, "%.1f pts %s", fabs(d), d >= 0 ? "better" : "worse");
    else if (r >= 1) snprintf(verdict, sizeof verdict, "%.2fx better", r);
    else snprintf(verdict, sizeof verdict, "%.2fx worse", 1 / r);
    if (!overlap) (points ? d >= 0 : r >= 1) ? better++ : worse++;
    char mark[48] = "";
    if (!intervals) strcat(mark, ", may be noise"), no_interval = 1;
    if (a->fewer_threads || b->fewer_threads) strcat(mark, ", fewer threads"), fewer = 1;
    buf_printf(&t, "  %-44s %-12s %10s %10s   %s%s\n", m->name, m->unit, fmt(x, sizeof x, a->est),
               fmt(y, sizeof y, b->est), verdict, mark);
    shown++;
  }
  if (!shown) buf_printf(&t, "  no headline result in both\n");
  else
    buf_printf(&t, "\nA is better on %d, worse on %d, the same within the 95 %% ranges on %d.\n", better, worse,
               shown - better - worse);
  char *miss = buf_take(&missing);
  if (only[0] + only[1] > 4) { /* a partial run: say how many, not which */
    for (int k = 0; k < 2; k++)
      if (only[k])
        buf_printf(&t, "%d result%s only in %s: %s did not run those tests.\n", only[k], only[k] == 1 ? " is" : "s are",
                   k ? "B" : "A", k ? "A" : "B");
  } else if (miss && *miss) {
    buf_printf(&t, "Not in both: %s.\n", miss);
  }
  free(miss);
  buf_printf(&t, "\"better\" and \"worse\" follow each unit's direction: a shorter time or a higher speed is better.\n");
  if (any_points) buf_printf(&t, "Percentages are compared by their difference in points.\n");
  if (no_interval) buf_printf(&t, "\"may be noise\": one side has no 95 %% range, so the difference may not be real.\n");
  if (fewer) buf_printf(&t, "\"fewer threads\": measured on fewer threads than the machine has (--max-threads).\n");
  const char *ida = A.reference ? NULL : jv_str(jv_get(A.root, "run_id"), NULL);
  const char *idb = B.reference ? NULL : jv_str(jv_get(B.root, "run_id"), NULL);
  if (ida && idb && !strcmp(ida, idb)) buf_printf(&t, "Note: A and B are the same run.\n");
  int qa = !A.reference && jv_bool(jv_path(A.root, "config.quick"), 0);
  int qb = !B.reference && jv_bool(jv_path(B.root, "config.quick"), 0);
  if (qa != qb || (qa && (A.reference || B.reference)))
    buf_printf(&t, "Note: %s a quick run, measured without warm-up and with short series; quick and full runs are "
               "not comparable.\n", qa && qb ? "both are" : qa ? "A is" : "B is");
  if ((A.reference && jv_bool(jv_get(A.root, "placeholder"), 0)) ||
      (B.reference && jv_bool(jv_get(B.root, "placeholder"), 0)))
    buf_printf(&t, "Note: a placeholder holds example values typed in by hand, not measurements.\n");

  jdoc_free(A.d);
  jdoc_free(B.d);
  if (report_text) *report_text = buf_take(&t);
  else buf_free(&t);
  return PMK_OK;
}
