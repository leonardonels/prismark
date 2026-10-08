/*
 * Statistics over a run's raw samples: medians with bootstrap CIs, R_resp
 * and periodic tail quantiles. Computed once per run; the JSON and the text summary render
 * the same numbers. Front-ends only display what is computed here.
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

#define MAX_W 64

typedef struct cb_row { /* one (core type, W) point of the responsiveness curve */
  int type;
  double w_ms;
  const pmk_result *wr, *cr;
  pmk_ci tw, tc, rr;
  double wake_med;
} cb_row;

typedef struct pd_row {
  const pmk_result *r;
  double p50, p99, p999, max;
} pd_row;

struct pmk_analysis {
  cb_row *cb;
  size_t ncb;
  pd_row *pd;
  size_t npd;
};

static const pmk_result *find_cold_burst(const pmk_ctx *c, int type, double w_ms, int warm) {
  for (size_t i = 0; i < c->nres; i++) {
    const pmk_result *r = &c->res[i];
    if (strcmp(r->mode, "cold_burst") || strcmp(r->kernel, "K9") || r->type != type || r->warm != warm ||
        r->w_ms != w_ms)
      continue;
    if (r->n > 0) return r;
  }
  return NULL;
}

/* Distinct W values of cold-burst results for one core type, ascending. */
static int w_values(const pmk_ctx *c, int type, double *out) {
  int n = 0;
  for (size_t i = 0; i < c->nres; i++) {
    const pmk_result *r = &c->res[i];
    if (strcmp(r->mode, "cold_burst") || strcmp(r->kernel, "K9") || r->type != type) continue;
    int seen = 0;
    for (int k = 0; k < n; k++) seen |= out[k] == r->w_ms;
    if (!seen && n < MAX_W) out[n++] = r->w_ms;
  }
  for (int a = 0; a < n; a++)
    for (int b = a + 1; b < n; b++)
      if (out[b] < out[a]) {
        double t = out[a];
        out[a] = out[b];
        out[b] = t;
      }
  return n;
}

void analysis_compute(pmk_ctx *c) {
  pmk_analysis *a = calloc(1, sizeof *a);
  if (!a) return;
  c->an = a;
  size_t cap = c->nres ? c->nres : 1;
  a->cb = calloc(cap, sizeof *a->cb);
  a->pd = calloc(cap, sizeof *a->pd);
  if (!a->cb || !a->pd) return;

  double ws[MAX_W];
  for (int t = 0; t < c->m.ntypes; t++) {
    int nw = w_values(c, t, ws);
    for (int i = 0; i < nw; i++) {
      const pmk_result *wr = find_cold_burst(c, t, ws[i], 1);
      const pmk_result *cr = find_cold_burst(c, t, ws[i], 0);
      if (!wr || !cr) continue;
      cb_row *row = &a->cb[a->ncb++];
      row->type = t;
      row->w_ms = ws[i];
      row->wr = wr;
      row->cr = cr;
      row->tw = pmk_boot_median(wr->samples, wr->n, PMK_BOOT_B, &c->stat_rng);
      row->tc = pmk_boot_median(cr->samples, cr->n, PMK_BOOT_B, &c->stat_rng);
      row->rr = pmk_boot_median_ratio(wr->samples, wr->n, cr->samples, cr->n, PMK_BOOT_B, &c->stat_rng);
      row->wake_med = pmk_median(cr->wake, cr->n);
    }
  }

  for (size_t i = 0; i < c->nres; i++) {
    const pmk_result *r = &c->res[i];
    if (strcmp(r->mode, "periodic") || r->n == 0) continue;
    pd_row *row = &a->pd[a->npd++];
    row->r = r;
    row->p50 = pmk_quantile(r->samples, r->n, 0.5);
    row->p99 = pmk_quantile(r->samples, r->n, 0.99);
    row->p999 = pmk_quantile(r->samples, r->n, 0.999);
    row->max = pmk_max(r->samples, r->n);
  }
}

