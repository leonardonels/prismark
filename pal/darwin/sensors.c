/*
 * Clocks, CPU energy and die temperature on Apple silicon Macs, read without root.
 *
 * Apple documents none of these. They come from private interfaces that every macOS on Apple silicon ships
 * and that powermetrics itself is built on:
 *
 *   IOReport "CPU Stats" / "CPU Core Performance States": for every core, the time spent in each DVFS state.
 *     With the frequency of each state (IORegistry, pmgr "voltage-states*-sram") the average clock of a core
 *     type over an interval is the residency-weighted mean over its cores and active states,
 *         f = sum_{c,s} dr_{c,s} f_s / sum_{c,s} dr_{c,s}       (s not idle or off).
 *     Threads are steered to a core type, not pinned (pal_darwin.c), so the clock of a type, weighted by how
 *     long each core ran, is the clock the work ran at: a busy core dominates its idle neighbours.
 *   IOReport "Energy Model" / "CPU Energy": the energy of the CPU clusters (not the GPU, ANE or DRAM), summed
 *     here into a counter, so power is dE/dt between two readings, like RAPL.
 *   IOHIDEventSystemClient: the SoC die temperature sensors ("PMU tdie*"); the hottest is the CPU temperature.
 *
 * Every private symbol is looked up at run time: where one is missing the reading is reported unavailable
 * instead of the program failing to load. One IOReport sample costs well under a millisecond and is taken at
 * most once per 50 ms, by the monitoring thread (once per measurement window).
 *
 * Copyright 2026 The Prismark Authors. Apache-2.0.
 */
#include <TargetConditionals.h>
#include <math.h>

#include "sensors.h"

#if TARGET_OS_OSX && defined(__aarch64__)
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_STATES 64

typedef struct IOReportSubscription *IOReportSubscriptionRef;
static struct {
  CFDictionaryRef (*copy_channels)(CFStringRef, CFStringRef, uint64_t, uint64_t, uint64_t);
  void (*merge)(CFDictionaryRef, CFDictionaryRef, CFTypeRef);
  IOReportSubscriptionRef (*subscribe)(void *, CFMutableDictionaryRef, CFMutableDictionaryRef *, uint64_t, CFTypeRef);
  CFDictionaryRef (*samples)(IOReportSubscriptionRef, CFMutableDictionaryRef, CFTypeRef);
  CFDictionaryRef (*delta)(CFDictionaryRef, CFDictionaryRef, CFTypeRef);
  CFStringRef (*group)(CFDictionaryRef);
  CFStringRef (*name)(CFDictionaryRef);
  CFStringRef (*unit)(CFDictionaryRef);
  int64_t (*int_value)(CFDictionaryRef, int32_t);
  int32_t (*state_count)(CFDictionaryRef);
  CFStringRef (*state_name)(CFDictionaryRef, int32_t);
  int64_t (*state_residency)(CFDictionaryRef, int32_t);
} ior;

static struct {
  CFTypeRef (*create)(CFAllocatorRef);
  int (*set_matching)(CFTypeRef, CFDictionaryRef);
  CFArrayRef (*copy_services)(CFTypeRef);
  CFTypeRef (*copy_property)(CFTypeRef, CFStringRef);
  CFTypeRef (*copy_event)(CFTypeRef, int64_t, int32_t, int64_t);
  double (*float_value)(CFTypeRef, int32_t);
} hid;
#define HID_EVENT_TEMPERATURE 15

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static IOReportSubscriptionRef g_sub;
static CFMutableDictionaryRef g_subbed;
static CFDictionaryRef g_prev;
static uint64_t g_prev_ns;
static double g_tab[2][MAX_STATES]; /* kHz per DVFS state; [0] performance, [1] efficiency */
static int g_ntab[2];
static int g_khz[2];
static int g_have_clock, g_have_energy, g_seen_clock, g_seen_energy;
static double g_energy_uj;
static CFTypeRef g_hid;
static CFMutableArrayRef g_tdie;

static uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

