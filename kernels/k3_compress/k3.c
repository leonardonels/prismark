/*
 * K3 — compression. zstd at a fixed level over a self-generated corpus of
 * text, log records and structured binary data. Each task compresses one
 * chunk independently with a per-thread context. zstd is built without
 * assembly and without intrinsics (see CMakeLists), and its output for a
 * given version, level and input is identical on every platform.
 *
 * The corpus is generated from fixed seeds, so no third-party data needs a
 * licence review.
 *
 * Copyright 2026 The Prismark Authors. Apache-2.0.
 */
#include <stdio.h>
#include <stdlib.h>

#include "kernels.h"
#include "rng.h"
#include "zstd.h"

#define LEVEL 3
#define CORPUS_SEED 0x6b33c0de

typedef struct k3 {
  unsigned char *data;
  size_t chunk, nchunks;
} k3;

typedef struct scratch {
  ZSTD_CCtx *cctx;
  void *dst;
  size_t cap;
} scratch;

/* ---------- corpus ---------- */

typedef struct gen {
  pmk_rng r;
  unsigned char *p, *end;
  char words[2048][16];
  int nwords;
} gen;

static void put(gen *g, const char *s, size_t n) {
  for (size_t i = 0; i < n && g->p < g->end; i++) *g->p++ = (unsigned char)s[i];
}

static void make_vocabulary(gen *g) {
  static const char *syl[] = {"ka", "lo", "mi", "ne", "ru", "sa", "to", "ve", "pri", "sma", "ton", "der", "ian",
                              "ex", "or", "al", "is", "en", "qua", "ber", "ti", "ga", "mo", "ul", "re", "con",
                              "ste", "pa", "lin", "dor", "fi", "nu", "che", "wa", "yo", "zi", "bra", "cle"};
  int nsyl = (int)(sizeof syl / sizeof *syl);
  g->nwords = 2048;
  for (int w = 0; w < g->nwords; w++) {
    int ns = 1 + (int)pmk_rng_below(&g->r, w < 64 ? 2 : 4);
    char *o = g->words[w];
    o[0] = 0;
    for (int s = 0; s < ns; s++) strncat(o, syl[pmk_rng_below(&g->r, (uint64_t)nsyl)], 15 - strlen(o));
  }
}

/* Zipf-like: frequent words have low indices. */
static const char *word(gen *g) {
  double u = pmk_rng_unit(&g->r);
  return g->words[(int)(u * u * u * (double)g->nwords)];
}

static void text_segment(gen *g, size_t n) {
  unsigned char *stop = g->p + n < g->end ? g->p + n : g->end;
  while (g->p < stop) {
    int len = 5 + (int)pmk_rng_below(&g->r, 16);
    for (int i = 0; i < len && g->p < stop; i++) {
      const char *w = word(g);
      char buf[20];
      snprintf(buf, sizeof buf, "%s", w);
      if (i == 0 && buf[0] >= 'a' && buf[0] <= 'z') buf[0] = (char)(buf[0] - 32);
      put(g, buf, strlen(buf));
      if (i + 1 < len) {
        const char *sep = pmk_rng_below(&g->r, 12) == 0 ? ", " : " ";
        put(g, sep, strlen(sep));
      }
    }
    const char *end = pmk_rng_below(&g->r, 6) == 0 ? ".\n\n" : ". ";
    put(g, end, strlen(end));
  }
}

static void log_segment(gen *g, size_t n, uint64_t *clock) {
  static const char *levels[] = {"INFO ", "INFO ", "INFO ", "DEBUG", "WARN ", "ERROR"};
  static const char *paths[] = {"/api/v1/items", "/api/v1/users", "/static/app.js", "/api/v2/search", "/healthz"};
  unsigned char *stop = g->p + n < g->end ? g->p + n : g->end;
  char line[256];
  while (g->p < stop) {
    *clock += 1 + pmk_rng_below(&g->r, 40);
    uint64_t ms = *clock, s = ms / 1000;
    /* One draw per statement: argument evaluation order is unspecified in C. */
    const char *level = levels[pmk_rng_below(&g->r, 6)];
    unsigned worker = (unsigned)pmk_rng_below(&g->r, 16);
    unsigned id = (unsigned)pmk_rng_next(&g->r);
    const char *path = paths[pmk_rng_below(&g->r, 5)];
    unsigned item = (unsigned)pmk_rng_below(&g->r, 5000);
    unsigned status = pmk_rng_below(&g->r, 20) ? 200u : 404u;
    unsigned bytes = (unsigned)pmk_rng_below(&g->r, 65536);
    unsigned latency = (unsigned)pmk_rng_below(&g->r, 300);
    int len = snprintf(line, sizeof line,
                       "2026-10-04T%02u:%02u:%02u.%03uZ %s [worker-%02u] request id=%08x path=%s/%u status=%u "
                       "bytes=%u latency_ms=%u\n",
                       (unsigned)(s / 3600 % 24), (unsigned)(s / 60 % 60), (unsigned)(s % 60), (unsigned)(ms % 1000),
                       level, worker, id, path, item, status, bytes, latency);
    put(g, line, (size_t)len);
  }
}

