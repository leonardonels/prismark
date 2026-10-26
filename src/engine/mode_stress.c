/*
 * Stress test: minutes of full load, to show how the speed holds up as the machine heats up and whether it stays
 * correct. Each part is one long sustained series with no warm-up, since the heating is what it measures:
 *
 *   one core     vector maths (K12: AVX-512, AVX2 or NEON fused multiply-adds, the hottest load) on one CPU of
 *                the fastest core type
 *   all cores    vector maths on every CPU
 *   each core    vector maths on one core at a time, moving to the next physical core every 10 s and around again
 *                (OCCT's core cycling): each core at its single-core boost, where per-core instability shows
 *   memory       K11 on every CPU: 2 GiB written and checked over and over
 *
 * A part runs for cfg.stress_s, or until pmk_cancel when that is 0; its windows, temperatures, clocks and power
 * are kept, and every job's checksum is checked against the first. The score (analysis) is the speed in the
 * 10th minute, so runs of any length from 10 minutes on compare.
 *
 * Copyright 2026 The Prismark Authors. Apache-2.0.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ctx.h"

static const struct {
  uint32_t bit;
  const char *name, *label, *kernel;
  int all_cpus;
} PARTS[] = {
    {PMK_STRESS_ONE_CORE, "one_core", "one core", "K12", 0},
    {PMK_STRESS_ALL_CORES, "all_cores", "all cores", "K12", 1},
    {PMK_STRESS_EACH_CORE, "each_core", "each core in turn", "K12", 0},
    {PMK_STRESS_MEMORY, "memory", "memory", "K11", 1},
};
#define NPARTS ((int)(sizeof PARTS / sizeof *PARTS))

/* K11's buffer, and a quarter more so the rest of the machine keeps room. */
#define K11_NEED ((uint64_t)5 << 29)

const char *stress_part_name(uint32_t bit) {
  for (int i = 0; i < NPARTS; i++)
    if (PARTS[i].bit == bit) return PARTS[i].name;
  return "?";
}

static double part_seconds(const pmk_ctx *c) { return c->cfg.stress_s > 0 ? c->cfg.stress_s : PMK_STRESS_MAX_S; }

static int part_selected(const pmk_ctx *c, int i) {
  return (c->cfg.stress_parts & PARTS[i].bit) && ctx_kernel_selected(c, PARTS[i].kernel) &&
         pmk_find_tk(c->k, PARTS[i].kernel, NULL);
}

int mode_stress_steps(const pmk_ctx *c) {
  int n = 0;
  for (int i = 0; i < NPARTS; i++) n += part_selected(c, i);
  return n;
}

/* NaN for a run without a set length: there is no time left to estimate. */
double mode_stress_est(const pmk_ctx *c) {
  if (!(c->cfg.stress_s > 0)) return mode_stress_steps(c) ? NAN : 0;
  return (part_seconds(c) + 2) * mode_stress_steps(c);
}

/* Core cycling: seconds on each core per visit. */
#define CYCLE_S 10.0

