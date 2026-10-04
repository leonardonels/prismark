/*
 * K2 — path tracer. A fixed Cornell-box scene with diffuse, mirror and glass
 * spheres, rendered tile by tile. Each pixel sample has its own RNG stream
 * derived from its index, so the image does not depend on how tiles are
 * distributed over threads.
 *
 * Only +, -, *, / and sqrt are used on floats (no libm transcendentals) and
 * kernels are built without FP contraction, so the image, and therefore the
 * checksum, is bit-identical on every IEEE-754 host and every ISA tier.
 *
 * Copyright 2026 The Prismark Authors. Apache-2.0.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "kernels.h"

#define TILE 16
#define MAX_DEPTH 8
#define EPS 1e-4f
#define NSPHERES 24
#define NPLANES 6
#define FRAME_SEED 0x4b32u

typedef struct v3 {
  float x, y, z;
} v3;

static inline v3 mk(float x, float y, float z) {
  v3 r = {x, y, z};
  return r;
}
static inline v3 add(v3 a, v3 b) { return mk(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline v3 sub(v3 a, v3 b) { return mk(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline v3 scale(v3 a, float s) { return mk(a.x * s, a.y * s, a.z * s); }
static inline v3 mulv(v3 a, v3 b) { return mk(a.x * b.x, a.y * b.y, a.z * b.z); }
static inline float dot(v3 a, v3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline v3 norm(v3 a) { return scale(a, 1.0f / sqrtf(dot(a, a))); }

enum { DIFF, SPEC, GLASS };

typedef struct material {
  v3 albedo, emission;
  int type;
} material;

/* Axis-aligned rectangle: points with coordinate `axis` == pos, bounded on the other two axes. */
typedef struct plane {
  int axis;
  float pos;
  v3 normal;
  float lo[2], hi[2];
  int mat;
} plane;

typedef struct k2 {
  int w, h, spp;
  /* Spheres as structure of arrays, so the intersection loop can be vectorised. */
  float cx[NSPHERES], cy[NSPHERES], cz[NSPHERES], r2[NSPHERES], r[NSPHERES];
  int smat[NSPHERES];
  plane planes[NPLANES];
  material mats[16];
  int nmats;
  v3 cam;
  float tan_half;
} k2;

typedef struct rng32 {
  uint64_t s;
} rng32;

static inline float rnd(rng32 *r) {
  uint64_t x = r->s;
  x ^= x >> 12;
  x ^= x << 25;
  x ^= x >> 27;
  r->s = x;
  return (float)((x * 2685821657736338717ull) >> 40) * 0x1.0p-24f;
}

static int add_mat(k2 *s, v3 albedo, v3 emission, int type) {
  material *m = &s->mats[s->nmats];
  m->albedo = albedo;
  m->emission = emission;
  m->type = type;
  return s->nmats++;
}

