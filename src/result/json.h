/* Minimal streaming JSON writer. Copyright 2026 The Prismark Authors. Apache-2.0. */
#ifndef PMK_JSON_H
#define PMK_JSON_H

#include <stdint.h>

#include "buf.h"

#define PMK_JW_MAX_DEPTH 32

/* Every call takes a key: pass a name inside objects, NULL inside arrays. */
typedef struct pmk_jw {
  pmk_buf b;
  int depth;
  unsigned char nonempty[PMK_JW_MAX_DEPTH];
} pmk_jw;

void jw_obj_begin(pmk_jw *w, const char *key);
void jw_obj_end(pmk_jw *w);
void jw_arr_begin(pmk_jw *w, const char *key);
void jw_arr_end(pmk_jw *w);
void jw_str(pmk_jw *w, const char *key, const char *v); /* NULL -> null */
void jw_num(pmk_jw *w, const char *key, double v);      /* non-finite -> null */
void jw_int(pmk_jw *w, const char *key, int64_t v);
void jw_bool(pmk_jw *w, const char *key, int v);
void jw_null(pmk_jw *w, const char *key);
/* Inserts an already-serialized JSON value. */
void jw_raw(pmk_jw *w, const char *key, const char *json);

#endif
