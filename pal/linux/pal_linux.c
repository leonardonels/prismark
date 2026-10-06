/*
 * Linux platform layer: sysfs/procfs for machine state, sched_setaffinity
 * for pinning, perf_event_open for cycle counts, RAPL or INA3221 for power.
 *
 * Copyright 2026 The Prismark Authors. Apache-2.0.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/perf_event.h>
#ifdef __GLIBC__
#include <malloc.h>
#endif
#include <math.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/random.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>
#include <limits.h>

#include "pal.h"
#include "sysfs.h"


/* ---------- global platform state ---------- */

static int g_ncpu_conf;
static unsigned char *g_online;
static char g_cpu_temp_path[300];


static uint64_t parse_size(const char *s) {
  char *e;
  unsigned long long v = strtoull(s, &e, 10);
  if (*e == 'K') v <<= 10;
  else if (*e == 'M') v <<= 20;
  else if (*e == 'G') v <<= 30;
  return v;
}

static void find_cpu_temp(void) {
  char p[300], name[64], label[64];
  g_cpu_temp_path[0] = 0;
  for (int h = 0; h < 64; h++) {
    snprintf(p, sizeof p, "/sys/class/hwmon/hwmon%d/name", h);
    if (sysfs_read(p, name, sizeof name)) continue;
    if (!strcmp(name, "k10temp") || !strcmp(name, "zenpower") || !strcmp(name, "cpu_thermal")) {
      snprintf(g_cpu_temp_path, sizeof g_cpu_temp_path, "/sys/class/hwmon/hwmon%d/temp1_input", h);
      return;
    }
    if (!strcmp(name, "coretemp")) {
      for (int t = 1; t < 64; t++) {
        snprintf(p, sizeof p, "/sys/class/hwmon/hwmon%d/temp%d_label", h, t);
        if (sysfs_read(p, label, sizeof label)) continue;
        if (!strncmp(label, "Package id", 10)) {
          snprintf(g_cpu_temp_path, sizeof g_cpu_temp_path, "/sys/class/hwmon/hwmon%d/temp%d_input", h, t);
          return;
        }
      }
    }
  }
  static const char *zones[] = {"x86_pkg_temp", "cpu-thermal", "cpu_thermal", "CPU-therm", "cpu"};
  for (size_t k = 0; k < sizeof zones / sizeof *zones; k++) {
    for (int z = 0; z < 64; z++) {
      snprintf(p, sizeof p, "/sys/class/thermal/thermal_zone%d/type", z);
      if (sysfs_read(p, name, sizeof name)) continue;
      if (!strcmp(name, zones[k])) {
        snprintf(g_cpu_temp_path, sizeof g_cpu_temp_path, "/sys/class/thermal/thermal_zone%d/temp", z);
        return;
      }
    }
  }
}

/* ---------- power telemetry ---------- */

#define RAPL_PKG "/sys/class/powercap/intel-rapl:0"
static char g_ina_in[300], g_ina_curr[300];

static void find_power(pmk_caps *c) {
  c->power[0] = 0;
  if (sysfs_read_ll(RAPL_PKG "/energy_uj", -1) >= 0) {
    snprintf(c->power, sizeof c->power, "rapl");
    return;
  }
  char p[300], name[64], label[64];
  for (int h = 0; h < 64; h++) {
    snprintf(p, sizeof p, "/sys/class/hwmon/hwmon%d/name", h);
    if (sysfs_read(p, name, sizeof name) || strcmp(name, "ina3221")) continue;
    for (int ch = 1; ch <= 3; ch++) {
      snprintf(p, sizeof p, "/sys/class/hwmon/hwmon%d/in%d_label", h, ch);
      if (sysfs_read(p, label, sizeof label) || strcmp(label, "VDD_IN")) continue;
      snprintf(g_ina_in, sizeof g_ina_in, "/sys/class/hwmon/hwmon%d/in%d_input", h, ch);
      snprintf(g_ina_curr, sizeof g_ina_curr, "/sys/class/hwmon/hwmon%d/curr%d_input", h, ch);
      snprintf(c->power, sizeof c->power, "ina3221:VDD_IN");
      return;
    }
  }
}