#define SYM(lib, dst, name) (*(void **)&(dst) = dlsym(lib, name))

/* ---------- DVFS tables ---------- */

static int read_table(io_registry_entry_t e, const char *key, double *out) {
  CFStringRef k = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);
  CFTypeRef v = IORegistryEntryCreateCFProperty(e, k, kCFAllocatorDefault, 0);
  CFRelease(k);
  int n = 0;
  if (v && CFGetTypeID(v) == CFDataGetTypeID()) {
    const uint8_t *p = CFDataGetBytePtr(v);
    CFIndex len = CFDataGetLength(v);
    /* Pairs of 32-bit (frequency, voltage); the frequency is in Hz up to M3 and in kHz from M4. */
    for (CFIndex i = 0; i + 8 <= len && n < MAX_STATES; i += 8) {
      uint32_t f;
      memcpy(&f, p + i, 4);
      out[n++] = f > 100000000u ? f / 1000.0 : (double)f;
    }
  }
  if (v) CFRelease(v);
  return n;
}

static double tab_max(int t) {
  double x = 0;
  for (int i = 0; i < g_ntab[t]; i++) x = fmax(x, g_tab[t][i]);
  return x;
}

static double tab_min(int t) {
  double x = 0;
  for (int i = 0; i < g_ntab[t]; i++)
    if (g_tab[t][i] > 0 && (x == 0 || g_tab[t][i] < x)) x = g_tab[t][i];
  return x;
}

static void read_tables(void) {
  io_iterator_t it;
  if (IOServiceGetMatchingServices(MACH_PORT_NULL, IOServiceMatching("AppleARMIODevice"), &it) != KERN_SUCCESS) return;
  io_object_t e;
  double a[MAX_STATES], b[MAX_STATES];
  int na = 0, nb = 0;
  while ((e = IOIteratorNext(it))) {
    io_name_t nm;
    if (IORegistryEntryGetName(e, nm) == KERN_SUCCESS && !strcmp(nm, "pmgr")) {
      na = read_table(e, "voltage-states1-sram", a);
      nb = read_table(e, "voltage-states5-sram", b);
    }
    IOObjectRelease(e);
  }
  IOObjectRelease(it);
  /* Which table is which core type: the performance cores are the faster ones. */
  g_ntab[0] = nb, g_ntab[1] = na;
  memcpy(g_tab[0], b, sizeof b);
  memcpy(g_tab[1], a, sizeof a);
  if (tab_max(1) > tab_max(0)) {
    g_ntab[0] = na, g_ntab[1] = nb;
    memcpy(g_tab[0], a, sizeof a);
    memcpy(g_tab[1], b, sizeof b);
  }
}

/* ---------- IOReport ---------- */

static int cfstr(CFStringRef s, char *out, size_t n) {
  return s && CFStringGetCString(s, out, (CFIndex)n, kCFStringEncodingUTF8);
}

