/*
 * Runner: preflight, randomized mode order with cool-downs, machine state at
 * start and end, and the result document.
 *
 * Copyright 2026 The Prismark Authors. Apache-2.0.
 */
#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ctx.h"
#include "pmk_build_info.h"
#include "stats.h"

#ifndef PMK_VERSION
#define PMK_VERSION "0.0.0"
#endif
#ifndef PMK_GIT_SHA
#define PMK_GIT_SHA "unknown"
#endif

#define SCHEMA "prismark/1"

static atomic_int g_cancel;
static atomic_int g_input;
static atomic_flag g_running = ATOMIC_FLAG_INIT;

const char *pmk_version(void) { return PMK_VERSION; }

const char *pmk_strerror(int err) {
  switch (err) {
    case PMK_OK: return "ok";
    case PMK_ERR_INVALID: return "invalid configuration";
    case PMK_ERR_CANCELLED: return "cancelled";
    case PMK_ERR_BUSY: return "machine is not idle enough (background load above threshold)";
    case PMK_ERR_SYSTEM: return "platform error";
    case PMK_ERR_NOMEM: return "out of memory";
    case PMK_ERR_RUNNING: return "a run is already in progress";
    default: return "unknown error";
  }
}

void pmk_cancel(void) { atomic_store(&g_cancel, 1); }
void pmk_notify_input(void) { atomic_store(&g_input, 1); }

int ctx_interrupt(pmk_ctx *c) {
  if (ctx_cancelled()) return PMK_ERR_CANCELLED;
  if (!c->quiet) return PMK_OK;
  if (atomic_load_explicit(&g_input, memory_order_relaxed)) return PMK_SKIP_INPUT;
  int64_t idle = pal_input_idle_ms();
  if (idle >= 0 && (uint64_t)idle * 1000000ull < pal_now_ns() - c->quiet_since) {
    atomic_store(&g_input, 1);
    return PMK_SKIP_INPUT;
  }
  return PMK_OK;
}
int ctx_cancelled(void) { return atomic_load_explicit(&g_cancel, memory_order_relaxed); }
void ctx_uncancel(void) { atomic_store(&g_cancel, 0); }
void pmk_free(void *p) { free(p); }

void pmk_config_init(pmk_config *cfg) {
  memset(cfg, 0, sizeof *cfg);
  cfg->struct_size = sizeof *cfg;
  cfg->modes = PMK_MODE_ALL;
  cfg->cpu = -1;
  cfg->min_reps = 20;
  cfg->max_reps = 100;
  cfg->cold_max_reps = 300;
  cfg->periodic_period_ms = 1.0;
  cfg->periodic_seconds = 60.0;
  cfg->max_background_load = 0.05;
  cfg->frontend = "cli";
  cfg->ui_state = "none";
  cfg->window_ms = 1000;
  cfg->warmup_s = 60;
  cfg->settle_s = 5;
  cfg->measure_s = 20;
  cfg->k1x_reps = 3;
  cfg->cooldown = 0;
  cfg->stress_s = 600;
  cfg->stress_parts = PMK_STRESS_ALL;
  cfg->stress_stop_on_error = 1;
  cfg->stress_max_temp_c = 0;
}

/* ---------- context helpers ---------- */

int dvec_push(pmk_dvec *d, double x) {
  if (d->n == d->cap) {
    size_t cap = d->cap ? d->cap * 2 : 64;
    double *v = realloc(d->v, cap * sizeof *v);
    if (!v) return -1;
    d->v = v;
    d->cap = cap;
  }
  d->v[d->n++] = x;
  return 0;
}

void dvec_free(pmk_dvec *d) {
  free(d->v);
  d->v = NULL;
  d->n = d->cap = 0;
}

void ctx_unavailable(pmk_ctx *c, const char *kernel, const char *variant, const char *mode, const char *reason) {
  jw_obj_begin(&c->avail, NULL);
  jw_str(&c->avail, "kernel", kernel);
  jw_str(&c->avail, "variant", variant);
  jw_str(&c->avail, "mode", mode);
  jw_str(&c->avail, "reason", reason);
  jw_obj_end(&c->avail);
  ctx_emit(c, PMK_EV_WARNING, mode, kernel, 0, 0, "%s%s%s unavailable in %s: %s", kernel, variant ? " " : "",
           variant ? variant : "", mode, reason);
}

/*
 * Whether n threads of a kernel that needs per_thread bytes each fit in the memory available now. When they do
 * not, the thread count is recorded as unavailable (variant "n=<n>") instead of being run: the OS would end the
 * whole run when memory runs out. Unknown available memory allows it.
 */
int ctx_memory_allows(pmk_ctx *c, const char *kernel, int n, uint64_t per_thread) {
  uint64_t avail = pal_mem_available();
  uint64_t need = (uint64_t)n * per_thread;
  if (!avail || need <= avail) return 1;
  char variant[16], reason[160];
  snprintf(variant, sizeof variant, "n=%d", n);
  snprintf(reason, sizeof reason, "not enough memory: %d threads need about %.2f GB, %.2f GB available", n,
           (double)need / 1e9, (double)avail / 1e9);
  ctx_unavailable(c, kernel, variant, "mc_threaded", reason);
  return 0;
}

