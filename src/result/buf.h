/* Growable string buffer. Copyright 2026 The Prismark Authors. Apache-2.0. */
#ifndef PMK_BUF_H
#define PMK_BUF_H

#include <stddef.h>

typedef struct pmk_buf {
  char *p;
  size_t len, cap;
  int oom; /* sticky: set once an allocation fails */
} pmk_buf;

void buf_putn(pmk_buf *b, const char *s, size_t n);
void buf_puts(pmk_buf *b, const char *s);
void buf_putc(pmk_buf *b, char c);
void buf_printf(pmk_buf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/* Returns the heap string (never NULL unless out of memory) and resets b. */
char *buf_take(pmk_buf *b);
void buf_free(pmk_buf *b);

#endif
