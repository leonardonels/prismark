/*
 * K5 — text/JSON parsing. A portable scalar, validating JSON parser that
 * builds a tape (one entry per value, strings unescaped into a side buffer)
 * over generated documents. Numbers are kept exactly as a decimal mantissa
 * and exponent instead of being converted with strtod, so the result does
 * not depend on the C library.
 *
 * Copyright 2026 The Prismark Authors. Apache-2.0.
 */
#include <stdio.h>
#include <stdlib.h>

#include "kernels.h"
#include "rng.h"

#define DOC_SEED 0x6b35
#define MAX_DEPTH 64

typedef struct doc {
  char *text;
  size_t len;
} doc;

typedef struct k5 {
  doc *docs;
  size_t n;
} k5;

enum { T_NULL, T_FALSE, T_TRUE, T_INT, T_DEC, T_STR, T_ARR, T_OBJ, T_END };

typedef struct tape_entry {
  uint32_t type;
  uint32_t aux;    /* string length, decimal exponent bias, or container size */
  uint64_t value;  /* integer, decimal mantissa, or string offset */
} tape_entry;

typedef struct scratch {
  tape_entry *tape;
  size_t tape_cap;
  char *strings;
  size_t str_cap;
} scratch;

/* ---------- document generator ---------- */

typedef struct out {
  char *p;
  size_t len, cap;
} out;

static void emit(out *o, const char *s, size_t n) {
  if (o->len + n + 1 > o->cap) {
    size_t cap = (o->cap ? o->cap * 2 : 1 << 16) + n;
    char *p = realloc(o->p, cap);
    if (!p) return;
    o->p = p;
    o->cap = cap;
  }
  memcpy(o->p + o->len, s, n);
  o->len += n;
  o->p[o->len] = 0;
}

static void emits(out *o, const char *s) { emit(o, s, strlen(s)); }

static void gen_string(out *o, pmk_rng *r) {
  static const char *parts[] = {"alpha", "beta", "gamma", " ", "delta", "\\\"quoted\\\"", "line\\nbreak",
                                "caf\\u00e9", "tab\\tstop", "\\u263a", "\\ud83d\\ude00", "slash\\/", "x"};
  int n = 1 + (int)pmk_rng_below(r, 6);
  emits(o, "\"");
  for (int i = 0; i < n; i++) emits(o, parts[pmk_rng_below(r, sizeof parts / sizeof *parts)]);
  emits(o, "\"");
}

/* One draw per statement throughout: argument evaluation order is unspecified in C. */
static void gen_number(out *o, pmk_rng *r) {
  char b[48];
  uint64_t kind = pmk_rng_below(r, 4);
  if (kind == 0) {
    long long v = (long long)pmk_rng_below(r, 1000000) - 500000;
    snprintf(b, sizeof b, "%lld", v);
  } else if (kind == 1) {
    long long ip = (long long)pmk_rng_below(r, 2000) - 1000;
    unsigned fp = (unsigned)pmk_rng_below(r, 1000);
    snprintf(b, sizeof b, "%lld.%03u", ip, fp);
  } else if (kind == 2) {
    unsigned m = (unsigned)pmk_rng_below(r, 10);
    unsigned f = (unsigned)pmk_rng_below(r, 100000);
    const char *sign = pmk_rng_below(r, 2) ? "-" : "+";
    unsigned e = (unsigned)pmk_rng_below(r, 30);
    snprintf(b, sizeof b, "%u.%ue%s%u", m, f, sign, e);
  } else {
    unsigned long long v = (unsigned long long)(pmk_rng_next(r) >> 12);
    snprintf(b, sizeof b, "%llu", v);
  }
  emits(o, b);
}

static void gen_record(out *o, pmk_rng *r, unsigned id) {
  char b[64];
  snprintf(b, sizeof b, "{\"id\":%u,\"name\":", id);
  emits(o, b);
  gen_string(o, r);
  emits(o, ",\"active\":");
  emits(o, pmk_rng_below(r, 2) ? "true" : "false");
  emits(o, ",\"score\":");
  gen_number(o, r);
  emits(o, ",\"tags\":[");
  int nt = (int)pmk_rng_below(r, 5);
  for (int i = 0; i < nt; i++) {
    if (i) emits(o, ",");
    gen_string(o, r);
  }
  emits(o, "],\"geo\":{\"lat\":");
  gen_number(o, r);
  emits(o, ",\"lon\":");
  gen_number(o, r);
  emits(o, "},\"parent\":");
  if (pmk_rng_below(r, 3) == 0) emits(o, "null");
  else gen_number(o, r);
  emits(o, ",\"history\":[");
  int nh = (int)pmk_rng_below(r, 4);
  for (int i = 0; i < nh; i++) {
    if (i) emits(o, ",");
    emits(o, "{\"t\":");
    gen_number(o, r);
    emits(o, ",\"v\":[");
    for (int k = 0; k < 4; k++) {
      if (k) emits(o, ", ");
      gen_number(o, r);
    }
    emits(o, "]}");
  }
  emits(o, "],\n \"bio\":");
  gen_string(o, r);
  emits(o, "}");
}

