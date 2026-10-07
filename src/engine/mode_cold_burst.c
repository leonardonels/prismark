/*
 * Cold burst: how much slower is a task started from idle?
 *
 * K9 work of W cycles is issued at a deadline after a random idle gap
 * (t_cold, measured from the deadline, so timer wake-up and idle exit are
 * included) and back to back on a busy core (t_warm). The machine is measured
 * as it is configured: power settings are recorded, never changed.
 *
 * Copyright 2026 The Prismark Authors. Apache-2.0.
 */
#include <math.h>
#include <stdlib.h>

#include "ctx.h"
#include "stats.h"

/* Logarithmic sweep, defined in ms at the CPU's maximum clock and converted to cycles. */
static const double W_MS[] = {0.1, 0.2, 0.5, 1, 2, 5, 10, 20, 50, 100};
#define NW ((int)(sizeof W_MS / sizeof *W_MS))

#define GAP_MIN_MS 50.0
#define GAP_MAX_MS 500.0
#define CALIB_RUNS 7
#define CALIB_ITERS 1000000

static volatile uint64_t g_sink;

static void spin_k9(pmk_ctx *c, uint64_t ns) {
  uint64_t end = pal_now_raw_ns() + ns, x = 0;
  while (pal_now_raw_ns() < end) x = c->k->k9_run(4096, x);
  g_sink = x;
}

/*
 * Measures c, the cycles per K9 iteration, with the hardware cycle counter.
 * Without counters c is assumed to be one cycle per add, and the implied
 * clock is recorded so the assumption can be checked against the known
 * frequency.
 */
static void calibrate(pmk_ctx *c, int type, int cpu) {
  pmk_calib *cal = &c->calib[type];
  double cyc[CALIB_RUNS], ns[CALIB_RUNS];
  spin_k9(c, 300000000ull);
  int fd = pal_cycles_open();
  for (int i = 0; i < CALIB_RUNS; i++) {
    uint64_t c0 = pal_cycles_read(fd), t0 = pal_now_raw_ns();
    g_sink = c->k->k9_run(CALIB_ITERS, g_sink);
    uint64_t t1 = pal_now_raw_ns(), c1 = pal_cycles_read(fd);
    cyc[i] = (double)(c1 - c0) / CALIB_ITERS;
    ns[i] = (double)(t1 - t0) / CALIB_ITERS;
  }
  pal_cycles_close(fd);
  cal->valid = 1;
  cal->cpu = cpu;
  cal->ns_per_iter = pmk_median(ns, CALIB_RUNS);
  if (fd >= 0) {
    cal->method = "perf";
    cal->cycles_per_iter = pmk_median(cyc, CALIB_RUNS);
    double lo = cyc[0], hi = cyc[0];
    for (int i = 1; i < CALIB_RUNS; i++) {
      lo = fmin(lo, cyc[i]);
      hi = fmax(hi, cyc[i]);
    }
    cal->cycles_spread_rel = (hi - lo) / cal->cycles_per_iter;
  } else {
    cal->method = "assumed";
    cal->cycles_per_iter = c->k->k9_adds_per_iter;
    cal->cycles_spread_rel = NAN;
  }
  cal->implied_mhz = cal->cycles_per_iter / cal->ns_per_iter * 1000.0;
}

static uint64_t iters_for(const pmk_ctx *c, int type, uint64_t w_cycles) {
  uint64_t it = (uint64_t)llround((double)w_cycles / c->calib[type].cycles_per_iter);
  return it ? it : 1;
}

static int measure_warm(pmk_ctx *c, pmk_result *r) {
  uint64_t iters = iters_for(c, r->type, r->w_cycles);
  uint64_t x = g_sink;
  spin_k9(c, (uint64_t)fmax(50e6, 3 * r->w_ms * 1e6));
  while (!ctx_precise_enough(c, r, c->cfg.max_reps)) {
    int irc = ctx_interrupt(c);
    if (irc) return irc;
    int cpu = pal_current_cpu();
    uint64_t t0 = pal_now_raw_ns();
    x = c->k->k9_run(iters, x);
    uint64_t t1 = pal_now_raw_ns();
    if (result_push(r, (double)(t1 - t0), NAN, cpu)) return PMK_ERR_NOMEM;
  }
  g_sink = x;
  return PMK_OK;
}

