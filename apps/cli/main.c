/*
 * prismark — command-line front-end. Starts a run through the C ABI, prints
 * progress between measurement windows, writes the result JSON and prints the
 * summary computed by the core. Also compares two results and prints kernel
 * checksums.
 *
 *   prismark [run options]
 *   prismark compare A.json B.json [--profiles FILE] [-o report.json]
 *   prismark checksums [--k1-data DIR] [-o checksums.json]
 *   prismark profiles
 *   prismark info                                 what this build can run here (JSON)
 *
 * Copyright 2026 The Prismark Authors. Apache-2.0.
 */
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "prismark/prismark.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

static void usage(FILE *f) {
  fprintf(f,
          "usage: prismark [options]                      run the benchmark\n"
          "       prismark compare A.json B.json [--profiles FILE] [-o FILE]\n"
          "       prismark checksums [--k1-data DIR] [--kernels LIST] [--burst-only] [-o FILE]\n"
          "       prismark profiles                       print the default profiles\n"
          "       prismark info                           what this build can run on this machine (JSON)\n"
          "\nrun options:\n"
          "  --mode LIST            comma-separated: st_burst, st_sustained, mc_threaded, mc_instances,\n"
          "                         cold_burst, periodic (default: all)\n"
          "  --kernels LIST         only these kernels, e.g. K2,K3,K9 (default: all)\n"
          "  --cpu N                run single-core work on CPU N (default: one per core type)\n"
          "  --max-threads N        largest n in the multi-core modes (default: all CPUs)\n"
          "  --seed N               experiment seed (default: random, recorded)\n"
          "  --no-isa-uplift        skip the max-level tier runs of K2, K3, K4, K8\n"
          "  --min-reps N           minimum repetitions per series (default 20)\n"
          "  --max-reps N           maximum repetitions per warm series (default 100)\n"
          "  --cold-max-reps N      maximum repetitions per cold-burst series (default 1000)\n"
          "  --window-ms X          perf(t) window of sustained runs (default 1000)\n"
          "  --sustained-min S      shortest sustained run, seconds (default 30)\n"
          "  --sustained-max S      longest sustained run without steady state (default 600)\n"
          "  --no-cooldown          do not wait for the idle temperature before sustained runs\n"
          "  --period-ms X          periodic mode period (default 1)\n"
          "  --periodic-seconds X   duration of each periodic condition (default 60)\n"
          "  --k1-data DIR          prepared K1/K1x snapshot (tools/k1x/prepare.py)\n"
          "  --k1x-reps N           K1x builds per thread count (default 3)\n"
          "  --quick                smoke run: short series and sustained runs (not for comparison)\n"
          "  --skip-preflight       skip the background-load check and temperature wait\n"
          "  --max-load X           preflight background-load threshold, fraction (default 0.05)\n"
          "  --frontend NAME        recorded front-end kind (default cli; the GUI passes gui)\n"
          "  --progress-json        progress events as JSON lines on stderr (for GUIs)\n"
          "  --cancel-on-stdin      read GUI commands on stdin: \"cancel\" (also on close), \"input\" (user activity)\n"
          "  --input-watch NAME     how the front-end watches for input, recorded with the result\n"

          "  -o, --output FILE      result file (default: prismark-<run_id>.json in the results folder\n"
          "                         shared with the desktop app, see below)\n"
          "  -q, --quiet            no progress output\n"
          "  --version\n"
          "\nNothing on the machine is changed: it is measured as it is configured, and its power\n"
          "settings are recorded with the result.\n"
          "\nResults are saved in the folder the desktop app reads: ~/.local/share/prismark/results/runs\n"
          "(Linux), ~/Library/Application Support/prismark/results/runs (macOS),\n"
          "%%LOCALAPPDATA%%\\prismark\\results\\runs (Windows); in the current folder when run as root.\n");
}

static void on_signal(int sig) {
  (void)sig;
  pmk_cancel();
}

enum { OUT_TEXT, OUT_JSON, OUT_QUIET };

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

