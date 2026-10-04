/* Copyright 2026 The Prismark Authors. Apache-2.0. */
#include "buf.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int grow(pmk_buf *b, size_t need) {
  if (b->oom) return -1;
  if (b->len + need + 1 <= b->cap) return 0;
  size_t cap = b->cap ? b->cap : 256;
  while (cap < b->len + need + 1) cap *= 2;
  char *p = realloc(b->p, cap);
  if (!p) {
    b->oom = 1;
    return -1;
  }
  b->p = p;
  b->cap = cap;
  return 0;
}

void buf_putn(pmk_buf *b, const char *s, size_t n) {
  if (grow(b, n)) return;
  memcpy(b->p + b->len, s, n);
  b->len += n;
  b->p[b->len] = 0;
}

void buf_puts(pmk_buf *b, const char *s) { buf_putn(b, s, strlen(s)); }

void buf_putc(pmk_buf *b, char c) { buf_putn(b, &c, 1); }

void buf_printf(pmk_buf *b, const char *fmt, ...) {
  va_list ap, ap2;
  va_start(ap, fmt);
  va_copy(ap2, ap);
  int n = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  if (n >= 0 && !grow(b, (size_t)n)) {
    vsnprintf(b->p + b->len, (size_t)n + 1, fmt, ap2);
    b->len += (size_t)n;
  }
  va_end(ap2);
}

char *buf_take(pmk_buf *b) {
  char *p = b->oom ? NULL : b->p;
  if (b->oom) free(b->p);
  else if (!p) p = calloc(1, 1);
  b->p = NULL;
  b->len = b->cap = 0;
  b->oom = 0;
  return p;
}

void buf_free(pmk_buf *b) {
  free(b->p);
  b->p = NULL;
  b->len = b->cap = 0;
  b->oom = 0;
}