static int measure_cold(pmk_ctx *c, pmk_result *r) {
  uint64_t iters = iters_for(c, r->type, r->w_cycles);
  uint64_t x = g_sink;
  while (!ctx_precise_enough(c, r, c->cfg.cold_max_reps)) {
    int irc = ctx_interrupt(c);
    if (irc) return irc;
    double gap_ms = GAP_MIN_MS + (GAP_MAX_MS - GAP_MIN_MS) * pmk_rng_unit(&c->rng);
    uint64_t deadline = pal_now_ns() + (uint64_t)(gap_ms * 1e6);
    pal_sleep_until_ns(deadline);
    /* Wake lateness is relative to the sleep clock; the work itself is timed on the raw clock. */
    uint64_t woke = pal_now_ns(), t0 = pal_now_raw_ns();
    int cpu = pal_current_cpu();
    x = c->k->k9_run(iters, x);
    uint64_t t1 = pal_now_raw_ns();
    double wake = (double)(woke - deadline);
    if (result_push(r, wake + (double)(t1 - t0), wake, cpu)) return PMK_ERR_NOMEM;
  }
  g_sink = x;
  return PMK_OK;
}

static int run_type(pmk_ctx *c, int type) {
  int cpu = ctx_cpu_for_type(c, type);
  if (ctx_can_place(c) && pal_pin_self(cpu))
    ctx_emit(c, PMK_EV_WARNING, "cold_burst", "K9", 0, 0, "could not pin to CPU %d", cpu);
  const pmk_cpu *info = NULL;
  for (int i = 0; i < c->m.ncpu; i++)
    if (c->m.cpus[i].id == cpu) info = &c->m.cpus[i];
  double khz = info && info->max_khz > 0 ? info->max_khz : 1e6;

  calibrate(c, type, cpu);
  const pmk_calib *cal = &c->calib[type];
  ctx_emit(c, PMK_EV_INFO, "cold_burst", "K9", 0, 0, "core %s (CPU %d): %.2f cycles/iteration (%s), %.0f MHz",
           c->m.type_names[type], cpu, cal->cycles_per_iter, cal->method, cal->implied_mhz);

  int worder[NW];
  for (int i = 0; i < NW; i++) worder[i] = i;
  ctx_shuffle(&c->rng, worder, NW);
  int rc = PMK_OK;
  for (int wi = 0; wi < NW && rc == PMK_OK; wi++) {
    double w_ms = W_MS[worder[wi]];
    ctx_emit(c, PMK_EV_PHASE, "cold_burst", "K9", (uint32_t)wi + 1, NW, "core %s, task of %g ms",
             c->m.type_names[type], w_ms);
    for (int warm = 1; warm >= 0 && rc == PMK_OK; warm--) {
      pmk_result *r = ctx_new_result(c, "K9", "cold_burst", "ns");
      if (!r) {
        rc = PMK_ERR_NOMEM;
        break;
      }
      r->type = type;
      r->cpu = cpu;
      r->warm = warm;
      r->w_ms = w_ms;
      r->w_cycles = (uint64_t)llround(w_ms * khz);
      r->iters = iters_for(c, type, r->w_cycles);
      rc = warm ? measure_warm(c, r) : measure_cold(c, r);
    }
  }
  return rc;
}

/*
 * Realistic cold bursts: one burst-size job of K4 or K6, warm (back to back)
 * and cold (after a random idle gap, timed from the deadline), reported
 * separately from the calibrated K9 curve.
 */
