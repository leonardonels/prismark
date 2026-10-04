/* Copyright 2026 The Prismark Authors. Apache-2.0. */
#include "json.h"

#include <inttypes.h>
#include <math.h>

static void escape(pmk_buf *b, const char *s) {
  buf_putc(b, '"');
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    switch (*p) {
      case '"': buf_puts(b, "\\\""); break;
      case '\\': buf_puts(b, "\\\\"); break;
      case '\n': buf_puts(b, "\\n"); break;
      case '\r': buf_puts(b, "\\r"); break;
      case '\t': buf_puts(b, "\\t"); break;
      default:
        if (*p < 0x20) buf_printf(b, "\\u%04x", *p);
        else buf_putc(b, (char)*p);
    }
  }
  buf_putc(b, '"');
}

static void sep(pmk_jw *w, const char *key) {
  if (w->depth > 0) {
    if (w->nonempty[w->depth - 1]) buf_putc(&w->b, ',');
    w->nonempty[w->depth - 1] = 1;
  }
  if (key) {
    escape(&w->b, key);
    buf_putc(&w->b, ':');
  }
}

static void push(pmk_jw *w, char c) {
  buf_putc(&w->b, c);
  if (w->depth < PMK_JW_MAX_DEPTH) w->nonempty[w->depth] = 0;
  else w->b.oom = 1; /* nesting too deep: poison the output */
  w->depth++;
}

static void pop(pmk_jw *w, char c) {
  if (w->depth > 0) w->depth--;
  buf_putc(&w->b, c);
}

void jw_obj_begin(pmk_jw *w, const char *key) { sep(w, key); push(w, '{'); }
void jw_obj_end(pmk_jw *w) { pop(w, '}'); }
void jw_arr_begin(pmk_jw *w, const char *key) { sep(w, key); push(w, '['); }
void jw_arr_end(pmk_jw *w) { pop(w, ']'); }

void jw_str(pmk_jw *w, const char *key, const char *v) {
  sep(w, key);
  if (v) escape(&w->b, v);
  else buf_puts(&w->b, "null");
}

void jw_num(pmk_jw *w, const char *key, double v) {
  sep(w, key);
  if (isfinite(v)) buf_printf(&w->b, "%.10g", v);
  else buf_puts(&w->b, "null");
}

void jw_int(pmk_jw *w, const char *key, int64_t v) {
  sep(w, key);
  buf_printf(&w->b, "%" PRId64, v);
}

void jw_bool(pmk_jw *w, const char *key, int v) {
  sep(w, key);
  buf_puts(&w->b, v ? "true" : "false");
}

void jw_null(pmk_jw *w, const char *key) {
  sep(w, key);
  buf_puts(&w->b, "null");
}

void jw_raw(pmk_jw *w, const char *key, const char *json) {
  sep(w, key);
  buf_puts(&w->b, json);
}