void analysis_free(pmk_ctx *c) {
  if (!c->an) return;
  free(c->an->cb);
  free(c->an->pd);
  free(c->an);
  c->an = NULL;
}

/* ---------- JSON ---------- */

static void ci_json(pmk_jw *w, const char *key, pmk_ci ci) {
  jw_obj_begin(w, key);
  jw_num(w, "est", ci.est);
  jw_num(w, "lo", ci.lo);
  jw_num(w, "hi", ci.hi);
  jw_obj_end(w);
}

/* Relative CI half-width of a median: the achieved precision. */
static double rel_halfwidth(pmk_ci ci) { return (ci.hi - ci.lo) / 2 / ci.est; }

void analysis_json(pmk_ctx *c, pmk_jw *w, const char *key) {
  const pmk_analysis *a = c->an;
  jw_obj_begin(w, key);
  if (!a) {
    jw_obj_end(w);
    return;
  }

  jw_arr_begin(w, "calibration");
  for (int t = 0; t < c->m.ntypes; t++) {
    const pmk_calib *cal = &c->calib[t];
    if (!cal->valid) continue;
    jw_obj_begin(w, NULL);
    jw_str(w, "kernel", "K9");
    jw_str(w, "core_type", c->m.type_names[t]);
    jw_int(w, "cpu", cal->cpu);
    jw_str(w, "method", cal->method);
    jw_int(w, "adds_per_iter", c->k->k9_adds_per_iter);
    jw_num(w, "cycles_per_iter", cal->cycles_per_iter);
    jw_num(w, "cycles_per_add", cal->cycles_per_iter / c->k->k9_adds_per_iter);
    jw_num(w, "cycles_spread_rel", cal->cycles_spread_rel);
    jw_num(w, "ns_per_iter", cal->ns_per_iter);
    jw_num(w, "implied_mhz", cal->implied_mhz);
    jw_obj_end(w);
  }
  jw_arr_end(w);

  jw_arr_begin(w, "cold_burst");
  for (size_t i = 0; i < a->ncb; i++) {
    const cb_row *r = &a->cb[i];
    jw_obj_begin(w, NULL);
    jw_str(w, "core_type", c->m.type_names[r->type]);
    jw_int(w, "cpu", r->cr->cpu);
    jw_num(w, "W_ms", r->w_ms);
    jw_int(w, "W_cycles", (int64_t)r->cr->w_cycles);
    jw_int(w, "n_warm", (int64_t)r->wr->n);
    jw_int(w, "n_cold", (int64_t)r->cr->n);
    ci_json(w, "t_warm_ns", r->tw);
    ci_json(w, "t_cold_ns", r->tc);
    jw_num(w, "t_warm_ci_rel", rel_halfwidth(r->tw));
    jw_num(w, "t_cold_ci_rel", rel_halfwidth(r->tc));
    jw_num(w, "wake_ns_median", r->wake_med);
    ci_json(w, "R_resp", r->rr);
    jw_obj_end(w);
  }
  jw_arr_end(w);

  jw_arr_begin(w, "periodic");
  for (size_t i = 0; i < a->npd; i++) {
    const pd_row *p = &a->pd[i];
    jw_obj_begin(w, NULL);
    jw_str(w, "condition", p->r->condition);
    jw_int(w, "cpu", p->r->cpu);
    jw_num(w, "period_ms", p->r->period_ms);
    jw_int(w, "n", (int64_t)p->r->n);
    jw_int(w, "overruns", (int64_t)p->r->overruns);
    jw_num(w, "p50_ns", p->p50);
    jw_num(w, "p99_ns", p->p99);
    jw_num(w, "p999_ns", p->p999);
    jw_num(w, "max_ns", p->max);
    /* n(1-q) >= 10 samples beyond the quantile. */
    jw_bool(w, "p99_reliable", p->r->n >= 1000);
    jw_bool(w, "p999_reliable", p->r->n >= 10000);
    jw_obj_end(w);
  }
  jw_arr_end(w);

  analysis_tp_json(c, w);
  jw_obj_end(w);
}

