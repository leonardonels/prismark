/*
 * K4 — image decode and resize. Each task decodes one baseline JPEG with
 * stb_image (its SSE2/NEON paths disabled: STBI_NO_SIMD) and downscales it to
 * 60 % with a separable triangle filter written here.
 *
 * The JPEGs are produced at create() time: a procedural image is encoded with
 * stb_image_write at a fixed quality, so no third-party images are shipped.
 *
 * Copyright 2026 The Prismark Authors. Apache-2.0.
 */
#include <stdio.h>
#include <stdlib.h>

#include "kernels.h"
#include "rng.h"

#define STBI_NO_SIMD
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#if defined(__clang__) || defined(__GNUC__)
/* stb declares API functions this file does not use; that warning is issued at the end of the file. */
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#endif
#include "stb_image.h"
#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#define QUALITY 90
#define SCALE_NUM 3
#define SCALE_DEN 5
#define IMAGE_SEED 0x6b34

typedef struct jpeg {
  unsigned char *data;
  size_t len, cap;
  int w, h;
} jpeg;

typedef struct k4 {
  jpeg *img;
  size_t n;
} k4;

typedef struct scratch {
  float *row;     /* horizontally filtered rows */
  unsigned char *out;
  size_t row_cap, out_cap;
} scratch;

/* ---------- procedural source image ---------- */

static uint32_t hash2(int x, int y, uint32_t seed) {
  return (uint32_t)pmk_mix64(((uint64_t)(uint32_t)x << 32) ^ (uint64_t)(uint32_t)y ^ ((uint64_t)seed << 20));
}

/* Value noise on a grid of `cell` pixels, bilinear in integer arithmetic. */
static int value_noise(int x, int y, int cell, uint32_t seed) {
  int gx = x / cell, gy = y / cell, fx = x % cell, fy = y % cell;
  int a = (int)(hash2(gx, gy, seed) & 255), b = (int)(hash2(gx + 1, gy, seed) & 255);
  int c = (int)(hash2(gx, gy + 1, seed) & 255), d = (int)(hash2(gx + 1, gy + 1, seed) & 255);
  int top = a * (cell - fx) + b * fx, bot = c * (cell - fx) + d * fx;
  return (top * (cell - fy) + bot * fy) / (cell * cell);
}

static void render(unsigned char *px, int w, int h, uint32_t seed) {
  pmk_rng r;
  pmk_rng_seed(&r, seed);
  enum { NSHAPES = 40 };
  int sx[NSHAPES], sy[NSHAPES], sr[NSHAPES], sc[NSHAPES][3];
  for (int i = 0; i < NSHAPES; i++) {
    sx[i] = (int)pmk_rng_below(&r, (uint64_t)w);
    sy[i] = (int)pmk_rng_below(&r, (uint64_t)h);
    sr[i] = 10 + (int)pmk_rng_below(&r, (uint64_t)(h / 6));
    for (int k = 0; k < 3; k++) sc[i][k] = (int)pmk_rng_below(&r, 256);
  }
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      int n1 = value_noise(x, y, 64, seed), n2 = value_noise(x, y, 8, seed + 1);
      int c[3] = {(x * 255 / w + n1) / 2, (y * 255 / h + n1) / 2, (n1 + n2) / 2};
      for (int i = 0; i < NSHAPES; i++) {
        int dx = x - sx[i], dy = y - sy[i];
        if (dx * dx + dy * dy < sr[i] * sr[i]) {
          for (int k = 0; k < 3; k++) c[k] = (c[k] + 3 * sc[i][k]) / 4;
          break;
        }
      }
      if (((x / 3) ^ (y / 5)) % 17 == 0) c[0] = c[1] = c[2] = 250; /* fine detail, text-like */
      unsigned char *p = &px[((size_t)y * (size_t)w + (size_t)x) * 3];
      for (int k = 0; k < 3; k++) p[k] = (unsigned char)(c[k] < 0 ? 0 : c[k] > 255 ? 255 : c[k]);
    }
}

static void jpeg_sink(void *ctx, void *data, int size) {
  jpeg *j = ctx;
  if (j->len + (size_t)size > j->cap) {
    size_t cap = (j->cap ? j->cap * 2 : 1 << 20) + (size_t)size;
    unsigned char *p = realloc(j->data, cap);
    if (!p) return;
    j->data = p;
    j->cap = cap;
  }
  memcpy(j->data + j->len, data, (size_t)size);
  j->len += (size_t)size;
}

/* ---------- kernel ---------- */

static void k4_destroy(void *p) {
  k4 *s = p;
  if (!s) return;
  for (size_t i = 0; i < s->n; i++) free(s->img[i].data);
  free(s->img);
  free(s);
}

static void *k4_create(const pmk_tk_args *a) {
  k4 *s = calloc(1, sizeof *s);
  int w = a->size == PMK_SIZE_BURST ? 1280 : 1920, h = a->size == PMK_SIZE_BURST ? 720 : 1080;
  size_t n = a->size == PMK_SIZE_BURST ? 2 : 12;
  unsigned char *px = malloc((size_t)w * (size_t)h * 3);
  if (s) s->img = calloc(n, sizeof *s->img);
  if (!s || !s->img || !px) goto fail;
  s->n = n;
  for (size_t i = 0; i < n; i++) {
    render(px, w, h, IMAGE_SEED + (uint32_t)i);
    jpeg *j = &s->img[i];
    j->w = w;
    j->h = h;
    if (!stbi_write_jpg_to_func(jpeg_sink, j, w, h, 3, px, QUALITY) || !j->len) goto fail;
  }
  free(px);
  return s;
fail:
  free(px);
  k4_destroy(s);
  snprintf(a->err, a->errlen, "could not generate the JPEG inputs");
  return NULL;
}