/*
 * Tests a platform offers at all. The others are left out of its test set rather than reported as
 * unavailable: phones (Android, iPhone) do not compile code. Compiling in process (K1) runs on desktops
 * and iPads; the full build (K1x) also starts other programs, which iPadOS does not allow.
 */
static int kernel_offered(const pmk_ctx *c, const char *id) {
  int phone = !strcmp(c->m.os, "android") || !strcmp(c->m.os, "ios");
  if (!strcmp(id, "K1")) return !phone;
  if (!strcmp(id, "K1x")) return !phone && strcmp(c->m.os, "ipados");
  return 1;
}

int ctx_kernel_selected(const pmk_ctx *c, const char *id) {
  if (!kernel_offered(c, id)) return 0;
  const char *f = c->cfg.kernels;
  if (!f || !*f) return 1;
  size_t n = strlen(id);
  for (const char *p = f; *p;) {
    size_t len = strcspn(p, ",");
    if (len == n && !strncmp(p, id, n)) return 1;
    p += len;
    if (*p == ',') p++;
  }
  return 0;
}

int ctx_type_of_cpu(const pmk_ctx *c, int cpu) {
  for (int i = 0; i < c->m.ncpu; i++)
    if (c->m.cpus[i].id == cpu) return c->m.cpus[i].type;
  return 0;
}

static int cmp_cpu_order(const void *a, const void *b) {
  const pmk_cpu *x = a, *y = b;
  if (x->type != y->type) return x->type - y->type; /* fastest type first */
  if (x->smt != y->smt) return x->smt - y->smt;     /* one thread per core before SMT siblings */
  return x->id - y->id;
}

int ctx_cpu_order(const pmk_ctx *c, int *out) {
  pmk_cpu *tmp = malloc((size_t)c->m.ncpu * sizeof *tmp);
  if (!tmp) {
    for (int i = 0; i < c->m.ncpu; i++) out[i] = c->m.cpus[i].id;
    return c->m.ncpu;
  }
  memcpy(tmp, c->m.cpus, (size_t)c->m.ncpu * sizeof *tmp);
  qsort(tmp, (size_t)c->m.ncpu, sizeof *tmp, cmp_cpu_order);
  for (int i = 0; i < c->m.ncpu; i++) out[i] = tmp[i].id;
  free(tmp);
  return c->m.ncpu;
}

int ctx_thread_steps(const pmk_ctx *c, int *out, int max) {
  int top = c->m.ncpu;
  if (c->cfg.max_threads > 0 && c->cfg.max_threads < top) top = c->cfg.max_threads;
  int n = 0;
  for (int t = 1; t < top && n < max - 1; t *= 2) out[n++] = t;
  out[n++] = top;
  return n;
}

/*
 * Seconds left in the run: the modes still to come at their estimates, and the running mode's estimate less the
 * time it has run. A mode slower than its estimate is extrapolated from its own pace over the steps it finished.
 */
static double ctx_remaining_s(const pmk_ctx *c) {
  if (!c->mode_t0) return NAN;
  double ran = (double)(pal_now_ns() - c->mode_t0) * 1e-9, left = c->mode_est_s - ran;
  uint32_t done = c->run_step > c->mode_step0 + 1 ? c->run_step - c->mode_step0 - 1 : 0;
  if (done && done < c->mode_steps) {
    double pace = ran / done * (c->mode_steps - done);
    if (pace > left) left = pace;
  }
  return c->later_est_s + (left > 0 ? left : 0);
}