/* One interval (an IOReport delta): the clock of each core type, and the energy added to the counter. */
static void process(CFDictionaryRef d) {
  CFArrayRef arr = CFDictionaryGetValue(d, CFSTR("IOReportChannels"));
  if (!arr || CFGetTypeID(arr) != CFArrayGetTypeID()) return;
  double fw[2] = {0}, rw[2] = {0};
  for (CFIndex i = 0; i < CFArrayGetCount(arr); i++) {
    CFDictionaryRef ch = CFArrayGetValueAtIndex(arr, i);
    char group[64], name[64];
    if (!cfstr(ior.group(ch), group, sizeof group) || !cfstr(ior.name(ch), name, sizeof name)) continue;
    if (!strcmp(group, "Energy Model")) {
      if (strcmp(name, "CPU Energy")) continue;
      g_seen_energy = 1;
      char unit[16] = "";
      cfstr(ior.unit(ch), unit, sizeof unit);
      double scale = !strcmp(unit, "mJ") ? 1e3 : !strcmp(unit, "nJ") ? 1e-3 : 1.0; /* to microjoules */
      int64_t v = ior.int_value(ch, 0);
      if (v > 0) g_energy_uj += (double)v * scale;
    } else if (!strcmp(group, "CPU Stats")) {
      /* Per-core channels: "ECPU0", "PCPU3", ... (the first letter is the core type). */
      int t = name[0] == 'P' ? 0 : name[0] == 'E' ? 1 : -1;
      if (t < 0 || !strstr(name, "CPU") || !g_ntab[t]) continue;
      g_seen_clock = 1;
      int ns = ior.state_count(ch), act = 0;
      int64_t res[MAX_STATES];
      for (int s = 0; s < ns && act < MAX_STATES; s++) {
        char sn[32];
        if (!cfstr(ior.state_name(ch, s), sn, sizeof sn)) continue;
        if (strstr(sn, "IDLE") || strstr(sn, "DOWN") || strstr(sn, "OFF")) continue;
        res[act++] = ior.state_residency(ch, s);
      }
      /* The active states are the table's states in order; with fewer of them, they are its top ones. */
      int off = g_ntab[t] > act ? g_ntab[t] - act : 0;
      for (int j = 0; j < act; j++) {
        int k = j + off;
        if (k >= g_ntab[t] || g_tab[t][k] <= 0 || res[j] <= 0) continue;
        fw[t] += (double)res[j] * g_tab[t][k];
        rw[t] += (double)res[j];
      }
    }
  }
  for (int t = 0; t < 2; t++) g_khz[t] = rw[t] > 0 ? (int)(fw[t] / rw[t] + 0.5) : 0;
}

static void sample_locked(void) {
  CFDictionaryRef cur = ior.samples(g_sub, g_subbed, NULL);
  uint64_t t = now_ns();
  if (!cur) return;
  if (g_prev) {
    CFDictionaryRef d = ior.delta(g_prev, cur, NULL);
    if (d) {
      process(d);
      CFRelease(d);
    }
    CFRelease(g_prev);
  }
  g_prev = cur;
  g_prev_ns = t;
}

/* A reading no older than 50 ms; after a long gap (or none yet), one over a fresh 100 ms interval. */
static void refresh_locked(void) {
  uint64_t t = now_ns();
  if (g_prev && t - g_prev_ns < 50000000ull) return;
  if (!g_prev || t - g_prev_ns > 10000000000ull) {
    sample_locked();
    struct timespec ts = {0, 100000000};
    nanosleep(&ts, NULL);
  }
  sample_locked();
}

static void init_ioreport(void) {
  void *lib = dlopen("/usr/lib/libIOReport.dylib", RTLD_LAZY | RTLD_LOCAL);
  if (!lib) return;
  if (!SYM(lib, ior.copy_channels, "IOReportCopyChannelsInGroup") || !SYM(lib, ior.merge, "IOReportMergeChannels") ||
      !SYM(lib, ior.subscribe, "IOReportCreateSubscription") || !SYM(lib, ior.samples, "IOReportCreateSamples") ||
      !SYM(lib, ior.delta, "IOReportCreateSamplesDelta") || !SYM(lib, ior.group, "IOReportChannelGetGroup") ||
      !SYM(lib, ior.name, "IOReportChannelGetChannelName") || !SYM(lib, ior.unit, "IOReportChannelGetUnitLabel") ||
      !SYM(lib, ior.int_value, "IOReportSimpleGetIntegerValue") || !SYM(lib, ior.state_count, "IOReportStateGetCount") ||
      !SYM(lib, ior.state_name, "IOReportStateGetNameForIndex") ||
      !SYM(lib, ior.state_residency, "IOReportStateGetResidency"))
    return;
  CFDictionaryRef cpu = ior.copy_channels(CFSTR("CPU Stats"), CFSTR("CPU Core Performance States"), 0, 0, 0);
  CFDictionaryRef energy = ior.copy_channels(CFSTR("Energy Model"), NULL, 0, 0, 0);
  CFDictionaryRef base = cpu ? cpu : energy;
  if (!base) return;
  if (cpu && energy) ior.merge(cpu, energy, NULL);
  CFMutableDictionaryRef want = CFDictionaryCreateMutableCopy(kCFAllocatorDefault, CFDictionaryGetCount(base), base);
  if (cpu) CFRelease(cpu);
  if (energy) CFRelease(energy);
  g_sub = ior.subscribe(NULL, want, &g_subbed, 0, NULL);
  CFRelease(want);
  if (!g_sub) return;
  /* What the first interval holds tells which readings this Mac has. */
  sample_locked();
  struct timespec ts = {0, 50000000};
  nanosleep(&ts, NULL);
  sample_locked();
  g_have_energy = g_seen_energy;
  g_have_clock = g_seen_clock;
  g_energy_uj = 0;
}

