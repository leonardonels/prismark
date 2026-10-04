/* Minimal JSON reader (DOM), used to compare result documents.
 * Copyright 2026 The Prismark Authors. Apache-2.0. */
#ifndef PMK_JPARSE_H
#define PMK_JPARSE_H

#include <stddef.h>

typedef enum { JV_NULL, JV_BOOL, JV_NUM, JV_STR, JV_ARR, JV_OBJ } jv_type;

typedef struct jv {
  jv_type t;
  int b;
  double num;
  const char *str;   /* JV_STR, NUL-terminated (embedded NULs are not supported) */
  size_t n;          /* JV_ARR / JV_OBJ: number of children */
  struct jv *items;  /* children */
  const char **keys; /* JV_OBJ: key of each child */
} jv;

typedef struct jdoc jdoc;

/* Parses text; NULL on error with a message in err. */
jdoc *jdoc_parse(const char *text, char *err, size_t errlen);
const jv *jdoc_root(const jdoc *d);
void jdoc_free(jdoc *d);

const jv *jv_get(const jv *obj, const char *key); /* NULL if absent or obj is not an object */
const jv *jv_path(const jv *v, const char *dotted); /* e.g. "machine.capabilities.pinning" */
double jv_num(const jv *v, double def);
const char *jv_str(const jv *v, const char *def);
int jv_bool(const jv *v, int def);
size_t jv_len(const jv *v);

#endif
