# Measuring

[README](../README.md) · [Measuring](measuring.md) · [Command line](command-line.md) · [Desktop app](desktop-app.md) · [Building](building.md) · [Packaging](packaging.md) · [Development](development.md) · [Design](design.md)

What Prismark runs, in which modes, and how it turns samples into results.

## Tests

The desktop app names every test by what it does. The command line
(`--kernels`), result files and source folders use a short kernel ID instead.
These pages use the app's names, with the ID in brackets where it helps.

| Test (as in the app) | ID | What it runs | Source |
| --- | --- | --- | --- |
| Compiling code | K1 | In-process compile: Clang as a library, aarch64 target, in-memory files | `kernels/k1_compile`; needs the [compile-test setup](building.md#compile-tests-setup) |
| Full software build | K1x | Build of every 8th unit (73 of 577) with CMake + Ninja, all threads, three times | `src/engine/k1x.c`; needs the [compile-test setup](building.md#compile-tests-setup) |
| 3D rendering | K2 | Path tracer | `kernels/k2_render` |
| Compression | K3 | zstd 1.5.6, level 3, self-generated corpus | `kernels/k3_compress` |
| Opening a photo | K4 | JPEG decode (stb_image, SIMD off) and triangle-filter resize | `kernels/k4_image` |
| Reading JSON | K5 | Validating JSON parser, exact number handling | `kernels/k5_json` |
| Starting a script | K6 | Lua 5.4.7: new state, load and run a script | `kernels/k6_lua` |
| Memory delay | K7 | Pointer chase over L1-to-DRAM working sets | `kernels/k7_chase` |
| Wake-up test | K9 | Calibrated dependent-add loop | `kernels/k9_calib` |
| Timer punctuality | K10 | Periodic timer | `src/engine/mode_periodic.c` |
| Memory bandwidth | K11 | Most of the free memory written and checked on every core, four patterns (stress test only) | `kernels/k11_stream` |
| Vector maths | K12 | Double-precision matrix multiply with fused multiply-adds at the widest vectors (AVX-512, AVX2, NEON), GFLOP/s | `kernels/k12_vector` |

## Measurement modes

Each test runs in one or more modes. The app groups them into the tabs
All cores, One core, Memory & timing, Responsiveness and Stress test; the
command line selects modes by ID (`--mode`).

| Mode | Shown in the app as | CLI ID | Measured quantity | Tests |
| --- | --- | --- | --- | --- |
| Single-thread burst | one core, short task | `st_burst` | Execution time of short interactive workloads on one core | Compression, Opening a photo, Reading JSON, Starting a script |
| Single-thread sustained | one core, after warming up | `st_sustained` | Single-core throughput after a warm-up at full load | Compiling code, 3D rendering, Vector maths |
| Multi-thread | all cores, working together | `mc_threaded` | Scaling of multithreaded workloads, including serial fractions | Compiling code, Full software build, 3D rendering, Vector maths |
| Multi-instance | all cores, each on its own | `mc_instances` | Memory latency while every core uses memory | Memory delay |
| Cold start | started from rest | `cold_burst` | Latency penalty of work issued from idle, on the machine as configured | Wake-up test; Opening a photo, Starting a script |
| Periodic | timer punctuality | `periodic` | Wake-up latency distribution of periodic tasks | Timer punctuality |
| Stress | stress test | `stress` | Speed, clock, temperature and correctness over 10 minutes or more of full load | Vector maths (one core, all cores, each core in turn), Memory bandwidth |

The stress test is not part of a full run: it runs only when asked for (its
cards in the app, or `--mode stress`). Each part runs for 10 minutes, 30
minutes or until stopped; its score is the speed in the 10th minute, so runs
of every length compare, and a run stopped before 10 minutes keeps its curve
and verdict without a score. Like OCCT, it is a stability test as much as a
benchmark:

- Its CPU load is Vector maths, wide fused multiply-adds: the hottest load a
  processor gets. *Each core in turn* runs it on one physical core at a time,
  moving on every 10 s (core cycling), so every core reaches its own
  single-core boost, where per-core instability shows; its score is the
  slowest core.
- The memory part is a memory test: about three quarters of the free memory,
  checked word by word with four patterns in turn.
- Every job is checked against the result a correct machine produces (known in
  advance: outputs are identical on every host) and every task against its own
  first result, so a wrong result is counted against the CPU that computed it.
  The test stops at the first one (`--stress-on-error continue` keeps going)
  or at a temperature limit (`--stress-max-temp 95`), and ends with a verdict:
  stable, failed (when, on which CPU) or stopped hot. The command line exits
  with status 4 when it failed.

Vector maths is also a benchmark (One core, All cores) with an absolute score
in GFLOP/s. It runs at the widest vector instructions the processor has,
picked at run time; every width computes bit-identical results, and each
result records which one ran (`vector_isa`).

Every input is generated from fixed seeds or fetched by hash, so every host does
identical work. Kernels are built without FP contraction and without libm
transcendentals in their outputs, and their output checksums are bit-identical
across architectures and compilers (`prismark checksums`; CI compares x86-64
against ARM64).

## Methodology

- Results are reported per kernel and per mode. Measurements from different
  modes are not combined.
- Derived metrics (scaling S/E/p, throttling ratio, build overhead R_build,
  stress hold-up) compare the same kernel and the same binary, with bootstrap
  95 % intervals.
- Sustained runs are fixed in length, so a run takes the same time on every
  machine of a kind and includes the slowdown sustained use causes. In a full
  run each sustained mode starts with a 60 s warm-up (3D rendering at the
  mode's full load: one core, or every thread), then every series runs 5 s
  unscored and is measured for 20 s (Compiling code: 60 s) in 1 s windows
  (5 s for Compiling code); the score is the median window. The warm-up is
  recorded: its end against its first seconds is the "when hot" slowdown.
  Quick runs skip the warm-up and measure 3 s. Each window also records the
  clock of the CPUs in use: windows below the CPU's own hardware minimum (or
  15 % of its maximum where that is unknown) are throttling forced from
  outside the processor, such as laptop firmware asserting BD PROCHOT. They
  are counted and reported ("throttled"), and the score keeps them. Measured on a 4-core laptop,
  60 s covers the turbo budget (about 28 s on Intel laptops) and the thermal
  time constant (2–20 s). Short tasks, responsiveness, memory and timer
  tests have no warm-up.
- A full run takes about 30 minutes on a 4-core laptop (i5-1035G1), less on
  faster machines.
- Everything is built once, for a common instruction-set level (x86-64-v2,
  Armv8.2-A), with no vendor-specific intrinsics: every machine of an
  architecture runs the same instructions.
- Each result records the machine state: frequency governor, power mode,
  idle-state configuration, temperature and idle power. Prismark never
  changes any of it: every test measures the machine as it is configured.
- Tests that a platform cannot execute are listed as unavailable, not estimated.
