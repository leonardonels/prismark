/*
 * Windows platform layer (x86-64, ARM64).
 *
 *   topology     GetLogicalProcessorInformationEx; hybrid core types from
 *                EfficiencyClass (higher = faster), SMT from the core masks
 *   pinning      thread group affinity; K1x child processes through the
 *                process affinity mask
 *   timing       QueryPerformanceCounter; deadlines with high-resolution
 *                waitable timers
 *   state        power plan (AC values of the active scheme): processor
 *                minimum and maximum state, idle disable. Read only: the
 *                machine is measured as it is configured.
 *   telemetry    package power from the Energy Meter Interface (EMI), the
 *                in-box RAPL front end; its device may admit administrators
 *                only. Clock from the "% Processor Performance" counter.
 *                No temperature without a kernel driver.
 *
 * Copyright 2026 The Prismark Authors. Apache-2.0.
 */
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <bcrypt.h>
#include <malloc.h>
#include <math.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <powrprof.h>
#include <setupapi.h>
#include <winioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <timeapi.h>

#include "pal.h"

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

static LARGE_INTEGER g_freq;
static _Thread_local HANDLE t_timer;
static int g_high_res_timer;
static GROUP_AFFINITY *g_cpu_affinity; /* per CPU index in pmk_machine.cpus order */
static int g_ncpu;


/* Power and clock telemetry (below). */
static HANDLE g_emi = INVALID_HANDLE_VALUE;
static int g_emi_channel, g_emi_channels; /* the package channel and the device's channel count */
static SRWLOCK g_clock_lock = SRWLOCK_INIT;
static PDH_HQUERY g_clock_query;
static PDH_HCOUNTER g_clock_perf, g_clock_nominal;
static uint64_t g_clock_ns; /* last collection */
static int *g_clock_khz;    /* per CPU id */
static int g_clock_n;
static void find_power(pmk_caps *c);
static void clock_open(void);

/* ---------- machine description ---------- */

static void os_version(char *out, size_t n) {
  typedef LONG(WINAPI * rtl_get_version)(OSVERSIONINFOEXW *);
  OSVERSIONINFOEXW v = {0};
  v.dwOSVersionInfoSize = sizeof v;
  rtl_get_version f = (rtl_get_version)(void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
  if (f && f(&v) == 0) snprintf(out, n, "%lu.%lu.%lu", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber);
  else snprintf(out, n, "unknown");
}

static void cpu_model(char *out, size_t n) {
  HKEY k;
  out[0] = 0;
  if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ, &k) == 0) {
    DWORD len = (DWORD)n;
    if (RegQueryValueExA(k, "ProcessorNameString", NULL, NULL, (BYTE *)out, &len)) out[0] = 0;
    RegCloseKey(k);
  }
  /* Trim the padding some vendors put in the name. */
  size_t len = strlen(out);
  while (len && out[len - 1] == ' ') out[--len] = 0;
}

static int popcount64(KAFFINITY m) {
  int c = 0;
  for (; m; m &= m - 1) c++;
  return c;
}

