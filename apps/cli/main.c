/*
 * prismark — command-line front-end. Starts a run through the C ABI, shows
 * progress between measurement windows, writes the result JSON and prints the
 * summary computed by the core. Also lists, shows and compares stored results
 * and prints kernel checksums.
 *
 *   prismark [run options]
 *   prismark tests | list | show RUN | compare RUN RUN | profiles | checksums | info | help [COMMAND]
 *
 * On a terminal, progress is one status line redrawn in place, with colour
 * (NO_COLOR or --no-color turn colour off); otherwise one plain line per
 * event. --progress-json is the desktop app's format and does not change.
 *
 * Copyright 2026 The Prismark Authors. Apache-2.0.
 */
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "prismark/prismark.h"

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#define isatty _isatty
#define fileno _fileno
#else
#include <dirent.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

/* ---------- terminal ---------- */

static struct {
  int color_out, color_err; /* ANSI styles on stdout / stderr */
  int live;                 /* stderr is a terminal: progress is one line redrawn in place */
} term;

#define BOLD "\033[1m"
#define DIM "\033[2m"
#define RED "\033[31m"
#define GREEN "\033[32m"
#define YELLOW "\033[33m"
#define CYAN "\033[36m"
#define RESET "\033[0m"

/* The style s on stdout (o) or stderr (e), or nothing when colour is off there. */
static const char *o(const char *s) { return term.color_out ? s : ""; }
static const char *e(const char *s) { return term.color_err ? s : ""; }

static void term_init(int no_color) {
  const char *t = getenv("TERM");
  const char *nc = getenv("NO_COLOR");
  int dumb = t && !strcmp(t, "dumb");
  int out_tty = isatty(fileno(stdout)), err_tty = isatty(fileno(stderr));
#ifdef _WIN32
  /* Styles and the redrawn line need virtual-terminal processing (Windows 10 and later). */
  SetConsoleOutputCP(CP_UTF8);
  int vt = 1;
  DWORD handles[2] = {STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
  for (int i = 0; i < 2; i++) {
    HANDLE h = GetStdHandle(handles[i]);
    DWORD m;
    if (GetConsoleMode(h, &m) && !SetConsoleMode(h, m | 0x0004 /* ENABLE_VIRTUAL_TERMINAL_PROCESSING */)) vt = 0;
  }
  if (!vt) dumb = 1;
#endif
  int color = !no_color && !(nc && *nc) && !dumb;
  term.color_out = color && out_tty;
  term.color_err = color && err_tty;
  term.live = err_tty && !dumb;
}

static int term_width(void) {
  int w = 0;
#ifdef _WIN32
  CONSOLE_SCREEN_BUFFER_INFO info;
  if (GetConsoleScreenBufferInfo(GetStdHandle(STD_ERROR_HANDLE), &info))
    w = info.srWindow.Right - info.srWindow.Left + 1;
#else
  struct winsize ws;
  if (ioctl(2, TIOCGWINSZ, &ws) == 0) w = ws.ws_col;
#endif
  if (w <= 0 && getenv("COLUMNS")) w = atoi(getenv("COLUMNS"));
  return w >= 40 ? w : 80;
}

static double now_s(void) {
#ifdef _WIN32
  return (double)GetTickCount64() / 1000;
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
#endif
}

static const char *fmt_duration(char *out, size_t n, double s) {
  long t = (long)(s + 0.5);
  if (t >= 3600) snprintf(out, n, "%ld:%02ld:%02ld", t / 3600, t / 60 % 60, t % 60);
  else snprintf(out, n, "%ld:%02ld", t / 60, t % 60);
  return out;
}

/* path with the home folder shown as ~, for display only. */
static const char *pretty_path(char *out, size_t n, const char *path) {
  const char *home = getenv("HOME");
  size_t hl = home ? strlen(home) : 0;
  if (hl > 1 && !strncmp(path, home, hl) && (path[hl] == '/' || !path[hl])) snprintf(out, n, "~%s", path + hl);
  else snprintf(out, n, "%s", path);
  return out;
}

/* ---------- names ---------- */

/*
 * Kernels by ID, with the short names the command line also accepts, the modes they run in, and what the
 * tests list says about them (the desktop app's wording, apps/gui/metrics.cpp).
 */
#define ST_B PMK_MODE_ST_BURST
#define ST_S PMK_MODE_ST_SUSTAINED /* also the new-instruction gain of K2, K3, K4 and K8 */
#define MC_T PMK_MODE_MC_THREADED
static const struct {
  const char *id, *slug;
  uint32_t modes;
  int isa;               /* also measured for the gain from the newest instructions */
  const char *tag;       /* a few words, for the menu */
  const char *what;      /* what it runs */
  const char *reports;   /* what it reports */
} KERNELS[] = {
    {"K1", "compile", ST_S | MC_T, 0, "C++ compiler, in memory",
     "Compiles a fixed set of C++ files with the Clang compiler built into Prismark, entirely in memory.",
     "builds per hour (higher is better). Needs the compile-test setup."},
    {"K1x", "build", MC_T, 0, "CMake + Ninja build",
     "Builds part of LLVM with CMake and Ninja on all cores, three times: compiling plus the build tools.",
     "seconds per build (lower is better). Needs the compile-test setup."},
    {"K2", "render", ST_S | MC_T, 1, "path tracer",
     "A small path tracer draws a fixed scene, as 3D rendering and video software do.",
     "million light samples per second (higher is better)."},
    {"K3", "compression", ST_B | ST_S, 1, "zstd on a small file",
     "Compresses a small generated file with zstd (level 3), a common compression format.",
     "milliseconds per file (lower is better)."},
    {"K4", "photo", ST_B | ST_S | PMK_MODE_COLD_BURST, 1, "JPEG decode and resize",
     "Decodes a JPEG photo and resizes it, as a photo viewer or a web page does.",
     "milliseconds per photo (lower is better)."},
    {"K5", "json", ST_B, 0, "web-style text data",
     "Reads JSON documents, the text format most websites and online services exchange.",
     "milliseconds per batch (lower is better)."},
    {"K6", "script", ST_B | PMK_MODE_COLD_BURST, 0, "start Lua, run a script",
     "Starts the Lua language, loads a small script and runs it, as when a plugin or a game script starts.",
     "milliseconds per start (lower is better)."},
    {"K7", "memory", PMK_MODE_MC_INSTANCES, 0, "wait for main memory",
     "Follows a chain of links through memory larger than every cache, so each step waits for main memory.",
     "nanoseconds per read, alone and with every core reading (lower is better)."},
    {"K8", "matrix", ST_S, 1, "decimal and int8 maths",
     "Multiplies large tables of decimal numbers (as in graphics) and small integers (as in AI models).",
     "only the gain from the newest instructions (higher is better)."},
    {"K9", "wakeup", PMK_MODE_COLD_BURST, 0, "tasks after a rest",
     "Times short tasks that arrive while the processor rests, against the same tasks on a busy core.",
     "speed as a percentage of a busy core's (higher is better; 100 % = no penalty)."},
    {"K10", "timer", PMK_MODE_PERIODIC, 0, "1 ms timer lateness",
     "A task asks to wake up every millisecond for a minute, on an idle computer and with other cores busy.",
     "how late the worst 1 in 1000 wake-ups are, in microseconds (lower is better)."},
};
#undef ST_B
#undef ST_S
#undef MC_T
#define NKERNELS (sizeof KERNELS / sizeof *KERNELS)

static const struct {
  const char *id, *slug, *how;
  uint32_t bit;
} MODES[] = {
    {"st_burst", "short-task", "short jobs on a core that is already running, until each time is known to ~1 %",
     PMK_MODE_ST_BURST},
    {"st_sustained", "warmed-up",
     "one core after a minute of full load, measured for 20 s (compiling: 60 s); also the new-instruction builds",
     PMK_MODE_ST_SUSTAINED},
    {"mc_threaded", "all-cores", "every core on the same job, from 1 thread up to all of them, to show the scaling",
     PMK_MODE_MC_THREADED},
    {"mc_instances", "each-core", "every core runs its own copy, so they compete for memory", PMK_MODE_MC_INSTANCES},
    {"cold_burst", "from-rest", "tasks started after a random 50-500 ms rest, wake-up included; hands off the keyboard",
     PMK_MODE_COLD_BURST},
    {"periodic", "timer", "a 1 ms timer for a minute, idle and with other cores busy; hands off the keyboard",
     PMK_MODE_PERIODIC},
};
#define NMODES (sizeof MODES / sizeof *MODES)

/* The desktop app's categories (apps/gui/metrics.cpp), with what each item runs. */
static const struct {
  const char *name, *about;
} CATEGORIES[] = {
    {"All cores", "heavy jobs on every core at once, after a minute of full load"},
    {"One core", "short everyday tasks, and long tasks once the core has warmed up"},
    {"Responsiveness", "how quickly work starts while the processor rests; hands off the keyboard"},
    {"Memory & timing", "waits for main memory, and how punctual a 1 ms timer is"},
    {"New instructions", "the same code built for the common and the newest instructions"},
};
#define NCATEGORIES (sizeof CATEGORIES / sizeof *CATEGORIES)

static const struct {
  int cat;
  const char *name, *kernels, *mode;
  int needs_k1, isa;
} ITEMS[] = {
    {0, "3D rendering", "K2", "mc_threaded", 0, 0},
    {0, "Compiling code", "K1", "mc_threaded", 1, 0},
    {0, "Full software build", "K1x", "mc_threaded", 1, 0},
    {1, "3D rendering, after warming up", "K2", "st_sustained", 0, 0},
    {1, "Compiling code, after warming up", "K1", "st_sustained", 1, 0},
    {1, "Compression", "K3", "st_burst", 0, 0},
    {1, "Opening a photo", "K4", "st_burst", 0, 0},
    {1, "Reading JSON", "K5", "st_burst", 0, 0},
    {1, "Starting a script", "K6", "st_burst", 0, 0},
    {2, "Wake-up test: 0.1 to 100 ms tasks from rest", "K9", "cold_burst", 0, 0},
    {2, "Opening a photo from rest", "K4", "cold_burst", 0, 0},
    {2, "Starting a script from rest", "K6", "cold_burst", 0, 0},
    {3, "Memory delay, alone and with every core reading", "K7", "mc_instances", 0, 0},
    {3, "Timer punctuality, idle and with other cores busy", "K10", "periodic", 0, 0},
    {4, "Gain in 3D rendering, compression, photos, matrix maths", "K2,K3,K4,K8", "st_sustained", 0, 1},
};
#define NITEMS (sizeof ITEMS / sizeof *ITEMS)


/* Lower case without spaces, '-' and '_', so "Wake-up test", "wakeup" and "WAKE_UP_TEST" compare equal. */
static void norm(char *out, size_t n, const char *s, size_t len) {
  size_t k = 0;
  for (size_t i = 0; i < len && s[i] && k + 1 < n; i++)
    if (s[i] != ' ' && s[i] != '-' && s[i] != '_' && s[i] != ',') out[k++] = (char)tolower((unsigned char)s[i]);
  out[k] = 0;
}

static int name_matches(const char *tok, size_t len, const char *id, const char *slug) {
  char a[64], b[64];
  norm(a, sizeof a, tok, len);
  if (!a[0]) return 0;
  const char *cands[3] = {id, slug, pmk_display_name(id)};
  for (int i = 0; i < 3; i++) {
    norm(b, sizeof b, cands[i], strlen(cands[i]));
    if (!strcmp(a, b)) return 1;
  }
  return 0;
}

/* "compression,photo" or "K3,K4" -> "K3,K4"; -1 naming the first unknown entry. */
static int parse_tests(const char *s, char *out, size_t n, const char **bad, size_t *badlen) {
  size_t k = 0;
  out[0] = 0;
  while (*s) {
    size_t len = strcspn(s, ",");
    size_t i = 0;
    while (i < NKERNELS && !name_matches(s, len, KERNELS[i].id, KERNELS[i].slug)) i++;
    if (i == NKERNELS) {
      *bad = s;
      *badlen = len;
      return -1;
    }
    k += (size_t)snprintf(out + k, k < n ? n - k : 0, "%s%s", k ? "," : "", KERNELS[i].id);
    s += len;
    if (*s == ',') s++;
  }
  return out[0] ? 0 : -1;
}

static int parse_modes(const char *s, uint32_t *out, const char **bad, size_t *badlen) {
  *out = 0;
  while (*s) {
    size_t len = strcspn(s, ",");
    size_t i = 0;
    while (i < NMODES && !name_matches(s, len, MODES[i].id, MODES[i].slug)) i++;
    if (i == NMODES) {
      *bad = s;
      *badlen = len;
      return -1;
    }
    *out |= MODES[i].bit;
    s += len;
    if (*s == ',') s++;
  }
  return *out ? 0 : -1;
}

/* "One core, short task" for st_burst: the app's mode name, capitalised. */
static const char *mode_title(char *out, size_t n, const char *id) {
  snprintf(out, n, "%s", pmk_display_name(id));
  out[0] = (char)toupper((unsigned char)out[0]);
  return out;
}

/* ---------- results folder ---------- */

/* Creates every missing directory of path; 0 on success. */
static int make_dirs(char *path) {
  for (char *p = path + 1;; p++) {
    char ch = *p;
    if (ch != '/' && ch != '\\' && ch != 0) continue;
    *p = 0;
#ifdef _WIN32
    int ok = CreateDirectoryA(path, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
#else
    struct stat st;
    int ok = mkdir(path, 0755) == 0 || (stat(path, &st) == 0 && S_ISDIR(st.st_mode));
#endif
    *p = ch;
    if (!ok) return -1;
    if (!ch) return 0;
  }
}

/*
 * The results folder shared with the desktop app: <user data>/prismark/results/runs, where <user data> is
 * what Qt calls GenericDataLocation. -1 where there is none (Android) or when running as root through sudo or
 * pkexec, whose folders would belong to root: the result then goes to the current folder.
 */
static int results_dir(char *out, size_t n, int create) {
#if defined(_WIN32)
  const char *base = getenv("LOCALAPPDATA");
  if (!base || !*base) return -1;
  snprintf(out, n, "%s\\prismark\\results\\runs", base);
#elif defined(__ANDROID__)
  (void)out;
  (void)n;
  return -1;
#else
  if (geteuid() == 0 && (getenv("SUDO_UID") || getenv("PKEXEC_UID"))) return -1;
  const char *home = getenv("HOME");
#if defined(__APPLE__)
  if (!home || !*home) return -1;
  snprintf(out, n, "%s/Library/Application Support/prismark/results/runs", home);
#else
  const char *xdg = getenv("XDG_DATA_HOME");
  if (xdg && *xdg) snprintf(out, n, "%s/prismark/results/runs", xdg);
  else if (home && *home) snprintf(out, n, "%s/.local/share/prismark/results/runs", home);
  else return -1;
#endif
#endif
  return create ? make_dirs(out) : 0;
}

/* When elevated through sudo or pkexec, hand the result file back to the user who asked for the run. */
static void give_back(const char *path) {
#ifndef _WIN32
  if (geteuid() != 0) return;
  const char *uid = getenv("PKEXEC_UID"), *suid = getenv("SUDO_UID"), *sgid = getenv("SUDO_GID");
  long u = uid ? atol(uid) : suid ? atol(suid) : -1;
  if (u < 0) return;
  long g = sgid ? atol(sgid) : -1;
  if (g < 0) { /* pkexec gives only the uid: use the group of the directory the file is in */
    char dir[1024];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = 0;
    else snprintf(dir, sizeof dir, ".");
    struct stat st;
    if (stat(dir, &st) == 0) g = (long)st.st_gid;
  }
  if (chown(path, (uid_t)u, (gid_t)g)) perror(path);
#else
  (void)path;
#endif
}

static char *read_file(const char *path, int quiet) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    if (!quiet) fprintf(stderr, "prismark: %s: %s\n", path, strerror(errno));
    return NULL;
  }
  size_t cap = 1 << 20, len = 0;
  char *buf = malloc(cap);
  size_t got;
  while (buf && (got = fread(buf + len, 1, cap - len - 1, f)) > 0) {
    len += got;
    if (len + 1 == cap) {
      char *p = realloc(buf, cap *= 2);
      if (!p) free(buf);
      buf = p;
    }
  }
  fclose(f);
  if (buf) buf[len] = 0;
  return buf;
}

