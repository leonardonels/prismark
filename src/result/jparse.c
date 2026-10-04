/* Copyright 2026 The Prismark Authors. Apache-2.0. */
#include "jparse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_DEPTH 64

typedef struct block {
  struct block *next;
  size_t used, cap;
  _Alignas(16) unsigned char data[];
} block;

struct jdoc {
  jv root;
  block *blocks;
};

typedef struct ps {
  const char *p;
  jdoc *d;
  char *err;
  size_t errlen;
  int depth;
} ps;

static void *arena(jdoc *d, size_t n) {
  n = (n + 15) & ~(size_t)15;
  block *b = d->blocks;
  if (!b || b->used + n > b->cap) {
    size_t cap = n > (1u << 20) ? n : (1u << 20);
    b = malloc(sizeof *b + cap);
    if (!b) return NULL;
    b->next = d->blocks;
    b->used = 0;
    b->cap = cap;
    d->blocks = b;
  }
  void *p = b->data + b->used;
  b->used += n;
  return p;
}

static int fail(ps *s, const char *msg) {
  if (s->err && !s->err[0]) snprintf(s->err, s->errlen, "%s", msg);
  return -1;
}

static void ws(ps *s) {
  while (*s->p == ' ' || *s->p == '\n' || *s->p == '\r' || *s->p == '\t') s->p++;
}

static int hex4(const char *p, unsigned *v) {
  *v = 0;
  for (int i = 0; i < 4; i++) {
    char c = p[i];
    unsigned d = c >= '0' && c <= '9' ? (unsigned)(c - '0') : c >= 'a' && c <= 'f' ? (unsigned)(c - 'a' + 10)
               : c >= 'A' && c <= 'F' ? (unsigned)(c - 'A' + 10) : 16u;
    if (d == 16) return -1;
    *v = *v << 4 | d;
  }
  return 0;
}

static int string(ps *s, const char **out) {
  const char *p = s->p + 1;
  size_t len = 0;
  for (const char *q = p; *q != '"'; q++) { /* upper bound of the decoded length */
    if (!*q) return fail(s, "unterminated string");
    if (*q == '\\' && q[1]) q++;
    len++;
  }
  char *o = arena(s->d, len * 3 + 1), *w = o;
  if (!o) return fail(s, "out of memory");
  while (*p != '"') {
    if ((unsigned char)*p < 0x20) return fail(s, "control character in string");
    if (*p != '\\') {
      *w++ = *p++;
      continue;
    }
    char e = p[1];
    p += 2;
    switch (e) {
      case '"': *w++ = '"'; break;
      case '\\': *w++ = '\\'; break;
      case '/': *w++ = '/'; break;
      case 'b': *w++ = '\b'; break;
      case 'f': *w++ = '\f'; break;
      case 'n': *w++ = '\n'; break;
      case 'r': *w++ = '\r'; break;
      case 't': *w++ = '\t'; break;
      case 'u': {
        unsigned cp;
        if (hex4(p, &cp)) return fail(s, "bad \\u escape");
        p += 4;
        if (cp >= 0xd800 && cp < 0xdc00 && p[0] == '\\' && p[1] == 'u') {
          unsigned lo;
          if (hex4(p + 2, &lo) == 0 && lo >= 0xdc00 && lo < 0xe000) {
            cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
            p += 6;
          }
        }
        if (cp < 0x80) *w++ = (char)cp;
        else if (cp < 0x800) *w++ = (char)(0xc0 | cp >> 6), *w++ = (char)(0x80 | (cp & 0x3f));
        else if (cp < 0x10000)
          *w++ = (char)(0xe0 | cp >> 12), *w++ = (char)(0x80 | ((cp >> 6) & 0x3f)), *w++ = (char)(0x80 | (cp & 0x3f));
        else
          *w++ = (char)(0xf0 | cp >> 18), *w++ = (char)(0x80 | ((cp >> 12) & 0x3f)),
          *w++ = (char)(0x80 | ((cp >> 6) & 0x3f)), *w++ = (char)(0x80 | (cp & 0x3f));
        break;
      }
      default: return fail(s, "bad escape");
    }
  }
  *w = 0;
  *out = o;
  s->p = p + 1;
  return 0;
}

static int value(ps *s, jv *v);

