/*
 * K1x — full build. The prepared snapshot (tools/k1x/prepare.py) holds the
 * same translation units that K1 compiles in-process, with pre-generated
 * sources, a pinned aarch64 sysroot and a generated CMake project. Each
 * measurement is one `ninja -j n` of a fixed subset of the units (every 8th
 * in largest-first order; 3 builds in full runs, 1 in quick) from clean in a RAM-backed
 * copy of the tree, with the build processes confined to the same n CPUs that
 * K1's threads use. R_build compares it with K1's time for the same share of
 * the work, isolating process creation, file-system I/O and build scheduling.
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
 * The object files of every stride-th unit in largest-first order (the manifest order): quick runs build every
 * PMK_K1_QUICK_STRIDE-th (the units K1 compiles in a quick run), full runs every PMK_K1X_FULL_STRIDE-th. CMake
 * names the object of an absolute source path CMakeFiles/<target>.dir<path>.o. Returns a NULL-terminated argv
 * tail, or NULL; *fraction is the share of the whole snapshot's compile cost (cost_us) these units carry.
 */
static char **unit_targets(const char *data, const char *root, size_t stride, int *count, double *fraction) {
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
  char **out = calloc(n / stride + 2, sizeof *out);
  double all = 0, mine = 0;
  for (size_t i = 0; i < n; i++) {
    double cost = jv_num(jv_get(&units->items[i], "cost_us"), 1);
    all += cost;
    if (i % stride == 0) mine += cost;
  }
  *fraction = all > 0 ? mine / all : 0;
  for (size_t i = 0; out && i < n; i += stride) {
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
  double fraction = 0;
  char **quick = unit_targets(data, root, c->cfg.quick_inputs ? PMK_K1_QUICK_STRIDE : PMK_K1X_FULL_STRIDE, &nquick,
                              &fraction);
  if (!quick) {
    ctx_unavailable(c, "K1x", NULL, "mc_threaded", "cannot read the snapshot manifest");
    free(cpus);
    pal_remove_tree(dir);
    return PMK_OK;
  }
  for (int si = 0; si < nsteps && rc == PMK_OK; si++) {
    int n = steps[order[si]];
    /* ninja -j n runs n compilers at once, each as large as one K1 thread. */
    if (!ctx_memory_allows(c, "K1x", n, PMK_COMPILE_MEM_PER_THREAD)) continue;
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
    r->size = c->cfg.quick_inputs ? "burst" : "full";
    r->work_fraction = fraction;
    snprintf(buf, sizeof buf, "-j%d", n);
    const char *clean[] = {"ninja", "-C", out, "-t", "clean", NULL};
    /* ninja -C out -jN <objects...>: the selected units only */
    const char **build = calloc((size_t)nquick + 5, sizeof *build);
    if (!build) {
      rc = PMK_ERR_NOMEM;
      break;
    }
    build[0] = "ninja";
    build[1] = "-C";
    build[2] = out;
    build[3] = buf;
    for (int q = 0; q < nquick; q++) build[4 + q] = quick[q];
    for (int rep = 0; rep < reps && rc == PMK_OK; rep++) {
      ctx_emit(c, PMK_EV_PHASE, "mc_threaded", "K1x", (uint32_t)(si * reps + rep + 1), (uint32_t)(nsteps * reps),
               "build of %d units, -j%d, repetition %d/%d", nquick, n, rep + 1, reps);
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
    free(build);
  }
  for (int q = 0; quick && q < nquick; q++) free(quick[q]);
  free(quick);
  free(cpus);
  pal_remove_tree(dir);
  /* A failed build makes K1x unavailable; it does not abort the other kernels. */
  return rc == PMK_ERR_SYSTEM ? PMK_OK : rc;
}