static int write_file(const char *path, const char *text) {
  FILE *f = fopen(path, "w");
  if (f && fputs(text, f) >= 0 && fputc('\n', f) != EOF && fclose(f) == 0) return 0;
  int err = errno;
  if (f) fclose(f);
  fprintf(stderr, "prismark: %s: %s\n", path, strerror(err));
  return -1;
}

/* The run id is the only field read back after a run, to name the output file. */
static void run_id_of(const char *json, char *out, size_t n) {
  const char *p = strstr(json, "\"run_id\":\"");
  snprintf(out, n, "unknown");
  if (p) {
    p += 10;
    size_t len = strcspn(p, "\"");
    if (len < n) snprintf(out, n, "%.*s", (int)len, p);
  }
}

typedef struct {
  char path[1200];
  pmk_brief b;
} run_entry;

static int by_start_desc(const void *x, const void *y) {
  return strcmp(((const run_entry *)y)->b.started_utc, ((const run_entry *)x)->b.started_utc);
}

typedef struct {
  run_entry *v;
  size_t n, cap;
} run_list;

/* Calls fn for every *.json file in dir. */
static void each_json(const char *dir, void (*fn)(void *ctx, const char *dir, const char *name), void *ctx) {
#ifdef _WIN32
  char pat[1200];
  snprintf(pat, sizeof pat, "%s\\*.json", dir);
  WIN32_FIND_DATAA fd;
  HANDLE h = FindFirstFileA(pat, &fd);
  if (h == INVALID_HANDLE_VALUE) return;
  do fn(ctx, dir, fd.cFileName);
  while (FindNextFileA(h, &fd));
  FindClose(h);
#else
  DIR *d = opendir(dir);
  for (struct dirent *de; d && (de = readdir(d));) {
    size_t len = strlen(de->d_name);
    if (len > 5 && !strcmp(de->d_name + len - 5, ".json")) fn(ctx, dir, de->d_name);
  }
  if (d) closedir(d);
#endif
}

static void add_run(void *ctx, const char *dir, const char *name) {
  run_list *l = ctx;
  if (l->n == l->cap) {
    run_entry *p = realloc(l->v, (l->cap = l->cap ? l->cap * 2 : 32) * sizeof *p);
    if (!p) return;
    l->v = p;
  }
  run_entry *r = &l->v[l->n];
  snprintf(r->path, sizeof r->path, "%s/%s", dir, name);
  char *json = read_file(r->path, 1);
  r->b.struct_size = sizeof r->b;
  if (json && pmk_describe(json, &r->b, NULL) == PMK_OK) l->n++;
  free(json);
}

/* Every readable result in dir, newest first. *out is malloc'd; returns the count. */
static size_t scan_runs(const char *dir, run_entry **out) {
  run_list l = {NULL, 0, 0};
  each_json(dir, add_run, &l);
  if (l.n) qsort(l.v, l.n, sizeof *l.v, by_start_desc);
  *out = l.v;
  return l.n;
}

/* ---------- reference systems ---------- */

/*
 * Reference systems (references/README.md): the user's own in <results>/references, then the ones that come
 * with Prismark, installed beside the program (share/prismark/references, or references/ next to it on
 * Windows), or in the source tree when it runs from the build folder. Named by file name without .json.
 */
typedef struct {
  char id[96], path[1200], name[160];
  const char *kind; /* "measured", "measured, quick", "placeholder" */
  int own;          /* from the user's folder */
} ref_entry;

typedef struct {
  ref_entry v[256];
  size_t n;
  int own;
} ref_list;

/* The string value of the first "key" in json, or "" (only for small, flat fields). */
static void json_field(const char *json, const char *key, char *out, size_t n) {
  char pat[64];
  snprintf(pat, sizeof pat, "\"%s\"", key);
  out[0] = 0;
  const char *p = strstr(json, pat);
  if (!p) return;
  p += strlen(pat);
  p += strspn(p, " \t\r\n");
  if (*p++ != ':') return;
  p += strspn(p, " \t\r\n");
  if (*p == '"') snprintf(out, n, "%.*s", (int)strcspn(p + 1, "\""), p + 1);
  else snprintf(out, n, "%.*s", (int)strcspn(p, ",} \t\r\n"), p);
}

