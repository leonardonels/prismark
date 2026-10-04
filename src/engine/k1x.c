/*
 * K1x — full build. The prepared snapshot (tools/k1x/prepare.py) holds the
 * same translation units that K1 compiles in-process, with pre-generated
 * sources, a pinned aarch64 sysroot and a generated CMake project. Each
 * measurement is one `ninja -j n` from clean in a RAM-backed copy of the
 * tree, with the build processes confined to the same n CPUs that K1's
 * threads use, so R_build = T_K1x(n) / T_K1(n) isolates process creation,
 * file-system I/O, linking and build scheduling.
 *
 * Configuring (cmake) and cleaning are not timed.
 *
 * Copyright 2026 The Prismark Authors. Apache-2.0.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "ctx.h"
#include "jparse.h"

static int exists(const char *path) {
  struct stat st;
  return stat(path, &st) == 0;
}

static int tool_ok(const char *tool) {
  const char *argv[] = {tool, "--version", NULL};
  return pal_run(argv, NULL, NULL, NULL) == 0;
}

/*
 * Quick runs build only the object files of every PMK_K1_QUICK_STRIDE-th unit in largest-first order
 * (the manifest order), the same units K1 compiles in a quick run. CMake names the object of an
 * absolute source path CMakeFiles/<target>.dir<path>.o. Returns a NULL-terminated argv tail, or NULL.
 */
static char **quick_targets(const char *data, const char *root, int *count) {
  char path[1400], err[160];
  snprintf(path, sizeof path, "%s/manifest.json", data);
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long len = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *text = malloc((size_t)len + 1);
  if (!text || fread(text, 1, (size_t)len, f) != (size_t)len) {
    fclose(f);
    free(text);
    return NULL;
  }
  fclose(f);
  text[len] = 0;
  jdoc *d = jdoc_parse(text, err, sizeof err);
  free(text);
  if (!d) return NULL;
  const jv *units = jv_get(jdoc_root(d), "units");
  const char *vroot = jv_str(jv_get(jdoc_root(d), "root"), "/k1x");
  size_t n = jv_len(units), k = 0;
  char **out = calloc(n / PMK_K1_QUICK_STRIDE + 2, sizeof *out);
  for (size_t i = 0; out && i < n; i += PMK_K1_QUICK_STRIDE) {
    const jv *u = &units->items[i];
    const char *file = jv_str(jv_get(u, "file"), ""), *target = jv_str(jv_get(u, "target"), "");
    size_t vl = strlen(vroot);
    if (strncmp(file, vroot, vl)) continue;
    char obj[1600];
    snprintf(obj, sizeof obj, "CMakeFiles/%s.dir%s%s.o", target, root, file + vl);
    out[k++] = strdup(obj);
  }
  jdoc_free(d);
  *count = (int)k;
  return out;
}

