/*
 * Darwin platform layer (macOS, iOS, iPadOS).
 *
 * There is no thread pinning and no frequency or idle control. Threads are
 * steered to a core type with QoS classes (USER_INTERACTIVE for P cores,
 * BACKGROUND for E cores), and the observed "CPU" of a repetition is the
 * core type requested, i.e. inferred, as the spec allows (5.2, 8.1); the
 * capabilities record qos_only so this is never mistaken for pinning.
 * Temperatures are not readable without private interfaces; the thermal
 * pressure level is recorded instead.
 *
 * Copyright 2026 The Prismark Authors. Apache-2.0.
 */
#include <TargetConditionals.h>
#if TARGET_OS_OSX
#include <CoreGraphics/CoreGraphics.h>
#endif
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <math.h>
#include <notify.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <sys/utsname.h>
#include <unistd.h>

#include "pal.h"

static mach_timebase_info_data_t g_tb;
static __thread int t_requested_type = -1;
static int g_nperf; /* perflevel0 logical CPUs */

static uint64_t to_ns(uint64_t abs) { return abs * g_tb.numer / g_tb.denom; }
static uint64_t to_abs(uint64_t ns) { return ns * g_tb.denom / g_tb.numer; }

static int sysctl_int(const char *name, int def) {
  int v = 0;
  size_t n = sizeof v;
  return sysctlbyname(name, &v, &n, NULL, 0) == 0 ? v : def;
}

static uint64_t sysctl_u64(const char *name, uint64_t def) {
  uint64_t v = 0;
  size_t n = sizeof v;
  if (sysctlbyname(name, &v, &n, NULL, 0)) return def;
  if (n == 4) return (uint32_t)v;
  return v;
}

int pal_init(pmk_machine *m) {
  memset(m, 0, sizeof *m);
  mach_timebase_info(&g_tb);
#if TARGET_OS_IPHONE
  snprintf(m->os, sizeof m->os, "ios");
#else
  snprintf(m->os, sizeof m->os, "macos");
#endif
  struct utsname u;
  if (!uname(&u)) {
    snprintf(m->kernel, sizeof m->kernel, "%s", u.release);
    snprintf(m->isa, sizeof m->isa, "%s", strcmp(u.machine, "arm64") && strncmp(u.machine, "iP", 2) ? u.machine : "aarch64");
  }
#if defined(__aarch64__)
  snprintf(m->isa, sizeof m->isa, "aarch64");
#endif
  size_t len = sizeof m->model;
  if (sysctlbyname("machdep.cpu.brand_string", m->model, &len, NULL, 0)) m->model[0] = 0;
  len = sizeof m->board;
  if (sysctlbyname("hw.model", m->board, &len, NULL, 0)) m->board[0] = 0;

  int n = sysctl_int("hw.logicalcpu", 1);
  m->cpus = calloc((size_t)n, sizeof *m->cpus);
  if (!m->cpus) return -1;
  m->ncpu = n;
  int levels = sysctl_int("hw.nperflevels", 1);
  if (levels < 1) levels = 1;
  if (levels > PMK_MAX_TYPES) levels = PMK_MAX_TYPES;
  m->ntypes = levels;
  static const char *names[][PMK_MAX_TYPES] = {{"P"}, {"P", "E"}, {"P", "M", "E"}, {"P", "M1", "M2", "E"}};
  /* CPU ids are labels only (nothing can be pinned): the first perflevel0 count are P, then E. */
  int id = 0;
  for (int lv = 0; lv < levels; lv++) {
    char key[64];
    snprintf(m->type_names[lv], sizeof m->type_names[lv], "%s", names[levels - 1][lv]);
    snprintf(key, sizeof key, "hw.perflevel%d.logicalcpu", lv);
    int cnt = levels == 1 ? n : sysctl_int(key, 0);
    if (lv == 0) g_nperf = cnt;
    for (int k = 0; k < cnt && id < n; k++, id++) {
      m->cpus[id].id = id;
      m->cpus[id].type = lv;
      m->cpus[id].core = id;
      m->cpus[id].smt = 0;
    }
  }
  for (; id < n; id++) m->cpus[id] = (pmk_cpu){id, levels - 1, 0, 0, id, 0};
  m->llc_bytes = sysctl_u64("hw.l3cachesize", 0);
  if (!m->llc_bytes) m->llc_bytes = sysctl_u64("hw.perflevel0.l2cachesize", sysctl_u64("hw.l2cachesize", 0));
  m->caps.qos_only = 1;
  return 0;
}

void pal_fini(pmk_machine *m) {
  free(m->cpus);
  m->cpus = NULL;
}