static void add_ref(void *ctx, const char *dir, const char *name) {
  ref_list *l = ctx;
  size_t len = strlen(name) - 5;
  for (size_t i = 0; i < l->n; i++)
    if (strlen(l->v[i].id) == len && !strncmp(l->v[i].id, name, len)) return; /* the user's own copy wins */
  if (l->n == sizeof l->v / sizeof *l->v) return;
  ref_entry *r = &l->v[l->n];
  snprintf(r->id, sizeof r->id, "%.*s", (int)len, name);
  snprintf(r->path, sizeof r->path, "%s/%s", dir, name);
  r->own = l->own;
  char *json = read_file(r->path, 1);
  if (!json) return;
  pmk_brief b;
  memset(&b, 0, sizeof b);
  b.struct_size = sizeof b;
  char schema[48], ph[8];
  json_field(json, "schema", schema, sizeof schema);
  if (pmk_describe(json, &b, NULL) == PMK_OK) {
    snprintf(r->name, sizeof r->name, "%s", b.model);
    r->kind = b.quick == 1 ? "measured, quick" : "measured";
    l->n++;
  } else if (!strcmp(schema, "prismark-reference/1")) {
    json_field(json, "name", r->name, sizeof r->name);
    json_field(json, "placeholder", ph, sizeof ph);
    r->kind = !strcmp(ph, "true") ? "placeholder" : "typed in";
    l->n++;
  }
  free(json);
}

/* The folder this program is in, or -1. */
static int exe_dir(char *out, size_t n) {
#if defined(_WIN32)
  DWORD len = GetModuleFileNameA(NULL, out, (DWORD)n);
  if (!len || len >= n) return -1;
#elif defined(__linux__)
  ssize_t len = readlink("/proc/self/exe", out, n - 1);
  if (len <= 0) return -1;
  out[len] = 0;
#else
  (void)out;
  (void)n;
  return -1;
#endif
  char *slash = strrchr(out, '/');
  char *bslash = strrchr(out, '\\');
  if (bslash > slash) slash = bslash;
  if (!slash) return -1;
  *slash = 0;
  return 0;
}

/* Measured references first, then the rest; by name within each. */
static int ref_order(const void *x, const void *y) {
  const ref_entry *a = x, *b = y;
  int ma = !strncmp(a->kind, "measured", 8), mb = !strncmp(b->kind, "measured", 8);
  if (ma != mb) return mb - ma;
  return strcmp(a->id, b->id);
}

static size_t scan_refs(ref_list *l) {
  char dir[1200], exe[1100];
  l->n = 0;
  if (results_dir(dir, sizeof dir, 0) == 0 && strlen(dir) > 5) {
    strcpy(dir + strlen(dir) - 5, "/references"); /* .../results/runs -> .../results/references */
    l->own = 1;
    each_json(dir, add_ref, l);
  }
  l->own = 0;
  if (exe_dir(exe, sizeof exe) == 0) {
    snprintf(dir, sizeof dir, "%s/references", exe);
    each_json(dir, add_ref, l);
    snprintf(dir, sizeof dir, "%s/../share/prismark/references", exe);
    each_json(dir, add_ref, l);
  }
#ifdef PRISMARK_REFERENCES_DIR
  each_json(PRISMARK_REFERENCES_DIR, add_ref, l);
#endif
  qsort(l->v, l->n, sizeof *l->v, ref_order);
  return l->n;
}

/* A reference by its name, "ref:" and its name, or the start of its name; NULL, after listing the candidates
   when the start fits several. */
static const ref_entry *find_ref(const ref_list *l, const char *arg, int quiet, int *ambiguous) {
  if (!strncmp(arg, "ref:", 4)) arg += 4;
  size_t len = strlen(arg), hits = 0;
  const ref_entry *hit = NULL;
  for (size_t i = 0; i < l->n; i++) {
    if (!strcmp(l->v[i].id, arg)) return &l->v[i];
    if (len && !strncmp(l->v[i].id, arg, len)) hit = &l->v[i], hits++;
  }
  if (hits == 1) return hit;
  if (ambiguous) *ambiguous = hits > 1;
  if (hits > 1 && !quiet) {
    fprintf(stderr, "prismark: '%s' matches %zu reference systems:\n", arg, hits);
    for (size_t i = 0; i < l->n; i++)
      if (!strncmp(l->v[i].id, arg, len)) fprintf(stderr, "  %s\n", l->v[i].id);
  }
  return NULL;
}

/*
 * A RUN argument: an existing file, "latest", or a run id (or the start of one) of a result in the results
 * folder. 0 with the file in out; -1 after explaining why.
 */
static int resolve_run(const char *arg, char *out, size_t n) {
  struct stat st;
  if (stat(arg, &st) == 0) {
    snprintf(out, n, "%s", arg);
    return 0;
  }
  char dir[1100];
  if (results_dir(dir, sizeof dir, 0)) snprintf(dir, sizeof dir, ".");
  run_entry *v = NULL;
  size_t nv = scan_runs(dir, &v), hit = 0, nhit = 0;
  int latest = !strcmp(arg, "latest") || !strcmp(arg, "last");
  if (latest && nv) nhit = 1;
  for (size_t i = 0; !latest && i < nv; i++)
    if (*arg && !strncmp(v[i].b.run_id, arg, strlen(arg))) {
      if (!nhit++) hit = i;
    }
  if (nhit == 0 || !strncmp(arg, "ref:", 4)) {
    static ref_list refs;
    scan_refs(&refs);
    int ambiguous = 0;
    const ref_entry *r = find_ref(&refs, arg, 0, &ambiguous);
    free(v);
    if (r) {
      snprintf(out, n, "%s", r->path);
      return 0;
    }
    if (!ambiguous) fprintf(stderr, "prismark: no result file, run or reference system '%s' (prismark list, prismark references)\n",
            arg);
    return -1;
  }
  if (nhit == 1) snprintf(out, n, "%s", v[hit].path);
  else {
    fprintf(stderr, "prismark: '%s' matches %zu runs; give more of the run id:\n", arg, nhit);
    for (size_t i = 0; i < nv; i++)
      if (!strncmp(v[i].b.run_id, arg, strlen(arg))) fprintf(stderr, "  %s  %s\n", v[i].b.run_id, v[i].b.started_utc);
  }
  free(v);
  return nhit == 1 ? 0 : -1;
}

/* ---------- progress ---------- */

enum { OUT_TEXT, OUT_JSON, OUT_QUIET };

typedef struct {
  int mode;               /* OUT_* */
  uint32_t step, steps;   /* position in the whole run */
  double t0;
  double temp;
  char activity[512];     /* what runs now, shown on the status line */
  int shown;              /* the status line is on screen */
} progress;

static void json_string(FILE *f, const char *s) {
  fputc('"', f);
  for (; s && *s; s++) {
    unsigned char ch = (unsigned char)*s;
    if (ch == '"' || ch == '\\') fprintf(f, "\\%c", ch);
    else if (ch < 0x20) fprintf(f, "\\u%04x", ch);
    else fputc(ch, f);
  }
  fputc('"', f);
}

static void status_clear(progress *p) {
  if (!p->shown) return;
  fputs("\r\033[K", stderr);
  p->shown = 0;
}

/*   ━━━━━━━━━━━━━━━━━━━━  45%  5/11  0:42  34°C  Wake-up test: core P, task of 0.5 ms */
static void status_draw(progress *p) {
  int width = term_width() - 1, cols = 0;
  char line[1024], el[16];
  size_t k = 0;
#define PUT(...) (k += (size_t)snprintf(line + k, k < sizeof line ? sizeof line - k : 0, __VA_ARGS__))
  if (p->steps) {
    int bar = 20, done = (int)((double)bar * p->step / p->steps + 0.5);
    PUT("  %s", e(CYAN));
    for (int i = 0; i < bar; i++) {
      if (i == done) PUT("%s%s", e(RESET), e(DIM));
      PUT("━");
    }
    PUT("%s %3d%%  %u/%u", e(RESET), (int)(100.0 * p->step / p->steps), p->step, p->steps);
    cols = 2 + bar + 5 + 2 + snprintf(NULL, 0, "%u/%u", p->step, p->steps);
  } else {
    PUT("  ");
    cols = 2;
  }
  int n = snprintf(NULL, 0, "  %s", fmt_duration(el, sizeof el, now_s() - p->t0));
  PUT("  %s", el);
  cols += n;
  if (isfinite(p->temp)) {
    n = snprintf(NULL, 0, "  %.0f C", p->temp);
    PUT("  %s%.0f°C%s", e(DIM), p->temp, e(RESET));
    cols += n;
  }
  int room = width - cols - 2;
  if (room > 3 && p->activity[0]) {
    int len = (int)strlen(p->activity);
    if (len > room) PUT("  %.*s...", room - 3, p->activity);
    else PUT("  %s", p->activity);
  }
#undef PUT
  fprintf(stderr, "\r%s\033[K", line);
  fflush(stderr);
  p->shown = 1;
}

/* A line above the status line (the status line is redrawn below it). */
static void live_line(progress *p, const char *style, const char *text) {
  status_clear(p);
  fprintf(stderr, "%s%s%s\n", e(style), text, e(*style ? RESET : ""));
  status_draw(p);
}