static void build_scene(k2 *s) {
  int white = add_mat(s, mk(0.75f, 0.75f, 0.75f), mk(0, 0, 0), DIFF);
  int red = add_mat(s, mk(0.75f, 0.2f, 0.2f), mk(0, 0, 0), DIFF);
  int green = add_mat(s, mk(0.2f, 0.75f, 0.25f), mk(0, 0, 0), DIFF);
  int light = add_mat(s, mk(0, 0, 0), mk(18.0f, 16.0f, 13.0f), DIFF);
  int mirror = add_mat(s, mk(0.95f, 0.95f, 0.95f), mk(0, 0, 0), SPEC);
  int glass = add_mat(s, mk(0.98f, 0.98f, 0.98f), mk(0, 0, 0), GLASS);
  int blue = add_mat(s, mk(0.25f, 0.35f, 0.8f), mk(0, 0, 0), DIFF);
  int tints[4] = {
      add_mat(s, mk(0.8f, 0.6f, 0.2f), mk(0, 0, 0), DIFF),
      add_mat(s, mk(0.3f, 0.7f, 0.7f), mk(0, 0, 0), DIFF),
      add_mat(s, mk(0.7f, 0.3f, 0.6f), mk(0, 0, 0), DIFF),
      add_mat(s, mk(0.6f, 0.6f, 0.6f), mk(0, 0, 0), SPEC),
  };

  /* Box: x in [-1, 1], y in [0, 2], z in [-1, 1]; open at the front. Normals point inwards. */
  plane p[NPLANES] = {
      {0, -1.0f, {1, 0, 0}, {0.0f, -1.0f}, {2.0f, 1.0f}, red},     /* left: bounds on (y, z) */
      {0, 1.0f, {-1, 0, 0}, {0.0f, -1.0f}, {2.0f, 1.0f}, green},   /* right */
      {2, -1.0f, {0, 0, 1}, {-1.0f, 0.0f}, {1.0f, 2.0f}, white},   /* back: bounds on (x, y) */
      {1, 0.0f, {0, 1, 0}, {-1.0f, -1.0f}, {1.0f, 1.0f}, white},   /* floor: bounds on (x, z) */
      {1, 2.0f, {0, -1, 0}, {-1.0f, -1.0f}, {1.0f, 1.0f}, white},  /* ceiling */
      {1, 1.995f, {0, -1, 0}, {-0.3f, -0.3f}, {0.3f, 0.3f}, light} /* area light under the ceiling */
  };
  for (int i = 0; i < NPLANES; i++) s->planes[i] = p[i];

  struct {
    float x, y, z, r;
    int mat;
  } big[3] = {{-0.45f, 0.35f, -0.35f, 0.35f, blue}, {0.5f, 0.4f, -0.5f, 0.4f, mirror}, {0.1f, 0.25f, 0.35f, 0.25f, glass}};
  int n = 0;
  for (; n < 3; n++) {
    s->cx[n] = big[n].x;
    s->cy[n] = big[n].y;
    s->cz[n] = big[n].z;
    s->r[n] = big[n].r;
    s->smat[n] = big[n].mat;
  }
  /* Small spheres on the floor, placed by a fixed integer sequence. */
  uint64_t seed = 0x5ce9e;
  for (; n < NSPHERES; n++) {
    uint64_t a = pmk_mix64(seed + (uint64_t)n);
    float u = (float)(a & 0xffff) / 65536.0f, v = (float)((a >> 16) & 0xffff) / 65536.0f;
    float rad = 0.04f + 0.06f * (float)((a >> 32) & 0xff) / 256.0f;
    s->cx[n] = -0.9f + 1.8f * u;
    s->cz[n] = -0.9f + 1.6f * v;
    s->cy[n] = rad;
    s->r[n] = rad;
    s->smat[n] = tints[(a >> 40) & 3];
  }
  for (int i = 0; i < NSPHERES; i++) s->r2[i] = s->r[i] * s->r[i];
  s->cam = mk(0.0f, 1.0f, 3.6f);
  s->tan_half = 0.41421356f; /* tan(22.5 degrees): 45 degree vertical field of view */
}

typedef struct hit {
  float t;
  v3 n; /* geometric normal, outward for spheres */
  int mat;
} hit;

