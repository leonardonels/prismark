/*
 * Display names and result descriptions for front-ends: the names the desktop
 * app uses for kernels and modes, and what a front-end lists about a stored
 * result without reading its samples.
 *
 * Copyright 2026 The Prismark Authors. Apache-2.0.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jparse.h"
#include "prismark/prismark.h"

static const struct {
  const char *id, *name;
} NAMES[] = {
    {"K1", "Compiling code"},       {"K1x", "Full software build"},
    {"K2", "3D rendering"},         {"K3", "Compression"},
    {"K4", "Opening a photo"},      {"K5", "Reading JSON"},
    {"K6", "Starting a script"},    {"K7", "Memory delay"},
    {"K8", "Matrix maths"},         {"K9", "Wake-up test"},
    {"K10", "Timer punctuality"},   {"st_burst", "one core, short task"},
    {"st_sustained", "one core, after warming up"},
    {"mc_threaded", "all cores, working together"},
    {"mc_instances", "all cores, each on its own"},
    {"cold_burst", "started from rest"},
    {"periodic", "timer punctuality"},
};

const char *pmk_display_name(const char *id) {
  if (!id) return "";
  for (size_t i = 0; i < sizeof NAMES / sizeof *NAMES; i++)
    if (!strcmp(NAMES[i].id, id)) return NAMES[i].name;
  return id;
}

static const struct {
  const char *name;
  uint32_t bit;
} MODE_BITS[] = {
    {"cold_burst", PMK_MODE_COLD_BURST},   {"periodic", PMK_MODE_PERIODIC},
    {"st_burst", PMK_MODE_ST_BURST},       {"st_sustained", PMK_MODE_ST_SUSTAINED},
    {"mc_threaded", PMK_MODE_MC_THREADED}, {"mc_instances", PMK_MODE_MC_INSTANCES},
};

static void copy(char *out, size_t n, const char *s) { snprintf(out, n, "%s", s ? s : ""); }

int pmk_describe(const char *result_json, pmk_brief *brief, char **summary_text) {
  if (summary_text) *summary_text = NULL;
  if (!result_json || !brief || brief->struct_size < sizeof *brief) return PMK_ERR_INVALID;
  memset((char *)brief + sizeof brief->struct_size, 0, sizeof *brief - sizeof brief->struct_size);
  char err[160];
  jdoc *d = jdoc_parse(result_json, err, sizeof err);
  if (!d) return PMK_ERR_INVALID;
  const jv *root = jdoc_root(d);
  const char *schema = jv_str(jv_get(root, "schema"), "");
  if (strncmp(schema, "prismark/", 9)) {
    jdoc_free(d);
    return PMK_ERR_INVALID;
  }
  copy(brief->run_id, sizeof brief->run_id, jv_str(jv_get(root, "run_id"), ""));
  copy(brief->started_utc, sizeof brief->started_utc, jv_str(jv_get(root, "started_utc"), ""));
  copy(brief->model, sizeof brief->model, jv_str(jv_path(root, "machine.cpu.model"), ""));
  copy(brief->frontend, sizeof brief->frontend, jv_str(jv_path(root, "frontend.kind"), ""));
  const jv *quick = jv_path(root, "config.quick");
  brief->quick = quick ? jv_bool(quick, 0) : -1;
  brief->complete = jv_bool(jv_get(root, "complete"), 0);
  const jv *res = jv_get(root, "results");
  for (size_t i = 0; res && res->t == JV_ARR && i < res->n; i++) {
    const char *mode = jv_str(jv_get(&res->items[i], "mode"), "");
    for (size_t k = 0; k < sizeof MODE_BITS / sizeof *MODE_BITS; k++)
      if (!strcmp(mode, MODE_BITS[k].name)) brief->modes |= MODE_BITS[k].bit;
  }
  int rc = PMK_OK;
  const char *summary = jv_str(jv_get(root, "summary"), NULL);
  if (summary_text && summary && !(*summary_text = strdup(summary))) rc = PMK_ERR_NOMEM;
  jdoc_free(d);
  return rc;
}