static void on_event(const pmk_event *ev, void *user) {
  progress *p = user;
  if (p->mode == OUT_QUIET) return;
  static const char *const kinds[] = {"phase", "info", "warning"};
  if (p->mode == OUT_JSON) { /* the desktop app's protocol: unchanged */
    fprintf(stderr, "{\"event\":\"%s\",\"phase\":", kinds[ev->kind <= PMK_EV_WARNING ? ev->kind : 1]);
    json_string(stderr, ev->phase);
    fprintf(stderr, ",\"kernel\":");
    if (ev->kernel) json_string(stderr, ev->kernel);
    else fputs("null", stderr);
    fprintf(stderr, ",\"step\":%u,\"steps\":%u,\"message\":", ev->step, ev->steps);
    json_string(stderr, ev->message);
    if (isfinite(ev->temp_c)) fprintf(stderr, ",\"temp_c\":%.1f}\n", ev->temp_c);
    else fputs(",\"temp_c\":null}\n", stderr);
    fflush(stderr);
    return;
  }

  const char *phase = ev->phase ? ev->phase : "", *msg = ev->message ? ev->message : "";
  char text[768], title[64];
  p->temp = ev->temp_c;
  if (!strcmp(phase, "progress")) { /* the run-wide position; the step it announces follows */
    p->step = ev->step;
    p->steps = ev->steps;
    return;
  }
  if (!strcmp(phase, "run")) { /* "part 2 of 5: st_burst" */
    const char *id = strstr(msg, ": ");
    id = id ? id + 2 : msg;
    snprintf(text, sizeof text, "\nPart %u of %u · %s", ev->step, ev->steps, mode_title(title, sizeof title, id));
    p->activity[0] = 0;
    if (term.live) live_line(p, BOLD, text);
    else fprintf(stderr, "%s (%s)\n", text, id);
    return;
  }

  /* What runs now: the test's name, then the core's message. */
  if (!strcmp(phase, "preflight")) snprintf(p->activity, sizeof p->activity, "Checking the machine: %s", msg);
  else if (!strcmp(phase, "done")) snprintf(p->activity, sizeof p->activity, "Computing statistics");
  else if (ev->kernel) snprintf(p->activity, sizeof p->activity, "%s: %s", pmk_display_name(ev->kernel), msg);
  else snprintf(p->activity, sizeof p->activity, "%s", msg);
  p->activity[0] = (char)toupper((unsigned char)p->activity[0]);

  /* Warnings and notices without a step (hands off, warming up, waiting for the temperature) stay on screen. */
  int keep = ev->kind == PMK_EV_WARNING || (ev->kind == PMK_EV_PHASE && ev->steps == 0 && strcmp(phase, "done"));
  int hands_off = !strncmp(msg, "hands off", 9);
  if (term.live) {
    if (keep) p->activity[0] = 0; /* the line says it already */
    if (ev->kind == PMK_EV_WARNING) {
      snprintf(text, sizeof text, "  ! %s%s%s", ev->kernel ? pmk_display_name(ev->kernel) : "", ev->kernel ? ": " : "", msg);
      live_line(p, YELLOW, text);
    } else if (hands_off) {
      const char *why = strchr(msg, ':');
      snprintf(text, sizeof text, "  Hands off the keyboard and mouse: %s", why ? why + 1 + (why[1] == ' ') : msg);
      live_line(p, BOLD YELLOW, text);
    } else if (keep) {
      snprintf(text, sizeof text, "  %s%s%s", ev->kernel ? pmk_display_name(ev->kernel) : "", ev->kernel ? ": " : "", msg);
      text[2] = (char)toupper((unsigned char)text[2]);
      live_line(p, "", text);
    } else {
      status_draw(p);
    }
    return;
  }
  char pos[32] = "";
  if (p->steps) snprintf(pos, sizeof pos, "[%*u/%u] ", (int)snprintf(NULL, 0, "%u", p->steps), p->step, p->steps);
  char temp[24] = "";
  if (isfinite(ev->temp_c)) snprintf(temp, sizeof temp, "  (%.1f C)", ev->temp_c);
  fprintf(stderr, "%s%s%s%s\n", pos, ev->kind == PMK_EV_WARNING ? "warning: " : "", p->activity, temp);
}

/* ---------- signals and the desktop app's stdin ---------- */

static volatile sig_atomic_t g_live_signal;

static void on_signal(int sig) {
  pmk_cancel();
  if (g_live_signal) {
    static const char msg[] = "\r\033[K  Stopping after the current measurement; what was measured is kept. "
                              "Ctrl+C again quits at once.\n";
#ifdef _WIN32
    fputs(msg, stderr);
#else
    ssize_t r = write(2, msg, sizeof msg - 1);
    (void)r;
#endif
  }
  signal(sig, SIG_DFL); /* a second Ctrl+C ends the process */
}

/*
 * A GUI may run this process with elevated rights, so it cannot signal it;
 * it cancels by writing "cancel" or by closing the pipe (also when it exits).
 */
