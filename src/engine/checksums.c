/* Kernel output checksums for cross-ISA verification. Copyright 2026 The Prismark Authors. Apache-2.0. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "buf.h"
#include "ctx.h"

static int selected(const char *list, const char *id) {
  if (!list || !*list) return 1;
  size_t n = strlen(id);
  for (const char *p = list; *p;) {
    size_t len = strcspn(p, ",");
    if (len == n && !strncmp(p, id, n)) return 1;
    p += len;
    if (*p == ',') p++;
  }
  return 0;
}

static void tier_checksums(const pmk_kernels *k, const char *k1_data, const char *kernels, int burst_only, pmk_jw *w,
                           pmk_buf *t) {
  for (size_t i = 0; i < k->ntk; i++) {
    const pmk_tk *tk = k->tk[i];
    if (!selected(kernels, tk->id)) continue;
    for (int size = PMK_SIZE_BURST; size <= (burst_only ? PMK_SIZE_BURST : PMK_SIZE_FULL); size++) {
      char err[256] = "", label[32];
      pmk_tk_args a = {size, k1_data, err, sizeof err};
      snprintf(label, sizeof label, "%s%s%s", tk->id, tk->variant ? " " : "", tk->variant ? tk->variant : "");
      jw_obj_begin(w, NULL);
      jw_str(w, "kernel", tk->id);
      jw_str(w, "variant", tk->variant);
      jw_str(w, "size", size == PMK_SIZE_BURST ? "burst" : "full");
      jw_str(w, "tier", k->tier);
      jw_str(w, "isa_level", k->level);
      void *inst = tk->create(&a);
      if (!inst) {
        jw_str(w, "unavailable", err);
        buf_printf(t, "  %-10s %-5s %-9s unavailable: %s\n", label, size ? "full" : "burst", k->tier, err);
        jw_obj_end(w);
        continue;
      }
      void *scratch = tk->scratch_new ? tk->scratch_new(inst) : NULL;
      uint64_t in = tk->input_hash(inst), out = 0, again = 0;
      if (!tk->scratch_new || scratch) {
        out = wl_job(tk, inst, scratch);
        again = wl_job(tk, inst, scratch); /* a second job must reproduce the first */
      }
      char hin[19], hout[19];
      snprintf(hin, sizeof hin, "%016llx", (unsigned long long)in);
      snprintf(hout, sizeof hout, "%016llx", (unsigned long long)out);
      jw_str(w, "input_hash", hin);
      jw_str(w, "checksum", hout);
      jw_bool(w, "repeatable", out == again);
      jw_obj_end(w);
      buf_printf(t, "  %-10s %-5s %-9s input %s  output %s%s\n", label, size ? "full" : "burst", k->tier, hin, hout,
                 out == again ? "" : "  NOT REPEATABLE");
      if (scratch && tk->scratch_free) tk->scratch_free(scratch);
      tk->destroy(inst);
    }
  }
}

int pmk_checksums(const char *k1_data, char **report_json, char **report_text) {
  return pmk_checksums_ex(k1_data, NULL, 0, report_json, report_text);
}

int pmk_checksums_ex(const char *k1_data, const char *kernels, int burst_only, char **report_json, char **report_text) {
  pmk_jw w = {0};
  pmk_buf t = {0};
  const pmk_kernels *kmax = NULL;
  char status[400] = "this build has no max-level tiers";
#ifdef PMK_MAX_TIERS
  static const char *const tiers[] = {PMK_MAX_TIERS};
  static const char *const levels[] = {PMK_MAX_LEVELS};
  snprintf(status, sizeof status, "the CPU supports no max-level tier");
  for (int i = (int)(sizeof tiers / sizeof *tiers) - 1; i >= 0 && !kmax; i--)
    if (pal_isa_supported(levels[i]) && !(kmax = pal_load_tier(tiers[i], status, sizeof status))) break;
  if (kmax) snprintf(status, sizeof status, "%s", kmax->level);
#endif
  jw_obj_begin(&w, NULL);
  jw_str(&w, "schema", "prismark-checksums/1");
  jw_str(&w, "version", pmk_version());
  jw_str(&w, "baseline_level", pmk_kernels_baseline.level);
  jw_str(&w, "max_tier", status);
  jw_arr_begin(&w, "kernels");
  buf_printf(&t, "Prismark %s kernel checksums (baseline %s; max tier: %s)\n", pmk_version(), pmk_kernels_baseline.level,
             status);
  tier_checksums(&pmk_kernels_baseline, k1_data, kernels, burst_only, &w, &t);
  if (kmax) tier_checksums(kmax, k1_data, kernels, burst_only, &w, &t);
  jw_arr_end(&w);
  jw_obj_end(&w);
  if (report_json) *report_json = buf_take(&w.b);
  else buf_free(&w.b);
  if (report_text) *report_text = buf_take(&t);
  else buf_free(&t);
  return PMK_OK;
}