static int gen_doc(doc *d, size_t target, uint64_t seed) {
  out o = {0};
  pmk_rng r;
  pmk_rng_seed(&r, seed);
  emits(&o, "{\"version\":1,\"records\":[\n");
  for (unsigned id = 0; o.len < target; id++) {
    if (id) emits(&o, ",\n");
    gen_record(&o, &r, id);
  }
  emits(&o, "\n]}\n");
  d->text = o.p;
  d->len = o.len;
  return o.p ? 0 : -1;
}

/* ---------- parser ---------- */

typedef struct parser {
  const char *p, *end;
  scratch *sc;
  size_t nt, ns;
} parser;

static int push(parser *ps, uint32_t type, uint32_t aux, uint64_t value) {
  scratch *sc = ps->sc;
  if (ps->nt == sc->tape_cap) {
    size_t cap = sc->tape_cap ? sc->tape_cap * 2 : 1 << 16;
    tape_entry *t = realloc(sc->tape, cap * sizeof *t);
    if (!t) return -1;
    sc->tape = t;
    sc->tape_cap = cap;
  }
  tape_entry *e = &sc->tape[ps->nt++];
  e->type = type;
  e->aux = aux;
  e->value = value;
  return 0;
}

static int put_utf8(parser *ps, uint32_t cp) {
  char b[4];
  size_t n;
  if (cp < 0x80) b[0] = (char)cp, n = 1;
  else if (cp < 0x800) b[0] = (char)(0xc0 | (cp >> 6)), b[1] = (char)(0x80 | (cp & 0x3f)), n = 2;
  else if (cp < 0x10000)
    b[0] = (char)(0xe0 | (cp >> 12)), b[1] = (char)(0x80 | ((cp >> 6) & 0x3f)), b[2] = (char)(0x80 | (cp & 0x3f)), n = 3;
  else
    b[0] = (char)(0xf0 | (cp >> 18)), b[1] = (char)(0x80 | ((cp >> 12) & 0x3f)),
    b[2] = (char)(0x80 | ((cp >> 6) & 0x3f)), b[3] = (char)(0x80 | (cp & 0x3f)), n = 4;
  memcpy(ps->sc->strings + ps->ns, b, n);
  ps->ns += n;
  return 0;
}

