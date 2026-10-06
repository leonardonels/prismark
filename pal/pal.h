/*
 * Platform layer: everything that differs per OS. The modes call only these
 * functions, and every capability the layer lacks is reported, never faked.
 *
 * Implementations: pal/linux (Linux, Android), pal/darwin (macOS, iOS,
 * iPadOS), pal/windows. pal/posix holds what the POSIX systems share and
 * pal/common/isa.c the ISA-level detection.
 *
 * Copyright 2026 The Prismark Authors. Apache-2.0.
 */
#ifndef PMK_PAL_H
#define PMK_PAL_H

#include <stddef.h>
#include <stdint.h>

#include "json.h"
#include "kernels.h"

#define PMK_MAX_TYPES 4

typedef struct pmk_cpu {
  int id;
  int type;    /* index into pmk_machine.type_names; 0 is the fastest type */
  int max_khz; /* 0 if unknown */
  int min_khz; /* hardware minimum; 0 if unknown */
  int core;    /* physical core; SMT siblings share it. -1 if unknown */
  int smt;     /* 0 for the first hardware thread of its core */
} pmk_cpu;

typedef struct pmk_caps {
  int pinning;       /* threads can be pinned to a CPU */
  int perf_counters; /* user-space cycle counter available */
  int timer_slack;   /* timer slack can be reduced */
  int qos_only;      /* no pinning; placement through QoS classes, core type inferred */
  char power[32];    /* power telemetry source, "" if none */
} pmk_caps;

typedef struct pmk_machine {
  char os[16];
  char kernel[128];
  char isa[16];
  char model[160];
  char board[160];
  int ncpu;        /* online CPUs */
  pmk_cpu *cpus;
  int ntypes;
  char type_names[PMK_MAX_TYPES][4];
  uint64_t llc_bytes;
  pmk_caps caps;
} pmk_machine;

int pal_init(pmk_machine *m);
void pal_fini(pmk_machine *m);

uint64_t pal_now_ns(void);            /* monotonic clock: deadlines and sleeps */
uint64_t pal_now_raw_ns(void);        /* raw monotonic clock: durations, immune to clock slewing */
void pal_sleep_until_ns(uint64_t t);  /* absolute, monotonic clock */
void pal_sleep_ns(uint64_t ns);
int pal_pin_self(int cpu);            /* 0 on success; with qos_only, requests the CPU's core type instead */
int pal_unpin_self(void);
int pal_pin_self_set(const int *cpus, int n); /* allow exactly these CPUs; children inherit the set */
int pal_current_cpu(void);            /* -1 if unknown; with qos_only, the inferred type index */
int pal_set_timer_slack_min(void);
/* Milliseconds since the last keyboard or mouse input anywhere in the session; -1 if unknown. */
int64_t pal_input_idle_ms(void);
/* Name of the mechanism behind pal_input_idle_ms, or NULL when there is none. */
const char *pal_input_source(void);
int pal_random_bytes(void *p, size_t n);

double pal_cpu_temp_c(void);                /* NaN if unknown */
double pal_background_load(double seconds); /* busy fraction of all CPUs, NaN on error */
double pal_idle_power_w(double seconds);    /* NaN without telemetry */

/* Machine state (governors, idle states, temperatures, power) as a JSON object. */
void pal_state_json(pmk_jw *w, const char *key, const pmk_machine *m, int measure_power);


/* User-space cycle counter for the calling thread; -1 if unavailable. */
int pal_cycles_open(void);
uint64_t pal_cycles_read(int fd);
void pal_cycles_close(int fd);

/* ---------- threads ---------- */

typedef struct pal_thread pal_thread;
pal_thread *pal_thread_start(void *(*fn)(void *), void *arg); /* NULL on failure */
void pal_thread_join(pal_thread *t);

/* A mutex with one condition variable. */
typedef struct pal_lock pal_lock;
pal_lock *pal_lock_new(void);
void pal_lock_free(pal_lock *l);
void pal_lock_acquire(pal_lock *l);
void pal_lock_release(pal_lock *l);
void pal_lock_wait(pal_lock *l); /* releases, waits for a broadcast, reacquires; may wake spuriously */
void pal_lock_broadcast(pal_lock *l);

void *pal_aligned_alloc(size_t align, size_t n);
void pal_aligned_free(void *p);

/* ---------- ISA tiers ---------- */

/* True when the CPU and OS can run code built for an ISA level such as "x86-64-v3". */
int pal_isa_supported(const char *level);
/*
 * Loads the max-level tier module prismark-kernels-<tier> from the directory
 * of the core library (or $PRISMARK_KERNEL_DIR). NULL with a reason in err.
 * Modules stay loaded until the process exits.
 */
const pmk_kernels *pal_load_tier(const char *tier, char *err, size_t errlen);

/* ---------- processes and files (K1x) ---------- */

/*
 * Runs argv[0] (searched in PATH) in cwd with stdout and stderr appended to
 * log_path, and waits. Polls cancelled() and terminates the child when it
 * returns true. Returns the exit status, or -1 if the process could not run.
 */
int pal_run(const char *const *argv, const char *cwd, const char *log_path, int (*cancelled)(void));
/*
 * Memory a new allocation can get without swapping, in bytes (Linux: MemAvailable, after returning the
 * process's own unused heap to the OS); 0 when the OS does not say.
 */
uint64_t pal_mem_available(void);
/* Current clock of a CPU in kHz as the OS reports it (averaged over a short interval); 0 if unknown. */
int pal_cpu_cur_khz(int cpu);
/* A scratch directory for build trees, RAM-backed where the OS has one; 0 on success. */
int pal_scratch_dir(char *out, size_t n, int *ram_backed);
int pal_copy_tree(const char *src, const char *dst);
int pal_remove_tree(const char *path);

#endif
