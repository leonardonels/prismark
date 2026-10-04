/*
 * K7 — pointer chase. Nodes are cache lines linked into one random cycle
 * (Sattolo's algorithm), so each step is a dependent load whose address the
 * prefetchers cannot predict.
 *
 * Copyright 2026 The Prismark Authors. Apache-2.0.
 */
#include "rng.h"
#include "kernels.h"

void PMK_SYM(pmk_k7_build)(pmk_k7_node *nodes, size_t n, uint64_t seed) {
  pmk_rng r;
  pmk_rng_seed(&r, seed);
  for (size_t i = 0; i < n; i++) nodes[i].next = i;
  /* Sattolo: a uniformly random single cycle through all n nodes. */
  for (size_t i = n - 1; i > 0; i--) {
    size_t j = (size_t)pmk_rng_below(&r, i);
    uint64_t t = nodes[i].next;
    nodes[i].next = nodes[j].next;
    nodes[j].next = t;
  }
}

uint64_t PMK_SYM(pmk_k7_chase)(const pmk_k7_node *nodes, uint64_t start, uint64_t steps) {
  uint64_t p = start;
  for (uint64_t i = 0; i < steps; i++) p = nodes[p].next;
  return p;
}
