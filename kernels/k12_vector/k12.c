/*
 * K12 — vector maths: a double-precision matrix multiply C = A × B, the dense floating-point load of scientific
 * and engineering software (and of Linpack). A task computes one 64 × 64 tile of C.
 *
 * It runs at the widest vectors the CPU offers (AVX-512, AVX2, or the baseline's SSE / NEON), chosen once per
 * instance. Every element of C is one chain of fused multiply-adds over k, in the same order whatever the
 * vector width and the register blocking, so every width gives bit-identical results and the same checksum:
 * the width changes only how many chains run side by side.
 *
 * Copyright 2026 The Prismark Authors. Apache-2.0.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kernels.h"
#include "rng.h"

#define K12_TILE 64 /* a task: a TILE x TILE block of C */
#define K12_ROWS 4  /* rows of C kept in registers at once */
#define K12_COLS 2  /* vectors of C per row kept in registers at once */

typedef void (*k12_block_fn)(const double *a, size_t lda, const double *bp, int n, double *c);

/*
 * One tile: c (TILE x TILE, row-major) = a (TILE rows of A, row stride lda) x bp (the tile's columns of B,
 * packed: n rows of TILE). k runs in chunks of KC so the chunk of B and of A stays in the L2 cache; for each block
 * of ROWS x (COLS x W) elements of c the accumulators stay in registers over a chunk, and carry over to the next
 * through c, so every element is still one fma chain over k = 0 ... n - 1.
 */
#define K12_KC 128
#define K12_BLOCK(NAME, W, ATTR)                                                                           \
  typedef double NAME##_v __attribute__((vector_size((W) * 8), aligned(8)));                             \
  ATTR static void NAME(const double *a, size_t lda, const double *bp, int n, double *c) {               \
    for (int k0 = 0; k0 < n; k0 += K12_KC) {                                                              \
      int k1 = k0 + K12_KC < n ? k0 + K12_KC : n;                                                         \
      for (int i = 0; i < K12_TILE; i += K12_ROWS)                                                        \
        for (int j = 0; j < K12_TILE; j += K12_COLS * (W)) {                                              \
          NAME##_v acc[K12_ROWS][K12_COLS];                                                               \
          if (k0 == 0) memset(acc, 0, sizeof acc);                                                        \
          else                                                                                            \
            for (int r = 0; r < K12_ROWS; r++)                                                            \
              for (int v = 0; v < K12_COLS; v++) memcpy(&acc[r][v], c + (i + r) * K12_TILE + j + v * (W), (W) * 8); \
          for (int k = k0; k < k1; k++) {                                                                 \
            NAME##_v b[K12_COLS];                                                                         \
            for (int v = 0; v < K12_COLS; v++) memcpy(&b[v], bp + (size_t)k * K12_TILE + j + v * (W), (W) * 8); \
            for (int r = 0; r < K12_ROWS; r++) {                                                          \
              NAME##_v x = (NAME##_v){0} + a[(size_t)(i + r) * lda + (size_t)k];                          \
              for (int v = 0; v < K12_COLS; v++) acc[r][v] = __builtin_elementwise_fma(x, b[v], acc[r][v]); \
            }                                                                                             \
          }                                                                                               \
          for (int r = 0; r < K12_ROWS; r++)                                                              \
            for (int v = 0; v < K12_COLS; v++) memcpy(c + (i + r) * K12_TILE + j + v * (W), &acc[r][v], (W) * 8); \
        }                                                                                                 \
    }                                                                                                     \
  }

K12_BLOCK(block_base, 2, ) /* SSE (x86-64-v2 has no FMA: each fma is a library call, slow but exact) or NEON */
#if defined(__x86_64__) || defined(_M_X64)
K12_BLOCK(block_avx2, 4, __attribute__((target("avx2,fma"))))
K12_BLOCK(block_avx512, 8, __attribute__((target("avx512f,fma"), min_vector_width(512))))
#endif

typedef struct k12 {
  int n;
  double *a, *b; /* n x n, row-major */
  k12_block_fn block;
  const char *isa;
} k12;