#ifdef _WIN32
static DWORD WINAPI stdin_watch(LPVOID arg) {
#else
static void *stdin_watch(void *arg) {
#endif
  (void)arg;
#ifdef _WIN32
  char line[64];
  while (fgets(line, sizeof line, stdin)) {
    if (!strncmp(line, "input", 5)) pmk_notify_input(); /* keyboard or mouse activity seen by the GUI */
    else if (!strncmp(line, "cancel", 6)) break;
  }
#else
  /* Raw read(), not stdio: a thread blocked in fgets holds the stdin lock, and exit() in newer glibc (2.39+)
     takes every stream's lock to flush it, so the finished runner would hang at exit while the GUI keeps the
     pipe open. */
  char line[64];
  size_t len = 0;
  for (;;) {
    ssize_t n = read(0, line + len, sizeof line - 1 - len);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break; /* closed (the GUI cancelled or exited) or failed */
    len += (size_t)n;
    line[len] = 0;
    int cancel = 0;
    char *nl;
    while ((nl = strchr(line, '\n'))) {
      *nl = 0;
      if (!strncmp(line, "input", 5)) pmk_notify_input(); /* keyboard or mouse activity seen by the GUI */
      else if (!strncmp(line, "cancel", 6)) cancel = 1;
      len -= (size_t)(nl + 1 - line);
      memmove(line, nl + 1, len + 1);
    }
    if (cancel) break;
    if (len == sizeof line - 1) len = 0; /* an overlong line is not a command */
  }
#endif
  pmk_cancel();
  return 0;
}

static void start_stdin_watch(void) {
#ifdef _WIN32
  HANDLE h = CreateThread(NULL, 0, stdin_watch, NULL, 0, NULL);
  if (h) CloseHandle(h);
#else
  pthread_t th;
  if (pthread_create(&th, NULL, stdin_watch, NULL) == 0) pthread_detach(th);
#endif
}

/* ---------- help ---------- */

static void help_main(FILE *f) {
  char dir[1100], pd[1200];
  int has_dir = results_dir(dir, sizeof dir, 0) == 0;
  fprintf(f,
          "%sPrismark %s%s: a CPU benchmark. It measures this machine as it is configured and changes nothing on it.\n"
          "\n%susage:%s prismark                    a menu: run, browse and compare (on a terminal)\n"
          "       prismark run [options]      run the benchmark (options alone also start a run)\n"
          "       prismark COMMAND [...]\n"
          "\n%scommands:%s\n"
          "  run                      run the benchmark with the options below\n"
          "  tests                    the tests and modes, with their names and IDs\n"
          "  list                     past runs in the results folder\n"
          "  show RUN                 the summary of a past run\n"
          "  compare RUN RUN          compare two runs, or a run and a reference system\n"
          "  references               the reference systems to compare with\n"
          "  profiles                 the default comparison profiles (JSON)\n"
          "  checksums                kernel output checksums, no timing\n"
          "  info                     what this build can run on this machine (JSON)\n"
          "  help [COMMAND|advanced]  this text, a command's options, or the advanced run options\n"
          "\nRUN is a result file, a run ID or its first characters (see prismark list), 'latest', or for compare\n"
          "a reference system's name (see prismark references).\n"
          "\n%swhat to run:%s\n"
          "  --quick                  smoke run of a few minutes: no warm-up, short series (not for comparison)\n"
          "  --tests LIST             only these tests, by name or ID: compression,photo or K3,K4 (default: all)\n"
          "  --mode LIST              only these modes, by name or ID: short-task,from-rest (default: all)\n"
          "  --no-isa-uplift          skip the new-instruction runs (3D rendering, Compression, Opening a photo,\n"
          "                           Matrix maths at the newest instruction level)\n"
          "  --k1-data DIR            prepared snapshot for Compiling code and Full software build\n"
          "                           (tools/k1x/prepare.py)\n"
          "\n%swhere it runs:%s\n"
          "  --cpu N                  single-core work on CPU N (default: one CPU per core type)\n"
          "  --max-threads N          most threads in the all-cores modes (default: all CPUs)\n"
          "\n%soutput:%s\n"
          "  -o, --output FILE        result file (default: the results folder below)\n"
          "  -q, --quiet              no progress\n"
          "  --no-color               no colours (also off with NO_COLOR set, or when not on a terminal)\n"
          "  --version\n"
          "\nA full run takes about 30 minutes on a 4-core laptop. Ctrl+C stops it and keeps what was measured.\n"
          "Results go to %s%s.\n",
          o(BOLD), pmk_version(), o(RESET), o(BOLD), o(RESET), o(BOLD), o(RESET), o(BOLD), o(RESET), o(BOLD),
          o(RESET), o(BOLD), o(RESET), has_dir ? pretty_path(pd, sizeof pd, dir) : "the current folder",
          has_dir ? ",\nwhich the desktop app reads too (the current folder when run as root)" : "");
}

static void help_advanced(FILE *f) {
  fprintf(f,
          "%sAdvanced run options.%s Results stay comparable with other runs only with the defaults.\n"
          "\n%smeasurement:%s\n"
          "  --seed N                 experiment seed (default: random, recorded)\n"
          "  --min-reps N             minimum repetitions per series (default 20)\n"
          "  --max-reps N             maximum repetitions per warm series (default 100)\n"
          "  --cold-max-reps N        maximum repetitions per started-from-rest series (default 1000)\n"
          "  --warmup S               full load before each warmed-up mode, not scored (default 60)\n"
          "  --settle S               before each warmed-up series, not scored (default 5)\n"
          "  --measure S              measured per warmed-up series; Compiling code three times (default 20)\n"
          "  --window-ms X            measurement window of warmed-up series (default 1000)\n"
          "  --cooldown               wait for the idle temperature before each mode (default: no)\n"
          "  --period-ms X            timer punctuality period (default 1)\n"
          "  --periodic-seconds X     duration of each timer punctuality condition (default 60)\n"
          "  --k1x-reps N             Full software build: builds per thread count (default 3)\n"
          "  --skip-preflight         skip the background-load check and temperature wait\n"
          "  --max-load X             preflight background-load threshold, fraction (default 0.05)\n"
          "\n%sused by the desktop app:%s\n"
          "  --frontend NAME          recorded front-end kind (default cli)\n"
          "  --progress-json          progress events as JSON lines on stderr\n"
          "  --cancel-on-stdin        read commands on stdin: \"cancel\" (also on close), \"input\" (user activity)\n"
          "  --input-watch NAME       how the front-end watches for input, recorded with the result\n"
          "\n--kernels is the same as --tests.\n",
          o(BOLD), o(RESET), o(BOLD), o(RESET), o(BOLD), o(RESET));
}

static void help_cmd(FILE *f, const char *cmd) {
  if (!strcmp(cmd, "advanced") || !strcmp(cmd, "run")) help_advanced(f);
  else if (!strcmp(cmd, "tests")) fprintf(f, "usage: prismark tests\n\nThe tests and measurement modes, with the names --tests and --mode accept.\n");
  else if (!strcmp(cmd, "list"))
    fprintf(f, "usage: prismark list [--dir DIR]\n\nPast runs in the results folder (or DIR), newest first.\n");
  else if (!strcmp(cmd, "show"))
    fprintf(f, "usage: prismark show RUN\n\nThe summary of a past run. RUN is a result file, a run ID or its first\n"
               "characters, or 'latest'.\n");
  else if (!strcmp(cmd, "compare"))
    fprintf(f, "usage: prismark compare RUN RUN [--detail] [--profiles FILE] [-o FILE]\n\n"
               "The headline results side by side (the ones the desktop app shows), A against B, with which is\n"
               "better. Either side may be a reference system (prismark references), e.g.\n"
               "  prismark compare latest intel-i5-1035g1\n\n"
               "  --detail          every measured series with 95%% ranges, and the profiles (Daily, Dev, Render,\n"
               "                    Realtime); two measured results under the same capabilities only\n"
               "  --profiles FILE   profiles to use instead of the defaults (implies --detail)\n"
               "  -o FILE           also write the detailed comparison as JSON\n");
  else if (!strcmp(cmd, "references"))
    fprintf(f, "usage: prismark references\n\nThe reference systems compare accepts: the ones that come with Prismark\n"
               "and your own, in the references folder next to your results (see references/README.md).\n");
  else if (!strcmp(cmd, "checksums"))
    fprintf(f, "usage: prismark checksums [--k1-data DIR] [--tests LIST] [--burst-only] [-o FILE]\n\n"
               "Runs one job of every kernel at every instruction level and prints input hashes and output\n"
               "checksums. No timing.\n");
  else if (!strcmp(cmd, "profiles")) fprintf(f, "usage: prismark profiles\n\nThe default profiles as JSON, editable and usable with compare --profiles.\n");
  else if (!strcmp(cmd, "info")) fprintf(f, "usage: prismark info\n\nWhat this build can run on this machine, as JSON.\n");
  else help_main(f);
}

static int is_help(const char *a) { return !strcmp(a, "-h") || !strcmp(a, "--help"); }

static int bad_usage(const char *cmd, const char *fmt, const char *arg) {
  fprintf(stderr, "prismark: ");
  fprintf(stderr, fmt, arg);
  fprintf(stderr, "\nRun 'prismark help%s%s' for the options.\n", cmd ? " " : "", cmd ? cmd : "");
  return 2;
}

/* ---------- commands ---------- */

static size_t kernel_index(const char *id) {
  size_t k = 0;
  while (k < NKERNELS - 1 && strcmp(KERNELS[k].id, id)) k++;
  return k;
}

static int cmd_tests(void) {
  int described[NKERNELS] = {0};
  printf("%sThe tests, by category as in the desktop app.%s The options in brackets run just that one.\n", o(BOLD),
         o(RESET));
  for (size_t c = 0; c < NCATEGORIES; c++) {
    printf("\n%s%s%s  %s%s%s\n", o(BOLD), CATEGORIES[c].name, o(RESET), o(DIM), CATEGORIES[c].about, o(RESET));
    for (size_t i = 0; i < NITEMS; i++) {
      if (ITEMS[i].cat != (int)c) continue;
      char slugs[96] = "";
      for (const char *p = ITEMS[i].kernels; *p;) {
        char id[16];
        size_t len = strcspn(p, ",");
        snprintf(id, sizeof id, "%.*s", (int)len, p);
        snprintf(slugs + strlen(slugs), sizeof slugs - strlen(slugs), "%s%s", *slugs ? "," : "", KERNELS[kernel_index(id)].slug);
        p += len + (p[len] == ',');
      }
      const char *mode_slug = "";
      for (size_t m = 0; m < NMODES; m++)
        if (!strcmp(MODES[m].id, ITEMS[i].mode)) mode_slug = MODES[m].slug;
      int extra_isa = !ITEMS[i].isa && !strcmp(ITEMS[i].mode, "st_sustained"); /* else the gain runs too */
      printf("\n  %s%s%s  %s(--tests %s --mode %s%s)%s\n", o(BOLD), ITEMS[i].name, o(RESET), o(DIM), slugs, mode_slug,
             extra_isa ? " --no-isa-uplift" : "", o(RESET));
      if (ITEMS[i].isa) {
        printf("    Each test built twice: for the instructions every processor of its kind has, and for the\n"
               "    newest this one supports. Matrix maths multiplies large tables of decimal numbers (graphics)\n"
               "    and small integers (AI models).\n"
               "    %sReports:%s how many times faster the newest instructions are (higher is better; 1.00x = none).\n",
               o(DIM), o(RESET));
        continue;
      }
      size_t k = kernel_index(ITEMS[i].kernels);
      if (described[k]) {
        printf("    The %s test above, %s.\n", pmk_display_name(KERNELS[k].id), pmk_display_name(ITEMS[i].mode));
        continue;
      }
      described[k] = 1;
      printf("    %s\n    %sReports:%s %s\n", KERNELS[k].what, o(DIM), o(RESET), KERNELS[k].reports);
    }
  }
  printf("\n%sModes%s  (--mode takes the name, the short name or the ID)\n\n", o(BOLD), o(RESET));
  for (size_t i = 0; i < NMODES; i++) {
    char title[64];
    printf("  %s%s%s  %s%s, %s%s\n    %s\n", o(BOLD), mode_title(title, sizeof title, MODES[i].id), o(RESET), o(DIM),
           MODES[i].slug, MODES[i].id, o(RESET), MODES[i].how);
  }
  printf("\n--tests also takes a test's ID or full name (prismark help). A full run runs every test in every mode\n"
         "it has, in random order: about 30 minutes on a 4-core laptop. Every machine does the same work.\n");
  return 0;
}

static int cmd_list(int argc, char **argv) {
  char dir[1100], pd[1200];
  int have = 0;
  for (int i = 2; i < argc; i++) {
    if (is_help(argv[i])) return help_cmd(stdout, "list"), 0;
    if (!strcmp(argv[i], "--dir") && i + 1 < argc) snprintf(dir, sizeof dir, "%s", argv[++i]), have = 1;
    else return bad_usage("list", "unexpected '%s'", argv[i]);
  }
  if (!have && results_dir(dir, sizeof dir, 0)) snprintf(dir, sizeof dir, ".");
  run_entry *v = NULL;
  size_t n = scan_runs(dir, &v);
  pretty_path(pd, sizeof pd, dir);
  if (!n) {
    printf("No runs in %s yet. Start one with: prismark --quick (a few minutes) or prismark (a full run).\n", pd);
    free(v);
    return 0;
  }
  printf("%s%-10s %-17s %-6s %-11s %-4s %-6s %s%s\n", o(DIM), "run", "started (UTC)", "kind", "state", "from",
         "modes", "CPU", o(RESET));
  for (size_t i = 0; i < n; i++) {
    const pmk_brief *b = &v[i].b;
    char when[32];
    snprintf(when, sizeof when, "%.10s %.5s", b->started_utc, strlen(b->started_utc) > 11 ? b->started_utc + 11 : "");
    int nm = 0;
    for (size_t k = 0; k < NMODES; k++) nm += !!(b->modes & MODES[k].bit);
    char modes[16];
    snprintf(modes, sizeof modes, "%d/%d", nm, (int)NMODES);
    printf("%s%-10.8s%s %-17s %-6s %s%-11s%s %-4s %-6s %.48s\n", o(BOLD), b->run_id, o(RESET), when,
           b->quick == 1 ? "quick" : b->quick == 0 ? "full" : "?", b->complete ? "" : o(YELLOW),
           b->complete ? "complete" : "incomplete", o(RESET), b->frontend, modes, b->model);
  }
  printf("\n%s%zu run%s in %s. Details: prismark show RUN. Compare: prismark compare RUN RUN.%s\n", o(DIM), n,
         n == 1 ? "" : "s", pd, o(RESET));
  free(v);
  return 0;
}

/* The summary text with its section titles (lines that start a section) in bold. */
static void print_summary(const char *s) {
  while (*s) {
    size_t len = strcspn(s, "\n");
    int title = len && s[0] != ' ';
    printf("%s%.*s%s\n", title ? o(BOLD) : "", (int)len, s, title ? o(RESET) : "");
    s += len;
    if (*s) s++;
  }
}

static int cmd_show(int argc, char **argv) {
  const char *arg = NULL;
  for (int i = 2; i < argc; i++) {
    if (is_help(argv[i])) return help_cmd(stdout, "show"), 0;
    if (!arg && argv[i][0] != '-') arg = argv[i];
    else return bad_usage("show", "unexpected '%s'", argv[i]);
  }
  char path[1200], pd[1200];
  if (!arg) arg = "latest";
  if (resolve_run(arg, path, sizeof path)) return 1;
  char *json = read_file(path, 0);
  if (!json) return 1;
  pmk_brief b;
  memset(&b, 0, sizeof b);
  b.struct_size = sizeof b;
  char *summary = NULL;
  int rc = pmk_describe(json, &b, &summary);
  free(json);
  if (rc != PMK_OK) {
    if (strstr(path, "references"))
      fprintf(stderr, "prismark: %s is a reference system; compare a run with it: prismark compare latest %s\n", arg,
              arg);
    else fprintf(stderr, "prismark: %s is not a Prismark result\n", path);
    return 1;
  }
  if (summary) print_summary(summary);
  else
    printf("%sPrismark run %s%s\n%s, started %s, %s run%s\n\nThis result was saved by an older Prismark, which did "
           "not store its summary.\nOpen it in the desktop app, or: cd analyzer && uv run prismark-analyze plot FILE\n",
           o(BOLD), b.run_id, o(RESET), b.model, b.started_utc, b.quick == 1 ? "quick" : "full",
           b.complete ? "" : " (incomplete)");
  printf("\n%sfile: %s%s\n", o(DIM), pretty_path(pd, sizeof pd, path), o(RESET));
  pmk_free(summary);
  return 0;
}

/* The headline comparison: section titles in bold, "better" in green and "worse" in yellow. */
static void print_headline(const char *s) {
  for (int first = 1; *s; first = 0) {
    size_t len = strcspn(s, "\n");
    const char *better = NULL, *worse = NULL;
    if (len > 2 && s[0] == ' ' && s[1] == ' ' && s[2] != ' ') {
      for (const char *p = s; p < s + len; p++) {
        if (!strncmp(p, "x better", 8)) better = p + 2;
        if (!strncmp(p, "x worse", 7)) worse = p + 2;
      }
    }
    int title = first || (len && s[0] != ' ' && len < 20 && !memchr(s, '.', len));
    const char *word = better ? better : worse;
    if (word && term.color_out) {
      const char *start = word - 2;
      while (start > s && start[-1] != ' ') start--;
      printf("%.*s%s%.*s%s\n", (int)(start - s), s, better ? GREEN : YELLOW, (int)(s + len - start), start, RESET);
    } else {
      printf("%s%.*s%s\n", title ? o(BOLD) : "", (int)len, s, title ? o(RESET) : "");
    }
    s += len;
    if (*s) s++;
  }
}

static int cmd_compare(int argc, char **argv) {
  const char *runs[2] = {NULL, NULL}, *profiles = NULL, *output = NULL;
  int nf = 0, detail = 0;
  for (int i = 2; i < argc; i++) {
    if (is_help(argv[i])) return help_cmd(stdout, "compare"), 0;
    if (!strcmp(argv[i], "--profiles") && i + 1 < argc) profiles = argv[++i];
    else if ((!strcmp(argv[i], "-o") || !strcmp(argv[i], "--output")) && i + 1 < argc) output = argv[++i];
    else if (!strcmp(argv[i], "--detail")) detail = 1;
    else if (nf < 2 && argv[i][0] != '-') runs[nf++] = argv[i];
    else return bad_usage("compare", "unexpected '%s'", argv[i]);
  }
  if (nf != 2) return bad_usage("compare", "%s", "compare needs two runs, or a run and a reference system");
  char files[2][1200];
  for (int i = 0; i < 2; i++)
    if (resolve_run(runs[i], files[i], sizeof files[i])) return 1;
  char *a = read_file(files[0], 0), *b = read_file(files[1], 0), *p = profiles ? read_file(profiles, 0) : NULL;
  int rc = PMK_ERR_INVALID;
  if (a && b && (!profiles || p)) {
    char *json = NULL, *text = NULL;
    if (detail || profiles) { /* per series, with the profiles: two measured results only */
      rc = pmk_compare(a, b, p, &json, &text);
      if (text && rc == PMK_OK) print_summary(text);
      else if (text) fputs(text, stderr);
      if (rc == PMK_ERR_INVALID)
        fprintf(stderr, "The headline comparison works for any two runs or reference systems: prismark compare %s %s\n",
                runs[0], runs[1]);
    } else {
      rc = pmk_compare_headline(a, b, &text);
      if (text) {
        if (rc == PMK_OK) print_headline(text);
        else fputs(text, stderr);
      }
      if (rc == PMK_OK && output) rc = pmk_compare(a, b, NULL, &json, NULL); /* -o writes the detailed report */
      pmk_brief ba, bb; /* the detailed report needs two measured results */
      memset(&ba, 0, sizeof ba);
      memset(&bb, 0, sizeof bb);
      ba.struct_size = bb.struct_size = sizeof ba;
      if (rc == PMK_OK && pmk_describe(a, &ba, NULL) == PMK_OK && pmk_describe(b, &bb, NULL) == PMK_OK)
        printf("%sPer series, with 95 %% ranges and the profiles: prismark compare --detail %s %s%s\n", o(DIM), runs[0],
               runs[1], o(RESET));
    }
    if (rc == PMK_OK && output && json && write_file(output, json)) rc = PMK_ERR_SYSTEM;
    pmk_free(json);
    pmk_free(text);
  }
  free(a);
  free(b);
  free(p);
  return rc == PMK_OK ? 0 : 1;
}

static int cmd_references(int argc, char **argv) {
  if (argc > 2 && is_help(argv[2])) return help_cmd(stdout, "references"), 0;
  static ref_list l;
  if (!scan_refs(&l)) {
    printf("No reference systems found (references/ beside the program, or results/references in your data).\n");
    return 0;
  }
  printf("%s%-28s %-16s %s%s\n", o(DIM), "reference", "kind", "system", o(RESET));
  for (size_t i = 0; i < l.n; i++)
    printf("%s%-28s%s %s%-16s%s %s%s\n", o(BOLD), l.v[i].id, o(RESET), !strcmp(l.v[i].kind, "placeholder") ? o(YELLOW) : "",
           l.v[i].kind, o(RESET), l.v[i].name, l.v[i].own ? "  (yours)" : "");
  printf("\n%sCompare a run with one: prismark compare latest %s. Placeholders hold example values, not\n"
         "measurements. Add your own in the references folder next to your results (see references/README.md).%s\n",
         o(DIM), l.v[0].id, o(RESET));
  return 0;
}

static int cmd_checksums(int argc, char **argv) {
  const char *k1 = NULL, *output = NULL, *kernels = NULL;
  char ids[256];
  int burst_only = 0;
  for (int i = 2; i < argc; i++) {
    if (is_help(argv[i])) return help_cmd(stdout, "checksums"), 0;
    if (!strcmp(argv[i], "--k1-data") && i + 1 < argc) k1 = argv[++i];
    else if ((!strcmp(argv[i], "--kernels") || !strcmp(argv[i], "--tests")) && i + 1 < argc) {
      const char *bad;
      size_t badlen;
      if (parse_tests(argv[++i], ids, sizeof ids, &bad, &badlen)) {
        fprintf(stderr, "prismark: unknown test '%.*s' (prismark tests lists them)\n", (int)badlen, bad);
        return 2;
      }
      kernels = ids;
    } else if (!strcmp(argv[i], "--burst-only")) burst_only = 1;
    else if ((!strcmp(argv[i], "-o") || !strcmp(argv[i], "--output")) && i + 1 < argc) output = argv[++i];
    else return bad_usage("checksums", "unexpected '%s'", argv[i]);
  }
  char *json = NULL, *text = NULL;
  int rc = pmk_checksums_ex(k1, kernels, burst_only, &json, &text);
  if (text) fputs(text, stdout);
  if (output && json && write_file(output, json)) rc = PMK_ERR_SYSTEM;
  pmk_free(json);
  pmk_free(text);
  return rc == PMK_OK ? 0 : 1;
}

/* ---------- run ---------- */

static int cmd_run(int argc, char **argv) {
  pmk_config cfg;
  pmk_config_init(&cfg);
  cfg.frontend = "cli";
  cfg.ui_state = "none";
  const char *output = NULL;
  int out_mode = OUT_TEXT, cancel_on_stdin = 0, quick = 0;
  char tests[256];

  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    const char *v = i + 1 < argc ? argv[i + 1] : NULL;
#define NEED_ARG                                                    \
  do {                                                              \
    if (!v) return bad_usage(NULL, "%s needs a value", a);          \
    i++;                                                            \
  } while (0)
    const char *bad;
    size_t badlen;
    if (!strcmp(a, "--mode")) {
      NEED_ARG;
      if (parse_modes(v, &cfg.modes, &bad, &badlen)) {
        fprintf(stderr, "prismark: unknown mode '%.*s' (prismark tests lists them)\n", (int)badlen, bad);
        return 2;
      }
    } else if (!strcmp(a, "--kernels") || !strcmp(a, "--tests")) {
      NEED_ARG;
      if (parse_tests(v, tests, sizeof tests, &bad, &badlen)) {
        fprintf(stderr, "prismark: unknown test '%.*s' (prismark tests lists them)\n", (int)badlen, bad);
        return 2;
      }
      cfg.kernels = tests;
    }
    else if (!strcmp(a, "--cpu")) { NEED_ARG; cfg.cpu = atoi(v); }
    else if (!strcmp(a, "--max-threads")) { NEED_ARG; cfg.max_threads = atoi(v); }
    else if (!strcmp(a, "--seed")) { NEED_ARG; cfg.seed = strtoull(v, NULL, 0); }
    else if (!strcmp(a, "--no-isa-uplift")) cfg.isa_uplift = 0;
    else if (!strcmp(a, "--min-reps")) { NEED_ARG; cfg.min_reps = atoi(v); }
    else if (!strcmp(a, "--max-reps")) { NEED_ARG; cfg.max_reps = atoi(v); }
    else if (!strcmp(a, "--cold-max-reps")) { NEED_ARG; cfg.cold_max_reps = atoi(v); }
    else if (!strcmp(a, "--window-ms")) { NEED_ARG; cfg.window_ms = atof(v); }
    else if (!strcmp(a, "--warmup")) { NEED_ARG; cfg.warmup_s = atof(v); }
    else if (!strcmp(a, "--settle")) { NEED_ARG; cfg.settle_s = atof(v); }
    else if (!strcmp(a, "--measure")) { NEED_ARG; cfg.measure_s = atof(v); }
    else if (!strcmp(a, "--cooldown")) cfg.cooldown = 1;
    else if (!strcmp(a, "--no-cooldown")) cfg.cooldown = 0;
    else if (!strcmp(a, "--period-ms")) { NEED_ARG; cfg.periodic_period_ms = atof(v); }
    else if (!strcmp(a, "--periodic-seconds")) { NEED_ARG; cfg.periodic_seconds = atof(v); }
    else if (!strcmp(a, "--k1-data")) { NEED_ARG; cfg.k1_data = v; }
    else if (!strcmp(a, "--k1x-reps")) { NEED_ARG; cfg.k1x_reps = atoi(v); }
    else if (!strcmp(a, "--quick")) {
      quick = 1;
      cfg.max_reps = 20;
      cfg.cold_max_reps = 20;
      cfg.periodic_seconds = 10;
      cfg.window_ms = 250;
      cfg.warmup_s = 0;
      cfg.settle_s = 0;
      cfg.measure_s = 3;
      cfg.k1x_reps = 1;
      cfg.cooldown = 0;
      cfg.quick_inputs = 1;
    }
    else if (!strcmp(a, "--skip-preflight")) cfg.skip_preflight = 1;
    else if (!strcmp(a, "--max-load")) { NEED_ARG; cfg.max_background_load = atof(v); }
    else if (!strcmp(a, "--frontend")) { NEED_ARG; cfg.frontend = v; cfg.ui_state = strcmp(v, "cli") ? "frozen" : "none"; }
    else if (!strcmp(a, "--progress-json")) out_mode = OUT_JSON;
    else if (!strcmp(a, "--cancel-on-stdin")) cancel_on_stdin = 1;
    else if (!strcmp(a, "--input-watch")) { NEED_ARG; cfg.input_watch = v; }
    else if (!strcmp(a, "-o") || !strcmp(a, "--output")) { NEED_ARG; output = v; }
    else if (!strcmp(a, "-q") || !strcmp(a, "--quiet")) out_mode = OUT_QUIET;
    else if (!strcmp(a, "--no-color") || !strcmp(a, "--no-colour")) {} /* read by main */
    else if (!strcmp(a, "--version")) { printf("prismark %s\n", pmk_version()); return 0; }
    else return bad_usage(NULL, a[0] == '-' ? "unknown option '%s'" : "unknown command '%s'", a);
#undef NEED_ARG
  }

  progress prog = {out_mode, 0, 0, now_s(), NAN, "", 0};
  if (out_mode == OUT_JSON) term.live = 0;
  int live = out_mode == OUT_TEXT && term.live;
  g_live_signal = live;
  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);
  if (cancel_on_stdin) start_stdin_watch();

  if (out_mode == OUT_TEXT) {
    int subset = (cfg.modes && cfg.modes != PMK_MODE_ALL) || cfg.kernels;
    fprintf(stderr, "%sPrismark %s%s  %s%s%s\n", e(BOLD), pmk_version(), e(RESET),
            quick ? "quick run: a smoke test, not for comparison" : subset ? "selected tests" : "full run",
            quick || subset ? "" : ", about 30 minutes on a 4-core laptop",
            live ? ". Ctrl+C stops and keeps what was measured." : "");
  }

  char *json = NULL, *summary = NULL;
  int rc = pmk_start(&cfg, on_event, &prog, &json, &summary);
  g_live_signal = 0;
  if (live) status_clear(&prog);
  char el[16];
  fmt_duration(el, sizeof el, now_s() - prog.t0);
  if (rc == PMK_ERR_CANCELLED) fprintf(stderr, "%sCancelled after %s.%s%s\n", e(YELLOW), el, e(RESET), json ? " The part measured so far is saved." : "");
  else if (rc != PMK_OK) fprintf(stderr, "%sprismark: %s%s\n", e(RED), pmk_strerror(rc), e(RESET));
  else if (out_mode == OUT_TEXT) fprintf(stderr, "%sFinished in %s.%s\n", e(GREEN), el, e(RESET));

  if (json) {
    char name[1200], dir[1100], id[40], pd[1200];
    run_id_of(json, id, sizeof id);
    if (!output) {
      if (results_dir(dir, sizeof dir, 1) == 0) snprintf(name, sizeof name, "%s/prismark-%s.json", dir, id);
      else snprintf(name, sizeof name, "prismark-%s.json", id);
      output = name;
    }
    if (write_file(output, json) == 0) {
      give_back(output);
      if (out_mode == OUT_JSON) printf("\nresult: %s\n", output); /* the desktop app's protocol: unchanged */
      else {
        if (summary) {
          printf("\n");
          print_summary(summary);
        }
        printf("\n%sresult:%s %s\n", o(BOLD), o(RESET), pretty_path(pd, sizeof pd, output));
        if (out_mode == OUT_TEXT && !strcmp(output, name))
          printf("%sagain: prismark show %.8s   compare: prismark compare %.8s RUN   all runs: prismark list%s\n",
                 o(DIM), id, id, o(RESET));
      }
    } else if (rc == PMK_OK) {
      rc = PMK_ERR_SYSTEM;
    }
  }
  pmk_free(json);
  pmk_free(summary);
  return rc == PMK_OK ? 0 : 1;
}