double pal_idle_power_w(double seconds) {
  if (sysfs_read_ll(RAPL_PKG "/energy_uj", -1) >= 0) {
    long long range = sysfs_read_ll(RAPL_PKG "/max_energy_range_uj", 0);
    long long e0 = sysfs_read_ll(RAPL_PKG "/energy_uj", -1);
    uint64_t t0 = pal_now_ns();
    pal_sleep_ns((uint64_t)(seconds * 1e9));
    long long e1 = sysfs_read_ll(RAPL_PKG "/energy_uj", -1);
    uint64_t t1 = pal_now_ns();
    if (e0 < 0 || e1 < 0) return NAN;
    long long de = e1 - e0;
    if (de < 0) de += range;
    return (double)de * 1e-6 / ((double)(t1 - t0) * 1e-9);
  }
  if (g_ina_in[0]) {
    double sum = 0;
    int n = 0;
    uint64_t end = pal_now_ns() + (uint64_t)(seconds * 1e9);
    while (pal_now_ns() < end) {
      long long mv = sysfs_read_ll(g_ina_in, -1), ma = sysfs_read_ll(g_ina_curr, -1);
      if (mv >= 0 && ma >= 0) {
        sum += (double)mv * (double)ma * 1e-6;
        n++;
      }
      pal_sleep_ns(100000000);
    }
    return n ? sum / n : NAN;
  }
  return NAN;
}

/* ---------- cycle counter ---------- */

int pal_cycles_open(void) {
  struct perf_event_attr a;
  memset(&a, 0, sizeof a);
  a.size = sizeof a;
  a.type = PERF_TYPE_HARDWARE;
  a.config = PERF_COUNT_HW_CPU_CYCLES;
  a.exclude_kernel = 1;
  a.exclude_hv = 1;
  long fd = syscall(SYS_perf_event_open, &a, 0, -1, -1, PERF_FLAG_FD_CLOEXEC);
  return fd < 0 ? -1 : (int)fd;
}

uint64_t pal_cycles_read(int fd) {
  uint64_t v = 0;
  if (fd < 0 || read(fd, &v, sizeof v) != (ssize_t)sizeof v) return 0;
  return v;
}

void pal_cycles_close(int fd) {
  if (fd >= 0) close(fd);
}

/* ---------- machine description ---------- */

static void read_model(pmk_machine *m) {
  FILE *f = fopen("/proc/cpuinfo", "r");
  char line[512], impl[32] = "", part[32] = "";
  m->model[0] = 0;
  if (f) {
    while (fgets(line, sizeof line, f)) {
      char *colon = strchr(line, ':');
      if (!colon) continue;
      char *v = colon + 1;
      while (*v == ' ') v++;
      v[strcspn(v, "\n")] = 0;
      if (!strncmp(line, "model name", 10) && !m->model[0]) snprintf(m->model, sizeof m->model, "%s", v);
      else if (!strncmp(line, "CPU implementer", 15) && !impl[0]) snprintf(impl, sizeof impl, "%s", v);
      else if (!strncmp(line, "CPU part", 8) && !part[0]) snprintf(part, sizeof part, "%s", v);
    }
    fclose(f);
  }
  if (!m->model[0] && impl[0])
    snprintf(m->model, sizeof m->model, "arm implementer %s part %s", impl, part);
  if (sysfs_read("/proc/device-tree/model", m->board, sizeof m->board)) m->board[0] = 0;
}