/* The widest the CPU (and the OS) supports; PRISMARK_VECTOR_ISA=base|avx2|avx512 asks for a narrower one. */
static void pick(k12 *s) {
  const char *want = getenv("PRISMARK_VECTOR_ISA");
  s->block = block_base;
#if defined(__aarch64__) || defined(_M_ARM64)
  s->isa = "NEON";
#else
  s->isa = "SSE4.2";
  int avx2 = __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma"), avx512 = avx2 && __builtin_cpu_supports("avx512f");
  if (want && !strcmp(want, "base")) return;
  if (avx512 && !(want && !strcmp(want, "avx2"))) s->block = block_avx512, s->isa = "AVX-512";
  else if (avx2) s->block = block_avx2, s->isa = "AVX2";
#endif
  (void)want;
}

static void k12_destroy(void *p) {
  k12 *s = p;
  if (!s) return;
  free(s->a);
  free(s->b);
  free(s);
}

static void *k12_create(const pmk_tk_args *a) {
  k12 *s = calloc(1, sizeof *s);
  int n = a->size == PMK_SIZE_FULL ? 2048 : 256;
  if (s) {
    s->n = n;
    s->a = malloc((size_t)n * n * sizeof *s->a);
    s->b = malloc((size_t)n * n * sizeof *s->b);
  }
  if (!s || !s->a || !s->b) {
    k12_destroy(s);
    snprintf(a->err, a->errlen, "out of memory");
    return NULL;
  }
  /* Values in [-1, 1) with 24-bit mantissas, from a fixed seed: identical on every host. */
  pmk_rng r;
  pmk_rng_seed(&r, 0x6b3132);
  for (size_t i = 0; i < (size_t)n * n; i++) s->a[i] = (double)((int64_t)(pmk_rng_next(&r) >> 40) - (1 << 23)) / (1 << 23);
  for (size_t i = 0; i < (size_t)n * n; i++) s->b[i] = (double)((int64_t)(pmk_rng_next(&r) >> 40) - (1 << 23)) / (1 << 23);
  pick(s);
  return s;
}

static size_t k12_ntasks(const void *p) {
  const k12 *s = p;
  return (size_t)(s->n / K12_TILE) * (size_t)(s->n / K12_TILE);
}

/* Floating-point operations: a multiply and an add per fma. */
static uint64_t k12_task_work(const void *p, size_t i) {
  (void)i;
  return 2ull * K12_TILE * K12_TILE * (uint64_t)((const k12 *)p)->n;
}

/* Per thread: the tile's packed columns of B (n x TILE) and the tile of C. */
static void *k12_scratch_new(const void *p) {
  const k12 *s = p;
  return malloc(((size_t)s->n * K12_TILE + K12_TILE * K12_TILE) * sizeof(double));
}

static uint64_t k12_task(const void *p, void *sp, size_t t) {
  const k12 *s = p;
  int tiles = s->n / K12_TILE;
  size_t ti = t / (size_t)tiles, tj = t % (size_t)tiles;
  double *bp = sp, *c = bp + (size_t)s->n * K12_TILE;
  for (int k = 0; k < s->n; k++) memcpy(bp + (size_t)k * K12_TILE, s->b + (size_t)k * s->n + tj * K12_TILE, K12_TILE * sizeof *bp);
  s->block(s->a + ti * K12_TILE * (size_t)s->n, (size_t)s->n, bp, s->n, c);
  return pmk_hash_bytes(c, K12_TILE * K12_TILE * sizeof *c, t);
}

static uint64_t k12_input_hash(const void *p) {
  const k12 *s = p;
  uint64_t h = pmk_hash_bytes(s->a, (size_t)s->n * s->n * sizeof *s->a, 12);
  return pmk_hash_bytes(s->b, (size_t)s->n * s->n * sizeof *s->b, h);
}

/* Job checksums of both inputs, the same at every vector width and on every host. */
static uint64_t k12_expected(const void *p) {
  uint64_t in = k12_input_hash(p);
  return in == 0x692cd15cb659f2ccull ? 0x70b3d88398f9bc8bull : in == 0x5d4a382bf54c1e29ull ? 0x0a182e17544fc450ull : 0;
}

static const char *k12_isa(const void *p) { return ((const k12 *)p)->isa; }

const pmk_tk PMK_SYM(pmk_k12_tk) = {
    .id = "K12",
    .name = "vector maths",
    .unit = "GFLOP/s",
    .unit_div = 1e9,
    .create = k12_create,
    .destroy = k12_destroy,
    .ntasks = k12_ntasks,
    .task_work = k12_task_work,
    .scratch_new = k12_scratch_new,
    .scratch_free = free,
    .task = k12_task,
    .input_hash = k12_input_hash,
    .expected = k12_expected,
    .isa = k12_isa,
};
