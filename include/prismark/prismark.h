/*
 * Prismark — stable C interface between the core library and every front-end.
 *
 * Copyright 2026 The Prismark Authors. Licensed under the Apache License 2.0.
 *
 * The core runs the modes, computes all statistics and returns one JSON
 * document per run. Front-ends (CLI, GUI, mobile) only start runs, show
 * progress events and display what the core returns.
 */
#ifndef PRISMARK_H
#define PRISMARK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PMK_ABI_VERSION 4

/* Return codes. */
enum {
  PMK_OK = 0,
  PMK_ERR_INVALID = -1,   /* bad configuration */
  PMK_ERR_CANCELLED = -2, /* pmk_cancel() was called; a partial result is still returned */
  PMK_ERR_BUSY = -3,      /* preflight found too much background load */
  PMK_ERR_SYSTEM = -4,    /* platform layer failure */
  PMK_ERR_NOMEM = -5,
  PMK_ERR_RUNNING = -6    /* a run is already in progress in this process */
};

/* Modes (bitmask). */
enum {
  PMK_MODE_COLD_BURST = 1u << 0,
  PMK_MODE_PERIODIC = 1u << 1,
  PMK_MODE_ST_BURST = 1u << 2,
  PMK_MODE_ST_SUSTAINED = 1u << 3,
  PMK_MODE_MC_THREADED = 1u << 4,
  PMK_MODE_MC_INSTANCES = 1u << 5,
  PMK_MODE_ALL = PMK_MODE_COLD_BURST | PMK_MODE_PERIODIC | PMK_MODE_ST_BURST | PMK_MODE_ST_SUSTAINED |
                 PMK_MODE_MC_THREADED | PMK_MODE_MC_INSTANCES
};

typedef struct pmk_config {
  uint32_t struct_size;      /* set to sizeof(pmk_config); later fields are optional */
  uint32_t modes;            /* bitmask of PMK_MODE_*; 0 means all */
  int32_t cpu;               /* CPU for single-core work; -1 picks one per core type */
  uint64_t seed;             /* 0 draws a random seed; the seed used is recorded */
  int32_t reserved0;         /* was `grid` up to ABI 3; ignored. Prismark never changes power settings */
  int32_t min_reps;          /* adaptive repetition bounds for medians */
  int32_t max_reps;          /* maximum repetitions per warm series */
  double periodic_period_ms; /* K10 period */
  double periodic_seconds;   /* duration of each periodic condition */
  int32_t skip_preflight;    /* skip background-load check and temperature wait */
  double max_background_load;/* preflight abort threshold, fraction of all CPUs */
  const char *frontend;      /* "cli", "gui", ... recorded with the result */
  const char *ui_state;      /* e.g. "none", "frozen" */
  int32_t cold_max_reps;     /* maximum repetitions per cold-burst series (default 1000) */
  /* ABI 2 */
  double window_ms;          /* perf(t) window for sustained runs (default 1000) */
  double sustained_min_s;    /* sustained runs last at least this long (default 30) */
  double sustained_max_s;    /* and stop here even without steady state (default 600) */
  int32_t isa_uplift;        /* also run K2, K3, K4, K8 at the max-level tier (default 1) */
  int32_t max_threads;       /* cap for n in the multi-core modes; 0 = all CPUs */
  const char *kernels;       /* comma-separated kernel filter, e.g. "K2,K3"; NULL = all */
  const char *k1_data;       /* prepared K1/K1x snapshot directory; NULL: K1 and K1x unavailable */
  int32_t k1x_reps;          /* builds per n for K1x (default 3) */
  int32_t cooldown;          /* wait for the idle temperature before each sustained run (default 1) */
  /* ABI 3 */
  const char *input_watch;   /* how the front-end watches for keyboard/mouse input, recorded; NULL if it does not */
  int32_t quick_inputs;      /* reduced inputs for the slow compile kernels (K1, K1x): every 8th unit, for quick
                                runs; recorded as size "burst", so never compared with full runs */
} pmk_config;

/* Fills cfg with defaults. */
void pmk_config_init(pmk_config *cfg);

typedef enum pmk_event_kind {
  PMK_EV_PHASE = 0,    /* a new phase or measurement point begins */
  PMK_EV_INFO = 1,
  PMK_EV_WARNING = 2
} pmk_event_kind;

/*
 * Progress events are emitted only between measurement windows, never inside
 * one, so a front-end reacting to them cannot disturb what is being measured.
 */
typedef struct pmk_event {
  uint32_t struct_size;
  pmk_event_kind kind;
  const char *phase;   /* "preflight", a mode name, "cooldown", "done" */
  const char *kernel;  /* "K1" ... "K10", or NULL */
  const char *message; /* human-readable */
  uint32_t step, steps;/* position within the phase; steps may be 0 if unknown */
  double temp_c;       /* CPU temperature, NaN if unavailable */
} pmk_event;

typedef void (*pmk_progress_fn)(const pmk_event *ev, void *user);

/*
 * Runs the configured modes. Blocking; call from a worker thread in a GUI.
 * On return, *result_json holds the result document (also on cancellation)
 * and *summary_text a human-readable summary; free both with pmk_free.
 * Either output pointer may be NULL.
 */
int pmk_start(const pmk_config *cfg, pmk_progress_fn cb, void *user,
              char **result_json, char **summary_text);

/*
 * Compares two result documents (A against B) per kernel and computes the
 * profiles. Ratios r = perf(A) / perf(B) are formed only between series with
 * the same kernel, mode, tier, parameters and input hash, measured under the
 * same capabilities; bootstrap CIs resample both sides independently.
 * profiles_json may be NULL for the default profiles (Daily, Dev, Render,
 * Realtime). Returns PMK_ERR_INVALID when a document cannot be parsed or the
 * runs are not comparable; *report_text then explains why.
 */
int pmk_compare(const char *a_json, const char *b_json, const char *profiles_json, char **report_json,
                char **report_text);

/* The default profiles as a JSON document, editable and passable to pmk_compare. Free with pmk_free. */
char *pmk_default_profiles(void);

/*
 * Runs one job of every throughput kernel at both input sizes, at the
 * baseline tier and at the loaded max-level tier, and reports input hashes
 * and output checksums. No timing: this is the cross-ISA verification that
 * every build performs identical work (gate 2), cheap enough for CI.
 * k1_data may be NULL (K1 is then listed as unavailable).
 */
int pmk_checksums(const char *k1_data, char **report_json, char **report_text);
/* As pmk_checksums, limited to some kernels (comma-separated ids, NULL = all) and optionally to burst inputs. */
int pmk_checksums_ex(const char *k1_data, const char *kernels, int burst_only, char **report_json, char **report_text);

/*
 * What this build can run on this machine, as JSON: version, whether K1 is
 * built in, the ISA tiers (built and selectable), the platform capabilities
 * of the calling process.
 * Cheap (no measurement); front-ends call it before offering tests.
 */
int pmk_info(char **info_json);

/*
 * Reports keyboard or mouse activity (async-signal-safe). Cold burst and
 * periodic measure waits and wake-ups and need an idle machine: input during
 * either skips that mode (its series are discarded and it is listed as
 * skipped), and the run continues. Where the platform layer can read the
 * last-input time itself (Windows, macOS) it also detects input on its own.
 */
void pmk_notify_input(void);

/* Requests cancellation of the running pmk_start. Async-signal-safe. */
void pmk_cancel(void);

void pmk_free(void *p);
const char *pmk_version(void);
const char *pmk_strerror(int err);

#ifdef __cplusplus
}
#endif

#endif /* PRISMARK_H */