static int intersect(const k2 *s, v3 o, v3 d, hit *h) {
  float best = 1e30f;
  int bi = -1;
  for (int i = 0; i < NSPHERES; i++) {
    float ox = o.x - s->cx[i], oy = o.y - s->cy[i], oz = o.z - s->cz[i];
    float b = ox * d.x + oy * d.y + oz * d.z;
    float c = ox * ox + oy * oy + oz * oz - s->r2[i];
    float disc = b * b - c;
    if (disc < 0) continue;
    float q = sqrtf(disc);
    float t = -b - q;
    if (t < EPS) t = -b + q;
    if (t >= EPS && t < best) {
      best = t;
      bi = i;
    }
  }
  int pi = -1;
  for (int i = 0; i < NPLANES; i++) {
    const plane *p = &s->planes[i];
    float oa = p->axis == 0 ? o.x : p->axis == 1 ? o.y : o.z;
    float da = p->axis == 0 ? d.x : p->axis == 1 ? d.y : d.z;
    if (da > -1e-8f && da < 1e-8f) continue;
    float t = (p->pos - oa) / da;
    if (t < EPS || t >= best) continue;
    v3 x = add(o, scale(d, t));
    float u = p->axis == 0 ? x.y : x.x;
    float v = p->axis == 2 ? x.y : x.z;
    if (u < p->lo[0] || u > p->hi[0] || v < p->lo[1] || v > p->hi[1]) continue;
    best = t;
    pi = i;
  }
  if (pi >= 0) {
    h->t = best;
    h->n = s->planes[pi].normal;
    h->mat = s->planes[pi].mat;
    return 1;
  }
  if (bi >= 0) {
    h->t = best;
    v3 x = add(o, scale(d, best));
    h->n = scale(mk(x.x - s->cx[bi], x.y - s->cy[bi], x.z - s->cz[bi]), 1.0f / s->r[bi]);
    h->mat = s->smat[bi];
    return 1;
  }
  return 0;
}

/* Cosine-weighted direction around n, from a rejection-sampled disk (no trigonometry). */
static v3 cosine_dir(v3 n, rng32 *r) {
  float x, y, d2;
  do {
    x = 2.0f * rnd(r) - 1.0f;
    y = 2.0f * rnd(r) - 1.0f;
    d2 = x * x + y * y;
  } while (d2 >= 1.0f);
  float z = sqrtf(1.0f - d2);
  /* Orthonormal basis (Duff et al. 2017). */
  float sign = copysignf(1.0f, n.z);
  float a = -1.0f / (sign + n.z);
  float b = n.x * n.y * a;
  v3 t = mk(1.0f + sign * n.x * n.x * a, sign * b, -sign * n.x);
  v3 bt = mk(b, sign + n.y * n.y * a, -n.y);
  return norm(add(add(scale(t, x), scale(bt, y)), scale(n, z)));
}

static v3 radiance(const k2 *s, v3 o, v3 d, rng32 *r) {
  v3 L = mk(0, 0, 0), T = mk(1, 1, 1);
  for (int depth = 0; depth < MAX_DEPTH; depth++) {
    hit h;
    if (!intersect(s, o, d, &h)) break;
    const material *m = &s->mats[h.mat];
    L = add(L, mulv(T, m->emission));
    v3 x = add(o, scale(d, h.t));
    int into = dot(d, h.n) < 0;
    v3 nl = into ? h.n : scale(h.n, -1.0f); /* normal facing the incoming ray */
    if (m->type == DIFF) {
      d = cosine_dir(nl, r);
    } else if (m->type == SPEC) {
      d = sub(d, scale(nl, 2.0f * dot(d, nl)));
    } else {
      float eta = into ? 1.0f / 1.5f : 1.5f;
      float cosi = -dot(d, nl);
      float k = 1.0f - eta * eta * (1.0f - cosi * cosi);
      v3 refl = sub(d, scale(nl, 2.0f * dot(d, nl)));
      if (k < 0) {
        d = refl; /* total internal reflection */
      } else {
        v3 tdir = norm(add(scale(d, eta), scale(nl, eta * cosi - sqrtf(k))));
        float c = 1.0f - (into ? cosi : -dot(tdir, h.n));
        float c5 = c * c * c * c * c;
        float fr = 0.04f + 0.96f * c5; /* Schlick, n = 1.5 */
        d = rnd(r) < fr ? refl : tdir;
      }
    }
    T = mulv(T, m->albedo);
    o = x;
    if (depth >= 3) { /* Russian roulette */
      float p = T.x > T.y ? (T.x > T.z ? T.x : T.z) : (T.y > T.z ? T.y : T.z);
      if (p < 0.05f) p = 0.05f;
      if (p > 0.95f) p = 0.95f;
      if (rnd(r) >= p) break;
      T = scale(T, 1.0f / p);
    }
  }
  return L;
}

typedef struct scratch {
  float px[TILE * TILE * 3];
} scratch;