/* ---------- text ---------- */

static const char *fmt_ns(char *out, size_t n, double ns) {
  double v = fabs(ns);
  if (!isfinite(ns)) snprintf(out, n, "n/a");
  else if (v >= 1e9) snprintf(out, n, "%.3g s", ns / 1e9);
  else if (v >= 1e6) snprintf(out, n, "%.3g ms", ns / 1e6);
  else if (v >= 1e3) snprintf(out, n, "%.3g us", ns / 1e3);
  else snprintf(out, n, "%.0f ns", ns);
  return out;
}

char *analysis_text(pmk_ctx *c, const char *run_id, const char *note) {
  const pmk_analysis *a = c->an;
  pmk_buf b = {0};
  char x[32], y[32], z[32], u[32];
  buf_printf(&b, "Prismark %s  run %s\n", pmk_version(), run_id);
  buf_printf(&b, "%s, %d CPUs, %s, %s %s, tiers: baseline %s, max %s\n", c->m.model, c->m.ncpu, c->m.isa, c->m.os,
             c->m.kernel, c->k->level, c->kmax ? c->kmax->level : c->kmax_status);
  buf_printf(&b, "capabilities: pinning %s, perf counters %s, power %s\n", c->m.caps.pinning ? "yes" : "no",
             c->m.caps.perf_counters ? "yes" : "no", c->m.caps.power[0] ? c->m.caps.power : "none");
  if (!a) return buf_take(&b);

  for (int t = 0; t < c->m.ntypes; t++) {
    const pmk_calib *cal = &c->calib[t];
    if (!cal->valid) continue;
    buf_printf(&b, "\nWake-up test, started from rest: core %s (CPU %d)\n  calibration: %.3f cycles/add (%s",
               c->m.type_names[t],
               cal->cpu, cal->cycles_per_iter / c->k->k9_adds_per_iter, cal->method);
    if (isfinite(cal->cycles_spread_rel)) buf_printf(&b, ", spread %.2f%%", cal->cycles_spread_rel * 100);
    buf_printf(&b, "), %.0f MHz\n", cal->implied_mhz);

    buf_printf(&b, "  %8s  %10s  %10s  %10s  %6s  %-22s\n", "W", "t_warm", "t_cold", "wake", "n", "R_resp [95% CI]");
    for (size_t i = 0; i < a->ncb; i++) {
      const cb_row *r = &a->cb[i];
      if (r->type != t) continue;
      buf_printf(&b, "  %6g ms  %10s  %10s  %10s  %6zu  %.3f [%.3f, %.3f]\n", r->w_ms, fmt_ns(x, sizeof x, r->tw.est),
                 fmt_ns(y, sizeof y, r->tc.est), fmt_ns(z, sizeof z, r->wake_med), r->cr->n, r->rr.est, r->rr.lo,
                 r->rr.hi);
    }
  }

  for (size_t i = 0; i < a->npd; i++) {
    const pd_row *p = &a->pd[i];
    if (i == 0)
      buf_printf(&b, "\nTimer punctuality: period %g ms, CPU %d\n  %-8s  %10s  %10s  %10s  %10s  %8s  %s\n",
                 p->r->period_ms, p->r->cpu, "", "p50", "p99", "p99.9", "max", "n", "overruns");
    buf_printf(&b, "  %-8s  %10s  %10s  %10s  %10s  %8zu  %llu%s\n", p->r->condition, fmt_ns(x, sizeof x, p->p50),
               fmt_ns(y, sizeof y, p->p99), fmt_ns(z, sizeof z, p->p999), fmt_ns(u, sizeof u, p->max), p->r->n,
               (unsigned long long)p->r->overruns, p->r->n < 10000 ? "  (p99.9 needs n >= 10^4)" : "");
  }
  analysis_tp_text(c, &b);
  if (note) buf_printf(&b, "\n%s\n", note);
  return buf_take(&b);
}
