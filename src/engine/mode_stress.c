/*
 * Stress test: minutes of full load, to show how the speed holds up as the machine heats up and whether it stays
 * correct. Each part is one long sustained series with no warm-up, since the heating is what it measures:
 *
 *   one core     3D rendering (K2) on one CPU of the fastest core type
 *   all cores    3D rendering on every CPU
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
    {PMK_STRESS_ONE_CORE, "one_core", "one core", "K2", 0},
    {PMK_STRESS_ALL_CORES, "all_cores", "all cores", "K2", 1},
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

/* Quick runs: a minute per part, for a first look (no score: it needs 10 minutes). */
static double part_seconds(const pmk_ctx *c) {
  if (c->cfg.quick_inputs) return 60;
  return c->cfg.stress_s > 0 ? c->cfg.stress_s : PMK_STRESS_MAX_S;
}

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
  if (!c->cfg.quick_inputs && !(c->cfg.stress_s > 0)) return mode_stress_steps(c) ? NAN : 0;
  return (part_seconds(c) + 2) * mode_stress_steps(c);
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
  int open_ended = !c->cfg.quick_inputs && !(c->cfg.stress_s > 0);
  ctx_emit(c, PMK_EV_PHASE, "stress", tk->id, step, steps, "stress test, %s: %s on %d thread%s, %s", PARTS[i].label,
           pmk_display_name(tk->id), n, n == 1 ? "" : "s", open_ended ? "until stopped" : "full load");
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
    sus_spec s = {tk, inst, n, cpus, 0, 0, part_seconds(c), PMK_STRESS_WINDOW_MS, open_ended};
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
