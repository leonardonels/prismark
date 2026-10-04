# Prismark

Prismark is a CPU benchmark for x86-64 and ARM64 processors. It characterises
processor performance through a set of independent measurement modes rather than
a single composite score. All results are derived from raw samples, and every
derived quantity compares identical kernels built from identical sources.

The full design is in [docs/spec.md](docs/spec.md).

## Measurement modes

| Mode | Measured quantity | Kernels |
| --- | --- | --- |
| Single-thread burst | Execution time of short interactive workloads on one core | K3, K4, K5, K6 |
| Single-thread sustained | Single-core throughput at thermal and power steady state | K1, K2 |
| Multi-thread | Scaling of multithreaded workloads, including serial fractions | K1, K1x, K2 |
| Multi-instance | Aggregate throughput of independent instances; memory latency under contention | K2, K7 |
| Cold start | Latency penalty of work issued from idle, on the machine as configured | K9; K4, K6 |
| Periodic | Wake-up latency distribution of periodic tasks | K10 |

ISA uplift (K2, K3, K4, K8 at the baseline and the max-level ISA) runs under
single-thread sustained conditions.

## Kernels

| ID | Kernel | Source |
| --- | --- | --- |
| K1 | In-process compile: Clang as a library, aarch64 target, in-memory files | `kernels/k1_compile` (needs Clang dev package) |
| K1x | Full build of the same units with CMake + Ninja | `src/engine/k1x.c`, snapshot from `tools/k1x/prepare.py` |
| K2 | Path tracer | `kernels/k2_render` |
| K3 | zstd 1.5.6, level 3, self-generated corpus | `kernels/k3_compress` |
| K4 | JPEG decode (stb_image, SIMD off) and triangle-filter resize | `kernels/k4_image` |
| K5 | Validating JSON parser, exact number handling | `kernels/k5_json` |
| K6 | Lua 5.4.7: new state, load and run a script | `kernels/k6_lua` |
| K7 | Pointer chase over L1-to-DRAM working sets | `kernels/k7_chase` |
| K8 | Matrix multiply, FP32 and INT8 | `kernels/k8_matmul` |
| K9 | Calibrated dependent-add loop | `kernels/k9_calib` |
| K10 | Periodic timer | `src/engine/mode_periodic.c` |

Every input is generated from fixed seeds or fetched by hash, so every host does
identical work. Kernels are built without FP contraction and without libm
transcendentals in their outputs, and their output checksums are bit-identical
across ISA tiers and compilers (`prismark checksums`; CI compares x86-64 against
ARM64).

## Methodology

- Results are reported per kernel and per mode. Measurements from different
  modes are not combined.
- Derived metrics (scaling S/E/p, throttling ratio, build overhead R_build,
  serialisation cost R_serial, instruction-set uplift U_ISA) compare the same
  kernel and the same binary, with bootstrap 95 % intervals.
- Sustained runs sample perf(t) in windows and stop at steady state: t > 5τ
  (τ fitted to the temperature trace) and no significant slope over the last
  ten windows, or at a duration cap, recorded either way.
- Baseline builds target a common instruction-set level (x86-64-v2, Armv8.2-A)
  and contain no vendor-specific intrinsics. Max-level tiers (x86-64-v3/v4,
  Armv8.2-A+dotprod, Armv9-A+SVE2) are separate modules selected at runtime.
- Each result records the machine state: frequency governor, power mode,
  idle-state configuration, temperature and idle power. Prismark never
  changes any of it: every test measures the machine as it is configured.
- Tests that a platform cannot execute are listed as unavailable, not estimated.

## Status

| Phase | Content | State |
| --- | --- | --- |
| 1 | Linux core, all six modes, K2, K3, K9, K10 | implemented; gate 1 needs repeat runs on an x86-64 and an ARM64 machine |
| 2 | K1, K1x, K4–K8, statistics in the core, analyzer | implemented; K1 and K1x untested (need Clang dev libraries and a snapshot) |
| 3 | Windows PAL, CLI | written, not yet compiled or run on Windows |
| 4 | Android (JNI bridge), GUI | JNI bridge and Kotlin wrapper written, untested; desktop app (Qt) working on Linux |
| 5 | macOS / iOS PAL, Swift wrapper | written, not yet compiled or run on Apple platforms |

On Linux x86-64 everything except K1/K1x has been run end to end. The gates in
the spec are still to be passed.

## Building

Requires CMake ≥ 3.25, Ninja, network access on first configure (zstd, Lua and
stb are fetched at pinned versions and verified by SHA-256), and the pinned
upstream Clang (currently 19):

```sh
wget https://apt.llvm.org/llvm.sh && sudo bash llvm.sh 19   # Ubuntu, x86-64 or arm64
cmake --preset linux-clang
cmake --build --preset linux-clang
ctest --preset linux-clang
```

For development with any other compiler, use the `dev` preset instead
(`cmake --preset dev`); results from such builds are not comparable to
official ones. Other presets: `windows-clang`, `macos-clang`, `android-arm64`
(needs `ANDROID_NDK`), `ios-arm64` (static libraries for `bindings/swift`).

K1 is built when the Clang development package is found (`libclang-19-dev`
and `llvm-19-dev`, or `-DPRISMARK_K1_LLVM_DIR=<prefix>`); otherwise results
list it as unavailable.

## Running