int k1x_run(pmk_ctx *c, const int *steps, int nsteps, const int *order) {
  const char *data = c->cfg.k1_data;
  char path[1400], dir[1024], root[1200], src[1200], out[1200], log[1200], buf[64];
  const char *why = NULL;
  if (!data || !*data) why = "no K1/K1x snapshot given (prepare one with tools/k1x/prepare.py, pass --k1-data)";
  if (!why) {
    snprintf(path, sizeof path, "%s/build/CMakeLists.txt", data);
    if (!exists(path)) why = "snapshot has no build/CMakeLists.txt";
  }
  if (!why && (!tool_ok("cmake") || !tool_ok("ninja"))) why = "cmake and ninja are needed in PATH";
  int ram = 0;
  if (!why && pal_scratch_dir(dir, sizeof dir, &ram)) why = "no writable scratch directory";
  if (why) {
    ctx_unavailable(c, "K1x", NULL, "mc_threaded", why);
    return PMK_OK;
  }

  size_t len = strlen(dir);
  snprintf(dir + len, sizeof dir - len, "/prismark-k1x-%016llx", (unsigned long long)c->seed);
  snprintf(root, sizeof root, "%s/snap/root", dir);
  snprintf(src, sizeof src, "%s/snap/build", dir);
  snprintf(out, sizeof out, "%s/out", dir);
  snprintf(log, sizeof log, "%s.log", dir);
  pal_remove_tree(dir);

  jw_obj_begin(&c->k1x, NULL);
  jw_str(&c->k1x, "snapshot", data);
  jw_str(&c->k1x, "scratch", dir);
  jw_bool(&c->k1x, "ram_backed", ram);
  jw_str(&c->k1x, "log", log);

  ctx_emit(c, PMK_EV_PHASE, "mc_threaded", "K1x", 0, 0, "copying the snapshot to %s", dir);
  int rc = PMK_OK;
#ifdef _WIN32
  const char *mkdir_argv[] = {"cmd", "/c", "mkdir", dir, NULL};
#else
  const char *mkdir_argv[] = {"mkdir", "-p", dir, NULL};
#endif
  snprintf(path, sizeof path, "%s/snap", dir);
  /* The whole snapshot: root/ (sources, headers), link-sysroot/ (crt, libraries), build/ (project). */
  if (pal_run(mkdir_argv, NULL, NULL, NULL) || pal_copy_tree(data, path)) {
    why = "could not copy the snapshot to the scratch directory";
  } else {
    char rootdef[1300], tc[1300];
    snprintf(rootdef, sizeof rootdef, "-DK1X_ROOT=%s", root);
    snprintf(tc, sizeof tc, "-DCMAKE_TOOLCHAIN_FILE=%s/toolchain.cmake", src);
    const char *configure[] = {"cmake", "-G", "Ninja", "-S", src, "-B", out, rootdef, tc, "-DCMAKE_BUILD_TYPE=Release",
                               NULL};
    ctx_emit(c, PMK_EV_PHASE, "mc_threaded", "K1x", 0, 0, "configuring (not timed)");
    if (pal_run(configure, NULL, log, ctx_cancelled)) why = "cmake configure failed (see the log)";
  }
  jw_str(&c->k1x, "error", why);
  jw_obj_end(&c->k1x);
  if (why) {
    ctx_unavailable(c, "K1x", NULL, "mc_threaded", why);
    pal_remove_tree(dir);
    return PMK_OK;
  }

  int *cpus = malloc((size_t)c->m.ncpu * sizeof *cpus);
  if (!cpus) return PMK_ERR_NOMEM;
  ctx_cpu_order(c, cpus);
  int reps = c->cfg.k1x_reps > 0 ? c->cfg.k1x_reps : 3;
  int nquick = 0;
  char **quick = c->cfg.quick_inputs ? quick_targets(data, root, &nquick) : NULL;
  if (c->cfg.quick_inputs && !quick) {
    ctx_unavailable(c, "K1x", NULL, "mc_threaded", "cannot read the snapshot manifest for the quick build");
    free(cpus);
    pal_remove_tree(dir);
    return PMK_OK;
  }
  for (int si = 0; si < nsteps && rc == PMK_OK; si++) {
    int n = steps[order[si]];
    ctx_cooldown(c, "mc_threaded");
    pmk_result *r = ctx_new_result(c, "K1x", "mc_threaded", "ns");
    if (!r) {
      rc = PMK_ERR_NOMEM;
      break;
    }
    r->k = c->k;
    r->type = ctx_type_of_cpu(c, cpus[0]);
    r->cpu = -1;
    r->nthreads = n;
    r->size = quick ? "burst" : "full";
    snprintf(buf, sizeof buf, "-j%d", n);
    const char *clean[] = {"ninja", "-C", out, "-t", "clean", NULL};
    const char *full_build[] = {"ninja", "-C", out, buf, NULL};
    const char **build = full_build;
    const char **quick_build = NULL;
    if (quick) { /* ninja -C out -jN <objects...> */
      quick_build = calloc((size_t)nquick + 5, sizeof *quick_build);
      if (!quick_build) {
        rc = PMK_ERR_NOMEM;
        break;
      }
      quick_build[0] = "ninja";
      quick_build[1] = "-C";
      quick_build[2] = out;
      quick_build[3] = buf;
      for (int q = 0; q < nquick; q++) quick_build[4 + q] = quick[q];
      build = quick_build;
    }
    for (int rep = 0; rep < reps && rc == PMK_OK; rep++) {
      ctx_emit(c, PMK_EV_PHASE, "mc_threaded", "K1x", (uint32_t)(si * reps + rep + 1), (uint32_t)(nsteps * reps),
               "full build, -j%d, repetition %d/%d", n, rep + 1, reps);
      if (pal_run(clean, NULL, log, NULL)) {
        ctx_unavailable(c, "K1x", NULL, "mc_threaded", "ninja clean failed (see the log)");
        rc = PMK_ERR_SYSTEM;
        break;
      }
      if (ctx_can_place(c)) pal_pin_self_set(cpus, n);
      uint64_t t0 = pal_now_raw_ns();
      int st = pal_run(build, NULL, log, ctx_cancelled);
      uint64_t t1 = pal_now_raw_ns();
      pal_unpin_self();
      if (ctx_cancelled()) rc = PMK_ERR_CANCELLED;
      else if (st) {
        ctx_unavailable(c, "K1x", NULL, "mc_threaded", "build failed (see the log)");
        rc = PMK_ERR_SYSTEM;
      } else if (result_push(r, (double)(t1 - t0), NAN, -1)) {
        rc = PMK_ERR_NOMEM;
      }
    }
    free(quick_build);
  }
  for (int q = 0; quick && q < nquick; q++) free(quick[q]);
  free(quick);
  free(cpus);
  pal_remove_tree(dir);
  /* A failed build makes K1x unavailable; it does not abort the other kernels. */
  return rc == PMK_ERR_SYSTEM ? PMK_OK : rc;
}