static void classify_types(pmk_machine *m) {
  char p[300], list[4096];
  unsigned char *core = calloc((size_t)g_ncpu_conf, 1), *atom = calloc((size_t)g_ncpu_conf, 1);
  m->ntypes = 1;
  snprintf(m->type_names[0], sizeof m->type_names[0], "P");
  /* Intel hybrid: separate PMUs list their CPUs. */
  if (core && atom && !sysfs_read("/sys/devices/cpu_core/cpus", list, sizeof list)) {
    sysfs_parse_cpulist(list, core, g_ncpu_conf);
    if (!sysfs_read("/sys/devices/cpu_atom/cpus", list, sizeof list)) sysfs_parse_cpulist(list, atom, g_ncpu_conf);
    m->ntypes = 2;
    snprintf(m->type_names[1], sizeof m->type_names[1], "E");
    for (int i = 0; i < m->ncpu; i++) m->cpus[i].type = atom[m->cpus[i].id] ? 1 : 0;
    free(core);
    free(atom);
    return;
  }
  free(core);
  free(atom);
  /* ARM big.LITTLE: rank the distinct cpu_capacity values. */
  long caps[PMK_MAX_TYPES];
  int ncaps = 0;
  long *capv = calloc((size_t)m->ncpu, sizeof *capv);
  if (!capv) return;
  for (int i = 0; i < m->ncpu; i++) {
    snprintf(p, sizeof p, SYS_CPU "/cpu%d/cpu_capacity", m->cpus[i].id);
    capv[i] = (long)sysfs_read_ll(p, -1);
    if (capv[i] < 0) {
      free(capv);
      return;
    }
    int seen = 0;
    for (int k = 0; k < ncaps; k++) seen |= caps[k] == capv[i];
    if (!seen && ncaps < PMK_MAX_TYPES) caps[ncaps++] = capv[i];
  }
  for (int a = 0; a < ncaps; a++) /* descending */
    for (int b = a + 1; b < ncaps; b++)
      if (caps[b] > caps[a]) {
        long t = caps[a];
        caps[a] = caps[b];
        caps[b] = t;
      }
  static const char *names[][PMK_MAX_TYPES] = {
      {"P"}, {"P", "E"}, {"P", "M", "E"}, {"P", "M1", "M2", "E"}};
  m->ntypes = ncaps;
  for (int k = 0; k < ncaps; k++) snprintf(m->type_names[k], sizeof m->type_names[k], "%s", names[ncaps - 1][k]);
  for (int i = 0; i < m->ncpu; i++)
    for (int k = 0; k < ncaps; k++)
      if (capv[i] == caps[k]) m->cpus[i].type = k;
  free(capv);
}

/* Physical core and SMT position: the first CPU in thread_siblings_list is SMT thread 0. */
static void read_topology(pmk_cpu *cpu) {
  char p[300], list[256];
  snprintf(p, sizeof p, SYS_CPU "/cpu%d/topology/core_id", cpu->id);
  long long core = sysfs_read_ll(p, -1);
  snprintf(p, sizeof p, SYS_CPU "/cpu%d/topology/physical_package_id", cpu->id);
  long long pkg = sysfs_read_ll(p, 0);
  cpu->core = core < 0 ? -1 : (int)(pkg * 65536 + core);
  cpu->smt = 0;
  snprintf(p, sizeof p, SYS_CPU "/cpu%d/topology/thread_siblings_list", cpu->id);
  if (!sysfs_read(p, list, sizeof list)) {
    int first = atoi(list);
    cpu->smt = first == cpu->id ? 0 : 1;
  }
}

