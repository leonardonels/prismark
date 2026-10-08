# Prismark

Prismark is a CPU benchmark for x86-64 and ARM64 processors. It characterises
processor performance through a set of independent measurement modes rather than
a single composite score. All results are derived from raw samples, and every
derived quantity compares identical kernels built from identical sources.

The full design is in [docs/spec.md](docs/spec.md).

## Tests

The desktop app names every test by what it does. The command line
(`--kernels`), result files and source folders use a short kernel ID instead.
This README uses the app's names, with the ID in brackets where it helps.

| Test (as in the app) | ID | What it runs | Source |
| --- | --- | --- | --- |
| Compiling code | K1 | In-process compile: Clang as a library, aarch64 target, in-memory files | `kernels/k1_compile`; needs the [compile-test setup](#compile-tests-setup) |
| Full software build | K1x | Build of every 8th unit (73 of 577) with CMake + Ninja, all threads, three times | `src/engine/k1x.c`; needs the [compile-test setup](#compile-tests-setup) |
| 3D rendering | K2 | Path tracer | `kernels/k2_render` |
| Compression | K3 | zstd 1.5.6, level 3, self-generated corpus | `kernels/k3_compress` |
| Opening a photo | K4 | JPEG decode (stb_image, SIMD off) and triangle-filter resize | `kernels/k4_image` |
| Reading JSON | K5 | Validating JSON parser, exact number handling | `kernels/k5_json` |
| Starting a script | K6 | Lua 5.4.7: new state, load and run a script | `kernels/k6_lua` |
| Memory delay | K7 | Pointer chase over L1-to-DRAM working sets | `kernels/k7_chase` |
| Matrix maths | K8 | Matrix multiply, FP32 and INT8 | `kernels/k8_matmul` |
| Wake-up test | K9 | Calibrated dependent-add loop | `kernels/k9_calib` |
| Timer punctuality | K10 | Periodic timer | `src/engine/mode_periodic.c` |

## Measurement modes

Each test runs in one or more modes. The app groups them into the tabs
All cores, One core, Responsiveness, Memory & timing and New instructions; the
command line selects modes by ID (`--mode`).

| Mode | Shown in the app as | CLI ID | Measured quantity | Tests |
| --- | --- | --- | --- | --- |
| Single-thread burst | one core, short task | `st_burst` | Execution time of short interactive workloads on one core | Compression, Opening a photo, Reading JSON, Starting a script |
| Single-thread sustained | one core, after warming up | `st_sustained` | Single-core throughput after a warm-up at full load | Compiling code, 3D rendering |
| Multi-thread | all cores, working together | `mc_threaded` | Scaling of multithreaded workloads, including serial fractions | Compiling code, Full software build, 3D rendering |
| Multi-instance | all cores, each on its own | `mc_instances` | Memory latency while every core uses memory | Memory delay |
| Cold start | started from rest | `cold_burst` | Latency penalty of work issued from idle, on the machine as configured | Wake-up test; Opening a photo, Starting a script |
| Periodic | timer punctuality | `periodic` | Wake-up latency distribution of periodic tasks | Timer punctuality |

ISA uplift (the New instructions tab: 3D rendering, Compression, Opening a
photo and Matrix maths, each built for the baseline and the max-level ISA) runs
under single-thread sustained conditions.

Every input is generated from fixed seeds or fetched by hash, so every host does
identical work. Kernels are built without FP contraction and without libm
transcendentals in their outputs, and their output checksums are bit-identical
across ISA tiers and compilers (`prismark checksums`; CI compares x86-64 against
ARM64).

## Methodology

- Results are reported per kernel and per mode. Measurements from different
  modes are not combined.
- Derived metrics (scaling S/E/p, throttling ratio, build overhead R_build,
  instruction-set uplift U_ISA) compare the same
  kernel and the same binary, with bootstrap 95 % intervals.
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
| 1 | Linux core, all six modes, 3D rendering, Compression, Wake-up test, Timer punctuality | implemented; gate 1 needs repeat runs on an x86-64 and an ARM64 machine |
| 2 | Compiling code, Full software build, Opening a photo, Reading JSON, Starting a script, Memory delay, Matrix maths, statistics in the core, analyzer | implemented; Compiling code and Full software build untested (need the Clang 19 development libraries and a snapshot) |
| 3 | Windows PAL, CLI | cross-compiles with llvm-mingw (`tools/package-windows.sh`); not yet run on Windows |
| 4 | Android (JNI bridge), GUI | JNI bridge and Kotlin wrapper written, untested; desktop app (Qt) working on Linux |
| 5 | macOS / iOS PAL, Swift wrapper | written, not yet compiled or run on Apple platforms |

On Linux x86-64 everything except Compiling code and Full software build has
been run end to end. The gates in
the spec are still to be passed.

## Building

Requires CMake ≥ 3.25, Ninja, network access on first configure (zstd, Lua and
stb are fetched at pinned versions and verified by SHA-256), and the pinned
upstream Clang (currently 19). The packages differ by distribution; each block
below installs everything: compiler, build tools, the Clang 19 development
libraries that build "Compiling code" in, and Qt for the desktop app.

### Ubuntu (x86-64 or arm64)

Clang 19 comes from apt.llvm.org:

```sh
wget https://apt.llvm.org/llvm.sh && sudo bash llvm.sh 19     # clang-19, clang++-19, lld-19
sudo apt install cmake ninja-build libclang-19-dev llvm-19-dev \
                 qt6-base-dev qt6-svg-dev
```

### Fedora

Clang 19 comes from Fedora's own versioned packages (installed under
`/usr/lib64/llvm19`, with `clang-19` and `ld.lld-19` in `/usr/bin`); no extra
repository is needed:

```sh
sudo dnf install cmake ninja-build clang19 lld19 clang19-devel llvm19-devel \
                 qt6-qtbase-devel qt6-qtsvg-devel
```

### Build and test (all distributions)

```sh
cmake --preset linux-clang
cmake --build --preset linux-clang
ctest --preset linux-clang     # the core, and the command line (tests/test_cli.py, needs python3)
```

The configure output says what will be built in. Look for
`K1: in-process Clang 19.1.7 from …` (Compiling code) and
`GUI: prismark-gui with Qt …` (desktop app). If the Clang 19 development
libraries are not found, Compiling code is listed as unavailable in results;
point CMake at a custom install with `-DPRISMARK_K1_LLVM_DIR=<prefix>`. Without
Qt, only the command-line runner is built.

For development with any other compiler, use the `dev` preset instead
(`cmake --preset dev`); results from such builds are not comparable to
official ones. Other presets: `windows-clang`, `macos-clang`, `android-arm64`
(needs `ANDROID_NDK`), `ios-arm64` (static libraries for `bindings/swift`).

### Install (adds Prismark to the applications menu)

After building, install Prismark so the desktop app shows up among the other
applications (Activities / app grid on GNOME, the application launcher on KDE).
The commands are the same on Ubuntu and Fedora:

```sh
sudo cmake --install build/linux-clang --prefix /usr/local
```

This copies `prismark-gui`, the `prismark` runner and its instruction-set
modules to `/usr/local/bin`, the launcher entry to
`/usr/local/share/applications/prismark.desktop`, the icon to
`/usr/local/share/icons/hicolor/scalable/apps/prismark.svg` and the
compile-test script to `/usr/local/share/prismark`. Prismark then appears as
**Prismark** in the applications menu (search for "Prismark"), and
`prismark` and `prismark-gui` work from any terminal. If the entry does not
show up at once, log out and back in.

To install for your user only, without `sudo`, use `--prefix ~/.local`
instead. The launcher then runs `~/.local/bin/prismark-gui`, which must be on
the session `PATH`: Fedora adds it by default; Ubuntu adds it at login when the
folder exists, so log out and back in after the first install.

The install is a copy: rebuilding does not change it. To update an installed
copy, rebuild and run the same install command again.

#### Developer install (follows the latest build)

> **For development only.** Use the copy install above for normal use. This
> variant links the menu entry to your build folder, so every rebuild is what
> the menu starts, but moving or deleting the source or build folder breaks it.

```sh
mkdir -p ~/.local/bin ~/.local/share/applications ~/.local/share/icons/hicolor/scalable/apps
ln -sf "$PWD/build/linux-clang/prismark-gui" ~/.local/bin/prismark-gui
ln -sf "$PWD/build/linux-clang/prismark"     ~/.local/bin/prismark
cp apps/gui/prismark.desktop ~/.local/share/applications/
cp apps/gui/icons/prismark-app.svg ~/.local/share/icons/hicolor/scalable/apps/prismark.svg
```

Run it from the source folder, on Ubuntu or Fedora. The app follows the link
back to `build/linux-clang/` and uses the runner and modules built there; the
compile-test script is found in `tools/k1x/` of the source folder. As with any
`~/.local` install, `~/.local/bin` must be on the session `PATH` (on Ubuntu,
log out and back in after the first time). Do not combine it with the copy
install: whichever `prismark-gui` comes first on `PATH` wins. To remove it:

```sh
rm -f ~/.local/bin/prismark-gui ~/.local/bin/prismark \
      ~/.local/share/applications/prismark.desktop \
      ~/.local/share/icons/hicolor/scalable/apps/prismark.svg
```

### Uninstall

The install writes the list of files it copied to
`build/linux-clang/install_manifest.txt`. Removing them takes Prismark out of
the applications menu and off the `PATH` (same on Ubuntu and Fedora):

```sh
sudo xargs -a build/linux-clang/install_manifest.txt rm -fv
sudo rmdir /usr/local/share/prismark
```

For a `~/.local` install, run the first line without `sudo` and skip the
second (that folder also holds your results). If the build folder is gone,
delete the files listed above by hand.

Your results and the compile-test snapshot are kept. To remove them too:

```sh
rm -rf ~/.local/share/prismark                 # results/ and the k1x/ snapshot
```

A portable Windows x86-64 build (desktop app, runner and DLLs in one zip) can be
cross-compiled on Linux with llvm-mingw and Qt for Windows:
`tools/package-windows.sh <llvm-mingw> <Qt/6.8.3/llvm-mingw_64> <Qt/6.8.3/gcc_64>`
(the script lists where to get them). It leaves out Compiling code and Full
software build.

## Running

```sh
./build/linux-clang/prismark                  # menu: full or quick run, choose tests, past runs, compare
./build/linux-clang/prismark run              # full run of all modes (about 30 min on a 4-core laptop)
./build/linux-clang/prismark run --quick      # smoke run, a few minutes; not for comparison
./build/linux-clang/prismark --mode short-task,from-rest --tests photo,script,wakeup
./build/linux-clang/prismark tests            # every test and mode, with the names and IDs it accepts
./build/linux-clang/prismark help             # options; `help advanced` for tuning and desktop-app options
```

`prismark` alone never starts a run: on a terminal it opens a menu (`b` at
any prompt goes one step back), and otherwise it exits with a hint. Options without a command (`prismark --quick
…`) still start a run, as before. Runs from the menu and from `prismark run`
include the compile tests when the compile-test snapshot is prepared (in the
app or with `tools/k1x/prepare.py`); `--k1-data DIR` points to another one.

`--tests` and `--mode` take the app's names (`"Opening a photo"`, `photo`), a
short name, or the ID (`K4`, `st_burst`); `--kernels` is the same as `--tests`.
A run refuses tests and modes that do not go together (`--tests photo --mode
all-cores` would measure nothing; `prismark tests` shows which do), and names
any chosen test that runs in none of the chosen modes. Number options are
checked too.
On a terminal the run shows one progress line (overall step, elapsed time,
an estimate of the time left, temperature, what runs now) and keeps only part
headers, notices and warnings on screen; piped, it prints one plain line per
event. The line is redrawn only when the core reports progress, between
measurements, so the display never wakes the machine during a test. Runs
started from the menu print the equivalent `prismark run` command first. Colour is used on a
terminal only, and `NO_COLOR` or `--no-color` turns it off. Ctrl+C stops the
run after the current measurement and saves what was measured; a second
Ctrl+C quits at once.

The exit status says how a run ended: 0 done, 1 failed, 2 wrong usage, 3
the machine was too busy to measure (another program was working; nothing was
measured), 130 stopped with Ctrl+C (what was measured is saved).

Each run writes `prismark-<run_id>.json` with every raw sample, the machine
state at start and end, and the statistics computed from them. It goes in the
results folder shared with the desktop app, so command-line runs appear there
too (`-o FILE` writes elsewhere; runs as root write to the current folder):

```
~/.local/share/prismark/results/        Linux; macOS: ~/Library/Application Support/prismark/results,
├── runs/                               Windows: %LOCALAPPDATA%\prismark\results
└── references/                         your own reference systems (see references/README.md)
```

```sh
prismark list                                 # past runs in the results folder, newest first (local times)
prismark show latest                          # a run's summary again
prismark compare latest intel-i5-1035g1       # the headline results against a reference system
prismark compare 6f07bd9e latest              # ... or against another run
prismark compare 6f07bd9e latest --detail     # every series with 95 % CIs, and the profiles
prismark compare a.json b.json --profiles my-profiles.json
prismark references                           # the reference systems compare accepts
prismark references add latest my-desktop     # save a run as your own reference system (remove NAME)
prismark profiles                             # the default profiles (Daily, Dev, Render, Realtime)
prismark checksums -o checksums.json          # kernel output checksums, no timing
eval "$(prismark completion bash)"            # tab completion (also zsh): commands, tests, runs, references
```

A run is named by its file, its run ID or the first characters of it (as
`list` shows them), or `latest`. Results store their summary, so `show` can
print it again; older result files have none, and `show` says so.

`compare` shows the headline results the desktop app shows, side by side, and
says for each whether A is better, worse or the same within the 95 % ranges:
times and speeds as a ratio (`1.37x better`), percentages as a difference in
points (`38.2 pts worse`), and "may be noise" where one side has no range.
Each side is labelled with its run ID and power source, so two runs of the same
processor can be told apart.
Either side may be a reference system from [`references/`](references/README.md)
(installed with the program) or from your own `results/references`, named by
its file name; placeholders are labelled as such. `references add RUN NAME`
(or Past runs in the menu) saves one of your runs there, and the desktop app
shows it in its rankings. `--detail` compares every
measured series instead, and needs two measured results. It refuses runs
measured under different capabilities, and reports a
profile only when every kernel it names exists in both runs.

Prismark measures the machine as it is configured and changes nothing on it:
no power settings, no frequency limits, no idle states. It needs no root
rights. What it finds (governor, power mode, idle states, temperatures) is
recorded with the result.

### Compile tests setup

"Compiling code" (K1) runs on Linux, Windows, macOS and iPadOS; "Full software
build" (K1x) on Linux, Windows and macOS (it starts other programs, which
iPadOS does not allow). Phones do not offer either, and they are not shown
there at all.

