/*
 * K11 — memory bandwidth and integrity, for the stress test. A buffer (2 GiB by default; for the stress test as
 * much of the free memory as is safe, pmk_tk_args.bytes; 64 MiB for burst inputs) is split into 8 MiB chunks, each
 * filled with a known pattern. A task reads its chunk, checks every word against the pattern it holds, and writes
 * the next pattern of four in turn:
 *
 *   0  an address pattern (a value per chunk, advancing per word), so a word read from the wrong place is caught
 *   1  its complement, so every bit is written both ways
 *   2  a walking one (one bit set, moving one place per word), for bits stuck at or coupled to their neighbours
 *   3  its complement
 *
 * Every job reads and writes the whole buffer, far larger than any cache, so the traffic goes to main memory.
 * A task's checksum depends only on its index and the number of words that were wrong, never on the pattern the
 * chunk held: every job of a correct machine returns the same checksum, and a memory error changes it, which the
 * engine counts.
 *
 * Copyright 2026 The Prismark Authors. Apache-2.0.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "kernels.h"

#define K11_SEED 0x6b31315f6d656dull
#define K11_STEP 0x9e3779b97f4a7c15ull /* the pattern advances by this per word */

typedef struct k11 {
  uint64_t *buf;
  size_t nchunks, words; /* words per chunk */
} k11;

static uint64_t chunk_base(size_t i) { return pmk_mix64(K11_SEED ^ ((uint64_t)i << 32)); }

static void k11_destroy(void *p) {
  k11 *s = p;
  if (!s) return;
  free(s->buf);
  free(s);
}

/* Pattern p's word j of chunk i (base = chunk_base(i)). */
static inline uint64_t pattern(int p, uint64_t base, size_t i, size_t j) {
  uint64_t walk = 1ull << ((j + i) & 63);
  return p == 0 ? base + j * K11_STEP : p == 1 ? ~(base + j * K11_STEP) : p == 2 ? walk : ~walk;
}

static void *k11_create(const pmk_tk_args *a) {
  k11 *s = calloc(1, sizeof *s);
  /* Full: 2 GiB, or the bytes asked for, in 8 MiB chunks (enough of them for every thread of a large machine).
     Burst: 64 MiB, for checks. */
  size_t chunk = (size_t)8 << 20;
  size_t bytes = a->size != PMK_SIZE_FULL ? (size_t)64 << 20 : a->bytes ? (size_t)(a->bytes / chunk) * chunk : (size_t)2 << 30;
  if (bytes < chunk) bytes = chunk;
  uint64_t *buf = s ? malloc(bytes) : NULL;
  if (!buf) {
    free(s);
    snprintf(a->err, a->errlen, "not enough memory for its %zu MiB buffer", bytes >> 20);
    return NULL;
  }
  s->buf = buf;
  s->nchunks = bytes / chunk;
  s->words = chunk / sizeof(uint64_t);
  for (size_t i = 0; i < s->nchunks; i++) {
    uint64_t *v = s->buf + i * s->words, base = chunk_base(i);
    for (size_t j = 0; j < s->words; j++) v[j] = pattern(0, base, i, j);
  }
  return s;
}

static size_t k11_ntasks(const void *p) { return ((const k11 *)p)->nchunks; }
/* Bytes read plus bytes written. */
static uint64_t k11_task_work(const void *p, size_t i) {
  (void)i;
  return 2 * ((const k11 *)p)->words * sizeof(uint64_t);
}

/* Checks words [0, n) of v against pattern p and writes pattern q in their place; returns the wrong ones. */
#define K11_PASS(P, Q)                                                                                      \
  for (size_t j = 0; j < n; j++) {                                                                        \
    bad += v[j] != pattern(P, base, i, j);                                                                \
    v[j] = pattern(Q, base, i, j);                                                                        \
  }

static uint64_t k11_task(const void *p, void *sp, size_t i) {
  (void)sp;
  const k11 *s = p;
  uint64_t *v = s->buf + i * s->words, base = chunk_base(i), bad = 0;
  size_t n = s->words;
  /* Which pattern the chunk holds, from its first word; a first word that is none of them makes every word of
     the chunk count as wrong against the address pattern. */
  int held = 0;
  while (held < 4 && v[0] != pattern(held, base, i, 0)) held++;
  switch (held) {
    case 1: K11_PASS(1, 2) break;
    case 2: K11_PASS(2, 3) break;
    case 3: K11_PASS(3, 0) break;
    default: K11_PASS(0, 1) break;
  }
  return pmk_mix64((uint64_t)i * K11_STEP ^ bad);
}

static uint64_t k11_input_hash(const void *p) {
  const k11 *s = p;
  uint64_t h[3] = {K11_SEED, s->nchunks, s->words};
  return pmk_hash_bytes(h, sizeof h, 11);
}

/* What every job returns when no word was wrong: it follows from the chunk count alone. */
static uint64_t k11_expected(const void *p) {
  const k11 *s = p;
  uint64_t acc = 0;
  for (size_t i = 0; i < s->nchunks; i++) acc = pmk_job_combine(acc, i, pmk_mix64((uint64_t)i * K11_STEP));
  return acc;
}

const pmk_tk PMK_SYM(pmk_k11_tk) = {
    .id = "K11",
    .name = "memory bandwidth",
    .unit = "GB/s",
    .unit_div = 1e9,
    .create = k11_create,
    .destroy = k11_destroy,
    .ntasks = k11_ntasks,
    .task_work = k11_task_work,
    .task = k11_task,
    .input_hash = k11_input_hash,
    .expected = k11_expected,
};