static void binary_segment(gen *g, size_t n) {
  unsigned char *stop = g->p + n < g->end ? g->p + n : g->end;
  uint32_t v = (uint32_t)pmk_rng_next(&g->r);
  int32_t phase = 0, step = 1 + (int32_t)pmk_rng_below(&g->r, 64);
  int kind = (int)pmk_rng_below(&g->r, 2);
  while (g->p + 4 <= stop) {
    unsigned char b[4];
    if (kind == 0) { /* sorted identifiers: small deltas */
      v += (uint32_t)pmk_rng_below(&g->r, 300);
      b[0] = (unsigned char)v;
      b[1] = (unsigned char)(v >> 8);
      b[2] = (unsigned char)(v >> 16);
      b[3] = (unsigned char)(v >> 24);
    } else { /* 16-bit stereo signal: triangle wave plus noise */
      phase += step;
      int32_t tri = (phase & 4096) ? 4096 - (phase & 4095) : (phase & 4095);
      int16_t l = (int16_t)(tri * 6 - 12288 + (int32_t)pmk_rng_below(&g->r, 64));
      int16_t r = (int16_t)(tri * 5 - 10240 + (int32_t)pmk_rng_below(&g->r, 64));
      b[0] = (unsigned char)l;
      b[1] = (unsigned char)((uint16_t)l >> 8);
      b[2] = (unsigned char)r;
      b[3] = (unsigned char)((uint16_t)r >> 8);
    }
    put(g, (const char *)b, 4);
  }
  while (g->p < stop) *g->p++ = 0;
}

static void generate(unsigned char *out, size_t n) {
  gen *g = calloc(1, sizeof *g);
  if (!g) return;
  pmk_rng_seed(&g->r, CORPUS_SEED);
  make_vocabulary(g);
  g->p = out;
  g->end = out + n;
  uint64_t clock = 36000000;
  while (g->p < g->end) {
    size_t seg = 4096 + (size_t)pmk_rng_below(&g->r, 60 * 1024);
    uint64_t k = pmk_rng_below(&g->r, 10);
    if (k < 5) text_segment(g, seg);
    else if (k < 8) log_segment(g, seg, &clock);
    else binary_segment(g, seg);
  }
  free(g);
}

/* ---------- kernel ---------- */

static void *k3_create(const pmk_tk_args *a) {
  k3 *s = calloc(1, sizeof *s);
  if (!s) goto oom;
  if (a->size == PMK_SIZE_BURST) {
    s->chunk = 512 << 10;
    s->nchunks = 8;
  } else {
    s->chunk = 1 << 20;
    s->nchunks = 32;
  }
  /* Burst input is a prefix of the full corpus, so both sizes share one generator. */
  s->data = malloc(s->chunk * s->nchunks);
  if (!s->data) goto oom;
  generate(s->data, s->chunk * s->nchunks);
  return s;
oom:
  free(s);
  snprintf(a->err, a->errlen, "out of memory");
  return NULL;
}

static void k3_destroy(void *p) {
  k3 *s = p;
  if (!s) return;
  free(s->data);
  free(s);
}

static size_t k3_ntasks(const void *p) { return ((const k3 *)p)->nchunks; }
static uint64_t k3_task_work(const void *p, size_t i) {
  (void)i;
  return ((const k3 *)p)->chunk;
}

static void *k3_scratch_new(const void *p) {
  const k3 *s = p;
  scratch *sc = calloc(1, sizeof *sc);
  if (!sc) return NULL;
  sc->cap = ZSTD_compressBound(s->chunk);
  sc->dst = malloc(sc->cap);
  sc->cctx = ZSTD_createCCtx();
  if (!sc->dst || !sc->cctx) {
    free(sc->dst);
    ZSTD_freeCCtx(sc->cctx);
    free(sc);
    return NULL;
  }
  return sc;
}

static void k3_scratch_free(void *p) {
  scratch *sc = p;
  if (!sc) return;
  ZSTD_freeCCtx(sc->cctx);
  free(sc->dst);
  free(sc);
}

static uint64_t k3_task(const void *p, void *sp, size_t i) {
  const k3 *s = p;
  scratch *sc = sp;
  size_t n = ZSTD_compressCCtx(sc->cctx, sc->dst, sc->cap, s->data + i * s->chunk, s->chunk, LEVEL);
  if (ZSTD_isError(n)) return 0;
  return pmk_hash_bytes(sc->dst, n, (uint64_t)n);
}

static uint64_t k3_input_hash(const void *p) {
  const k3 *s = p;
  return pmk_hash_bytes(s->data, s->chunk * s->nchunks, 3);
}

const pmk_tk PMK_SYM(pmk_k3_tk) = {
    .id = "K3",
    .name = "zstd compression (level 3)",
    .unit = "MB/s",
    .unit_div = 1e6,
    .create = k3_create,
    .destroy = k3_destroy,
    .ntasks = k3_ntasks,
    .task_work = k3_task_work,
    .scratch_new = k3_scratch_new,
    .scratch_free = k3_scratch_free,
    .task = k3_task,
    .input_hash = k3_input_hash,
};