int pal_init(pmk_machine *m) {
  memset(m, 0, sizeof *m);
  struct utsname u;
#ifdef __ANDROID__
  snprintf(m->os, sizeof m->os, "android");
#else
  snprintf(m->os, sizeof m->os, "linux");
#endif
  if (!uname(&u)) {
    snprintf(m->kernel, sizeof m->kernel, "%s", u.release);
    snprintf(m->isa, sizeof m->isa, "%.15s", u.machine);
  }
  if (!strcmp(m->isa, "arm64")) snprintf(m->isa, sizeof m->isa, "aarch64");

  g_ncpu_conf = (int)sysconf(_SC_NPROCESSORS_CONF);
  if (g_ncpu_conf <= 0) return -1;
  g_online = calloc((size_t)g_ncpu_conf, 1);
  m->cpus = calloc((size_t)g_ncpu_conf, sizeof *m->cpus);
  if (!g_online || !m->cpus) return -1;

  char list[4096], p[300];
  if (!sysfs_read(SYS_CPU "/online", list, sizeof list)) sysfs_parse_cpulist(list, g_online, g_ncpu_conf);
  else memset(g_online, 1, (size_t)g_ncpu_conf);
  for (int c = 0; c < g_ncpu_conf; c++) {
    if (!g_online[c]) continue;
    pmk_cpu *cpu = &m->cpus[m->ncpu++];
    cpu->id = c;
    snprintf(p, sizeof p, SYS_CPU "/cpu%d/cpufreq/cpuinfo_max_freq", c);
    cpu->max_khz = (int)sysfs_read_ll(p, 0);
    snprintf(p, sizeof p, SYS_CPU "/cpu%d/cpufreq/cpuinfo_min_freq", c);
    cpu->min_khz = (int)sysfs_read_ll(p, 0);
    read_topology(cpu);
  }
  read_model(m);
  classify_types(m);

  for (int i = 0; i < 10; i++) {
    char v[64];
    snprintf(p, sizeof p, SYS_CPU "/cpu0/cache/index%d/level", i);
    long long level = sysfs_read_ll(p, -1);
    if (level < 0) break;
    snprintf(p, sizeof p, SYS_CPU "/cpu0/cache/index%d/size", i);
    if (level >= 2 && !sysfs_read(p, v, sizeof v)) m->llc_bytes = parse_size(v);
  }

  cpu_set_t set;
  m->caps.pinning = sched_getaffinity(0, sizeof set, &set) == 0;
  int fd = pal_cycles_open();
  m->caps.perf_counters = fd >= 0;
  pal_cycles_close(fd);
  m->caps.timer_slack = prctl(PR_GET_TIMERSLACK, 0, 0, 0, 0) >= 0;
  find_power(&m->caps);
  find_cpu_temp();
  return 0;
}

void pal_fini(pmk_machine *m) {
  free(m->cpus);
  m->cpus = NULL;
  free(g_online);
  g_online = NULL;
}

/* ---------- time, pinning ---------- */

uint64_t pal_now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

uint64_t pal_now_raw_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void pal_sleep_until_ns(uint64_t t) {
  struct timespec ts = {(time_t)(t / 1000000000ull), (long)(t % 1000000000ull)};
  while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {
  }
}

void pal_sleep_ns(uint64_t ns) { pal_sleep_until_ns(pal_now_ns() + ns); }

int pal_pin_self(int cpu) {
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  return sched_setaffinity(0, sizeof set, &set);
}

int pal_unpin_self(void) {
  cpu_set_t set;
  CPU_ZERO(&set);
  for (int c = 0; c < g_ncpu_conf && c < CPU_SETSIZE; c++)
    if (!g_online || g_online[c]) CPU_SET(c, &set);
  return sched_setaffinity(0, sizeof set, &set);
}

int pal_pin_self_set(const int *cpus, int n) {
  cpu_set_t set;
  CPU_ZERO(&set);
  for (int i = 0; i < n; i++)
    if (cpus[i] >= 0 && cpus[i] < CPU_SETSIZE) CPU_SET(cpus[i], &set);
  return sched_setaffinity(0, sizeof set, &set);
}

int pal_current_cpu(void) { return sched_getcpu(); }

int pal_scratch_dir(char *out, size_t n, int *ram_backed) {
  /* /dev/shm is tmpfs on every mainstream distribution; Android has no writable tmpfs for apps. */
  if (access("/dev/shm", W_OK) == 0) {
    snprintf(out, n, "/dev/shm");
    *ram_backed = 1;
    return 0;
  }
  const char *t = getenv("TMPDIR");
  snprintf(out, n, "%s", t && *t ? t : "/tmp");
  *ram_backed = 0;
  return access(out, W_OK) == 0 ? 0 : -1;
}

int pal_set_timer_slack_min(void) { return prctl(PR_SET_TIMERSLACK, 1, 0, 0, 0); }

/* Input belongs to the desktop session (X11, Wayland), which the runner may not share (it can run as
 * root through pkexec); the desktop app watches it and reports it with pmk_notify_input. */
int64_t pal_input_idle_ms(void) { return -1; }

int pal_cpu_cur_khz(int cpu) {
  char p[128];
  snprintf(p, sizeof p, SYS_CPU "/cpu%d/cpufreq/scaling_cur_freq", cpu);
  return (int)sysfs_read_ll(p, 0);
}