```sh
./build/linux-clang/prismark --quick          # smoke run, a few minutes; not for comparison
./build/linux-clang/prismark                  # full run of all modes (long: sustained runs reach steady state)
./build/linux-clang/prismark --mode st_burst,cold_burst --kernels K4,K6,K9
./build/linux-clang/prismark --help
```

Each run writes `prismark-<run_id>.json` with every raw sample, the machine
state at start and end, and the statistics computed from them.

```sh
prismark compare a.json b.json                # per-kernel ratios with CIs, and the profiles
prismark compare a.json b.json --profiles my-profiles.json
prismark profiles                             # the default profiles (Daily, Dev, Render, Realtime)
prismark checksums -o checksums.json          # kernel output checksums, no timing
```

`compare` refuses runs measured under different capabilities, and reports a
profile only when every kernel it names exists in both runs.

Prismark measures the machine as it is configured and changes nothing on it:
no power settings, no frequency limits, no idle states. It needs no root
rights. What it finds (governor, power mode, idle states, temperatures) is
recorded with the result.

### K1 and K1x

Both use a prepared snapshot of a pinned LLVM subset (generated sources
included, so no host tool runs during the build) cross-compiled to aarch64:

```sh
tools/k1x/prepare.py                          # pinned sysroot downloaded; output in ~/.local/share/prismark/k1x
./build/linux-clang/prismark --k1-data ~/.local/share/prismark/k1x
```

The script checks the granularity rule (Σ T_i / T_max ≥ 4 n_max) and prints
the snapshot's statistics.

## Desktop app

`prismark-gui` is the desktop front-end: launch it, pick a test group at the
top, and press ▶ Run on a test card or "Run all tests"; each asks for a
**quick run** (minutes, a first look, not comparable) or a **full run**
(accurate, comparable; long). Each test is ranked on
its own, with your latest run highlighted against reference systems; the
details panel shows the machine state, capabilities, responsiveness curve and
scaling.

```sh
./build/linux-clang/prismark-gui              # or: prismark-gui --run [TEST] [--full], e.g. --run mc_k2
```

- Built automatically when Qt Widgets is found (Qt 6, or Qt 5.15:
  `sudo apt install qt6-base-dev` or `qtbase5-dev`); `-DPRISMARK_GUI=OFF`
  skips it. The Inter typeface is fetched at a pinned version and compiled in.
- **Isolation.** Runs execute in the `prismark` runner next to the app, as a
  separate process. During a run the main window closes and only a small
  measuring window remains, static except when a new phase begins (spec 7.1);
  closing it cancels the run (the partial result is kept).
- **Quick runs** use short series and, for Compile and Full build, every 8th
  compile unit (the same units for both); they are recorded as such and never
  compared with full runs.
- **As configured.** The app never asks for administrator rights and never
  changes power settings: every test measures the computer as it is set up.
- **Hands off.** Cold start and periodic measure waits and wake-ups. Keyboard
  or mouse input during them skips that test: its series are discarded, the
  result lists it as skipped, and the run continues. On GNOME the app watches
  through Mutter's idle monitor (event-driven, no polling); elsewhere on Linux
  it polls the ScreenSaver idle time every 2 s; on Windows and macOS the
  runner reads the last-input time itself. The method is recorded with the
  result.
- **Compile tests.** Compile (K1) and Full build (K1x) need a one-time
  snapshot: the app checks what is missing, shows the commands to install it,
  and prepares the snapshot with progress (Menu › Prepare compile tests). On
  Ubuntu:

  ```sh
  wget https://apt.llvm.org/llvm.sh && sudo bash llvm.sh 19     # clang-19, clang++-19, lld-19
  sudo apt install cmake ninja-build libclang-19-dev llvm-19-dev  # the -dev packages build K1 in
  cmake --preset linux-clang && cmake --build --preset linux-clang
  ```

- Every run is saved in `~/.local/share/prismark/results` (Windows:
  `%LOCALAPPDATA%\prismark`, macOS: `~/Library/Application Support/prismark`)
  and reloaded at start; the compile-test snapshot lives next to it in `k1x/`.
  Menu › Open result imports files; Menu › Compare runs `prismark compare`.
- The built-in reference systems are **placeholders**, not measurements, and
  are labelled as such. Put result files from real reference machines into the
  `references` folder next to `results` to add them.

`apps/viewer` is the static web page for cross-device comparison (spec 7.3):
open `index.html` and load or drop result files.

## Analyzer

```sh
cd analyzer
uv run prismark-analyze plot ../prismark-<id>.json      # responsiveness, scaling, latency, perf(t), jitter
uv run prismark-analyze check ../prismark-<id>.json     # recompute the core's medians from raw samples
uv run prismark-analyze table run1.json run2.json       # CSV of medians across runs
```

## Repository layout

```
include/prismark/prismark.h   public C ABI (start, cancel, progress events, compare, checksums)
src/engine/                   runner, modes, sustained-run engine, K1x driver
src/stats/                    medians, bootstrap, steady state, analysis, compare and profiles
src/result/                   JSON writer and reader
pal/                          platform layers: linux, darwin, windows, posix, common (ISA detection)
kernels/                      one directory per kernel; compiled once per ISA tier
apps/cli, apps/gui            command-line runner, desktop app
apps/viewer                   static web viewer
bindings/android, bindings/swift   JNI bridge and Kotlin wrapper; Swift package
tools/k1x, tools/ci           K1x snapshot preparation, checksum gate
analyzer/                     Python research tools
```

## License

Prismark is distributed under the Apache License 2.0. Results produced by
modified builds are marked as unverified.