/* Appends a visit's windows and checks to a core's series; offset_s is when the visit began in the part. */
static int merge_visit(pmk_result *dst, pmk_result *v, double offset_s) {
  for (size_t i = 0; i < v->n; i++)
    if (result_push(dst, v->samples[i], NAN, v->observed[i])) return PMK_ERR_NOMEM;
  for (size_t i = 0; i < v->temps.n; i++)
    if (dvec_push(&dst->temps, v->temps.v[i]) || dvec_push(&dst->mhz, i < v->mhz.n ? v->mhz.v[i] : 0) ||
        dvec_push(&dst->power, i < v->power.n ? v->power.v[i] : NAN))
      return PMK_ERR_NOMEM;
  for (size_t i = 0; i < v->job_ns.n; i++)
    if (dvec_push(&dst->job_ns, v->job_ns.v[i])) return PMK_ERR_NOMEM;
  dst->windowed = 1;
  dst->window_ms = v->window_ms;
  dst->nthreads = 1;
  dst->steady = 1;
  dst->elapsed_s += v->elapsed_s;
  dst->input_hash = v->input_hash;
  dst->checksum = v->checksum;
  dst->checksum_ok = (dst->checksum_ok || dst->n == v->n) && v->checksum_ok;
  dst->isa = v->isa;
  dst->errors += v->errors;
  dst->wrong_tasks += v->wrong_tasks;
  dst->clamped += v->clamped;
  if (v->clamped && !(dst->lowest_mhz <= v->lowest_mhz)) dst->lowest_mhz = v->lowest_mhz;
  if (isfinite(v->first_wrong_s) && !isfinite(dst->first_wrong_s)) {
    dst->first_wrong_s = offset_s + v->first_wrong_s;
    dst->first_wrong_cpu = v->first_wrong_cpu;
  }
  for (int i = 0; i < v->nwrong_cpu; i++) {
    int k = 0;
    while (k < dst->nwrong_cpu && dst->wrong_cpu[k] != v->wrong_cpu[i]) k++;
    if (k == dst->nwrong_cpu && k < PMK_MAX_WRONG_CPUS) dst->wrong_cpu[dst->nwrong_cpu++] = v->wrong_cpu[i], dst->wrong_cpu_n[k] = 0;
    if (k < PMK_MAX_WRONG_CPUS) dst->wrong_cpu_n[k] += v->wrong_cpu_n[i];
  }
  if (v->stopped) dst->stopped = 1, dst->stop_reason = v->stop_reason;
  else if (!dst->stopped) dst->stop_reason = v->stop_reason;
  return PMK_OK;
}

/*
 * Core cycling: one series per physical core (its first CPU), filled visit by visit until the part's time is
 * up, a wrong result or the temperature limit ends it, or (without a set length) it is stopped.
 */
static int each_core(pmk_ctx *c, const pmk_tk *tk, void *inst, const int *order, int open_ended) {
  int *cores = malloc((size_t)c->m.ncpu * sizeof *cores);
  size_t *idx = malloc((size_t)c->m.ncpu * sizeof *idx);
  int n = 0, rc = PMK_OK;
  if (!cores || !idx) rc = PMK_ERR_NOMEM;
  for (int i = 0; rc == PMK_OK && i < c->m.ncpu; i++) { /* fastest type first; SMT siblings left out */
    const pmk_cpu *cpu = NULL;
    for (int k = 0; k < c->m.ncpu; k++)
      if (c->m.cpus[k].id == order[i]) cpu = &c->m.cpus[k];
    if (!cpu) continue;
    int dup = 0;
    for (int k = 0; k < n && cpu->core >= 0; k++)
      for (int q = 0; q < c->m.ncpu; q++)
        if (c->m.cpus[q].id == cores[k] && c->m.cpus[q].core == cpu->core) dup = 1;
    if (dup) continue;
    pmk_result *r = ctx_new_result(c, tk->id, "stress", tk->unit);
    if (!r) {
      rc = PMK_ERR_NOMEM;
      break;
    }
    r->variant = tk->variant;
    r->k = c->k;
    r->size = "full";
    r->purpose = "stress";
    r->part = "each_core";
    r->type = ctx_type_of_cpu(c, cpu->id);
    r->cpu = cpu->id;
    r->first_wrong_s = NAN;
    r->first_wrong_cpu = -1;
    r->lowest_mhz = NAN;
    idx[n] = c->nres - 1;
    cores[n++] = cpu->id;
  }
  uint64_t t0 = pal_now_ns();
  double total = part_seconds(c);
  for (int visit = 0; rc == PMK_OK && n > 0 && !c->stopped; visit++) {
    double done = (double)(pal_now_ns() - t0) * 1e-9;
    if (done >= total - 1) break;
    int k = visit % n;
    pmk_result v = {0};
    v.kernel = tk->id, v.mode = "stress", v.unit = tk->unit, v.k = c->k, v.part = "each_core";
    sus_spec s = {.tk = tk, .inst = inst, .nthreads = 1, .cpus = &cores[k],
                  .measure_s = CYCLE_S < total - done ? CYCLE_S : total - done, .window_ms = PMK_STRESS_WINDOW_MS,
                  .open_ended = open_ended, .check_tasks = 1, .stop_on_error = c->cfg.stress_stop_on_error,
                  .max_temp_c = c->cfg.stress_max_temp_c};
    if (visit % n == 0)
      ctx_emit(c, PMK_EV_INFO, "stress", tk->id, 0, 0, "each core in turn: round %d over %d cores", visit / n + 1, n);
    rc = wl_sustained(c, &s, &v);
    if (rc == PMK_OK && v.n) rc = merge_visit(&c->res[idx[k]], &v, done);
    if (v.stopped) c->stopped = 1;
    result_free(&v);
  }
  free(cores);
  free(idx);
  return rc;
}