uint64_t pal_mem_available(void) {
#ifdef __GLIBC__
  malloc_trim(0); /* freed heap of earlier steps would otherwise still count as used */
#endif
  FILE *f = fopen("/proc/meminfo", "r");
  if (!f) return 0;
  char line[256];
  unsigned long long kb = 0;
  while (fgets(line, sizeof line, f))
    if (sscanf(line, "MemAvailable: %llu kB", &kb) == 1) break;
  fclose(f);
  return (uint64_t)kb * 1024;
}
const char *pal_input_source(void) { return NULL; }

int pal_random_bytes(void *p, size_t n) {
  return getrandom(p, n, 0) == (ssize_t)n ? 0 : -1;
}

/* ---------- load and temperature ---------- */

double pal_cpu_temp_c(void) {
  if (!g_cpu_temp_path[0]) return NAN;
  long long v = sysfs_read_ll(g_cpu_temp_path, LLONG_MIN);
  return v == LLONG_MIN ? NAN : (double)v / 1000.0;
}

static int read_proc_stat(unsigned long long *busy, unsigned long long *total) {
  FILE *f = fopen("/proc/stat", "r");
  if (!f) return -1;
  unsigned long long v[10] = {0};
  int n = fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3],
                 &v[4], &v[5], &v[6], &v[7], &v[8], &v[9]);
  fclose(f);
  if (n < 5) return -1;
  *total = 0;
  for (int i = 0; i < 8; i++) *total += v[i]; /* guest time is already in user */
  *busy = *total - v[3] - v[4];
  return 0;
}

double pal_background_load(double seconds) {
  unsigned long long b0, t0, b1, t1;
  if (read_proc_stat(&b0, &t0)) return NAN;
  pal_sleep_ns((uint64_t)(seconds * 1e9));
  if (read_proc_stat(&b1, &t1) || t1 <= t0) return NAN;
  return (double)(b1 - b0) / (double)(t1 - t0);
}

/* ---------- state ---------- */

static void temps_json(pmk_jw *w) {
  char p[300], name[64], label[64], key[160];
  jw_obj_begin(w, "temp_c");
  jw_num(w, "cpu", pal_cpu_temp_c());
  for (int h = 0; h < 64; h++) {
    snprintf(p, sizeof p, "/sys/class/hwmon/hwmon%d/name", h);
    if (sysfs_read(p, name, sizeof name)) continue;
    for (int t = 1; t < 16; t++) {
      snprintf(p, sizeof p, "/sys/class/hwmon/hwmon%d/temp%d_input", h, t);
      long long v = sysfs_read_ll(p, LLONG_MIN);
      if (v == LLONG_MIN) continue;
      snprintf(p, sizeof p, "/sys/class/hwmon/hwmon%d/temp%d_label", h, t);
      if (sysfs_read(p, label, sizeof label)) snprintf(label, sizeof label, "temp%d", t);
      snprintf(key, sizeof key, "hwmon%d/%s/%s", h, name, label);
      jw_num(w, key, (double)v / 1000.0);
    }
  }
  for (int z = 0; z < 64; z++) {
    snprintf(p, sizeof p, "/sys/class/thermal/thermal_zone%d/type", z);
    if (sysfs_read(p, name, sizeof name)) continue;
    snprintf(p, sizeof p, "/sys/class/thermal/thermal_zone%d/temp", z);
    long long v = sysfs_read_ll(p, LLONG_MIN);
    if (v == LLONG_MIN || v < -50000) continue; /* absent sensors report about -273 C */
    snprintf(key, sizeof key, "thermal_zone%d/%s", z, name);
    jw_num(w, key, (double)v / 1000.0);
  }
  jw_obj_end(w);
}