static size_t k4_ntasks(const void *p) { return ((const k4 *)p)->n; }
static uint64_t k4_task_work(const void *p, size_t i) {
  const jpeg *j = &((const k4 *)p)->img[i];
  return (uint64_t)j->w * (uint64_t)j->h;
}

static void *k4_scratch_new(const void *p) {
  (void)p;
  return calloc(1, sizeof(scratch));
}

static void k4_scratch_free(void *p) {
  scratch *sc = p;
  if (!sc) return;
  free(sc->row);
  free(sc->out);
  free(sc);
}

/*
 * Separable triangle (tent) filter for downscaling: the support of each output
 * sample covers 2 * (in / out) input samples, weights fall off linearly.
 */
static void resize(scratch *sc, const unsigned char *in, int w, int h, int ow, int oh) {
  float fx = (float)w / (float)ow, fy = (float)h / (float)oh;
  size_t need_row = (size_t)ow * (size_t)h * 3, need_out = (size_t)ow * (size_t)oh * 3;
  if (sc->row_cap < need_row) {
    free(sc->row);
    sc->row = malloc(need_row * sizeof *sc->row);
    sc->row_cap = sc->row ? need_row : 0;
  }
  if (sc->out_cap < need_out) {
    free(sc->out);
    sc->out = malloc(need_out);
    sc->out_cap = sc->out ? need_out : 0;
  }
  if (!sc->row || !sc->out) return;

  for (int ox = 0; ox < ow; ox++) { /* horizontal pass: in (w x h) -> row (ow x h) */
    float center = ((float)ox + 0.5f) * fx;
    int x0 = (int)(center - fx), x1 = (int)(center + fx);
    if (x0 < 0) x0 = 0;
    if (x1 > w - 1) x1 = w - 1;
    for (int y = 0; y < h; y++) {
      float acc[3] = {0, 0, 0}, wsum = 0;
      const unsigned char *src = &in[((size_t)y * (size_t)w) * 3];
      for (int x = x0; x <= x1; x++) {
        float d = ((float)x + 0.5f - center) / fx;
        float wt = 1.0f - (d < 0 ? -d : d);
        if (wt <= 0) continue;
        acc[0] += wt * (float)src[x * 3];
        acc[1] += wt * (float)src[x * 3 + 1];
        acc[2] += wt * (float)src[x * 3 + 2];
        wsum += wt;
      }
      float *o = &sc->row[((size_t)y * (size_t)ow + (size_t)ox) * 3];
      for (int k = 0; k < 3; k++) o[k] = acc[k] / wsum;
    }
  }
  for (int oy = 0; oy < oh; oy++) { /* vertical pass: row (ow x h) -> out (ow x oh) */
    float center = ((float)oy + 0.5f) * fy;
    int y0 = (int)(center - fy), y1 = (int)(center + fy);
    if (y0 < 0) y0 = 0;
    if (y1 > h - 1) y1 = h - 1;
    float wts[16];
    float wsum = 0;
    for (int y = y0; y <= y1 && y - y0 < 16; y++) {
      float d = ((float)y + 0.5f - center) / fy;
      float wt = 1.0f - (d < 0 ? -d : d);
      wts[y - y0] = wt > 0 ? wt : 0;
      wsum += wts[y - y0];
    }
    if (y1 - y0 >= 16) y1 = y0 + 15;
    unsigned char *o = &sc->out[(size_t)oy * (size_t)ow * 3];
    for (int i = 0; i < ow * 3; i++) {
      float acc = 0;
      for (int y = y0; y <= y1; y++) acc += wts[y - y0] * sc->row[(size_t)y * (size_t)ow * 3 + (size_t)i];
      float v = acc / wsum + 0.5f;
      o[i] = (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v);
    }
  }
}

static uint64_t k4_task(const void *p, void *sp, size_t i) {
  const jpeg *j = &((const k4 *)p)->img[i];
  scratch *sc = sp;
  int w, h, comp;
  unsigned char *px = stbi_load_from_memory(j->data, (int)j->len, &w, &h, &comp, 3);
  if (!px) return 0;
  int ow = w * SCALE_NUM / SCALE_DEN, oh = h * SCALE_NUM / SCALE_DEN;
  resize(sc, px, w, h, ow, oh);
  uint64_t hsh = pmk_hash_bytes(px, (size_t)w * (size_t)h * 3, 4);
  stbi_image_free(px);
  return sc->out ? pmk_hash_bytes(sc->out, (size_t)ow * (size_t)oh * 3, hsh) : 0;
}

static uint64_t k4_input_hash(const void *p) {
  const k4 *s = p;
  uint64_t h = 4;
  for (size_t i = 0; i < s->n; i++) h = pmk_hash_bytes(s->img[i].data, s->img[i].len, h);
  return h;
}

const pmk_tk PMK_SYM(pmk_k4_tk) = {
    .id = "K4",
    .name = "JPEG decode and resize",
    .unit = "MP/s",
    .unit_div = 1e6,
    .create = k4_create,
    .destroy = k4_destroy,
    .ntasks = k4_ntasks,
    .task_work = k4_task_work,
    .scratch_new = k4_scratch_new,
    .scratch_free = k4_scratch_free,
    .task = k4_task,
    .input_hash = k4_input_hash,
};