static int stress_part(pmk_ctx *c, int i, const int *order, uint32_t step, uint32_t steps) {
  const pmk_tk *tk = pmk_find_tk(c->k, PARTS[i].kernel, NULL);
  int n = PARTS[i].all_cpus ? c->m.ncpu : 1;
  if (PARTS[i].all_cpus && c->cfg.max_threads > 0 && c->cfg.max_threads < n) n = c->cfg.max_threads;
  int cpu0 = ctx_cpu_for_type(c, 0);
  const int *cpus = PARTS[i].all_cpus ? order : &cpu0;
  uint64_t avail = pal_mem_available();
  if (!strcmp(tk->id, "K11") && avail && avail < K11_NEED) {
    char reason[128];
    snprintf(reason, sizeof reason, "not enough memory: it needs about %.1f GB free, %.1f GB available",
             (double)K11_NEED / 1e9, (double)avail / 1e9);
    ctx_unavailable(c, tk->id, NULL, "stress", reason);
    return PMK_OK;
  }
  void *inst = wl_create(c, tk, PMK_SIZE_FULL, "stress");
  if (!inst) return PMK_OK;
  int open_ended = !(c->cfg.stress_s > 0);
  ctx_emit(c, PMK_EV_PHASE, "stress", tk->id, step, steps, "stress test, %s: %s on %d thread%s, %s", PARTS[i].label,
           pmk_display_name(tk->id), n, n == 1 ? "" : "s", open_ended ? "until stopped" : "full load");
  if (PARTS[i].bit == PMK_STRESS_EACH_CORE) {
    int rc = each_core(c, tk, inst, order, open_ended);
    tk->destroy(inst);
    return rc;
  }
  int rc = PMK_ERR_NOMEM;
  pmk_result *r = ctx_new_result(c, tk->id, "stress", tk->unit);
  if (r) {
    r->variant = tk->variant;
    r->k = c->k;
    r->size = "full";
    r->purpose = "stress";
    r->part = PARTS[i].name;
    r->type = ctx_type_of_cpu(c, cpus[0]);
    r->cpu = n == 1 ? cpus[0] : -1;
    sus_spec s = {.tk = tk, .inst = inst, .nthreads = n, .cpus = cpus, .measure_s = part_seconds(c),
                  .window_ms = PMK_STRESS_WINDOW_MS, .open_ended = open_ended, .check_tasks = 1,
                  .stop_on_error = c->cfg.stress_stop_on_error, .max_temp_c = c->cfg.stress_max_temp_c};
    rc = wl_sustained(c, &s, r);
    if (r->stopped) c->stopped = 1;
  }
  tk->destroy(inst);
  return rc;
}

int mode_stress(pmk_ctx *c) {
  int *order = malloc((size_t)c->m.ncpu * sizeof *order);
  if (!order) return PMK_ERR_NOMEM;
  ctx_cpu_order(c, order);
  int steps = mode_stress_steps(c), step = 0, rc = PMK_OK;
  for (int i = 0; i < NPARTS && rc == PMK_OK; i++) {
    if (!(c->cfg.stress_parts & PARTS[i].bit) || !ctx_kernel_selected(c, PARTS[i].kernel)) continue;
    if (!pmk_find_tk(c->k, PARTS[i].kernel, NULL)) {
      ctx_unavailable(c, PARTS[i].kernel, NULL, "stress", "not built");
      continue;
    }
    if (c->stopped) { /* a part after the one that was stopped */
      ctx_unavailable(c, PARTS[i].kernel, PARTS[i].name, "stress", "not run: the stress test was stopped");
      continue;
    }
    if (step > 0) ctx_cooldown(c, "stress");
    rc = stress_part(c, i, order, (uint32_t)++step, (uint32_t)steps);
  }
  free(order);
  return rc;
}