void ctx_emit(pmk_ctx *c, pmk_event_kind kind, const char *phase, const char *kernel, uint32_t step,
              uint32_t steps, const char *fmt, ...) {
  if (!c->cb) return;
  char msg[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  double temp = pal_cpu_temp_c();
  /* A numbered step of a mode advances the run-wide position, reported just before it. */
  int advance = kind == PMK_EV_PHASE && step > 0 && c->run_steps > 0 && strcmp(phase, "preflight");
  if (advance && c->run_step < c->run_steps) c->run_step++;
  double left = ctx_remaining_s(c);
  if (advance) {
    char pos[64];
    snprintf(pos, sizeof pos, "step %u of %u", c->run_step, c->run_steps);
    pmk_event p = {sizeof p, PMK_EV_INFO, "progress", NULL, pos, c->run_step, c->run_steps, temp, left};
    c->cb(&p, c->user);
  }
  pmk_event ev = {sizeof ev, kind, phase, kernel, msg, step, steps, temp, left};
  c->cb(&ev, c->user);
}

pmk_result *ctx_new_result(pmk_ctx *c, const char *kernel, const char *mode, const char *unit) {
  if (c->nres == c->capres) {
    size_t cap = c->capres ? c->capres * 2 : 64;
    pmk_result *p = realloc(c->res, cap * sizeof *p);
    if (!p) return NULL;
    c->res = p;
    c->capres = cap;
  }
  pmk_result *r = &c->res[c->nres++];
  memset(r, 0, sizeof *r);
  r->kernel = kernel;
  r->mode = mode;
  r->unit = unit;
  r->k = c->k;
  r->condition = "";
  r->cpu = -1;
  r->tau_s = NAN;
  return r;
}

int result_push(pmk_result *r, double sample, double wake, int cpu) {
  if (r->n == r->cap) {
    size_t cap = r->cap ? r->cap * 2 : 128;
    double *s = realloc(r->samples, cap * sizeof *s);
    if (s) r->samples = s;
    double *w = realloc(r->wake, cap * sizeof *w);
    if (w) r->wake = w;
    int32_t *o = realloc(r->observed, cap * sizeof *o);
    if (o) r->observed = o;
    if (!s || !w || !o) return -1;
    r->cap = cap;
  }
  r->samples[r->n] = sample;
  r->wake[r->n] = wake;
  r->observed[r->n] = cpu;
  r->n++;
  return 0;
}

void result_free(pmk_result *r) {
  dvec_free(&r->temps);
  dvec_free(&r->mhz);
  for (int i = 0; i < r->nclock_dom; i++) dvec_free(&r->mhz_dom[i]);
  dvec_free(&r->power);
  dvec_free(&r->job_ns);
  free(r->samples);
  free(r->wake);
  free(r->observed);
}

int ctx_cpu_for_type(const pmk_ctx *c, int type) {
  if (c->cfg.cpu >= 0) return c->cfg.cpu;
  /* The highest-numbered CPU of the type; CPU 0 usually handles the most interrupts. */
  int best = -1;
  for (int i = 0; i < c->m.ncpu; i++)
    if (c->m.cpus[i].type == type && c->m.cpus[i].id > best) best = c->m.cpus[i].id;
  return best >= 0 ? best : 0;
}

int ctx_precise_enough(pmk_ctx *c, const pmk_result *r, int max_reps) {
  if (r->n < (size_t)c->cfg.min_reps) return 0;
  if (r->n >= (size_t)max_reps) return 1;
  if (r->n % 5) return 0;
  pmk_ci ci = pmk_boot_median(r->samples, r->n, 500, &c->stat_rng);
  return (ci.hi - ci.lo) / 2 <= 0.01 * ci.est;
}

void ctx_shuffle(pmk_rng *r, int *v, int n) {
  for (int i = n - 1; i > 0; i--) {
    int j = (int)pmk_rng_below(r, (uint64_t)i + 1);
    int t = v[i];
    v[i] = v[j];
    v[j] = t;
  }
}

/* ---------- preflight and cool-down ---------- */

typedef struct temp_wait {
  double seconds, temp_c;
  int settled;
} temp_wait;

/* Waits until the CPU temperature is within 2 C of target (if finite) or has stopped changing. */
static temp_wait wait_temperature(pmk_ctx *c, const char *phase, double target, double timeout_s) {
  enum { WINDOW = 10 };
  double hist[WINDOW];
  int n = 0;
  temp_wait tw = {0, pal_cpu_temp_c(), 0};
  if (!isfinite(tw.temp_c)) return tw;
  uint64_t t0 = pal_now_ns();
  ctx_emit(c, PMK_EV_PHASE, phase, NULL, 0, 0,
           "waiting up to %.0f s for the CPU temperature to settle, so every test starts from the same state",
           timeout_s);
  for (;;) {
    double t = pal_cpu_temp_c();
    hist[n % WINDOW] = t;
    n++;
    tw.temp_c = t;
    tw.seconds = (double)(pal_now_ns() - t0) * 1e-9;
    if (isfinite(target) && t <= target + 2.0 && n >= 3) {
      tw.settled = 1;
      break;
    }
    if (n >= WINDOW) {
      double lo = hist[0], hi = hist[0];
      for (int i = 1; i < WINDOW; i++) {
        if (hist[i] < lo) lo = hist[i];
        if (hist[i] > hi) hi = hist[i];
      }
      if (hi - lo <= 1.0) {
        tw.settled = 1;
        break;
      }
    }
    if (tw.seconds >= timeout_s || ctx_cancelled()) break;
    pal_sleep_ns(1000000000ull);
  }
  return tw;
}

void ctx_cooldown(pmk_ctx *c, const char *phase) {
  if (!c->cfg.cooldown || ctx_cancelled()) return;
  wait_temperature(c, phase, c->idle_temp, 120);
}

/* ---------- result document ---------- */

static void make_uuid(char out[37]) {
  unsigned char b[16];
  if (pal_random_bytes(b, sizeof b)) {
    uint64_t x = pal_now_ns();
    for (int i = 0; i < 16; i++) b[i] = (unsigned char)(pmk_splitmix64(&x) >> 32);
  }
  b[6] = (unsigned char)((b[6] & 0x0f) | 0x40);
  b[8] = (unsigned char)((b[8] & 0x3f) | 0x80);
  snprintf(out, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[0], b[1], b[2], b[3],
           b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

static const char *compiler_string(void) {
#if defined(__clang__)
#define PMK_STR2(x) #x
#define PMK_STR(x) PMK_STR2(x)
  return "clang " PMK_STR(__clang_major__) "." PMK_STR(__clang_minor__) "." PMK_STR(__clang_patchlevel__);
#elif defined(__GNUC__)
  return "gcc " __VERSION__;
#else
  return "unknown";
#endif
}

static void machine_json(pmk_jw *w, const pmk_machine *m) {
  jw_obj_begin(w, "machine");
  jw_str(w, "os", m->os);
  jw_str(w, "kernel", m->kernel);
  jw_str(w, "isa", m->isa);
  jw_str(w, "board", m->board[0] ? m->board : NULL);
  jw_obj_begin(w, "cpu");
  jw_str(w, "model", m->model);
  jw_int(w, "llc_bytes", (int64_t)m->llc_bytes);
  jw_arr_begin(w, "cores");
  for (int i = 0; i < m->ncpu; i++) {
    jw_obj_begin(w, NULL);
    jw_int(w, "id", m->cpus[i].id);
    jw_str(w, "type", m->type_names[m->cpus[i].type]);
    jw_int(w, "max_mhz", m->cpus[i].max_khz / 1000);
    jw_int(w, "llc", m->cpus[i].llc);
    jw_obj_end(w);
  }
  jw_arr_end(w);
  jw_obj_end(w);
  jw_obj_begin(w, "capabilities");
  jw_bool(w, "pinning", m->caps.pinning);
  jw_bool(w, "perf_counters", m->caps.perf_counters);
  jw_bool(w, "timer_slack", m->caps.timer_slack);
  jw_bool(w, "qos_only", m->caps.qos_only);
  jw_str(w, "power", m->caps.power[0] ? m->caps.power : NULL);
  jw_str(w, "power_note", m->caps.power_note[0] ? m->caps.power_note : NULL);
  jw_obj_end(w);
  jw_obj_end(w);
}

static void samples_json(pmk_jw *w, const char *key, const double *v, size_t n, int as_int) {
  jw_arr_begin(w, key);
  for (size_t i = 0; i < n; i++) {
    if (as_int) jw_int(w, NULL, (int64_t)v[i]);
    else jw_num(w, NULL, v[i]);
  }
  jw_arr_end(w);
}

static void hex64(char out[19], uint64_t v) { snprintf(out, 19, "%016llx", (unsigned long long)v); }

static void result_to_json(pmk_jw *w, const pmk_ctx *c, const pmk_result *r) {
  char hx[19];
  jw_obj_begin(w, NULL);
  jw_str(w, "kernel", r->kernel);
  if (r->variant) jw_str(w, "variant", r->variant);
  jw_str(w, "mode", r->mode);
  jw_str(w, "tier", r->k->tier);
  jw_str(w, "isa_level", r->k->level);
  jw_obj_begin(w, "params");
  if (!strcmp(r->mode, "cold_burst")) {
    jw_str(w, "start", r->warm ? "warm" : "cold");
    if (!strcmp(r->kernel, "K9")) {
      jw_int(w, "W_cycles", (int64_t)r->w_cycles);
      jw_int(w, "iterations", (int64_t)r->iters);
      jw_num(w, "W_ms", r->w_ms);
    } else {
      jw_str(w, "size", r->size);
    }
    if (!r->warm) {
      jw_arr_begin(w, "gap_ms");
      jw_num(w, NULL, 50);
      jw_num(w, NULL, 500);
      jw_arr_end(w);
    }
  } else if (!strcmp(r->mode, "periodic")) {
    jw_str(w, "condition", r->condition);
    jw_num(w, "period_ms", r->period_ms);
    jw_int(w, "load_instances", r->loaders);
    jw_int(w, "overruns", (int64_t)r->overruns);
  } else {
    jw_str(w, "size", r->size);
    if (r->purpose) jw_str(w, "purpose", r->purpose);
    if (r->part) jw_str(w, "part", r->part);
    if (r->isa) jw_str(w, "vector_isa", r->isa);
    if (r->nthreads) jw_int(w, !strcmp(r->mode, "mc_instances") ? "instances" : "threads", r->nthreads);
    if (r->ws_bytes) jw_int(w, "working_set_bytes", (int64_t)r->ws_bytes);
    if (r->windowed) jw_num(w, "window_ms", r->window_ms);
    if (r->job_work > 0) jw_num(w, "job_work", r->job_work);
  }
  jw_obj_end(w);
  jw_obj_begin(w, "core");
  jw_str(w, "requested", c->m.type_names[r->type]);
  jw_int(w, "requested_cpu", r->cpu);
  jw_arr_begin(w, "observed");
  for (size_t i = 0; i < r->n; i++) jw_int(w, NULL, r->observed[i]);
  jw_arr_end(w);
  jw_obj_end(w);
  jw_str(w, "unit", r->unit);
  samples_json(w, "samples", r->samples, r->n, !strcmp(r->unit, "ns") && strcmp(r->kernel, "K7"));
  if (!strcmp(r->mode, "cold_burst") && !r->warm) {
    jw_arr_begin(w, "wake_ns");
    for (size_t i = 0; i < r->n; i++) jw_int(w, NULL, (int64_t)r->wake[i]);
    jw_arr_end(w);
  }
  if (r->windowed) {
    samples_json(w, "temp_c", r->temps.v, r->temps.n, 0);
    samples_json(w, "mhz", r->mhz.v, r->mhz.n, 1);
    if (r->nclock_dom) {
      jw_obj_begin(w, "mhz_by_domain");
      for (int i = 0; i < r->nclock_dom; i++) samples_json(w, r->clock_dom[i], r->mhz_dom[i].v, r->mhz_dom[i].n, 1);
      jw_obj_end(w);
      jw_arr_begin(w, "idle_domains");
      for (int i = 0; i < r->nclock_dom; i++)
        if (r->clock_idle[i]) jw_str(w, NULL, r->clock_dom[i]);
      jw_arr_end(w);
    }
    samples_json(w, "power_w", r->power.v, r->power.n, 0);
    samples_json(w, "job_ns", r->job_ns.v, r->job_ns.n, 1);
    jw_obj_begin(w, "steady");
    jw_bool(w, "reached", r->steady);
    jw_int(w, "from_window", (int64_t)r->steady_from);
    jw_num(w, "tau_s", r->tau_s);
    jw_num(w, "elapsed_s", r->elapsed_s);
    jw_obj_end(w);
    jw_int(w, "clamped_windows", r->clamped);
    jw_num(w, "clamped_lowest_mhz", r->lowest_mhz);
  }
  if (r->input_hash) {
    hex64(hx, r->input_hash);
    jw_str(w, "input_hash", hx);
    hex64(hx, r->checksum);
    jw_str(w, "checksum", r->checksum_ok ? hx : NULL);
    jw_bool(w, "checksum_consistent", r->checksum_ok);
    if (r->part) jw_int(w, "checksum_errors", (int64_t)r->errors);
  }
  if (r->stopped) jw_bool(w, "stopped", 1);
  if (r->part) { /* the stress test's checks */
    jw_str(w, "stop_reason", r->stop_reason);
    jw_int(w, "wrong_tasks", (int64_t)r->wrong_tasks);
    jw_num(w, "first_wrong_s", r->first_wrong_s);
    jw_int(w, "first_wrong_cpu", r->first_wrong_cpu);
    jw_obj_begin(w, "wrong_by_cpu");
    for (int i = 0; i < r->nwrong_cpu; i++) {
      char k[16];
      snprintf(k, sizeof k, "%d", r->wrong_cpu[i]);
      jw_int(w, k, (int64_t)r->wrong_cpu_n[i]);
    }
    jw_obj_end(w);
  }
  jw_obj_end(w);
}

static void tiers_json(pmk_jw *w, const pmk_ctx *c) {
  jw_obj_begin(w, "tiers");
  jw_obj_begin(w, "baseline");
  jw_str(w, "level", c->k->level);
  jw_str(w, "march", c->k->march);
  jw_obj_end(w);
  jw_obj_end(w);
}

typedef int (*mode_fn)(pmk_ctx *);
typedef int (*steps_fn)(const pmk_ctx *);
typedef double (*est_fn)(const pmk_ctx *);

/* quiet: the mode measures waits and wake-ups and needs an idle machine (no keyboard or mouse input). */
static const struct {
  uint32_t bit;
  mode_fn fn;
  steps_fn steps;
  est_fn est;
  const char *name;
  int quiet;
  const char *kernels[3];
} MODES[] = {
    {PMK_MODE_COLD_BURST, mode_cold_burst, mode_cold_burst_steps, mode_cold_burst_est, "cold_burst", 1, {"K9", "K4", "K6"}},
    {PMK_MODE_PERIODIC, mode_periodic, mode_periodic_steps, mode_periodic_est, "periodic", 1, {"K10", NULL, NULL}},
    {PMK_MODE_ST_BURST, mode_st_burst, mode_st_burst_steps, mode_st_burst_est, "st_burst", 0, {NULL}},
    {PMK_MODE_ST_SUSTAINED, mode_st_sustained, mode_st_sustained_steps, mode_st_sustained_est, "st_sustained", 0, {NULL}},
    {PMK_MODE_MC_THREADED, mode_mc_threaded, mode_mc_threaded_steps, mode_mc_threaded_est, "mc_threaded", 0, {NULL}},
    {PMK_MODE_MC_INSTANCES, mode_mc_instances, mode_mc_instances_steps, mode_mc_instances_est, "mc_instances", 0, {NULL}},
    {PMK_MODE_STRESS, mode_stress, mode_stress_steps, mode_stress_est, "stress", 0, {NULL}},
};

/* Runs one mode; a quiet mode interrupted by input is discarded and recorded as skipped. */
static int run_mode(pmk_ctx *c, int i) {
  size_t before = c->nres;
  if (MODES[i].quiet) {
    atomic_store(&g_input, 0);
    c->quiet = 1;
    c->quiet_since = pal_now_ns();
    ctx_emit(c, PMK_EV_PHASE, MODES[i].name, NULL, 0, 0,
             "hands off: this test measures waits and wake-ups; keyboard or mouse input skips it");
  }
  int rc = MODES[i].fn(c);
  c->quiet = 0;
  if (rc != PMK_SKIP_INPUT) return rc;
  for (size_t k = before; k < c->nres; k++) result_free(&c->res[k]);
  c->nres = before;
  for (int k = 0; k < 3 && MODES[i].kernels[k]; k++)
    if (ctx_kernel_selected(c, MODES[i].kernels[k]))
      ctx_unavailable(c, MODES[i].kernels[k], NULL, MODES[i].name,
                      "skipped: keyboard or mouse input during a test that needs an idle machine");
  jw_str(&c->skipped, NULL, MODES[i].name);
  return PMK_OK;
}
#define NMODES ((int)(sizeof MODES / sizeof *MODES))

/* ---------- entry point ---------- */

int pmk_start(const pmk_config *cfg_in, pmk_progress_fn cb, void *user, char **result_json,
              char **summary_text) {
  if (result_json) *result_json = NULL;
  if (summary_text) *summary_text = NULL;
  if (!cfg_in || cfg_in->struct_size < offsetof(pmk_config, frontend)) return PMK_ERR_INVALID;
  if (atomic_flag_test_and_set(&g_running)) return PMK_ERR_RUNNING;

  pmk_ctx *c = calloc(1, sizeof *c);
  if (!c) {
    atomic_flag_clear(&g_running);
    return PMK_ERR_NOMEM;
  }
  pmk_config_init(&c->cfg);
  memcpy(&c->cfg, cfg_in, cfg_in->struct_size < sizeof c->cfg ? cfg_in->struct_size : sizeof c->cfg);
  c->cfg.struct_size = sizeof c->cfg;
  if (!c->cfg.modes) c->cfg.modes = PMK_MODE_ALL;
  if (!c->cfg.frontend) c->cfg.frontend = "unknown";
  if (!c->cfg.ui_state) c->cfg.ui_state = "unknown";
  if (c->cfg.cold_max_reps <= 0) c->cfg.cold_max_reps = 300;
  if (cfg_in->struct_size < offsetof(pmk_config, input_watch) + sizeof(const char *)) c->cfg.input_watch = NULL;
  if (cfg_in->struct_size < offsetof(pmk_config, window_ms) + sizeof(double)) { /* ABI 1 caller */
    pmk_config d;
    pmk_config_init(&d);
    memcpy((char *)&c->cfg + offsetof(pmk_config, window_ms), (char *)&d + offsetof(pmk_config, window_ms),
           sizeof d - offsetof(pmk_config, window_ms));
  }
  if (cfg_in->struct_size < offsetof(pmk_config, measure_s) + sizeof(double)) { /* before ABI 5 */
    pmk_config d;
    pmk_config_init(&d);
    c->cfg.warmup_s = c->cfg.quick_inputs ? 0 : d.warmup_s;
    c->cfg.settle_s = c->cfg.quick_inputs ? 0 : d.settle_s;
    c->cfg.measure_s = c->cfg.quick_inputs ? 3 : d.measure_s;
  }
  if (cfg_in->struct_size < offsetof(pmk_config, stress_parts) + sizeof(uint32_t)) { /* before ABI 8 */
    c->cfg.stress_s = 600;
    c->cfg.stress_parts = PMK_STRESS_ALL;
  }
  if (cfg_in->struct_size < offsetof(pmk_config, stress_max_temp_c) + sizeof(double)) { /* early ABI 8 */
    c->cfg.stress_stop_on_error = 1;
    c->cfg.stress_max_temp_c = 0;
  }
  if (!c->cfg.stress_parts) c->cfg.stress_parts = PMK_STRESS_ALL;
  c->cb = cb;
  c->user = user;
  c->k = &pmk_kernels_baseline;
  atomic_store(&g_cancel, 0);

  int rc = PMK_OK;
  if (c->cfg.min_reps < 3 || c->cfg.max_reps < c->cfg.min_reps || c->cfg.cold_max_reps < c->cfg.min_reps || !(c->cfg.periodic_period_ms > 0) ||
      !(c->cfg.periodic_seconds > 0) || (c->cfg.modes & ~(uint32_t)(PMK_MODE_ALL | PMK_MODE_STRESS)) ||
      !(c->cfg.window_ms >= 10) || !(c->cfg.measure_s > 0) || !(c->cfg.warmup_s >= 0) || !(c->cfg.settle_s >= 0) ||
      !(c->cfg.stress_s >= 0 && c->cfg.stress_s <= PMK_STRESS_MAX_S) || (c->cfg.stress_parts & ~(uint32_t)PMK_STRESS_ALL) ||
      !(c->cfg.stress_max_temp_c >= 0 && c->cfg.stress_max_temp_c < 150))
    rc = PMK_ERR_INVALID;

  if (rc == PMK_OK && pal_init(&c->m)) rc = PMK_ERR_SYSTEM;
  if (rc == PMK_OK && c->cfg.cpu >= 0) {
    int ok = 0;
    for (int i = 0; i < c->m.ncpu; i++) ok |= c->m.cpus[i].id == c->cfg.cpu;
    if (!ok) rc = PMK_ERR_INVALID;
  }
  if (rc != PMK_OK) {
    pal_fini(&c->m);
    free(c);
    atomic_flag_clear(&g_running);
    return rc;
  }

  c->seed = c->cfg.seed;
  if (!c->seed && pal_random_bytes(&c->seed, sizeof c->seed)) c->seed = pal_now_ns();
  pmk_rng_seed(&c->rng, c->seed);
  pmk_rng_seed(&c->stat_rng, c->seed ^ 0x5eed5eed5eed5eedull);
  jw_arr_begin(&c->avail, NULL);
  jw_arr_begin(&c->skipped, NULL);

  char run_id[37];
  make_uuid(run_id);
  pmk_jw w = {0};
  jw_obj_begin(&w, NULL);
  jw_str(&w, "schema", SCHEMA);
  jw_str(&w, "run_id", run_id);
  char started[32];
  time_t now = time(NULL);
  strftime(started, sizeof started, "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));
  jw_str(&w, "started_utc", started);
  jw_obj_begin(&w, "harness");
  jw_str(&w, "version", PMK_VERSION);
  jw_str(&w, "compiler", compiler_string());
  jw_str(&w, "git", PMK_GIT_SHA);
  jw_int(&w, "abi", PMK_ABI_VERSION);
  jw_raw(&w, "sources", PMK_SOURCES_JSON);
  jw_str(&w, "k1_llvm", PMK_K1_LLVM_VERSION[0] ? PMK_K1_LLVM_VERSION : NULL);
  jw_obj_end(&w);
  jw_bool(&w, "verified", 0);
  jw_obj_begin(&w, "frontend");
  jw_str(&w, "kind", c->cfg.frontend);
  jw_str(&w, "ui_state", c->cfg.ui_state);
  jw_str(&w, "input_watch", c->cfg.input_watch ? c->cfg.input_watch : pal_input_source());
  jw_obj_end(&w);
  jw_obj_begin(&w, "config");
  jw_int(&w, "seed", (int64_t)c->seed);
  jw_int(&w, "cpu", c->cfg.cpu);
  jw_int(&w, "min_reps", c->cfg.min_reps);
  jw_int(&w, "max_reps", c->cfg.max_reps);
  jw_int(&w, "cold_max_reps", c->cfg.cold_max_reps);
  jw_num(&w, "periodic_period_ms", c->cfg.periodic_period_ms);
  jw_num(&w, "periodic_seconds", c->cfg.periodic_seconds);
  jw_num(&w, "window_ms", c->cfg.window_ms);
  jw_num(&w, "warmup_s", c->cfg.warmup_s);
  jw_num(&w, "settle_s", c->cfg.settle_s);
  jw_num(&w, "measure_s", c->cfg.measure_s);
  if (c->cfg.modes & PMK_MODE_STRESS) {
    jw_num(&w, "stress_s", c->cfg.stress_s);
    jw_bool(&w, "stress_stop_on_error", c->cfg.stress_stop_on_error);
    jw_num(&w, "stress_max_temp_c", c->cfg.stress_max_temp_c > 0 ? c->cfg.stress_max_temp_c : NAN);
    jw_arr_begin(&w, "stress_parts");
    for (int i = 0; i < 4; i++)
      if (c->cfg.stress_parts & (1u << i)) jw_str(&w, NULL, stress_part_name(1u << i));
    jw_arr_end(&w);
  }
  jw_int(&w, "max_threads", c->cfg.max_threads);
  jw_str(&w, "kernels", c->cfg.kernels);
  jw_str(&w, "k1_data", c->cfg.k1_data);
  jw_bool(&w, "quick_inputs", c->cfg.quick_inputs);
  jw_bool(&w, "quick", c->cfg.quick_inputs); /* the --quick preset sets both; front-ends read this one */
  jw_obj_end(&w);
  machine_json(&w, &c->m);
  tiers_json(&w, c);

  /* Preflight. */
  double idle_temp = pal_cpu_temp_c();
  jw_obj_begin(&w, "preflight");
  jw_bool(&w, "skipped", c->cfg.skip_preflight);
  if (!c->cfg.skip_preflight) {
    ctx_emit(c, PMK_EV_PHASE, "preflight", NULL, 1, 2, "measuring background load for 5 s");
    double load = pal_background_load(5.0);
    jw_num(&w, "background_load", load);
    jw_num(&w, "max_background_load", c->cfg.max_background_load);
    if (isfinite(load) && load > c->cfg.max_background_load) {
      ctx_emit(c, PMK_EV_WARNING, "preflight", NULL, 0, 0, "background load %.1f%% is above %.1f%%", load * 100,
               c->cfg.max_background_load * 100);
      rc = PMK_ERR_BUSY;
    } else {
      temp_wait tw = wait_temperature(c, "preflight", NAN, 60);
      idle_temp = tw.temp_c;
      jw_num(&w, "temp_wait_s", tw.seconds);
      jw_bool(&w, "temp_settled", tw.settled);
    }
  }
  jw_num(&w, "idle_temp_c", idle_temp);
  jw_obj_end(&w);
  c->idle_temp = idle_temp;
  if (rc == PMK_OK && ctx_cancelled()) rc = PMK_ERR_CANCELLED; /* during the preflight waits */

  if (rc == PMK_OK) {
    ctx_emit(c, PMK_EV_PHASE, "preflight", NULL, 2, 2, "recording machine state");
    jw_obj_begin(&w, "state");
    pal_state_json(&w, "start", &c->m, 1);

    int modes[NMODES], nmodes = 0;
    for (int i = 0; i < NMODES; i++)
      if (c->cfg.modes & MODES[i].bit) modes[nmodes++] = i;
    ctx_shuffle(&c->rng, modes, nmodes);
    double est[NMODES];
    for (int i = 0; i < nmodes; i++) {
      c->run_steps += (uint32_t)MODES[modes[i]].steps(c);
      est[i] = MODES[modes[i]].est(c);
      c->later_est_s += est[i];
    }
    for (int i = 0; i < nmodes && rc == PMK_OK && !c->stopped; i++) {
      if (ctx_cancelled()) {
        rc = PMK_ERR_CANCELLED;
        break;
      }
      c->later_est_s -= est[i];
      c->mode_est_s = est[i];
      c->mode_t0 = pal_now_ns();
      c->mode_step0 = c->run_step;
      c->mode_steps = (uint32_t)MODES[modes[i]].steps(c);
      /* Overall position for front-ends: modes run in shuffled order, so they cannot work it out themselves. */
      ctx_emit(c, PMK_EV_INFO, "run", NULL, (uint32_t)i + 1, (uint32_t)nmodes, "part %d of %d: %s", i + 1, nmodes,
               MODES[modes[i]].name);
      if (i > 0) ctx_cooldown(c, "cooldown");
      rc = run_mode(c, modes[i]);
    }
    pal_state_json(&w, "end", &c->m, rc == PMK_OK);
    jw_obj_end(&w);
  }

  jw_bool(&w, "complete", rc == PMK_OK);
  /* A stress test without a set length ends when it is stopped: a normal end, the modes after it are not run. */
  if (c->stopped) jw_bool(&w, "stopped", 1);
  if (rc != PMK_OK) jw_str(&w, "error", pmk_strerror(rc));

  jw_arr_end(&c->avail);
  char *avail = buf_take(&c->avail.b);
  jw_raw(&w, "unavailable", avail && avail[0] ? avail : "[]");
  free(avail);
  jw_arr_end(&c->skipped);
  char *skipped = buf_take(&c->skipped.b);
  jw_raw(&w, "skipped_modes", skipped && skipped[0] ? skipped : "[]");
  free(skipped);
  char *k1x = buf_take(&c->k1x.b);
  jw_raw(&w, "k1x", k1x && k1x[0] ? k1x : "null");
  free(k1x);

  jw_arr_begin(&w, "results");
  for (size_t i = 0; i < c->nres; i++) result_to_json(&w, c, &c->res[i]);
  jw_arr_end(&w);

  ctx_emit(c, PMK_EV_PHASE, "done", NULL, 0, 0, "computing statistics");
  analysis_compute(c);
  analysis_tp_compute(c);
  analysis_json(c, &w, "analysis");
  /* Stored with the result, so front-ends can show it again later (pmk_describe). */
  char *summary = analysis_text(c, run_id, rc == PMK_OK ? NULL : "INCOMPLETE RUN: results above are partial.");
  jw_str(&w, "summary", summary);
  jw_obj_end(&w);

  char *json = buf_take(&w.b);
  if (!json) rc = PMK_ERR_NOMEM;
  if (result_json) *result_json = json;
  else free(json);
  if (summary_text) *summary_text = summary;
  else free(summary);

  analysis_free(c);
  analysis_tp_free(c);
  for (size_t i = 0; i < c->nres; i++) result_free(&c->res[i]);
  free(c->res);
  pal_fini(&c->m);
  free(c);
  atomic_flag_clear(&g_running);
  return rc;
}

int pmk_info(char **info_json) {
  if (!info_json) return PMK_ERR_INVALID;
  *info_json = NULL;
  pmk_ctx *c = calloc(1, sizeof *c);
  if (!c) return PMK_ERR_NOMEM;
  c->k = &pmk_kernels_baseline;
  int ok = pal_init(&c->m) == 0;
  pmk_jw w = {0};
  jw_obj_begin(&w, NULL);
  jw_str(&w, "schema", "prismark-info/1");
  jw_str(&w, "version", PMK_VERSION);
  jw_int(&w, "abi", PMK_ABI_VERSION);
  jw_bool(&w, "k1_built", pmk_find_tk(c->k, "K1", NULL) != NULL);
  jw_str(&w, "k1_llvm", PMK_K1_LLVM_VERSION[0] ? PMK_K1_LLVM_VERSION : NULL);
  if (ok) {
    tiers_json(&w, c);
    machine_json(&w, &c->m);
    pal_fini(&c->m);
  }
  jw_obj_end(&w);
  free(c);
  *info_json = buf_take(&w.b);
  return *info_json ? PMK_OK : PMK_ERR_NOMEM;
}