Both need a one-time setup, in this order:

1. **Install the packages** for your distribution from [Building](#building)
   (the Clang 19 development libraries, `lld` and `python3` among them).
2. **Rebuild Prismark** so Compiling code is built in (check for the `K1:`
   line in the configure output).
3. **Prepare the snapshot.** This step is not done by any package or by the
   build: you start it yourself. In the app, press ▶ Run on a compile test and
   then **Prepare now** (or Menu › Prepare compile tests). From a terminal:

   ```sh
   tools/k1x/prepare.py                       # output in ~/.local/share/prismark/k1x
   ./build/linux-clang/prismark run               # finds the snapshot there by itself
   ```

Compiling code compiles the units in a fixed shuffled order (the same on
every machine), so its fixed-length measurement covers small and large files
alike; each unit's work is weighted by its compile cost and credited to the
windows it ran in. Full software build runs only on all threads, and "Full
build vs. in-memory compile" (R_build) compares it with Compiling code's time
for the same share of the work.

Both compile tests need about 450 MB of memory per thread (Clang at `-O2` on
LLVM's largest file peaks at 375 MB): about 3.6 GB on 8 threads, 7.2 GB on
16. Before each thread count, Prismark checks the memory available and skips
the counts that do not fit, recording "not enough memory" instead of being
stopped by the system; the app then marks the result "N of M threads" rather
than ranking it as an all-cores result. Closing other programs before the run
frees memory for more threads.

The snapshot is a pinned LLVM subset cross-compiled to aarch64, with its
generated sources included so no host tool runs during the measured build.
Preparing it downloads about 160 MB (LLVM sources and a pinned aarch64
sysroot, both verified by SHA-256), builds the generated files once and
measures every unit: 20–60 minutes, about 4 GB of disk while it runs. It can be
cancelled and started again; downloads that were interrupted are fetched
again. The script checks the granularity rule (Σ T_i / T_max ≥ 4 n_max) and
prints the snapshot's statistics.

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

Once [installed](#install-adds-prismark-to-the-applications-menu), it is also
started from the applications menu like any other app.

- Built automatically when Qt Widgets and Svg are found (Qt 6, or Qt 5.15;
  the packages are in [Building](#building)); `-DPRISMARK_GUI=OFF` skips it. The Inter typeface is fetched at a pinned version and compiled in.
- **Isolation.** Runs execute in the `prismark` runner next to the app, as a
  separate process. During a run the main window closes and only a small
  measuring window remains, static except when a new phase begins (spec 7.1);
  closing it cancels the run (the partial result is kept).
- **Quick runs** skip the warm-up, use short series, compile every 8th unit in
  Compiling code and build once; they are recorded as such and never compared
  with full runs.
- **As configured.** The app never asks for administrator rights and never
  changes power settings: every test measures the computer as it is set up.
- **Hands off.** Cold start and periodic measure waits and wake-ups. Keyboard
  or mouse input during them skips that test: its series are discarded, the
  result lists it as skipped, and the run continues. On GNOME the app watches
  through Mutter's idle monitor (event-driven, no polling); elsewhere on Linux
  it polls the ScreenSaver idle time every 2 s; on Windows and macOS the
  runner reads the last-input time itself. The method is recorded with the
  result.
- **Compile tests.** Compiling code and Full software build need the
  one-time [setup](#compile-tests-setup). When you run one, the app checks
  each step: missing tools and libraries come first, with the install commands
  for your distribution (`apt` on Ubuntu, `dnf` on Fedora); preparing the
  snapshot is always the last step, started with **Prepare now**, and shows
  its progress.

- Every run is saved in `results/runs` of the shared results folder (see
  Running) and reloaded at start, along with runs from the command line; the
  compile-test snapshot lives in `k1x/` beside `results/`. Files from older
  versions are moved into this layout on first start.
  Menu › Open result imports files; Menu › Compare runs `prismark compare`.
- The reference systems in the rankings come from the repository's
  [`references/`](references/README.md) folder, one file per system, built into
  the app: add a file to add a system, delete it to remove one. Today they are
  **placeholders**, not measurements, and are labelled as such; result files from
  real reference machines go in the same folder. Users can add their own in
  `results/references` of the shared results folder, without rebuilding.

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
src/engine/                   runner, modes, sustained-run engine, Full software build (K1x) driver
src/stats/                    medians, bootstrap, analysis, compare and profiles
src/result/                   JSON writer and reader
pal/                          platform layers: linux, darwin, windows, posix, common (ISA detection)
kernels/                      one directory per kernel; compiled once per ISA tier
apps/cli, apps/gui            command-line runner, desktop app
apps/viewer                   static web viewer
bindings/android, bindings/swift   JNI bridge and Kotlin wrapper; Swift package
tools/k1x, tools/ci           compile-test snapshot preparation, checksum gate
analyzer/                     Python research tools
```

## License

Prismark is distributed under the Apache License 2.0. Results produced by
modified builds are marked as unverified.