/* ---------- menu ---------- */

/* One line from stdin, trimmed; -1 at end of input. */
static int ask(const char *prompt, char *buf, size_t n) {
  printf("%s", prompt);
  fflush(stdout);
  if (!fgets(buf, (int)n, stdin)) {
    printf("\n");
    return -1;
  }
  size_t len = strcspn(buf, "\r\n");
  buf[len] = 0;
  while (len && isspace((unsigned char)buf[len - 1])) buf[--len] = 0;
  size_t lead = strspn(buf, " \t");
  memmove(buf, buf + lead, len - lead + 1);
  return 0;
}

/* A yes/no question, asked again until answered; def on Enter, -1 at end of input. */
static int ask_yn(const char *prompt, int def) {
  char buf[16];
  for (;;) {
    if (ask(prompt, buf, sizeof buf)) return -1;
    for (char *c = buf; *c; c++) *c = (char)tolower((unsigned char)*c);
    if (!buf[0]) return def;
    if (!strcmp(buf, "y") || !strcmp(buf, "yes")) return 1;
    if (!strcmp(buf, "n") || !strcmp(buf, "no")) return 0;
    printf("  %sAnswer y or n.%s\n", o(YELLOW), o(RESET));
  }
}

static void pause_for_enter(void) {
  char buf[8];
  ask("\nPress Enter to go back to the menu.", buf, sizeof buf);
}

