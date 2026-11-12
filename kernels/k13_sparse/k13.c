/*
 * K13 — simulation solver: the residual r = b - A x of a sparse linear system, the step that the iterative
 * solvers of structural and fluid simulation (conjugate gradients, multigrid smoothers) repeat thousands of
 * times. A is the matrix of a 3D grid in which every point is coupled to its 26 neighbours, as a finite-element
 * mesh of bricks gives (the matrix of HPCG), stored row by row with explicit column numbers (CSR): 12 bytes per
 * entry, about 0.7 GiB for the full input.
 *
 * A job is one sweep over the whole matrix, far larger than any cache, so every entry comes from main memory:
 * unlike vector maths (K12), whose tiles stay in the cache, its speed is set by memory bandwidth, and it stops
 * scaling when the memory channels are full, not when the cores run out. A task is a block of rows; it also
 * sums the squares of its residuals, as a solver does for its stopping test.
 *
 * Plain scalar code at the build's ISA level: wider vectors do not help a loop that waits for memory. Matrix
 * entries are multiples of 1/8 and x, b have 24-bit mantissas, so every residual is exact, and the sum of
 * squares is added in row order: outputs are bit-identical on every host.
 *
 * Copyright 2026 The Prismark Authors. Apache-2.0.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kernels.h"
#include "rng.h"

#define K13_SEED 0x6b31335f737061ull
#define K13_ROWS 4096 /* a task: this many consecutive rows */

typedef struct k13 {
  int g;            /* grid points per side */
  size_t n, nnz;    /* rows, entries */
  uint32_t *rowptr; /* n + 1 */
  uint32_t *col;
  double *val, *x, *b;
  uint64_t hash;
} k13;

static void k13_destroy(void *p) {
  k13 *s = p;
  if (!s) return;
  free(s->rowptr);
  free(s->col);
  free(s->val);
  free(s->x);
  free(s->b);
  free(s);
}

/* The coupling of points lo < hi: -1 to -1.875 in steps of 1/8, the same seen from either side (A is symmetric). */
static double coupling(uint64_t lo, uint64_t hi) {
  return -(double)(8 + (pmk_mix64(K13_SEED ^ (lo << 32) ^ hi) & 7)) / 8;
}

static void *k13_create(const pmk_tk_args *a) {
  k13 *s = calloc(1, sizeof *s);
  int g = a->size == PMK_SIZE_FULL ? 128 : 64;
  if (s) {
    s->g = g;
    s->n = (size_t)g * g * g;
    size_t side = 3 * (size_t)g - 2; /* per axis: 3 neighbours inside, 2 at each end */
    s->nnz = side * side * side;
    s->rowptr = malloc((s->n + 1) * sizeof *s->rowptr);
    s->col = malloc(s->nnz * sizeof *s->col);
    s->val = malloc(s->nnz * sizeof *s->val);
    s->x = malloc(s->n * sizeof *s->x);
    s->b = malloc(s->n * sizeof *s->b);
  }
  if (!s || !s->rowptr || !s->col || !s->val || !s->x || !s->b) {
    k13_destroy(s);
    snprintf(a->err, a->errlen, "out of memory");
    return NULL;
  }
  size_t k = 0;
  for (int z = 0; z < g; z++)
    for (int y = 0; y < g; y++)
      for (int x = 0; x < g; x++) {
        size_t row = ((size_t)z * g + y) * g + x, diag = 0;
        double sum = 0;
        s->rowptr[row] = (uint32_t)k;
        for (int dz = -1; dz <= 1; dz++)
          for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
              int zz = z + dz, yy = y + dy, xx = x + dx;
              if (zz < 0 || zz >= g || yy < 0 || yy >= g || xx < 0 || xx >= g) continue;
              size_t c = ((size_t)zz * g + yy) * g + xx;
              s->col[k] = (uint32_t)c;
              if (c == row) diag = k;
              else sum -= s->val[k] = c < row ? coupling(c, row) : coupling(row, c);
              k++;
            }
        s->val[diag] = sum + 1; /* more than its row's couplings together: positive definite, as a stiffness matrix */
      }
  s->rowptr[s->n] = (uint32_t)k;
  /* Values in [-1, 1) with 24-bit mantissas, from a fixed seed: identical on every host. */
  pmk_rng r;
  pmk_rng_seed(&r, K13_SEED);
  for (size_t i = 0; i < s->n; i++) s->x[i] = (double)((int64_t)(pmk_rng_next(&r) >> 40) - (1 << 23)) / (1 << 23);
  for (size_t i = 0; i < s->n; i++) s->b[i] = (double)((int64_t)(pmk_rng_next(&r) >> 40) - (1 << 23)) / (1 << 23);
  /* Once, here: the matrix is too large to hash again for every series. */
  uint64_t h = pmk_hash_bytes(s->rowptr, (s->n + 1) * sizeof *s->rowptr, 13);
  h = pmk_hash_bytes(s->col, s->nnz * sizeof *s->col, h);
  h = pmk_hash_bytes(s->val, s->nnz * sizeof *s->val, h);
  h = pmk_hash_bytes(s->x, s->n * sizeof *s->x, h);
  s->hash = pmk_hash_bytes(s->b, s->n * sizeof *s->b, h);
  return s;
}

static size_t k13_ntasks(const void *p) { return ((const k13 *)p)->n / K13_ROWS; }

/* Floating-point operations: a multiply and a subtract per entry, a multiply and an add per row for the norm. */
static uint64_t k13_task_work(const void *p, size_t i) {
  const k13 *s = p;
  return 2ull * (s->rowptr[(i + 1) * K13_ROWS] - s->rowptr[i * K13_ROWS]) + 2ull * K13_ROWS;
}

/* Per thread: the residuals of the block. */
static void *k13_scratch_new(const void *p) {
  (void)p;
  return malloc(K13_ROWS * sizeof(double));
}

static uint64_t k13_task(const void *p, void *sp, size_t t) {
  const k13 *s = p;
  const uint32_t *rowptr = s->rowptr + t * K13_ROWS, *col = s->col;
  const double *val = s->val, *x = s->x, *b = s->b + t * K13_ROWS;
  double *r = sp, rr = 0;
  for (size_t i = 0; i < K13_ROWS; i++) {
    double v = b[i];
    for (uint32_t k = rowptr[i]; k < rowptr[i + 1]; k++) v -= val[k] * x[col[k]];
    r[i] = v;
    rr += v * v;
  }
  uint64_t bits;
  memcpy(&bits, &rr, sizeof bits);
  return pmk_hash_bytes(r, K13_ROWS * sizeof *r, t) ^ pmk_mix64(bits);
}

static uint64_t k13_input_hash(const void *p) { return ((const k13 *)p)->hash; }

/* Job checksums of both inputs, the same on every host. */
static uint64_t k13_expected(const void *p) {
  uint64_t in = k13_input_hash(p);
  return in == 0x4b7228510f8c7984ull ? 0x06ba25c67977b3d4ull : in == 0x0c4c9bed37337b99ull ? 0x0d673abb2afa9d10ull : 0;
}

const pmk_tk PMK_SYM(pmk_k13_tk) = {
    .id = "K13",
    .name = "simulation solver",
    .unit = "GFLOP/s",
    .unit_div = 1e9,
    .create = k13_create,
    .destroy = k13_destroy,
    .ntasks = k13_ntasks,
    .task_work = k13_task_work,
    .scratch_new = k13_scratch_new,
    .scratch_free = free,
    .task = k13_task,
    .input_hash = k13_input_hash,
    .expected = k13_expected,
};