static int hex4(const char *p, uint32_t *v) {
  *v = 0;
  for (int i = 0; i < 4; i++) {
    char c = p[i];
    uint32_t d;
    if (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
    else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
    else return -1;
    *v = *v << 4 | d;
  }
  return 0;
}

static int parse_string(parser *ps) {
  const char *p = ps->p + 1; /* past the quote */
  size_t start = ps->ns;
  for (;;) {
    if (p >= ps->end) return -1;
    unsigned char c = (unsigned char)*p;
    if (c == '"') break;
    if (c < 0x20) return -1;
    if (c != '\\') {
      ps->sc->strings[ps->ns++] = (char)c;
      p++;
      continue;
    }
    if (p + 1 >= ps->end) return -1;
    char e = p[1];
    p += 2;
    switch (e) {
      case '"': ps->sc->strings[ps->ns++] = '"'; break;
      case '\\': ps->sc->strings[ps->ns++] = '\\'; break;
      case '/': ps->sc->strings[ps->ns++] = '/'; break;
      case 'b': ps->sc->strings[ps->ns++] = '\b'; break;
      case 'f': ps->sc->strings[ps->ns++] = '\f'; break;
      case 'n': ps->sc->strings[ps->ns++] = '\n'; break;
      case 'r': ps->sc->strings[ps->ns++] = '\r'; break;
      case 't': ps->sc->strings[ps->ns++] = '\t'; break;
      case 'u': {
        uint32_t cp, lo;
        if (p + 4 > ps->end || hex4(p, &cp)) return -1;
        p += 4;
        if (cp >= 0xd800 && cp < 0xdc00) { /* surrogate pair */
          if (p + 6 > ps->end || p[0] != '\\' || p[1] != 'u' || hex4(p + 2, &lo) || lo < 0xdc00 || lo >= 0xe000)
            return -1;
          p += 6;
          cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
        } else if (cp >= 0xdc00 && cp < 0xe000) {
          return -1;
        }
        put_utf8(ps, cp);
        break;
      }
      default: return -1;
    }
  }
  ps->p = p + 1;
  return push(ps, T_STR, (uint32_t)(ps->ns - start), start);
}

static int is_digit(char c) { return c >= '0' && c <= '9'; }

static int parse_number(parser *ps) {
  const char *p = ps->p;
  int neg = 0, digits = 0, exp10 = 0;
  uint64_t m = 0;
  if (*p == '-') neg = 1, p++;
  if (p >= ps->end || !is_digit(*p)) return -1;
  if (*p == '0' && p + 1 < ps->end && is_digit(p[1])) return -1; /* no leading zeros */
  int is_int = 1;
  for (; p < ps->end && is_digit(*p); p++, digits++) {
    if (m < 1000000000000000000ull) m = m * 10 + (uint64_t)(*p - '0');
    else exp10++; /* keep 18-19 significant digits, count the rest */
  }
  if (p < ps->end && *p == '.') {
    is_int = 0;
    p++;
    if (p >= ps->end || !is_digit(*p)) return -1;
    for (; p < ps->end && is_digit(*p); p++)
      if (m < 1000000000000000000ull) m = m * 10 + (uint64_t)(*p - '0'), exp10--;
  }
  if (p < ps->end && (*p == 'e' || *p == 'E')) {
    is_int = 0;
    p++;
    int eneg = 0, e = 0;
    if (p < ps->end && (*p == '+' || *p == '-')) eneg = *p == '-', p++;
    if (p >= ps->end || !is_digit(*p)) return -1;
    for (; p < ps->end && is_digit(*p); p++)
      if (e < 100000) e = e * 10 + (*p - '0');
    exp10 += eneg ? -e : e;
  }
  ps->p = p;
  (void)digits;
  if (is_int && exp10 == 0) return push(ps, T_INT, (uint32_t)neg, neg ? (uint64_t)0 - m : m);
  return push(ps, T_DEC, (uint32_t)((exp10 + 0x40000) << 1 | neg), m);
}

static void skip_ws(parser *ps) {
  while (ps->p < ps->end && (*ps->p == ' ' || *ps->p == '\n' || *ps->p == '\r' || *ps->p == '\t')) ps->p++;
}

static int literal(parser *ps, const char *word, size_t n, uint32_t type) {
  if ((size_t)(ps->end - ps->p) < n || memcmp(ps->p, word, n)) return -1;
  ps->p += n;
  return push(ps, type, 0, 0);
}

/* Iterative parse with an explicit container stack; container entries are patched with their size. */
static int parse(parser *ps) {
  size_t stack[MAX_DEPTH];
  int is_obj[MAX_DEPTH];
  int depth = 0;
  skip_ws(ps);
  for (;;) {
    if (ps->p >= ps->end) return -1;
    char c = *ps->p;
    int r;
    if (c == '{' || c == '[') {
      if (depth == MAX_DEPTH) return -1;
      stack[depth] = ps->nt;
      is_obj[depth] = c == '{';
      depth++;
      if (push(ps, c == '{' ? T_OBJ : T_ARR, 0, 0)) return -1;
      ps->p++;
      skip_ws(ps);
      if (ps->p < ps->end && *ps->p == (c == '{' ? '}' : ']')) goto close;
      if (c == '{') goto key;
      continue;
    }
    if (c == '"') r = parse_string(ps);
    else if (c == 't') r = literal(ps, "true", 4, T_TRUE);
    else if (c == 'f') r = literal(ps, "false", 5, T_FALSE);
    else if (c == 'n') r = literal(ps, "null", 4, T_NULL);
    else r = parse_number(ps);
    if (r) return -1;
  after_value:
    skip_ws(ps);
    if (depth == 0) return ps->p == ps->end ? 0 : -1;
    if (ps->p >= ps->end) return -1;
    if (*ps->p == ',') {
      ps->p++;
      skip_ws(ps);
      if (is_obj[depth - 1]) goto key;
      continue;
    }
    if (*ps->p != (is_obj[depth - 1] ? '}' : ']')) return -1;
  close:
    ps->p++;
    depth--;
    ps->sc->tape[stack[depth]].aux = (uint32_t)(ps->nt - stack[depth]);
    if (push(ps, T_END, 0, 0)) return -1;
    goto after_value;
  key:
    if (ps->p >= ps->end || *ps->p != '"' || parse_string(ps)) return -1;
    skip_ws(ps);
    if (ps->p >= ps->end || *ps->p != ':') return -1;
    ps->p++;
    skip_ws(ps);
  }
}

/* ---------- kernel ---------- */

static void k5_destroy(void *p) {
  k5 *s = p;
  if (!s) return;
  for (size_t i = 0; i < s->n; i++) free(s->docs[i].text);
  free(s->docs);
  free(s);
}

static void *k5_create(const pmk_tk_args *a) {
  k5 *s = calloc(1, sizeof *s);
  size_t n = a->size == PMK_SIZE_BURST ? 8 : 32;
  if (s) s->docs = calloc(n, sizeof *s->docs);
  if (!s || !s->docs) goto fail;
  s->n = n;
  for (size_t i = 0; i < n; i++)
    if (gen_doc(&s->docs[i], 1 << 20, DOC_SEED + i)) goto fail;
  return s;
fail:
  k5_destroy(s);
  snprintf(a->err, a->errlen, "out of memory");
  return NULL;
}

static size_t k5_ntasks(const void *p) { return ((const k5 *)p)->n; }
static uint64_t k5_task_work(const void *p, size_t i) { return ((const k5 *)p)->docs[i].len; }

static void *k5_scratch_new(const void *p) {
  const k5 *s = p;
  scratch *sc = calloc(1, sizeof *sc);
  size_t maxlen = 0;
  for (size_t i = 0; i < s->n; i++)
    if (s->docs[i].len > maxlen) maxlen = s->docs[i].len;
  if (sc) {
    sc->str_cap = maxlen; /* unescaped strings never exceed the document */
    sc->strings = malloc(sc->str_cap);
    if (!sc->strings) {
      free(sc);
      sc = NULL;
    }
  }
  return sc;
}

static void k5_scratch_free(void *p) {
  scratch *sc = p;
  if (!sc) return;
  free(sc->tape);
  free(sc->strings);
  free(sc);
}

static uint64_t k5_task(const void *p, void *sp, size_t i) {
  const doc *d = &((const k5 *)p)->docs[i];
  parser ps = {d->text, d->text + d->len, sp, 0, 0};
  if (parse(&ps)) return 0;
  scratch *sc = sp;
  uint64_t h = pmk_hash_bytes(sc->strings, ps.ns, ps.nt);
  for (size_t k = 0; k < ps.nt; k++) {
    const tape_entry *e = &sc->tape[k];
    h = pmk_mix64(h ^ ((uint64_t)e->type << 56 ^ (uint64_t)e->aux << 24) ^ e->value);
  }
  return h;
}

static uint64_t k5_input_hash(const void *p) {
  const k5 *s = p;
  uint64_t h = 5;
  for (size_t i = 0; i < s->n; i++) h = pmk_hash_bytes(s->docs[i].text, s->docs[i].len, h);
  return h;
}

/* Exposed for the unit tests: parses text and returns the number of tape entries, or -1. */
long PMK_SYM(pmk_k5_parse_count)(const char *text, size_t len);
long PMK_SYM(pmk_k5_parse_count)(const char *text, size_t len) {
  scratch sc = {0};
  sc.str_cap = len + 1;
  sc.strings = malloc(sc.str_cap);
  parser ps = {text, text + len, &sc, 0, 0};
  long r = sc.strings && parse(&ps) == 0 ? (long)ps.nt : -1;
  free(sc.tape);
  free(sc.strings);
  return r;
}

const pmk_tk PMK_SYM(pmk_k5_tk) = {
    .id = "K5",
    .name = "JSON parsing",
    .unit = "MB/s",
    .unit_div = 1e6,
    .create = k5_create,
    .destroy = k5_destroy,
    .ntasks = k5_ntasks,
    .task_work = k5_task_work,
    .scratch_new = k5_scratch_new,
    .scratch_free = k5_scratch_free,
    .task = k5_task,
    .input_hash = k5_input_hash,
};