/* The desktop app's compile-test snapshot (<user data>/prismark/k1x), if prepared. */
static int k1_snapshot(char *out, size_t n) {
  char dir[1100];
  if (results_dir(dir, sizeof dir, 0)) return -1;
  size_t len = strlen(dir);
  if (len < 13) return -1;
  dir[len - 13] = 0; /* strip "/results/runs" */
  snprintf(out, n, "%s/k1x", dir);
  char manifest[1200];
  snprintf(manifest, sizeof manifest, "%s/manifest.json", out);
  struct stat st;
  return stat(manifest, &st) == 0 ? 0 : -1;
}

/* Starts a run with the menu's choices; the same code path as prismark run. */
static void menu_run(int quick, const char *tests, const char *modes, const char *k1, int isa) {
  const char *args[14];
  int n = 0;
  args[n++] = "prismark";
  if (quick) args[n++] = "--quick";
  if (tests && *tests) args[n++] = "--tests", args[n++] = tests;
  if (modes && *modes) args[n++] = "--mode", args[n++] = modes;
  if (k1) args[n++] = "--k1-data", args[n++] = k1;
  if (!isa) args[n++] = "--no-isa-uplift";
  args[n] = NULL;
  printf("\n");
  cmd_run(n, (char **)args);
  signal(SIGINT, SIG_DFL);
  signal(SIGTERM, SIG_DFL);
}

static int confirm_start(int quick, const char *what) {
  printf("\n%s%s%s: %s.\n", o(BOLD), quick ? "Quick run" : "Full run", o(RESET), what);
  printf("For steady results, close other programs and leave the computer alone while it runs.\n"
         "Some tests are skipped if the keyboard or mouse is used during them.\n");
  return ask_yn("Start now? [Y/n] ", 1) == 1;
}

/* Whether the comma-separated list has id. */
static int has_id(const char *list, const char *id) {
  size_t n = strlen(id);
  for (const char *p = list; *p;) {
    size_t len = strcspn(p, ",");
    if (len == n && !strncmp(p, id, n)) return 1;
    p += len + (p[len] == ',');
  }
  return 0;
}

/* Adds each id of ids to the comma-separated list once. */
static void add_ids(char *list, size_t n, const char *ids) {
  for (const char *p = ids; *p;) {
    char id[32];
    size_t len = strcspn(p, ",");
    snprintf(id, sizeof id, "%.*s", (int)len, p);
    if (!has_id(list, id)) snprintf(list + strlen(list), n - strlen(list), "%s%s", *list ? "," : "", id);
    p += len + (p[len] == ',');
  }
}

/* Whether a run of these tests and modes measures item i (the engine runs every test in every chosen mode). */
static int item_measured(size_t i, const char *tests, const char *modes, int isa) {
  if (!has_id(modes, ITEMS[i].mode) || (ITEMS[i].isa && !isa)) return 0;
  for (const char *p = ITEMS[i].kernels; *p;) {
    char id[32];
    size_t len = strcspn(p, ",");
    snprintf(id, sizeof id, "%.*s", (int)len, p);
    if (has_id(tests, id)) return 1;
    p += len + (p[len] == ',');
  }
  return 0;
}

