/*
 * K11 — memory bandwidth and integrity, for the stress test. A fixed buffer (2 GiB; 64 MiB for burst inputs)
 * is split into chunks, each filled with a known pattern. A task reads its chunk, checks every word against the
 * pattern, and writes the complement back, so the next job checks the complement: every job reads and writes
 * the whole buffer, far larger than any cache, so the traffic goes to main memory.
 *
 * A task's checksum depends only on its index and the number of words that were wrong, never on which of the two
 * patterns the chunk held: every job of a correct machine returns the same checksum, and a memory error changes
 * it, which the engine counts.
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

static void *k11_create(const pmk_tk_args *a) {
  k11 *s = calloc(1, sizeof *s);
  /* Full: 2 GiB in 8 MiB chunks, enough of them for every thread of a large machine. Burst: 64 MiB, for checks. */
  size_t bytes = a->size == PMK_SIZE_FULL ? (size_t)2 << 30 : (size_t)64 << 20, chunk = (size_t)8 << 20;
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
    uint64_t *v = s->buf + i * s->words, e = chunk_base(i);
    for (size_t j = 0; j < s->words; j++, e += K11_STEP) v[j] = e;
  }
  return s;
}

static size_t k11_ntasks(const void *p) { return ((const k11 *)p)->nchunks; }
/* Bytes read plus bytes written. */
static uint64_t k11_task_work(const void *p, size_t i) {
  (void)i;
  return 2 * ((const k11 *)p)->words * sizeof(uint64_t);
}

static uint64_t k11_task(const void *p, void *sp, size_t i) {
  (void)sp;
  const k11 *s = p;
  uint64_t *v = s->buf + i * s->words, base = chunk_base(i);
  /* Which pattern the chunk holds, from its first word; a wrong first word makes every word count as wrong. */
  uint64_t flip = v[0] == base ? 0 : ~0ull, e = base, bad = 0;
  for (size_t j = 0; j < s->words; j++, e += K11_STEP) {
    uint64_t want = e ^ flip;
    bad += v[j] != want;
    v[j] = ~want;
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
