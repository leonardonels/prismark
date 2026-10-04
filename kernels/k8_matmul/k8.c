/*
 * K8 — matrix multiply, FP32 and INT8 (int32 accumulation). Portable
 * reference code, blocked for cache; used for ISA uplift, where the same
 * source is compiled at the baseline and the max-level ISA.
 *
 * Each C[i][j] accumulates over k in ascending order whatever the blocking,
 * and builds have no FP contraction, so FP32 results are bit-identical on
 * every host and tier. Each task computes one strip of rows of C.
 *
 * Copyright 2026 The Prismark Authors. Apache-2.0.
 */
#include <stdio.h>
#include <stdlib.h>

#include "kernels.h"
#include "rng.h"

#define N 512
#define STRIP 32 /* rows of C per task */
#define KB 128   /* k block */
#define JB 256   /* j block */

typedef struct k8 {
  int is_int8;
  int n;
  void *a, *b; /* n x n, row major */
} k8;

static void *k8_create_common(const pmk_tk_args *a, int is_int8) {
  k8 *s = calloc(1, sizeof *s);
  int n = a->size == PMK_SIZE_BURST ? 256 : N;
  size_t esz = is_int8 ? 1 : sizeof(float);
  if (!s) goto oom;
  s->is_int8 = is_int8;
  s->n = n;
  s->a = malloc((size_t)n * (size_t)n * esz);
  s->b = malloc((size_t)n * (size_t)n * esz);
  if (!s->a || !s->b) goto oom;
  pmk_rng r;
  pmk_rng_seed(&r, 0x6b38);
  for (size_t i = 0; i < (size_t)n * (size_t)n; i++) {
    int32_t va = (int32_t)pmk_rng_below(&r, 255) - 127, vb = (int32_t)pmk_rng_below(&r, 255) - 127;
    if (is_int8) {
      ((int8_t *)s->a)[i] = (int8_t)va;
      ((int8_t *)s->b)[i] = (int8_t)vb;
    } else { /* exactly representable values in [-1, 1) */
      ((float *)s->a)[i] = (float)va / 128.0f;
      ((float *)s->b)[i] = (float)vb / 128.0f;
    }
  }
  return s;
oom:
  if (s) {
    free(s->a);
    free(s->b);
    free(s);
  }
  snprintf(a->err, a->errlen, "out of memory");
  return NULL;
}

static void *k8_create_fp32(const pmk_tk_args *a) { return k8_create_common(a, 0); }
static void *k8_create_int8(const pmk_tk_args *a) { return k8_create_common(a, 1); }

static void k8_destroy(void *p) {
  k8 *s = p;
  if (!s) return;
  free(s->a);
  free(s->b);
  free(s);
}

static size_t k8_ntasks(const void *p) { return (size_t)((const k8 *)p)->n / STRIP; }
static uint64_t k8_task_work(const void *p, size_t i) {
  (void)i;
  uint64_t n = (uint64_t)((const k8 *)p)->n;
  return 2 * STRIP * n * n; /* one multiply and one add per (i, j, k) */
}

static void *k8_scratch_new(const void *p) {
  const k8 *s = p;
  return malloc((size_t)STRIP * (size_t)s->n * 4); /* one strip of C, float or int32 */
}

/* The innermost loops are plain axpy over j with restrict pointers, so any tier can vectorise them. */
static void axpy_fp32(float *restrict c, const float *restrict b, float a, int n) {
  for (int j = 0; j < n; j++) c[j] += a * b[j];
}

static void axpy_int8(int32_t *restrict c, const int8_t *restrict b, int32_t a, int n) {
  for (int j = 0; j < n; j++) c[j] += a * (int32_t)b[j];
}

static void strip_fp32(const k8 *s, float *c, int i0) {
  const int n = s->n;
  const float *a = s->a, *b = s->b;
  memset(c, 0, (size_t)STRIP * (size_t)n * sizeof *c);
  for (int kk = 0; kk < n; kk += KB)
    for (int jj = 0; jj < n; jj += JB) {
      int jn = n - jj < JB ? n - jj : JB;
      for (int i = 0; i < STRIP; i++) {
        float *ci = &c[(size_t)i * (size_t)n + (size_t)jj];
        const float *ai = &a[(size_t)(i0 + i) * (size_t)n];
        for (int k = kk; k < kk + KB; k++) axpy_fp32(ci, &b[(size_t)k * (size_t)n + (size_t)jj], ai[k], jn);
      }
    }
}

static void strip_int8(const k8 *s, int32_t *c, int i0) {
  const int n = s->n;
  const int8_t *a = s->a, *b = s->b;
  memset(c, 0, (size_t)STRIP * (size_t)n * sizeof *c);
  for (int kk = 0; kk < n; kk += KB)
    for (int jj = 0; jj < n; jj += JB) {
      int jn = n - jj < JB ? n - jj : JB;
      for (int i = 0; i < STRIP; i++) {
        int32_t *ci = &c[(size_t)i * (size_t)n + (size_t)jj];
        const int8_t *ai = &a[(size_t)(i0 + i) * (size_t)n];
        for (int k = kk; k < kk + KB; k++) axpy_int8(ci, &b[(size_t)k * (size_t)n + (size_t)jj], ai[k], jn);
      }
    }
}

static uint64_t k8_task(const void *p, void *sp, size_t t) {
  const k8 *s = p;
  int i0 = (int)t * STRIP;
  if (s->is_int8) strip_int8(s, sp, i0);
  else strip_fp32(s, sp, i0);
  return pmk_hash_bytes(sp, (size_t)STRIP * (size_t)s->n * 4, t);
}

static uint64_t k8_input_hash(const void *p) {
  const k8 *s = p;
  size_t bytes = (size_t)s->n * (size_t)s->n * (s->is_int8 ? 1 : sizeof(float));
  return pmk_hash_bytes(s->b, bytes, pmk_hash_bytes(s->a, bytes, 8));
}

const pmk_tk PMK_SYM(pmk_k8_fp32_tk) = {
    .id = "K8",
    .variant = "fp32",
    .name = "matrix multiply FP32",
    .unit = "GFLOP/s",
    .unit_div = 1e9,
    .create = k8_create_fp32,
    .destroy = k8_destroy,
    .ntasks = k8_ntasks,
    .task_work = k8_task_work,
    .scratch_new = k8_scratch_new,
    .scratch_free = free,
    .task = k8_task,
    .input_hash = k8_input_hash,
};

const pmk_tk PMK_SYM(pmk_k8_int8_tk) = {
    .id = "K8",
    .variant = "int8",
    .name = "matrix multiply INT8",
    .unit = "GOP/s",
    .unit_div = 1e9,
    .create = k8_create_int8,
    .destroy = k8_destroy,
    .ntasks = k8_ntasks,
    .task_work = k8_task_work,
    .scratch_new = k8_scratch_new,
    .scratch_free = free,
    .task = k8_task,
    .input_hash = k8_input_hash,
};