static void menu_choose(const char *k1) {
  char buf[256];
  int number[NITEMS], chosen[NITEMS];
  int shown = 0;
  memset(number, 0, sizeof number);
  printf("\n%sChoose what to run%s\n", o(BOLD), o(RESET));
  for (size_t c = 0; c < NCATEGORIES; c++) {
    printf("\n  %s%s%s  %s%s%s\n", o(BOLD), CATEGORIES[c].name, o(RESET), o(DIM), CATEGORIES[c].about, o(RESET));
    for (size_t i = 0; i < NITEMS; i++) {
      if (ITEMS[i].cat != (int)c) continue;
      if (ITEMS[i].needs_k1 && !k1) continue; /* not built in, or the compile-test setup is missing */
      number[i] = ++shown;
      printf("  %2d  %s\n", shown, ITEMS[i].name);
    }
  }
  if (!k1) printf("\n  %sCompiling code and Full software build need the compile-test setup.%s\n", o(DIM), o(RESET));

  /* The numbers in front of the items; empty: everything. */
  int any;
  for (;;) {
    if (ask("\nWhich tests? Their numbers, e.g. 6 7 (Enter: everything): ", buf, sizeof buf)) return;
    memset(chosen, 0, sizeof chosen);
    any = 0;
    const char *in = buf;
    int ok = 1;
    while (ok && *in) {
      in += strspn(in, ", ");
      size_t tok = strcspn(in, ", ");
      if (!tok) break;
      char *end;
      long num = strtol(in, &end, 10);
      int matched = 0;
      for (size_t i = 0; end == in + tok && i < NITEMS; i++)
        if (number[i] && number[i] == num) chosen[i] = matched = 1;
      if (!matched) {
        printf("  %sChoose numbers from 1 to %d; '%.*s' is not one.%s\n", o(YELLOW), shown, (int)tok, in, o(RESET));
        ok = 0;
      }
      any |= matched;
      in += tok;
    }
    if (ok) break;
  }

  char tests[128] = "", modes[128] = "";
  int isa = !any; /* everything includes the new-instruction gain */
  for (size_t i = 0; i < NITEMS && any; i++)
    if (chosen[i]) {
      add_ids(tests, sizeof tests, ITEMS[i].kernels);
      add_ids(modes, sizeof modes, ITEMS[i].mode);
      isa |= ITEMS[i].isa;
    }

  int quick = ask_yn("\nQuick run, a few minutes and not for comparison? [y/N] ", 0);
  if (quick < 0) return;
  printf("\n%s%s%s%s\n", o(BOLD), quick ? "Quick run" : "Full run", o(RESET), any ? " of:" : " of everything.");
  int extra = 0;
  for (size_t i = 0; i < NITEMS && any; i++)
    if (chosen[i]) printf("  %s · %s\n", CATEGORIES[ITEMS[i].cat].name, ITEMS[i].name);
    else extra |= number[i] && item_measured(i, tests, modes, isa);
  if (extra) { /* one run measures every chosen test in every chosen mode */
    printf("%sAlso measured, because the chosen tests share these modes:%s\n", o(DIM), o(RESET));
    for (size_t i = 0; i < NITEMS; i++)
      if (!chosen[i] && number[i] && item_measured(i, tests, modes, isa))
        printf("  %s%s · %s%s\n", o(DIM), CATEGORIES[ITEMS[i].cat].name, ITEMS[i].name, o(RESET));
  }
  printf("For steady results, close other programs and leave the computer alone while it runs.\n"
         "Some tests are skipped if the keyboard or mouse is used during them.\n");
  if (ask_yn("Start now? [Y/n] ", 1) == 1) {
    menu_run(quick, tests, modes, k1, isa);
    pause_for_enter();
  }
}

/* Lists the runs numbered; returns how many (v is malloc'd). */
static size_t menu_runs(run_entry **v) {
  char dir[1100];
  if (results_dir(dir, sizeof dir, 0)) snprintf(dir, sizeof dir, ".");
  size_t n = scan_runs(dir, v);
  if (!n) {
    printf("\nNo runs yet. Start one from the menu.\n");
    return 0;
  }
  printf("\n%s   #  %-10s %-17s %-6s %-11s %s%s\n", o(DIM), "run", "started (UTC)", "kind", "state", "CPU", o(RESET));
  for (size_t i = 0; i < n; i++) {
    const pmk_brief *b = &(*v)[i].b;
    printf("  %s%2zu%s  %-10.8s %.10s %-6.5s %-6s %-11s %.40s\n", o(BOLD), i + 1, o(RESET), b->run_id, b->started_utc,
           strlen(b->started_utc) > 11 ? b->started_utc + 11 : "", b->quick == 1 ? "quick" : b->quick == 0 ? "full" : "?",
           b->complete ? "complete" : "incomplete", b->model);
  }
  return n;
}

/* A run number from the list just printed, asked again until valid; -1 when the answer is empty (back). */
static long ask_run(const char *prompt, size_t n) {
  char buf[32];
  for (;;) {
    if (ask(prompt, buf, sizeof buf) || !buf[0]) return -1;
    char *end;
    long k = strtol(buf, &end, 10);
    if (!*end && k >= 1 && (size_t)k <= n) return k - 1;
    printf("  %sChoose a number from 1 to %zu, or press Enter to go back.%s\n", o(YELLOW), n, o(RESET));
  }
}

static void menu_past(void) {
  for (;;) {
    run_entry *v = NULL;
    size_t n = menu_runs(&v);
    long k = n ? ask_run("\nShow which run? (number, Enter: back) ", n) : -1;
    if (k < 0) {
      free(v);
      return;
    }
    char *args[] = {"prismark", "show", v[k].path, NULL};
    printf("\n");
    cmd_show(3, args);
    free(v);
    pause_for_enter();
  }
}

static void menu_compare(void) {
  run_entry *v = NULL;
  size_t n = menu_runs(&v);
  if (!n) {
    free(v);
    return;
  }
  static ref_list refs;
  size_t nr = scan_refs(&refs);
  if (nr) {
    printf("\n%s   #  %-28s %-16s %s%s\n", o(DIM), "reference system", "kind", "system", o(RESET));
    for (size_t i = 0; i < nr; i++)
      printf("  %s%2zu%s  %-28s %-16s %.40s\n", o(BOLD), n + i + 1, o(RESET), refs.v[i].id, refs.v[i].kind,
             refs.v[i].name);
  }
  long a = ask_run("\nCompare which run? (number, Enter: back) ", n);
  long b = a < 0 ? -1 : ask_run("With which run or reference system? (number) ", n + nr);
  if (a >= 0 && b >= 0) {
    char *args[] = {"prismark", "compare", v[a].path, (size_t)b < n ? v[b].path : refs.v[b - n].path, NULL};
    printf("\n");
    cmd_compare(4, args);
    pause_for_enter();
  }
  free(v);
}

static int cmd_menu(void) {
  char *info = NULL, model[160] = "this machine", k1[1200];
  if (pmk_info(&info) == PMK_OK && info) {
    const char *p = strstr(info, "\"model\":\"");
    if (p) snprintf(model, sizeof model, "%.*s", (int)strcspn(p + 9, "\""), p + 9);
  }
  int k1_built = info && strstr(info, "\"k1_built\":true");
  pmk_free(info);
  int have_k1 = k1_built && k1_snapshot(k1, sizeof k1) == 0;

  for (;;) {
    printf("\n%sPrismark %s%s  %s\n", o(BOLD), pmk_version(), o(RESET), model);
    if (!have_k1)
      printf("%sCompiling code and Full software build are not available: %s.%s\n", o(DIM),
             k1_built ? "run the compile-test setup in the desktop app (or tools/k1x/prepare.py)"
                      : "this build has no Clang libraries",
             o(RESET));
    printf("\n"
           "  %s1%s  Full run          every test, about 30 minutes on a 4-core laptop\n"
           "  %s2%s  Quick run         a few minutes; a smoke test, not for comparison\n"
           "  %s3%s  Choose tests      pick tests and modes\n"
           "  %s4%s  Past runs         list them and show a summary\n"
           "  %s5%s  Compare           a run with another run or a reference system\n"
           "  %s6%s  About the tests   what each test and mode measures\n"
           "  %sq%s  Quit\n\n",
           o(BOLD), o(RESET), o(BOLD), o(RESET), o(BOLD), o(RESET), o(BOLD), o(RESET), o(BOLD), o(RESET), o(BOLD),
           o(RESET), o(BOLD), o(RESET));
    char buf[32];
    int c;
    for (;;) {
      if (ask("Choice: ", buf, sizeof buf)) return 0;
      c = tolower((unsigned char)buf[0]);
      if ((c >= '1' && c <= '6' && !buf[1]) || c == 'q' || !c) break;
      printf("  %sChoose 1 to 6, or q.%s\n", o(YELLOW), o(RESET));
    }
    switch (c) {
      case '1':
        if (confirm_start(0, "every test, about 30 minutes")) {
          menu_run(0, NULL, NULL, have_k1 ? k1 : NULL, 1);
          pause_for_enter();
        }
        break;
      case '2':
        if (confirm_start(1, "every test, a few minutes")) {
          menu_run(1, NULL, NULL, have_k1 ? k1 : NULL, 1);
          pause_for_enter();
        }
        break;
      case '3': menu_choose(have_k1 ? k1 : NULL); break;
      case '4': menu_past(); break;
      case '5': menu_compare(); break;
      case '6':
        printf("\n");
        cmd_tests();
        pause_for_enter();
        break;
      case 'q':
      default: return 0;
    }
  }
}

int main(int argc, char **argv) {
  int no_color = 0;
  for (int i = 1; i < argc; i++) no_color |= !strcmp(argv[i], "--no-color") || !strcmp(argv[i], "--no-colour");
  term_init(no_color);

  const char *cmd = argc > 1 ? argv[1] : "";
  if (argc == 1) { /* no automatic run: a menu on a terminal */
    if (isatty(fileno(stdin)) && isatty(fileno(stdout))) return cmd_menu();
    fprintf(stderr, "prismark: nothing to do. 'prismark run' starts a full run; 'prismark help' lists the options.\n");
    return 2;
  }
  if (!strcmp(cmd, "run")) {
    for (int i = 2; i < argc; i++)
      if (is_help(argv[i])) return help_cmd(stdout, ""), 0;
    return cmd_run(argc - 1, argv + 1);
  }
  if (!strcmp(cmd, "help") || is_help(cmd)) {
    help_cmd(stdout, argc > 2 && !is_help(cmd) ? argv[2] : "");
    return 0;
  }
  if (!strcmp(cmd, "--version")) {
    printf("prismark %s\n", pmk_version());
    return 0;
  }
  if (!strcmp(cmd, "tests")) return argc > 2 && is_help(argv[2]) ? (help_cmd(stdout, cmd), 0) : cmd_tests();
  if (!strcmp(cmd, "list")) return cmd_list(argc, argv);
  if (!strcmp(cmd, "show")) return cmd_show(argc, argv);
  if (!strcmp(cmd, "compare")) return cmd_compare(argc, argv);
  if (!strcmp(cmd, "references") || !strcmp(cmd, "refs")) return cmd_references(argc, argv);
  if (!strcmp(cmd, "checksums")) return cmd_checksums(argc, argv);
  if (!strcmp(cmd, "info") || !strcmp(cmd, "profiles")) {
    if (argc > 2 && is_help(argv[2])) return help_cmd(stdout, cmd), 0;
    char *out = NULL;
    int rc = PMK_OK;
    if (!strcmp(cmd, "info")) rc = pmk_info(&out);
    else out = pmk_default_profiles();
    if (out) fputs(out, stdout);
    if (out && out[0] && out[strlen(out) - 1] != '\n') fputc('\n', stdout);
    pmk_free(out);
    return rc == PMK_OK ? 0 : 1;
  }
  for (int i = 1; i < argc; i++)
    if (is_help(argv[i])) return help_cmd(stdout, ""), 0;
  return cmd_run(argc, argv);
}