static void on_event(const pmk_event *ev, void *user) {
  int mode = *(int *)user;
  if (mode == OUT_QUIET) return;
  static const char *const kinds[] = {"phase", "info", "warning"};
  if (mode == OUT_JSON) {
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
  const char *tag = ev->kind == PMK_EV_WARNING ? "warning: " : "";
  fprintf(stderr, "[%-12s]", ev->phase ? ev->phase : "");
  if (ev->steps) fprintf(stderr, " %u/%u", ev->step, ev->steps);
  fprintf(stderr, " %s%s", tag, ev->message);
  if (isfinite(ev->temp_c)) fprintf(stderr, "  (%.1f C)", ev->temp_c);
  fputc('\n', stderr);
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
  char line[64];
  while (fgets(line, sizeof line, stdin)) {
    if (!strncmp(line, "input", 5)) pmk_notify_input(); /* keyboard or mouse activity seen by the GUI */
    else if (!strncmp(line, "cancel", 6)) break;
  }
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
static int results_dir(char *out, size_t n) {
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
  return make_dirs(out);
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

static const struct {
  const char *name;
  uint32_t bit;
} MODE_NAMES[] = {
    {"cold_burst", PMK_MODE_COLD_BURST},     {"periodic", PMK_MODE_PERIODIC},
    {"st_burst", PMK_MODE_ST_BURST},         {"st_sustained", PMK_MODE_ST_SUSTAINED},
    {"mc_threaded", PMK_MODE_MC_THREADED},   {"mc_instances", PMK_MODE_MC_INSTANCES},
};

static int parse_modes(const char *s, uint32_t *out) {
  *out = 0;
  while (*s) {
    size_t len = strcspn(s, ",");
    int found = 0;
    for (size_t i = 0; i < sizeof MODE_NAMES / sizeof *MODE_NAMES; i++)
      if (strlen(MODE_NAMES[i].name) == len && !strncmp(s, MODE_NAMES[i].name, len)) {
        *out |= MODE_NAMES[i].bit;
        found = 1;
      }
    if (!found) return -1;
    s += len;
    if (*s == ',') s++;
  }
  return *out ? 0 : -1;
}

/* The run id is the only field the CLI reads back, to name the output file. */
static void run_id_of(const char *json, char *out, size_t n) {
  const char *p = strstr(json, "\"run_id\":\"");
  snprintf(out, n, "unknown");
  if (p) {
    p += 10;
    size_t len = strcspn(p, "\"");
    if (len < n) snprintf(out, n, "%.*s", (int)len, p);
  }
}

static char *read_file(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    perror(path);
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
  if (f) fclose(f);
  perror(path);
  return -1;
}

static int cmd_compare(int argc, char **argv) {
  const char *files[2] = {NULL, NULL}, *profiles = NULL, *output = NULL;
  int nf = 0;
  for (int i = 2; i < argc; i++) {
    if (!strcmp(argv[i], "--profiles") && i + 1 < argc) profiles = argv[++i];
    else if ((!strcmp(argv[i], "-o") || !strcmp(argv[i], "--output")) && i + 1 < argc) output = argv[++i];
    else if (nf < 2 && argv[i][0] != '-') files[nf++] = argv[i];
    else {
      usage(stderr);
      return 2;
    }
  }
  if (nf != 2) {
    usage(stderr);
    return 2;
  }
  char *a = read_file(files[0]), *b = read_file(files[1]), *p = profiles ? read_file(profiles) : NULL;
  if (!a || !b || (profiles && !p)) return 1;
  char *json = NULL, *text = NULL;
  int rc = pmk_compare(a, b, p, &json, &text);
  if (text) fputs(text, rc == PMK_OK ? stdout : stderr);
  if (rc == PMK_OK && output && json && write_file(output, json)) rc = PMK_ERR_SYSTEM;
  free(a);
  free(b);
  free(p);
  pmk_free(json);
  pmk_free(text);
  return rc == PMK_OK ? 0 : 1;
}

static int cmd_checksums(int argc, char **argv) {
  const char *k1 = NULL, *output = NULL, *kernels = NULL;
  int burst_only = 0;
  for (int i = 2; i < argc; i++) {
    if (!strcmp(argv[i], "--k1-data") && i + 1 < argc) k1 = argv[++i];
    else if (!strcmp(argv[i], "--kernels") && i + 1 < argc) kernels = argv[++i];
    else if (!strcmp(argv[i], "--burst-only")) burst_only = 1;
    else if ((!strcmp(argv[i], "-o") || !strcmp(argv[i], "--output")) && i + 1 < argc) output = argv[++i];
    else {
      usage(stderr);
      return 2;
    }
  }
  char *json = NULL, *text = NULL;
  int rc = pmk_checksums_ex(k1, kernels, burst_only, &json, &text);
  if (text) fputs(text, stdout);
  if (output && json && write_file(output, json)) rc = PMK_ERR_SYSTEM;
  pmk_free(json);
  pmk_free(text);
  return rc == PMK_OK ? 0 : 1;
}

int main(int argc, char **argv) {
  if (argc > 1 && !strcmp(argv[1], "compare")) return cmd_compare(argc, argv);
  if (argc > 1 && !strcmp(argv[1], "checksums")) return cmd_checksums(argc, argv);
  if (argc > 1 && !strcmp(argv[1], "info")) {
    char *info = NULL;
    int rc = pmk_info(&info);
    if (info) puts(info);
    pmk_free(info);
    return rc == PMK_OK ? 0 : 1;
  }
  if (argc > 1 && !strcmp(argv[1], "profiles")) {
    char *p = pmk_default_profiles();
    if (p) fputs(p, stdout);
    pmk_free(p);
    return 0;
  }

  pmk_config cfg;
  pmk_config_init(&cfg);
  cfg.frontend = "cli";
  cfg.ui_state = "none";
  const char *output = NULL;
  int out_mode = OUT_TEXT, cancel_on_stdin = 0;

  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    const char *v = i + 1 < argc ? argv[i + 1] : NULL;
#define NEED_ARG                                          \
  do {                                                    \
    if (!v) {                                             \
      fprintf(stderr, "prismark: %s needs a value\n", a); \
      return 2;                                           \
    }                                                     \
    i++;                                                  \
  } while (0)
    if (!strcmp(a, "--mode")) {
      NEED_ARG;
      if (parse_modes(v, &cfg.modes)) {
        fprintf(stderr, "prismark: unknown mode in '%s'\n", v);
        return 2;
      }
    } else if (!strcmp(a, "--kernels")) { NEED_ARG; cfg.kernels = v; }
    else if (!strcmp(a, "--cpu")) { NEED_ARG; cfg.cpu = atoi(v); }
    else if (!strcmp(a, "--max-threads")) { NEED_ARG; cfg.max_threads = atoi(v); }
    else if (!strcmp(a, "--seed")) { NEED_ARG; cfg.seed = strtoull(v, NULL, 0); }
    else if (!strcmp(a, "--no-isa-uplift")) cfg.isa_uplift = 0;
    else if (!strcmp(a, "--min-reps")) { NEED_ARG; cfg.min_reps = atoi(v); }
    else if (!strcmp(a, "--max-reps")) { NEED_ARG; cfg.max_reps = atoi(v); }
    else if (!strcmp(a, "--cold-max-reps")) { NEED_ARG; cfg.cold_max_reps = atoi(v); }
    else if (!strcmp(a, "--window-ms")) { NEED_ARG; cfg.window_ms = atof(v); }
    else if (!strcmp(a, "--sustained-min")) { NEED_ARG; cfg.sustained_min_s = atof(v); }
    else if (!strcmp(a, "--sustained-max")) { NEED_ARG; cfg.sustained_max_s = atof(v); }
    else if (!strcmp(a, "--no-cooldown")) cfg.cooldown = 0;
    else if (!strcmp(a, "--period-ms")) { NEED_ARG; cfg.periodic_period_ms = atof(v); }
    else if (!strcmp(a, "--periodic-seconds")) { NEED_ARG; cfg.periodic_seconds = atof(v); }
    else if (!strcmp(a, "--k1-data")) { NEED_ARG; cfg.k1_data = v; }
    else if (!strcmp(a, "--k1x-reps")) { NEED_ARG; cfg.k1x_reps = atoi(v); }
    else if (!strcmp(a, "--quick")) {
      cfg.max_reps = 20;
      cfg.cold_max_reps = 20;
      cfg.periodic_seconds = 10;
      cfg.window_ms = 250;
      cfg.sustained_min_s = 3;
      cfg.sustained_max_s = 6;
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
    else if (!strcmp(a, "--version")) { printf("prismark %s\n", pmk_version()); return 0; }
    else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(stdout); return 0; }
    else {
      fprintf(stderr, "prismark: unknown option '%s'\n", a);
      usage(stderr);
      return 2;
    }
#undef NEED_ARG
  }

  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);
  if (cancel_on_stdin) start_stdin_watch();

  char *json = NULL, *summary = NULL;
  int rc = pmk_start(&cfg, on_event, &out_mode, &json, &summary);
  if (rc != PMK_OK) fprintf(stderr, "prismark: %s\n", pmk_strerror(rc));

  if (json) {
    char name[1200], dir[1100], id[40];
    if (!output) {
      run_id_of(json, id, sizeof id);
      if (results_dir(dir, sizeof dir) == 0) snprintf(name, sizeof name, "%s/prismark-%s.json", dir, id);
      else snprintf(name, sizeof name, "prismark-%s.json", id);
      output = name;
    }
    if (write_file(output, json) == 0) {
      give_back(output);
      if (summary && out_mode != OUT_JSON) fputs(summary, stdout);
      printf("\nresult: %s\n", output);
    } else if (rc == PMK_OK) {
      rc = PMK_ERR_SYSTEM;
    }
  }
  pmk_free(json);
  pmk_free(summary);
  return rc == PMK_OK ? 0 : 1;
}
