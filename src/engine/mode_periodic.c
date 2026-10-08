/*
 * Periodic: how late can a deadline task wake up?
 *
 * K10 sleeps to absolute deadlines one period apart and records the lateness
 * L = t_actual - t_scheduled, first on an idle machine, then with K7 pointer
 * chases loading every other CPU.
 *
 * Copyright 2026 The Prismark Authors. Apache-2.0.
 */
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>

#include "ctx.h"

typedef struct loader {
  pal_thread *th;
  int cpu;
  uint64_t start;
  const pmk_ctx *c;
  const pmk_k7_node *nodes;
  atomic_int *stop;
} loader;

static volatile uint64_t g_sink;

static void *loader_main(void *arg) {
  loader *l = arg;
  pal_pin_self(l->cpu);
  uint64_t p = l->start;
  while (!atomic_load_explicit(l->stop, memory_order_relaxed)) p = l->c->k->k7_chase(l->nodes, p, 1u << 14);
  g_sink = p;
  return NULL;
}

static int run_condition(pmk_ctx *c, int cpu, int loaded, const pmk_k7_node *nodes, size_t nnodes) {
  double period_ms = c->cfg.periodic_period_ms;
  uint64_t period = (uint64_t)llround(period_ms * 1e6);
  size_t nsamples = (size_t)(c->cfg.periodic_seconds * 1e3 / period_ms);
  if (period == 0 || nsamples == 0) return PMK_ERR_INVALID;

  loader *ls = NULL;
  int nl = 0;
  atomic_int stop = 0;
  if (loaded) {
    ls = calloc((size_t)c->m.ncpu, sizeof *ls);
    if (!ls) return PMK_ERR_NOMEM;
    for (int i = 0; i < c->m.ncpu; i++) {
      if (c->m.cpus[i].id == cpu) continue;
      loader *l = &ls[nl];
      l->cpu = c->m.cpus[i].id;
      l->start = pmk_rng_below(&c->rng, nnodes);
      l->c = c;
      l->nodes = nodes;
      l->stop = &stop;
      if ((l->th = pal_thread_start(loader_main, l))) nl++;
    }
    pal_sleep_ns(1000000000ull); /* let the load reach steady state */
  }

  pmk_result *r = ctx_new_result(c, "K10", "periodic", "ns");
  int rc = r ? PMK_OK : PMK_ERR_NOMEM;
  if (r) {
    r->cpu = cpu;
    for (int i = 0; i < c->m.ncpu; i++)
      if (c->m.cpus[i].id == cpu) r->type = c->m.cpus[i].type;
    r->condition = loaded ? "loaded" : "idle";
    r->period_ms = period_ms;
    r->loaders = nl;
    uint64_t next = pal_now_ns() + period;
    for (size_t i = 0; i < nsamples; i++) {
      if ((i & 1023) == 0 && (rc = ctx_interrupt(c))) break;
      pal_sleep_until_ns(next);
      uint64_t now = pal_now_ns();
      if (result_push(r, (double)(now - next), NAN, pal_current_cpu())) {
        rc = PMK_ERR_NOMEM;
        break;
      }
      next += period;
      while (next <= now) { /* a whole period was missed: keep the absolute grid */
        next += period;
        r->overruns++;
      }
    }
  }

  atomic_store(&stop, 1);
  for (int i = 0; i < nl; i++) pal_thread_join(ls[i].th);
  free(ls);
  return rc;
}

int mode_periodic_steps(const pmk_ctx *c) {
  if (!ctx_kernel_selected(c, "K10")) return 0;
  return c->m.ncpu > 1 ? 2 : 1;
}

/* Each condition runs for cfg.periodic_seconds; building the memory ring takes about a second. */
double mode_periodic_est(const pmk_ctx *c) {
  int steps = mode_periodic_steps(c);
  return steps ? steps * c->cfg.periodic_seconds + 1 : 0;
}

int mode_periodic(pmk_ctx *c) {
  if (!ctx_kernel_selected(c, "K10")) return PMK_OK; /* --kernels without K10 */
  int cpu = ctx_cpu_for_type(c, 0);
  if (ctx_can_place(c) && pal_pin_self(cpu))
    ctx_emit(c, PMK_EV_WARNING, "periodic", "K10", 0, 0, "could not pin to CPU %d", cpu);
  pal_set_timer_slack_min();

  /* K7 working set: 4x the last-level cache, between 32 MiB and 256 MiB, shared read-only. */
  uint64_t ws = c->m.llc_bytes * 4;
  if (ws < (32ull << 20)) ws = 32ull << 20;
  if (ws > (256ull << 20)) ws = 256ull << 20;
  size_t nnodes = (size_t)(ws / sizeof(pmk_k7_node));
  pmk_k7_node *nodes = NULL;
  int has_load = c->m.ncpu > 1;
  if (has_load) {
    nodes = pal_aligned_alloc(64, nnodes * sizeof *nodes);
    if (!nodes) return PMK_ERR_NOMEM;
    c->k->k7_build(nodes, nnodes, pmk_rng_next(&c->rng));
  }

  int order[2] = {0, 1};
  int ncond = has_load ? 2 : 1;
  ctx_shuffle(&c->rng, order, ncond);
  int rc = PMK_OK;
  for (int i = 0; i < ncond && rc == PMK_OK; i++) {
    int loaded = order[i];
    ctx_emit(c, PMK_EV_PHASE, "periodic", "K10", (uint32_t)i + 1, (uint32_t)ncond,
             "%s, period %g ms, %g s on CPU %d", loaded ? "loaded" : "idle", c->cfg.periodic_period_ms,
             c->cfg.periodic_seconds, cpu);
    rc = run_condition(c, cpu, loaded, nodes, nnodes);
  }
  pal_aligned_free(nodes);
  return rc;
}