/* ---------- temperature ---------- */

static void init_hid(void) {
  void *lib = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_LAZY | RTLD_LOCAL);
  if (!lib) return;
  if (!SYM(lib, hid.create, "IOHIDEventSystemClientCreate") ||
      !SYM(lib, hid.set_matching, "IOHIDEventSystemClientSetMatching") ||
      !SYM(lib, hid.copy_services, "IOHIDEventSystemClientCopyServices") ||
      !SYM(lib, hid.copy_property, "IOHIDServiceClientCopyProperty") ||
      !SYM(lib, hid.copy_event, "IOHIDServiceClientCopyEvent") || !SYM(lib, hid.float_value, "IOHIDEventGetFloatValue"))
    return;
  g_hid = hid.create(kCFAllocatorDefault);
  if (!g_hid) return;
  int page = 0xff00, usage = 5; /* Apple vendor page, temperature sensors */
  CFNumberRef np = CFNumberCreate(NULL, kCFNumberIntType, &page), nu = CFNumberCreate(NULL, kCFNumberIntType, &usage);
  const void *keys[] = {CFSTR("PrimaryUsagePage"), CFSTR("PrimaryUsage")}, *vals[] = {np, nu};
  CFDictionaryRef match = CFDictionaryCreate(NULL, keys, vals, 2, &kCFTypeDictionaryKeyCallBacks,
                                             &kCFTypeDictionaryValueCallBacks);
  hid.set_matching(g_hid, match);
  CFRelease(match);
  CFRelease(np);
  CFRelease(nu);
  CFArrayRef services = hid.copy_services(g_hid);
  if (!services) return;
  g_tdie = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
  for (CFIndex i = 0; i < CFArrayGetCount(services); i++) {
    CFTypeRef s = CFArrayGetValueAtIndex(services, i);
    CFTypeRef p = hid.copy_property(s, CFSTR("Product"));
    char name[64];
    if (p && CFGetTypeID(p) == CFStringGetTypeID() && cfstr(p, name, sizeof name) && !strncmp(name, "PMU tdie", 8))
      CFArrayAppendValue(g_tdie, s);
    if (p) CFRelease(p);
  }
  CFRelease(services);
}

/* ---------- CPU numbers ---------- */

static int prop_u32(io_registry_entry_t e, CFStringRef key, int def) {
  CFTypeRef v = IORegistryEntryCreateCFProperty(e, key, kCFAllocatorDefault, 0);
  int r = def;
  if (v && CFGetTypeID(v) == CFDataGetTypeID() && CFDataGetLength(v) >= 4) {
    uint32_t x;
    memcpy(&x, CFDataGetBytePtr(v), 4);
    r = (int)x;
  } else if (v && CFGetTypeID(v) == CFNumberGetTypeID()) {
    CFNumberGetValue(v, kCFNumberIntType, &r);
  }
  if (v) CFRelease(v);
  return r;
}

/* The children of IODeviceTree:/cpus ("cpu0", "cpu1", ...), each with "cluster-type" ("E" or "P"); the CPU
   number is "logical-cpu-id", else "cpu-id", else the number in the node's name. */