/* Children are collected in a growable heap array, then copied into the arena. */
static int container(ps *s, jv *v, int obj) {
  if (++s->depth > MAX_DEPTH) return fail(s, "nesting too deep");
  size_t n = 0, cap = 8;
  jv *items = malloc(cap * sizeof *items);
  const char **keys = obj ? malloc(cap * sizeof *keys) : NULL;
  int rc = -1;
  if (!items || (obj && !keys)) {
    fail(s, "out of memory");
    goto done;
  }
  s->p++;
  ws(s);
  if (*s->p == (obj ? '}' : ']')) {
    s->p++;
    rc = 0;
    goto done;
  }
  for (;;) {
    if (n == cap) {
      cap *= 2;
      jv *ni = realloc(items, cap * sizeof *items);
      if (!ni) goto done;
      items = ni;
      if (obj) {
        const char **nk = realloc(keys, cap * sizeof *keys);
        if (!nk) goto done;
        keys = nk;
      }
    }
    ws(s);
    if (obj) {
      if (*s->p != '"' || string(s, &keys[n])) {
        fail(s, "expected a key");
        goto done;
      }
      ws(s);
      if (*s->p != ':') {
        fail(s, "expected ':'");
        goto done;
      }
      s->p++;
      ws(s);
    }
    if (value(s, &items[n])) goto done;
    n++;
    ws(s);
    if (*s->p == ',') {
      s->p++;
      continue;
    }
    if (*s->p == (obj ? '}' : ']')) {
      s->p++;
      rc = 0;
      break;
    }
    fail(s, "expected ',' or a closing bracket");
    goto done;
  }
done:
  if (rc == 0) {
    v->t = obj ? JV_OBJ : JV_ARR;
    v->n = n;
    v->items = n ? arena(s->d, n * sizeof *items) : NULL;
    if (n && !v->items) rc = fail(s, "out of memory");
    else if (n) memcpy(v->items, items, n * sizeof *items);
    if (obj && n && rc == 0) {
      v->keys = arena(s->d, n * sizeof *keys);
      if (!v->keys) rc = fail(s, "out of memory");
      else memcpy(v->keys, keys, n * sizeof *keys);
    }
  }
  free(items);
  free(keys);
  s->depth--;
  return rc;
}

static int value(ps *s, jv *v) {
  memset(v, 0, sizeof *v);
  ws(s);
  char c = *s->p;
  if (c == '{') return container(s, v, 1);
  if (c == '[') return container(s, v, 0);
  if (c == '"') {
    v->t = JV_STR;
    return string(s, &v->str);
  }
  if (!strncmp(s->p, "true", 4)) return v->t = JV_BOOL, v->b = 1, s->p += 4, 0;
  if (!strncmp(s->p, "false", 5)) return v->t = JV_BOOL, v->b = 0, s->p += 5, 0;
  if (!strncmp(s->p, "null", 4)) return v->t = JV_NULL, s->p += 4, 0;
  char *end;
  v->num = strtod(s->p, &end);
  if (end == s->p) return fail(s, "unexpected character");
  v->t = JV_NUM;
  s->p = end;
  return 0;
}

jdoc *jdoc_parse(const char *text, char *err, size_t errlen) {
  if (err && errlen) err[0] = 0;
  jdoc *d = calloc(1, sizeof *d);
  if (!d) return NULL;
  ps s = {text, d, err, errlen, 0};
  if (value(&s, &d->root) == 0) {
    ws(&s);
    if (!*s.p) return d;
    fail(&s, "trailing characters");
  }
  jdoc_free(d);
  return NULL;
}

const jv *jdoc_root(const jdoc *d) { return &d->root; }

void jdoc_free(jdoc *d) {
  if (!d) return;
  for (block *b = d->blocks, *n; b; b = n) {
    n = b->next;
    free(b);
  }
  free(d);
}

const jv *jv_get(const jv *o, const char *key) {
  if (!o || o->t != JV_OBJ) return NULL;
  for (size_t i = 0; i < o->n; i++)
    if (!strcmp(o->keys[i], key)) return &o->items[i];
  return NULL;
}

const jv *jv_path(const jv *v, const char *dotted) {
  char key[128];
  while (v && *dotted) {
    size_t len = strcspn(dotted, ".");
    if (len >= sizeof key) return NULL;
    memcpy(key, dotted, len);
    key[len] = 0;
    v = jv_get(v, key);
    dotted += len;
    if (*dotted == '.') dotted++;
  }
  return v;
}

double jv_num(const jv *v, double def) { return v && v->t == JV_NUM ? v->num : def; }
const char *jv_str(const jv *v, const char *def) { return v && v->t == JV_STR ? v->str : def; }
int jv_bool(const jv *v, int def) { return v && v->t == JV_BOOL ? v->b : def; }
size_t jv_len(const jv *v) { return v && (v->t == JV_ARR || v->t == JV_OBJ) ? v->n : 0; }