static int realistic(pmk_ctx *c, const char *id, int type) {
  const pmk_tk *tk = pmk_find_tk(c->k, id, NULL);
  if (!tk) {
    ctx_unavailable(c, id, NULL, "cold_burst", "not built");
    return PMK_OK;
  }
  int cpu = ctx_cpu_for_type(c, type);
  if (ctx_can_place(c)) pal_pin_self(cpu);
  void *inst = wl_create(c, tk, PMK_SIZE_BURST, "cold_burst");
  if (!inst) return PMK_OK;
  void *scratch = tk->scratch_new ? tk->scratch_new(inst) : NULL;
  int rc = tk->scratch_new && !scratch ? PMK_ERR_NOMEM : PMK_OK;
  uint64_t ref = rc == PMK_OK ? wl_job(tk, inst, scratch) : 0;
  int order[2] = {0, 1};
  ctx_shuffle(&c->rng, order, 2);
  for (int oi = 0; oi < 2 && rc == PMK_OK; oi++) {
    int warm = order[oi];
    ctx_emit(c, PMK_EV_PHASE, "cold_burst", id, (uint32_t)oi + 1, 2, "%s, %s, core %s (CPU %d)", tk->name,
             warm ? "warm" : "cold", c->m.type_names[type], cpu);
    pmk_result *r = ctx_new_result(c, id, "cold_burst", "ns");
    if (!r) {
      rc = PMK_ERR_NOMEM;
      break;
    }
    r->k = c->k;
    r->type = type;
    r->cpu = cpu;
    r->warm = warm;
    r->size = "burst";
    r->input_hash = tk->input_hash(inst);
    r->checksum = ref;
    r->checksum_ok = 1;
    if (warm) wl_job(tk, inst, scratch);
    while (!ctx_precise_enough(c, r, warm ? c->cfg.max_reps : c->cfg.cold_max_reps)) {
      if ((rc = ctx_interrupt(c))) break;
      uint64_t wake = 0, t0, h;
      if (warm) {
        t0 = pal_now_raw_ns();
      } else {
        double gap_ms = GAP_MIN_MS + (GAP_MAX_MS - GAP_MIN_MS) * pmk_rng_unit(&c->rng);
        uint64_t deadline = pal_now_ns() + (uint64_t)(gap_ms * 1e6);
        pal_sleep_until_ns(deadline);
        wake = pal_now_ns() - deadline;
        t0 = pal_now_raw_ns();
      }
      int obs = pal_current_cpu();
      h = wl_job(tk, inst, scratch);
      uint64_t t1 = pal_now_raw_ns();
      if (h != ref) r->checksum_ok = 0;
      if (result_push(r, (double)(wake + (t1 - t0)), warm ? NAN : (double)wake, obs)) rc = PMK_ERR_NOMEM;
    }
  }
  if (scratch && tk->scratch_free) tk->scratch_free(scratch);
  tk->destroy(inst);
  return rc;
}

int mode_cold_burst_steps(const pmk_ctx *c) {
  int ntypes = c->cfg.cpu >= 0 ? 1 : c->m.ntypes, n = ctx_kernel_selected(c, "K9") ? ntypes * NW : 0;
  static const char *const REALISTIC[] = {"K4", "K6"};
  for (int k = 0; k < 2; k++)
    if (ctx_kernel_selected(c, REALISTIC[k]) && pmk_find_tk(c->k, REALISTIC[k], NULL)) n += ntypes * 2;
  return n;
}

int mode_cold_burst(pmk_ctx *c) {
  pal_set_timer_slack_min();
  int order[PMK_MAX_TYPES] = {0, 1, 2, 3};
  ctx_shuffle(&c->rng, order, c->m.ntypes);
  int rc = PMK_OK;
  int ntypes = c->cfg.cpu >= 0 ? 1 : c->m.ntypes; /* an explicit CPU means one core type */
  for (int i = 0; i < ntypes && rc == PMK_OK; i++)
    if (ctx_kernel_selected(c, "K9")) rc = run_type(c, c->cfg.cpu >= 0 ? ctx_type_of_cpu(c, c->cfg.cpu) : order[i]);

  static const char *const REALISTIC[] = {"K4", "K6"};
  int jobs[2 * PMK_MAX_TYPES], nj = 0;
  for (int t = 0; t < ntypes; t++)
    for (int k = 0; k < 2; k++)
      if (ctx_kernel_selected(c, REALISTIC[k])) jobs[nj++] = t * 2 + k;
  ctx_shuffle(&c->rng, jobs, nj);
  for (int j = 0; j < nj && rc == PMK_OK; j++) {
    int type = c->cfg.cpu >= 0 ? ctx_type_of_cpu(c, c->cfg.cpu) : jobs[j] / 2;
    rc = realistic(c, REALISTIC[jobs[j] % 2], type);
  }
  return rc;
}