uint64_t pal_now_ns(void) { return to_ns(mach_absolute_time()); }
uint64_t pal_now_raw_ns(void) { return to_ns(mach_absolute_time()); }
void pal_sleep_until_ns(uint64_t t) {
  while (pal_now_ns() < t) mach_wait_until(to_abs(t));
}
void pal_sleep_ns(uint64_t ns) { pal_sleep_until_ns(pal_now_ns() + ns); }

/* QoS steers the thread to a core type; the type of the CPU id is what is requested. */
int pal_pin_self(int cpu) {
  int type = cpu < g_nperf ? 0 : 1;
  t_requested_type = type;
  return pthread_set_qos_class_self_np(type == 0 ? QOS_CLASS_USER_INTERACTIVE : QOS_CLASS_BACKGROUND, 0);
}
int pal_unpin_self(void) {
  t_requested_type = -1;
  return pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
}
int pal_pin_self_set(const int *cpus, int n) { return n > 0 ? pal_pin_self(cpus[0]) : -1; }
/* The inferred core type (spec 8.1): the CPU id is not observable from user space. */
int pal_current_cpu(void) { return t_requested_type < 0 ? -1 : (t_requested_type == 0 ? 0 : g_nperf); }
int pal_set_timer_slack_min(void) { return -1; }

int64_t pal_input_idle_ms(void) {
#if TARGET_OS_OSX
  double s = CGEventSourceSecondsSinceLastEventType(kCGEventSourceStateHIDSystemState, kCGAnyInputEventType);
  return s >= 0 ? (int64_t)(s * 1000) : -1;
#else
  return -1; /* iOS: the app is in the foreground with a frozen screen; touches are reported by the app */
#endif
}

const char *pal_input_source(void) {
#if TARGET_OS_OSX
  return "CGEventSource";
#else
  return NULL;
#endif
}
int pal_random_bytes(void *p, size_t n) {
  arc4random_buf(p, n);
  return 0;
}

double pal_cpu_temp_c(void) { return NAN; }

static int cpu_ticks(uint64_t *busy, uint64_t *total) {
  host_cpu_load_info_data_t info;
  mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;
  if (host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO, (host_info_t)&info, &count) != KERN_SUCCESS) return -1;
  *total = 0;
  for (int i = 0; i < CPU_STATE_MAX; i++) *total += info.cpu_ticks[i];
  *busy = *total - info.cpu_ticks[CPU_STATE_IDLE];
  return 0;
}

double pal_background_load(double seconds) {
  uint64_t b0, t0, b1, t1;
  if (cpu_ticks(&b0, &t0)) return NAN;
  pal_sleep_ns((uint64_t)(seconds * 1e9));
  if (cpu_ticks(&b1, &t1) || t1 <= t0) return NAN;
  return (double)(b1 - b0) / (double)(t1 - t0);
}

double pal_idle_power_w(double seconds) {
  (void)seconds;
  return NAN; /* powermetrics needs root and is not called from inside a measurement */
}

/* 0 nominal, 1 moderate, 2 heavy, 3 trapping, 4 sleeping; -1 if unknown. */
static int thermal_pressure(void) {
  int token;
  uint64_t state = 0;
  if (notify_register_check("com.apple.system.thermalpressurelevel", &token) != NOTIFY_STATUS_OK) return -1;
  int ok = notify_get_state(token, &state) == NOTIFY_STATUS_OK;
  notify_cancel(token);
  return ok ? (int)state : -1;
}

void pal_state_json(pmk_jw *w, const char *key, const pmk_machine *m, int measure_power) {
  (void)m;
  (void)measure_power;
  jw_obj_begin(w, key);
  jw_null(w, "power_mode");
  int tp = thermal_pressure();
  if (tp >= 0) jw_int(w, "thermal_pressure", tp);
  else jw_null(w, "thermal_pressure");
  jw_obj_begin(w, "temp_c");
  jw_null(w, "cpu");
  jw_obj_end(w);
  jw_null(w, "idle_power_w");
  jw_obj_end(w);
}


int pal_cycles_open(void) { return -1; }
uint64_t pal_cycles_read(int fd) {
  (void)fd;
  return 0;
}
void pal_cycles_close(int fd) { (void)fd; }

int pal_scratch_dir(char *out, size_t n, int *ram_backed) {
  const char *t = getenv("TMPDIR");
  snprintf(out, n, "%s", t && *t ? t : "/tmp");
  size_t len = strlen(out);
  if (len > 1 && out[len - 1] == '/') out[len - 1] = 0;
  *ram_backed = 0; /* Darwin has no tmpfs; the K1x result records it */
  return access(out, W_OK) == 0 ? 0 : -1;
}
