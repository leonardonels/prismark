/*
 * Kernel library interface. Every kernel is compiled once per ISA tier into
 * that tier's library; the core reaches a tier only through its pmk_kernels
 * table, so two tiers can never mix inside one measurement.
 *
 * The baseline tier is linked statically. Max-level tiers are loadable
 * modules built with hidden visibility, each exporting only
 * prismark_kernels_entry(), so their copies of third-party code (zstd, Lua,
 * stb) cannot collide with the baseline copies.
 *
 * Copyright 2026 The Prismark Authors. Apache-2.0.
 */
#ifndef PMK_KERNELS_H
#define PMK_KERNELS_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Inside a tier library, PMK_TIER is defined (e.g. baseline) and PMK_SYM(f) gives f_baseline. */
#define PMK_SYM3(a, b) a##_##b
#define PMK_SYM2(a, b) PMK_SYM3(a, b)
#define PMK_SYM(name) PMK_SYM2(name, PMK_TIER)

/* Bumped whenever pmk_kernels or pmk_tk changes layout; modules with another value are refused. */
#define PMK_KERNELS_ABI 2

/* K7: one node per cache line, linked into a single random cycle. */
typedef struct pmk_k7_node {
  uint64_t next;
  uint64_t pad[7];
} pmk_k7_node;

/* Input sets. Burst inputs keep one job within 10-500 ms; full inputs are for sustained modes. */
/* K1/K1x quick input: every PMK_K1_QUICK_STRIDE-th unit in largest-first order. */
#define PMK_K1_QUICK_STRIDE 8
/* K1x full build: every PMK_K1X_FULL_STRIDE-th unit (the quick units; full runs repeat the build three times):
   about 2 minutes per build on a 4-core laptop, instead of half an hour for every unit. */
#define PMK_K1X_FULL_STRIDE 8
enum { PMK_SIZE_BURST = 0, PMK_SIZE_FULL = 1 };

typedef struct pmk_tk_args {
  int size;             /* PMK_SIZE_* */
  const char *data_dir; /* K1: prepared snapshot directory; NULL if none */
  char *err;            /* on failure, create() writes the reason here */
  size_t errlen;
} pmk_tk_args;

/*
 * A throughput kernel. One job is ntasks independent tasks; the job is the
 * unit of ST burst and MC instances, and its tasks are what threads share in
 * MC threaded. Inputs are generated deterministically from fixed seeds, so
 * every host performs identical work; input_hash records that, and each task
 * returns a checksum of its output so identical results can be verified
 * across ISAs and tiers.
 */
typedef struct pmk_tk {
  const char *id;      /* "K2" */
  const char *variant; /* "fp32", "int8", or NULL */
  const char *name;
  const char *unit;    /* throughput unit, e.g. "Msamples/s" */
  double unit_div;     /* throughput = work / seconds / unit_div */
  void *(*create)(const pmk_tk_args *a); /* NULL on failure, reason in a->err */
  void (*destroy)(void *inst);
  size_t (*ntasks)(const void *inst);
  uint64_t (*task_work)(const void *inst, size_t i);
  void *(*scratch_new)(const void *inst); /* optional per-thread state; may be NULL */
  void (*scratch_free)(void *s);
  uint64_t (*task)(const void *inst, void *scratch, size_t i);
  uint64_t (*input_hash)(const void *inst);
} pmk_tk;

typedef struct pmk_kernels {
  uint32_t abi;      /* PMK_KERNELS_ABI */
  const char *tier;  /* "baseline" or "max" */
  const char *level; /* ISA level of the tier, e.g. "x86-64-v3" */
  const char *march; /* compiler ISA flag the tier was built with */

  /* K9: calibrated L1 loop, a dependent chain of k9_adds_per_iter integer adds per iteration. */
  unsigned k9_adds_per_iter;
  uint64_t (*k9_run)(uint64_t iters, uint64_t x);

  /* K7: pointer chase over n nodes (Sattolo cycle, so every node is visited). */
  void (*k7_build)(pmk_k7_node *nodes, size_t n, uint64_t seed);
  uint64_t (*k7_chase)(const pmk_k7_node *nodes, uint64_t start, uint64_t steps);

  /* Throughput kernels (K1-K6, K8). */
  const pmk_tk *const *tk;
  size_t ntk;
} pmk_kernels;

extern const pmk_kernels pmk_kernels_baseline;

/* Entry point exported by a max-level tier module. */
typedef const pmk_kernels *(*pmk_kernels_entry_fn)(void);
#define PMK_KERNELS_ENTRY "prismark_kernels_entry"

/* Looks up a throughput kernel by id and variant (variant NULL matches a kernel without one). */
static inline const pmk_tk *pmk_find_tk(const pmk_kernels *k, const char *id, const char *variant) {
  for (size_t i = 0; i < k->ntk; i++) {
    const pmk_tk *t = k->tk[i];
    if (strcmp(t->id, id)) continue;
    if (!variant && !t->variant) return t;
    if (variant && t->variant && !strcmp(variant, t->variant)) return t;
  }
  return NULL;
}

/* ---------- hashing shared by kernels and the engine ---------- */

static inline uint64_t pmk_mix64(uint64_t z) {
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
  return z ^ (z >> 31);
}

/* 64-bit hash of a byte string. Not cryptographic; identical on every little-endian host. */
static inline uint64_t pmk_hash_bytes(const void *p, size_t n, uint64_t seed) {
  const unsigned char *b = (const unsigned char *)p;
  uint64_t h = pmk_mix64(seed ^ (uint64_t)n);
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    uint64_t v;
    memcpy(&v, b + i, 8);
    h = pmk_mix64(h ^ v) + 0x9e3779b97f4a7c15ull;
  }
  uint64_t tail = 0;
  for (size_t k = 0; i < n; i++, k++) tail |= (uint64_t)b[i] << (8 * k);
  return pmk_mix64(h ^ tail);
}

/* Order-independent combination of task checksums into a job checksum. */
static inline uint64_t pmk_job_combine(uint64_t acc, size_t task, uint64_t h) {
  return acc + pmk_mix64(h ^ ((uint64_t)task * 0x9e3779b97f4a7c15ull));
}

#endif
