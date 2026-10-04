/* Deterministic PRNG (xoshiro256**, seeded by splitmix64).
 * Copyright 2026 The Prismark Authors. Apache-2.0. */
#ifndef PMK_RNG_H
#define PMK_RNG_H

#include <stdint.h>

typedef struct pmk_rng {
  uint64_t s[4];
} pmk_rng;

static inline uint64_t pmk_splitmix64(uint64_t *x) {
  uint64_t z = (*x += 0x9e3779b97f4a7c15ull);
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
  return z ^ (z >> 31);
}

static inline void pmk_rng_seed(pmk_rng *r, uint64_t seed) {
  for (int i = 0; i < 4; i++) r->s[i] = pmk_splitmix64(&seed);
}

static inline uint64_t pmk_rotl(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }

static inline uint64_t pmk_rng_next(pmk_rng *r) {
  uint64_t *s = r->s;
  uint64_t result = pmk_rotl(s[1] * 5, 7) * 9;
  uint64_t t = s[1] << 17;
  s[2] ^= s[0];
  s[3] ^= s[1];
  s[1] ^= s[2];
  s[0] ^= s[3];
  s[2] ^= t;
  s[3] = pmk_rotl(s[3], 45);
  return result;
}

/* Uniform in [0, 1). */
static inline double pmk_rng_unit(pmk_rng *r) { return (double)(pmk_rng_next(r) >> 11) * 0x1.0p-53; }

/* Uniform in [0, n). */
static inline uint64_t pmk_rng_below(pmk_rng *r, uint64_t n) {
  return (uint64_t)(((__uint128_t)pmk_rng_next(r) * n) >> 64);
}

#endif