static void *k2_create(const pmk_tk_args *a) {
  k2 *s = calloc(1, sizeof *s);
  if (!s) {
    snprintf(a->err, a->errlen, "out of memory");
    return NULL;
  }
  build_scene(s);
  if (a->size == PMK_SIZE_BURST) {
    s->w = 128;
    s->h = 128;
    s->spp = 8;
  } else {
    s->w = 384;
    s->h = 384;
    s->spp = 16;
  }
  return s;
}

static void k2_destroy(void *p) { free(p); }

static size_t tiles_x(const k2 *s) { return (size_t)(s->w + TILE - 1) / TILE; }

static size_t k2_ntasks(const void *p) {
  const k2 *s = p;
  return tiles_x(s) * (size_t)((s->h + TILE - 1) / TILE);
}

static uint64_t k2_task_work(const void *p, size_t i) {
  const k2 *s = p;
  size_t tx = tiles_x(s);
  int x0 = (int)(i % tx) * TILE, y0 = (int)(i / tx) * TILE;
  int w = s->w - x0 < TILE ? s->w - x0 : TILE, h = s->h - y0 < TILE ? s->h - y0 : TILE;
  return (uint64_t)w * (uint64_t)h * (uint64_t)s->spp;
}

static void *k2_scratch_new(const void *p) {
  (void)p;
  return malloc(sizeof(scratch));
}

static uint64_t k2_task(const void *p, void *sp, size_t i) {
  const k2 *s = p;
  scratch *sc = sp;
  size_t tx = tiles_x(s);
  int x0 = (int)(i % tx) * TILE, y0 = (int)(i / tx) * TILE;
  int tw = s->w - x0 < TILE ? s->w - x0 : TILE, th = s->h - y0 < TILE ? s->h - y0 : TILE;
  float aspect = (float)s->w / (float)s->h;
  float inv = 1.0f / (float)s->spp;
  for (int y = 0; y < th; y++)
    for (int x = 0; x < tw; x++) {
      int px = x0 + x, py = y0 + y;
      v3 acc = mk(0, 0, 0);
      for (int k = 0; k < s->spp; k++) {
        rng32 r = {pmk_mix64(((uint64_t)(py * s->w + px) << 16) ^ (uint64_t)k ^ ((uint64_t)FRAME_SEED << 48)) | 1};
        float sx = ((float)px + rnd(&r)) / (float)s->w * 2.0f - 1.0f;
        float sy = 1.0f - ((float)py + rnd(&r)) / (float)s->h * 2.0f;
        v3 d = norm(mk(sx * aspect * s->tan_half, sy * s->tan_half, -1.0f));
        acc = add(acc, radiance(s, s->cam, d, &r));
      }
      float *o = &sc->px[(y * TILE + x) * 3];
      o[0] = acc.x * inv;
      o[1] = acc.y * inv;
      o[2] = acc.z * inv;
    }
  uint64_t h = 0;
  for (int y = 0; y < th; y++) h = pmk_hash_bytes(&sc->px[y * TILE * 3], (size_t)tw * 3 * sizeof(float), h);
  return h;
}

static uint64_t k2_input_hash(const void *p) {
  const k2 *s = p;
  uint64_t h = pmk_hash_bytes(s->cx, sizeof s->cx, 2);
  h = pmk_hash_bytes(s->planes, sizeof s->planes, h);
  h = pmk_hash_bytes(s->mats, sizeof s->mats, h);
  uint32_t dims[3] = {(uint32_t)s->w, (uint32_t)s->h, (uint32_t)s->spp};
  return pmk_hash_bytes(dims, sizeof dims, h);
}

const pmk_tk PMK_SYM(pmk_k2_tk) = {
    .id = "K2",
    .name = "path tracer",
    .unit = "Msamples/s",
    .unit_div = 1e6,
    .create = k2_create,
    .destroy = k2_destroy,
    .ntasks = k2_ntasks,
    .task_work = k2_task_work,
    .scratch_new = k2_scratch_new,
    .scratch_free = free,
    .task = k2_task,
    .input_hash = k2_input_hash,
};