static int read_topology(pmk_machine *m) {
  DWORD len = 0;
  GetLogicalProcessorInformationEx(RelationProcessorCore, NULL, &len);
  unsigned char *buf = malloc(len);
  if (!buf || !GetLogicalProcessorInformationEx(RelationProcessorCore, (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buf, &len)) {
    free(buf);
    return -1;
  }
  int total = 0;
  for (DWORD off = 0; off < len;) {
    PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX e = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(buf + off);
    for (WORD g = 0; g < e->Processor.GroupCount; g++) total += popcount64(e->Processor.GroupMask[g].Mask);
    off += e->Size;
  }
  m->cpus = calloc((size_t)total, sizeof *m->cpus);
  g_cpu_affinity = calloc((size_t)total, sizeof *g_cpu_affinity);
  if (!m->cpus || !g_cpu_affinity) {
    free(buf);
    return -1;
  }
  int classes[256] = {0}, core = 0;
  for (DWORD off = 0; off < len; core++) {
    PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX e = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(buf + off);
    classes[e->Processor.EfficiencyClass] = 1;
    int smt = 0;
    for (WORD g = 0; g < e->Processor.GroupCount; g++) {
      const GROUP_AFFINITY *ga = &e->Processor.GroupMask[g];
      for (int bit = 0; bit < 64; bit++) {
        if (!(ga->Mask & ((KAFFINITY)1 << bit))) continue;
        pmk_cpu *c = &m->cpus[m->ncpu];
        c->id = ga->Group * 64 + bit;
        c->type = e->Processor.EfficiencyClass; /* remapped to a rank below */
        c->core = core;
        c->smt = smt++;
        g_cpu_affinity[m->ncpu].Group = ga->Group;
        g_cpu_affinity[m->ncpu].Mask = (KAFFINITY)1 << bit;
        m->ncpu++;
      }
    }
    off += e->Size;
  }
  free(buf);
  /* Last-level cache domains: the CPUs each L3 (or, without one, L2) serves. */
  for (int level = 3; level >= 2; level--) {
    len = 0;
    GetLogicalProcessorInformationEx(RelationCache, NULL, &len);
    buf = malloc(len);
    int domains = 0;
    if (buf && GetLogicalProcessorInformationEx(RelationCache, (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buf, &len))
      for (DWORD off = 0; off < len;) {
        PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX e = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(buf + off);
        if (e->Cache.Level == level && e->Cache.Type != CacheInstruction) {
          for (int i = 0; i < m->ncpu; i++)
            if (g_cpu_affinity[i].Group == e->Cache.GroupMask.Group && (g_cpu_affinity[i].Mask & e->Cache.GroupMask.Mask))
              m->cpus[i].llc = domains;
          domains++;
        }
        off += e->Size;
      }
    free(buf);
    if (domains) break;
  }
  /* Rank efficiency classes, highest (fastest) first. */
  int rank[256], ntypes = 0;
  for (int cls = 255; cls >= 0; cls--)
    if (classes[cls]) rank[cls] = ntypes < PMK_MAX_TYPES ? ntypes++ : PMK_MAX_TYPES - 1;
  static const char *names[][PMK_MAX_TYPES] = {{"P"}, {"P", "E"}, {"P", "M", "E"}, {"P", "M1", "M2", "E"}};
  m->ntypes = ntypes ? ntypes : 1;
  for (int t = 0; t < m->ntypes; t++) snprintf(m->type_names[t], sizeof m->type_names[t], "%s", names[m->ntypes - 1][t]);
  for (int i = 0; i < m->ncpu; i++) m->cpus[i].type = rank[m->cpus[i].type];
  g_ncpu = m->ncpu;
  return 0;
}

static uint64_t cache_size(int level) {
  DWORD len = 0;
  GetLogicalProcessorInformationEx(RelationCache, NULL, &len);
  unsigned char *buf = malloc(len);
  uint64_t size = 0;
  if (buf && GetLogicalProcessorInformationEx(RelationCache, (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buf, &len)) {
    for (DWORD off = 0; off < len;) {
      PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX e = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(buf + off);
      if (e->Cache.Level == level && e->Cache.CacheSize > size) size = e->Cache.CacheSize;
      off += e->Size;
    }
  }
  free(buf);
  return size;
}

int pal_init(pmk_machine *m) {
  memset(m, 0, sizeof *m);
  QueryPerformanceFrequency(&g_freq);
  snprintf(m->os, sizeof m->os, "windows");
  os_version(m->kernel, sizeof m->kernel);
  SYSTEM_INFO si;
  GetNativeSystemInfo(&si);
  snprintf(m->isa, sizeof m->isa, "%s",
           si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64 ? "aarch64" : "x86_64");
  cpu_model(m->model, sizeof m->model);
  if (read_topology(m)) return -1;
  m->llc_bytes = cache_size(3);
  if (!m->llc_bytes) m->llc_bytes = cache_size(2);
  HANDLE t = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  g_high_res_timer = t != NULL;
  if (t) CloseHandle(t);
  m->caps.pinning = 1;
  m->caps.perf_counters = 0; /* QueryThreadCycleTime counts reference cycles, not core cycles */
  m->caps.timer_slack = g_high_res_timer;
  find_power(&m->caps);
  clock_open();
  return 0;
}

void pal_fini(pmk_machine *m) {
  if (g_emi != INVALID_HANDLE_VALUE) CloseHandle(g_emi);
  g_emi = INVALID_HANDLE_VALUE;
  if (g_clock_query) PdhCloseQuery(g_clock_query);
  g_clock_query = NULL;
  free(g_clock_khz);
  g_clock_khz = NULL;
  g_clock_n = 0;
  free(m->cpus);
  m->cpus = NULL;
  free(g_cpu_affinity);
  g_cpu_affinity = NULL;
}

/* ---------- time, placement ---------- */

static uint64_t qpc_ns(void) {
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  return (uint64_t)((double)c.QuadPart * 1e9 / (double)g_freq.QuadPart);
}

uint64_t pal_now_ns(void) { return qpc_ns(); }
uint64_t pal_now_raw_ns(void) { return qpc_ns(); }

void pal_sleep_until_ns(uint64_t t) {
  if (!t_timer)
    t_timer = CreateWaitableTimerExW(NULL, NULL, g_high_res_timer ? CREATE_WAITABLE_TIMER_HIGH_RESOLUTION : 0,
                                     TIMER_ALL_ACCESS);
  for (;;) {
    uint64_t now = qpc_ns();
    if (now >= t) return;
    LARGE_INTEGER due;
    due.QuadPart = -(LONGLONG)((t - now + 99) / 100); /* relative, in 100 ns units */
    if (!t_timer || !SetWaitableTimer(t_timer, &due, 0, NULL, NULL, FALSE)) {
      Sleep((DWORD)((t - now) / 1000000));
      continue;
    }
    WaitForSingleObject(t_timer, INFINITE);
  }
}

void pal_sleep_ns(uint64_t ns) { pal_sleep_until_ns(pal_now_ns() + ns); }

/* CPU ids are group * 64 + bit. */
static int index_of(int cpu) {
  for (int i = 0; i < g_ncpu; i++)
    if (cpu / 64 == g_cpu_affinity[i].Group && g_cpu_affinity[i].Mask == ((KAFFINITY)1 << (cpu % 64))) return i;
  return -1;
}

int pal_pin_self(int cpu) {
  int i = index_of(cpu);
  if (i < 0) return -1;
  GROUP_AFFINITY ga = g_cpu_affinity[i];
  return SetThreadGroupAffinity(GetCurrentThread(), &ga, NULL) ? 0 : -1;
}

int pal_unpin_self(void) {
  DWORD_PTR proc, sys;
  if (!GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys)) return -1;
  SetProcessAffinityMask(GetCurrentProcess(), sys);
  GROUP_AFFINITY ga = {0};
  ga.Mask = sys;
  ga.Group = 0;
  return SetThreadGroupAffinity(GetCurrentThread(), &ga, NULL) ? 0 : -1;
}

/* Child processes inherit the process affinity mask (group 0 only). */
int pal_pin_self_set(const int *cpus, int n) {
  KAFFINITY mask = 0;
  for (int i = 0; i < n; i++)
    if (cpus[i] >= 0 && cpus[i] < 64) mask |= (KAFFINITY)1 << cpus[i];
  return mask && SetProcessAffinityMask(GetCurrentProcess(), mask) ? 0 : -1;
}

int pal_current_cpu(void) {
  PROCESSOR_NUMBER pn;
  GetCurrentProcessorNumberEx(&pn);
  return pn.Group * 64 + pn.Number;
}

/*
 * The clock: CallNtPowerInformation reports CurrentMhz capped at the base clock on current CPUs, so it is not used.
 * "% Processor Performance" (what Task Manager shows) is the delivered clock against the nominal one, averaged
 * between two collections; times "Processor Frequency" (the nominal clock, per core type) it is the clock.
 */
static void clock_open(void) {
  if (PdhOpenQueryW(NULL, 0, &g_clock_query)) return;
  if (PdhAddEnglishCounterW(g_clock_query, L"\\Processor Information(*)\\% Processor Performance", 0, &g_clock_perf) ||
      PdhAddEnglishCounterW(g_clock_query, L"\\Processor Information(*)\\Processor Frequency", 0, &g_clock_nominal) ||
      PdhCollectQueryData(g_clock_query)) {
    PdhCloseQuery(g_clock_query);
    g_clock_query = NULL;
    return;
  }
  g_clock_ns = pal_now_ns();
}

/* The counter's values by CPU id (instances are "group,number"); NULL on error. Free with free(). */
static double *counter_by_cpu(PDH_HCOUNTER h, int *n) {
  DWORD size = 0, count = 0;
  if (PdhGetFormattedCounterArrayW(h, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, &count, NULL) != (PDH_STATUS)PDH_MORE_DATA)
    return NULL;
  PDH_FMT_COUNTERVALUE_ITEM_W *items = malloc(size);
  double *v = NULL;
  if (items && PdhGetFormattedCounterArrayW(h, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, &count, items) == 0) {
    int max = -1;
    for (DWORD i = 0; i < count; i++) {
      int g, k;
      if (swscanf(items[i].szName, L"%d,%d", &g, &k) == 2 && g * 64 + k > max) max = g * 64 + k;
    }
    v = max >= 0 ? calloc((size_t)max + 1, sizeof *v) : NULL;
    for (DWORD i = 0; v && i < count; i++) {
      int g, k;
      if (swscanf(items[i].szName, L"%d,%d", &g, &k) == 2 && items[i].FmtValue.CStatus == 0)
        v[g * 64 + k] = items[i].FmtValue.doubleValue;
    }
    *n = max + 1;
  }
  free(items);
  return v;
}

int pal_cpu_cur_khz(int cpu) {
  if (!g_clock_query || cpu < 0) return 0;
  AcquireSRWLockExclusive(&g_clock_lock);
  uint64_t now = pal_now_ns();
  if (now - g_clock_ns >= 20000000 && PdhCollectQueryData(g_clock_query) == 0) { /* one collection per window */
    g_clock_ns = now;
    int np = 0, nn = 0;
    double *perf = counter_by_cpu(g_clock_perf, &np), *nominal = counter_by_cpu(g_clock_nominal, &nn);
    free(g_clock_khz);
    g_clock_khz = NULL;
    g_clock_n = 0;
    if (perf && nominal && (g_clock_khz = calloc((size_t)np, sizeof *g_clock_khz))) {
      g_clock_n = np;
      for (int i = 0; i < np && i < nn; i++) g_clock_khz[i] = (int)(perf[i] / 100 * nominal[i] * 1000);
    }
    free(perf);
    free(nominal);
  }
  int khz = cpu < g_clock_n ? g_clock_khz[cpu] : 0;
  ReleaseSRWLockExclusive(&g_clock_lock);
  return khz;
}

uint64_t pal_mem_available(void) {
  MEMORYSTATUSEX ms;
  ms.dwLength = sizeof ms;
  return GlobalMemoryStatusEx(&ms) ? (uint64_t)ms.ullAvailPhys : 0;
}

int64_t pal_input_idle_ms(void) {
  LASTINPUTINFO li = {sizeof li, 0};
  if (!GetLastInputInfo(&li)) return -1;
  return (int64_t)(DWORD)(GetTickCount() - li.dwTime);
}

const char *pal_input_source(void) { return "GetLastInputInfo"; }

int pal_set_timer_slack_min(void) { return g_high_res_timer ? 0 : (timeBeginPeriod(1) == 0 ? 0 : -1); }

int pal_random_bytes(void *p, size_t n) {
  return BCryptGenRandom(NULL, p, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0 ? 0 : -1;
}

/* ---------- load and telemetry ---------- */

double pal_cpu_temp_c(void) { return NAN; }

static uint64_t ft(FILETIME f) { return ((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime; }

double pal_background_load(double seconds) {
  FILETIME i0, k0, u0, i1, k1, u1;
  if (!GetSystemTimes(&i0, &k0, &u0)) return NAN;
  pal_sleep_ns((uint64_t)(seconds * 1e9));
  if (!GetSystemTimes(&i1, &k1, &u1)) return NAN;
  uint64_t idle = ft(i1) - ft(i0), total = (ft(k1) - ft(k0)) + (ft(u1) - ft(u0)); /* kernel time includes idle */
  return total ? (double)(total - idle) / (double)total : NAN;
}

/*
 * Package power from the Energy Meter Interface: Windows publishes the RAPL energy counters (and on some machines
 * other meters) as devices of the GUID_DEVICE_ENERGY_METER interface. Its structures (emi.h, Windows 10 1809+)
 * are declared here because not every SDK has the header. Energy is in picowatt-hours, time in 100 ns units.
 */
static const GUID PMK_ENERGY_METER = {0x45bd8344, 0x7ed6, 0x49cf, {0xa4, 0x40, 0xc2, 0x76, 0xc9, 0x33, 0xb0, 0x53}};
#define PMK_EMI_GET_VERSION CTL_CODE(FILE_DEVICE_UNKNOWN, 0, METHOD_BUFFERED, FILE_READ_ACCESS)
#define PMK_EMI_GET_METADATA_SIZE CTL_CODE(FILE_DEVICE_UNKNOWN, 1, METHOD_BUFFERED, FILE_READ_ACCESS)
#define PMK_EMI_GET_METADATA CTL_CODE(FILE_DEVICE_UNKNOWN, 2, METHOD_BUFFERED, FILE_READ_ACCESS)
#define PMK_EMI_GET_MEASUREMENT CTL_CODE(FILE_DEVICE_UNKNOWN, 3, METHOD_BUFFERED, FILE_READ_ACCESS)
#define PMK_EMI_NAME_MAX 16

typedef struct {
  ULONGLONG energy_pwh, time_100ns;
} pmk_emi_data;

/* The index of the package channel of an EMI v2 device, -1 if none, with its name in out. */
static int emi_package_channel(HANDLE h, int *count, char *out, size_t n) {
  ULONG size = 0;
  DWORD got;
  if (!DeviceIoControl(h, PMK_EMI_GET_METADATA_SIZE, NULL, 0, &size, sizeof size, &got, NULL) || !size) return -1;
  unsigned char *md = malloc(size);
  int found = -1;
  if (md && DeviceIoControl(h, PMK_EMI_GET_METADATA, NULL, 0, md, size, &got, NULL)) {
    /* EMI_METADATA_V2: OEM and model names, revision, channel count, then the channels, each a unit (4 bytes),
       a name size in bytes (2) and the name. */
    size_t off = 2 * PMK_EMI_NAME_MAX * sizeof(WCHAR) + 2;
    USHORT nch;
    memcpy(&nch, md + off, 2);
    off += 2;
    *count = nch;
    for (int i = 0; i < nch && off + 6 <= got; i++) {
      USHORT len;
      memcpy(&len, md + off + 4, 2);
      if (off + 6 + len > got) break;
      char name[64] = "";
      WideCharToMultiByte(CP_UTF8, 0, (const WCHAR *)(void *)(md + off + 6), len / 2, name, sizeof name - 1, NULL, NULL);
      size_t nl = strlen(name);
      /* RAPL_Package0_PKG on Intel and AMD; the first package channel wins. */
      if (found < 0 && nl >= 4 && !_stricmp(name + nl - 4, "_PKG")) {
        found = i;
        snprintf(out, n, "%s", name);
      }
      off += 6 + len;
    }
  }
  free(md);
  return found;
}

static void find_power(pmk_caps *c) {
  c->power[0] = c->power_note[0] = 0;
  HDEVINFO set = SetupDiGetClassDevsW(&PMK_ENERGY_METER, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
  if (set == INVALID_HANDLE_VALUE) return;
  int devices = 0, denied = 0;
  SP_DEVICE_INTERFACE_DATA ifd = {0};
  ifd.cbSize = sizeof ifd;
  for (DWORD i = 0; g_emi == INVALID_HANDLE_VALUE && SetupDiEnumDeviceInterfaces(set, NULL, &PMK_ENERGY_METER, i, &ifd); i++) {
    DWORD size = 0;
    SetupDiGetDeviceInterfaceDetailW(set, &ifd, NULL, 0, &size, NULL);
    SP_DEVICE_INTERFACE_DETAIL_DATA_W *d = malloc(size);
    if (!d) break;
    d->cbSize = sizeof *d;
    if (SetupDiGetDeviceInterfaceDetailW(set, &ifd, d, size, NULL, NULL)) {
      devices++;
      HANDLE h = CreateFileW(d->DevicePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
      if (h == INVALID_HANDLE_VALUE) {
        denied |= GetLastError() == ERROR_ACCESS_DENIED;
      } else {
        USHORT version = 0;
        DWORD got;
        char name[64];
        int count = 0, ch = -1;
        if (DeviceIoControl(h, PMK_EMI_GET_VERSION, NULL, 0, &version, sizeof version, &got, NULL) && version >= 2)
          ch = emi_package_channel(h, &count, name, sizeof name);
        if (ch >= 0) {
          g_emi = h;
          g_emi_channel = ch;
          g_emi_channels = count;
          snprintf(c->power, sizeof c->power, "emi:%s", name);
        } else {
          CloseHandle(h);
        }
      }
    }
    free(d);
  }
  SetupDiDestroyDeviceInfoList(set);
  if (g_emi == INVALID_HANDLE_VALUE)
    snprintf(c->power_note, sizeof c->power_note, "%s",
             denied ? "emi-needs-admin" : devices ? "emi-no-package" : "no-energy-meter");
}

/* The package channel's energy in microjoules and its time in ns; -1 on error. */
static long long emi_read(uint64_t *ns) {
  if (g_emi == INVALID_HANDLE_VALUE) return -1;
  pmk_emi_data data[32];
  int n = g_emi_channels < 32 ? g_emi_channels : 32;
  DWORD got;
  if (g_emi_channel >= n ||
      !DeviceIoControl(g_emi, PMK_EMI_GET_MEASUREMENT, NULL, 0, data, (DWORD)(n * sizeof *data), &got, NULL) ||
      got < (g_emi_channel + 1) * sizeof *data)
    return -1;
  *ns = data[g_emi_channel].time_100ns * 100;
  return (long long)((double)data[g_emi_channel].energy_pwh * 3.6e-3); /* 1 pWh = 3.6e-3 uJ */
}

void pal_power_start(pal_power *p) {
  p->ns = 0;
  p->uj = emi_read(&p->ns);
}

double pal_power_read(pal_power *p) {
  uint64_t t = 0;
  long long e = emi_read(&t);
  double w = NAN;
  if (e >= 0 && p->uj >= 0 && t > p->ns && e >= p->uj) w = (double)(e - p->uj) / ((double)(t - p->ns) / 1e3);
  p->uj = e;
  p->ns = t;
  return w;
}

double pal_idle_power_w(double seconds) {
  if (g_emi == INVALID_HANDLE_VALUE) return NAN;
  pal_power m;
  pal_power_start(&m);
  pal_sleep_ns((uint64_t)(seconds * 1e9));
  return pal_power_read(&m);
}

/* ---------- power plan ---------- */

static DWORD read_ac(const GUID *sub, const GUID *setting, DWORD def) {
  GUID *scheme;
  DWORD v = def;
  if (PowerGetActiveScheme(NULL, &scheme) == 0) {
    if (PowerReadACValueIndex(NULL, scheme, sub, setting, &v)) v = def;
    LocalFree(scheme);
  }
  return v;
}

/* GUID_PROCESSOR_IDLE_DISABLE is not in every SDK. */
static const GUID PMK_IDLE_DISABLE = {0x5d76a2ca, 0xe8c0, 0x402f, {0xa1, 0x33, 0x21, 0x58, 0x49, 0x2d, 0x58, 0xad}};

void pal_state_json(pmk_jw *w, const char *key, const pmk_machine *m, int measure_power) {
  (void)m;
  jw_obj_begin(w, key);
  GUID *scheme = NULL;
  char name[160] = "";
  if (PowerGetActiveScheme(NULL, &scheme) == 0) {
    WCHAR wname[128];
    DWORD len = sizeof wname;
    if (PowerReadFriendlyName(NULL, scheme, NULL, NULL, (UCHAR *)wname, &len) == 0)
      WideCharToMultiByte(CP_UTF8, 0, wname, -1, name, sizeof name, NULL, NULL);
    LocalFree(scheme);
  }
  jw_str(w, "power_mode", name[0] ? name : NULL);
  jw_int(w, "processor_min_state_pct", read_ac(&GUID_PROCESSOR_SETTINGS_SUBGROUP, &GUID_PROCESSOR_THROTTLE_MINIMUM, 0));
  jw_int(w, "processor_max_state_pct", read_ac(&GUID_PROCESSOR_SETTINGS_SUBGROUP, &GUID_PROCESSOR_THROTTLE_MAXIMUM, 100));
  jw_int(w, "idle_disable", read_ac(&GUID_PROCESSOR_SETTINGS_SUBGROUP, &PMK_IDLE_DISABLE, 0));
  SYSTEM_POWER_STATUS ps;
  if (GetSystemPowerStatus(&ps) && ps.ACLineStatus != 255) jw_bool(w, "ac_online", ps.ACLineStatus == 1);
  else jw_null(w, "ac_online");
  jw_obj_begin(w, "temp_c");
  jw_null(w, "cpu");
  jw_obj_end(w);
  jw_num(w, "idle_power_w", measure_power ? pal_idle_power_w(2.0) : NAN);
  jw_obj_end(w);
}

int pal_cycles_open(void) { return -1; }
uint64_t pal_cycles_read(int fd) {
  (void)fd;
  return 0;
}
void pal_cycles_close(int fd) { (void)fd; }

/* ---------- threads ---------- */

struct pal_thread {
  HANDLE h;
  void *(*fn)(void *);
  void *arg;
};

static DWORD WINAPI thread_main(LPVOID p) {
  pal_thread *t = p;
  t->fn(t->arg);
  return 0;
}

pal_thread *pal_thread_start(void *(*fn)(void *), void *arg) {
  pal_thread *t = malloc(sizeof *t);
  if (!t) return NULL;
  t->fn = fn;
  t->arg = arg;
  t->h = CreateThread(NULL, 0, thread_main, t, 0, NULL);
  if (!t->h) {
    free(t);
    return NULL;
  }
  return t;
}

void pal_thread_join(pal_thread *t) {
  if (!t) return;
  WaitForSingleObject(t->h, INFINITE);
  CloseHandle(t->h);
  free(t);
}

struct pal_lock {
  SRWLOCK l;
  CONDITION_VARIABLE c;
};

pal_lock *pal_lock_new(void) {
  pal_lock *l = malloc(sizeof *l);
  if (!l) return NULL;
  InitializeSRWLock(&l->l);
  InitializeConditionVariable(&l->c);
  return l;
}
void pal_lock_free(pal_lock *l) { free(l); }
void pal_lock_acquire(pal_lock *l) { AcquireSRWLockExclusive(&l->l); }
void pal_lock_release(pal_lock *l) { ReleaseSRWLockExclusive(&l->l); }
void pal_lock_wait(pal_lock *l) { SleepConditionVariableSRW(&l->c, &l->l, INFINITE, 0); }
void pal_lock_broadcast(pal_lock *l) { WakeAllConditionVariable(&l->c); }

void *pal_aligned_alloc(size_t align, size_t n) { return _aligned_malloc(n ? n : 1, align); }
void pal_aligned_free(void *p) { _aligned_free(p); }

/* ---------- tier modules ---------- */

const pmk_kernels *pal_load_tier(const char *tier, char *err, size_t errlen) {
  char dir[MAX_PATH], path[MAX_PATH + 64];
  const char *env = getenv("PRISMARK_KERNEL_DIR");
  if (env && *env) {
    snprintf(dir, sizeof dir, "%s", env);
  } else {
    HMODULE self = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)(void *)pal_load_tier, &self);
    GetModuleFileNameA(self, dir, sizeof dir);
    char *slash = strrchr(dir, '\\');
    if (slash) *slash = 0;
  }
  snprintf(path, sizeof path, "%s\\prismark-kernels-%s.dll", dir, tier);
  HMODULE h = LoadLibraryA(path);
  if (!h) {
    snprintf(err, errlen, "cannot load %s (error %lu)", path, GetLastError());
    return NULL;
  }
  pmk_kernels_entry_fn entry = (pmk_kernels_entry_fn)(void *)GetProcAddress(h, PMK_KERNELS_ENTRY);
  const pmk_kernels *k = entry ? entry() : NULL;
  if (!k || k->abi != PMK_KERNELS_ABI) {
    snprintf(err, errlen, "%s: %s", path, k ? "kernel ABI mismatch" : "no entry point");
    FreeLibrary(h);
    return NULL;
  }
  return k;
}

/* ---------- processes and files ---------- */

/* Appends one argument quoted by the CommandLineToArgvW rules. */
static void append_arg(char *cmd, size_t n, const char *a) {
  size_t len = strlen(cmd);
  if (len && len + 1 < n) cmd[len++] = ' ';
  int quote = !*a || strpbrk(a, " \t\"") != NULL;
  if (quote && len + 1 < n) cmd[len++] = '"';
  for (const char *p = a; *p && len + 3 < n; p++) {
    size_t bs = 0;
    while (*p == '\\') bs++, p++;
    if (!*p) { /* backslashes before the closing quote are doubled */
      for (size_t i = 0; i < bs * (quote ? 2 : 1) && len + 1 < n; i++) cmd[len++] = '\\';
      break;
    }
    if (*p == '"') {
      for (size_t i = 0; i < bs * 2 + 1 && len + 1 < n; i++) cmd[len++] = '\\';
    } else {
      for (size_t i = 0; i < bs && len + 1 < n; i++) cmd[len++] = '\\';
    }
    cmd[len++] = *p;
  }
  if (quote && len + 1 < n) cmd[len++] = '"';
  cmd[len] = 0;
}

int pal_run(const char *const *argv, const char *cwd, const char *log_path, int (*cancelled)(void)) {
  char cmd[32768] = "";
  for (int i = 0; argv[i]; i++) append_arg(cmd, sizeof cmd, argv[i]);
  SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
  HANDLE logf = INVALID_HANDLE_VALUE;
  if (log_path) {
    logf = CreateFileA(log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, NULL);
  }
  STARTUPINFOA si = {0};
  si.cb = sizeof si;
  if (logf != INVALID_HANDLE_VALUE) {
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = logf;
    si.hStdInput = NULL;
  }
  PROCESS_INFORMATION pi;
  BOOL ok = CreateProcessA(NULL, cmd, NULL, NULL, logf != INVALID_HANDLE_VALUE, CREATE_NO_WINDOW, NULL, cwd, &si, &pi);
  if (logf != INVALID_HANDLE_VALUE) CloseHandle(logf);
  if (!ok) return -1;
  for (;;) {
    DWORD w = WaitForSingleObject(pi.hProcess, cancelled ? 50 : INFINITE);
    if (w == WAIT_OBJECT_0) break;
    if (cancelled && cancelled()) {
      TerminateProcess(pi.hProcess, 1);
      WaitForSingleObject(pi.hProcess, INFINITE);
      break;
    }
  }
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  return (int)code;
}

int pal_scratch_dir(char *out, size_t n, int *ram_backed) {
  DWORD len = GetTempPathA((DWORD)n, out);
  if (!len || len >= n) return -1;
  if (out[len - 1] == '\\') out[len - 1] = 0;
  *ram_backed = 0; /* Windows has no tmpfs; the K1x result records it */
  return 0;
}

/* A path with '\\' separators: cmd and robocopy read a '/' in a path as the start of an option. */
static const char *backslashes(char *out, size_t n, const char *path) {
  snprintf(out, n, "%s", path);
  for (char *p = out; *p; p++)
    if (*p == '/') *p = '\\';
  return out;
}

/* Copies the directory tree src into dst (created with its parents), file by file; 0 on success. */
int pal_copy_tree(const char *src, const char *dst) {
  char s[MAX_PATH * 4], d[MAX_PATH * 4], pat[MAX_PATH * 4 + 4];
  backslashes(s, sizeof s, src);
  backslashes(d, sizeof d, dst);
  for (char *p = d + 1; *p; p++) /* the parents first: CreateDirectory makes one level */
    if (*p == '\\' && p[-1] != ':') {
      *p = 0;
      CreateDirectoryA(d, NULL);
      *p = '\\';
    }
  if (!CreateDirectoryA(d, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return -1;
  snprintf(pat, sizeof pat, "%s\\*", s);
  WIN32_FIND_DATAA fd;
  HANDLE h = FindFirstFileA(pat, &fd);
  if (h == INVALID_HANDLE_VALUE) return -1;
  int rc = 0;
  do {
    if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
    char from[MAX_PATH * 4], to[MAX_PATH * 4];
    snprintf(from, sizeof from, "%s\\%s", s, fd.cFileName);
    snprintf(to, sizeof to, "%s\\%s", d, fd.cFileName);
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) rc = pal_copy_tree(from, to);
    else if (!CopyFileA(from, to, FALSE)) rc = -1;
  } while (rc == 0 && FindNextFileA(h, &fd));
  FindClose(h);
  return rc;
}

int pal_remove_tree(const char *path) {
  if (!path || strlen(path) < 4) return -1;
  char p[MAX_PATH * 4];
  const char *argv[] = {"cmd", "/c", "rmdir", "/s", "/q", backslashes(p, sizeof p, path), NULL};
  pal_run(argv, NULL, NULL, NULL);
  return GetFileAttributesA(p) == INVALID_FILE_ATTRIBUTES ? 0 : -1;
}