int dsens_cpu_types(int *types, int max) {
  int found = 0;
  for (int i = 0; i < max; i++) types[i] = -1;
  io_registry_entry_t cpus = IORegistryEntryFromPath(MACH_PORT_NULL, "IODeviceTree:/cpus");
  if (cpus == MACH_PORT_NULL) return 0;
  io_iterator_t it;
  if (IORegistryEntryGetChildIterator(cpus, kIODeviceTreePlane, &it) != KERN_SUCCESS) {
    IOObjectRelease(cpus);
    return 0;
  }
  io_registry_entry_t e;
  while ((e = IOIteratorNext(it))) {
    io_name_t nm;
    int node = -1, type = -1;
    if (IORegistryEntryGetName(e, nm) == KERN_SUCCESS && !strncmp(nm, "cpu", 3)) node = atoi(nm + 3);
    CFTypeRef ct = IORegistryEntryCreateCFProperty(e, CFSTR("cluster-type"), kCFAllocatorDefault, 0);
    if (ct && CFGetTypeID(ct) == CFDataGetTypeID() && CFDataGetLength(ct) > 0) {
      char t = (char)CFDataGetBytePtr(ct)[0];
      type = t == 'P' ? 0 : t == 'E' ? 1 : -1;
    }
    if (ct) CFRelease(ct);
    int num = prop_u32(e, CFSTR("logical-cpu-id"), prop_u32(e, CFSTR("cpu-id"), node));
    IOObjectRelease(e);
    if (type >= 0 && num >= 0 && num < max && types[num] < 0) {
      types[num] = type;
      found++;
    }
  }
  IOObjectRelease(it);
  IOObjectRelease(cpus);
  return found;
}

/* ---------- interface ---------- */

void dsens_init(pmk_machine *m, int nperf) {
  pthread_mutex_lock(&g_mu);
  read_tables();
  init_ioreport();
  init_hid();
  pthread_mutex_unlock(&g_mu);
  for (int i = 0; i < m->ncpu; i++) {
    int t = m->cpus[i].id < nperf ? 0 : 1;
    m->cpus[i].max_khz = (int)tab_max(t);
    m->cpus[i].min_khz = (int)tab_min(t);
  }
  if (g_have_energy) snprintf(m->caps.power, sizeof m->caps.power, "ioreport:CPU Energy");
}

int dsens_cpu_khz(int efficiency) {
  if (!g_have_clock) return 0;
  pthread_mutex_lock(&g_mu);
  refresh_locked();
  int k = g_khz[efficiency ? 1 : 0];
  pthread_mutex_unlock(&g_mu);
  return k;
}

int dsens_energy_uj(long long *uj, uint64_t *ns) {
  if (!g_have_energy) return -1;
  pthread_mutex_lock(&g_mu);
  refresh_locked();
  *uj = (long long)g_energy_uj;
  *ns = g_prev_ns; /* the time of the sample the energy is counted to */
  pthread_mutex_unlock(&g_mu);
  return 0;
}

double dsens_temp_c(void) {
  if (!g_tdie || !CFArrayGetCount(g_tdie)) return NAN;
  double hot = NAN;
  pthread_mutex_lock(&g_mu);
  for (CFIndex i = 0; i < CFArrayGetCount(g_tdie); i++) {
    CFTypeRef ev = hid.copy_event(CFArrayGetValueAtIndex(g_tdie, i), HID_EVENT_TEMPERATURE, 0, 0);
    if (!ev) continue;
    double v = hid.float_value(ev, HID_EVENT_TEMPERATURE << 16);
    CFRelease(ev);
    if (v > 0 && v < 150 && !(v <= hot)) hot = v;
  }
  pthread_mutex_unlock(&g_mu);
  return hot;
}

#else /* Intel Macs, iOS, iPadOS */

void dsens_init(pmk_machine *m, int nperf) {
  (void)m;
  (void)nperf;
}
int dsens_cpu_khz(int efficiency) {
  (void)efficiency;
  return 0;
}
double dsens_temp_c(void) { return NAN; }
int dsens_cpu_types(int *types, int max) {
  (void)types;
  (void)max;
  return 0;
}
int dsens_energy_uj(long long *uj, uint64_t *ns) {
  (void)uj;
  (void)ns;
  return -1;
}

#endif