static void idle_states_json(pmk_jw *w, const pmk_machine *m) {
  char p[300], name[64];
  int ref = m->ncpu ? m->cpus[0].id : 0;
  int ns = sysfs_idle_state_count(ref);
  jw_arr_begin(w, "idle_states");
  for (int s = 0; s < ns; s++) {
    jw_obj_begin(w, NULL);
    snprintf(p, sizeof p, SYS_CPU "/cpu%d/cpuidle/state%d/name", ref, s);
    jw_str(w, "name", sysfs_read(p, name, sizeof name) ? NULL : name);
    snprintf(p, sizeof p, SYS_CPU "/cpu%d/cpuidle/state%d/latency", ref, s);
    jw_int(w, "exit_latency_us", sysfs_read_ll(p, -1));
    int disabled = 0;
    for (int i = 0; i < m->ncpu; i++) {
      snprintf(p, sizeof p, SYS_CPU "/cpu%d/cpuidle/state%d/disable", m->cpus[i].id, s);
      disabled += sysfs_read_ll(p, 0) != 0;
    }
    jw_int(w, "disabled_cpus", disabled);
    jw_obj_end(w);
  }
  jw_arr_end(w);
}

static void cpufreq_json(pmk_jw *w) {
  char p[300], v[128];
  static const char *str_fields[][2] = {{"scaling_driver", "driver"},
                                        {"scaling_governor", "governor"},
                                        {"energy_performance_preference", "epp"},
                                        {"related_cpus", "cpus"}};
  static const char *num_fields[][2] = {{"scaling_min_freq", "min_khz"},
                                        {"scaling_max_freq", "max_khz"},
                                        {"cpuinfo_min_freq", "hw_min_khz"},
                                        {"cpuinfo_max_freq", "hw_max_khz"}};
  jw_arr_begin(w, "cpufreq");
  for (int pol = 0; pol < g_ncpu_conf; pol++) {
    snprintf(p, sizeof p, SYS_CPU "/cpufreq/policy%d", pol);
    if (access(p, R_OK)) continue;
    jw_obj_begin(w, NULL);
    jw_int(w, "policy", pol);
    for (size_t i = 0; i < sizeof str_fields / sizeof *str_fields; i++) {
      snprintf(p, sizeof p, SYS_CPU "/cpufreq/policy%d/%s", pol, str_fields[i][0]);
      jw_str(w, str_fields[i][1], sysfs_read(p, v, sizeof v) ? NULL : v);
    }
    for (size_t i = 0; i < sizeof num_fields / sizeof *num_fields; i++) {
      snprintf(p, sizeof p, SYS_CPU "/cpufreq/policy%d/%s", pol, num_fields[i][0]);
      long long x = sysfs_read_ll(p, -1);
      if (x >= 0) jw_int(w, num_fields[i][1], x);
      else jw_null(w, num_fields[i][1]);
    }
    jw_obj_end(w);
  }
  jw_arr_end(w);
}

static int ac_online(void) {
  char p[300], v[32];
  DIR *d = opendir("/sys/class/power_supply");
  if (!d) return -1;
  int result = -1;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (e->d_name[0] == '.') continue;
    snprintf(p, sizeof p, "/sys/class/power_supply/%s/type", e->d_name);
    if (sysfs_read(p, v, sizeof v) || strcmp(v, "Mains")) continue;
    snprintf(p, sizeof p, "/sys/class/power_supply/%s/online", e->d_name);
    long long on = sysfs_read_ll(p, -1);
    if (on >= 0) result = result == 1 ? 1 : (int)on;
  }
  closedir(d);
  return result;
}

void pal_state_json(pmk_jw *w, const char *key, const pmk_machine *m, int measure_power) {
  char v[128];
  jw_obj_begin(w, key);
  jw_str(w, "power_mode", sysfs_read("/sys/firmware/acpi/platform_profile", v, sizeof v) ? NULL : v);
  long long smt = sysfs_read_ll(SYS_CPU "/smt/active", -1);
  if (smt >= 0) jw_bool(w, "smt_active", (int)smt);
  else jw_null(w, "smt_active");
  long long boost = sysfs_read_ll(SYS_CPU "/cpufreq/boost", -1);
  if (boost >= 0) jw_bool(w, "boost", (int)boost);
  else jw_null(w, "boost");
  int ac = ac_online();
  if (ac >= 0) jw_bool(w, "ac_online", ac);
  else jw_null(w, "ac_online");
  cpufreq_json(w);
  idle_states_json(w, m);
  temps_json(w);
  jw_num(w, "idle_power_w", measure_power ? pal_idle_power_w(2.0) : NAN);
  jw_obj_end(w);
}

