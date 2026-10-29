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

## Status

| Phase | Content | State |
| --- | --- | --- |
| 1 | Linux core, all six modes, 3D rendering, Compression, Wake-up test, Timer punctuality | implemented; gate 1 needs repeat runs on an x86-64 and an ARM64 machine |
| 2 | Compiling code, Full software build, Opening a photo, Reading JSON, Starting a script, Memory delay, statistics in the core, analyzer | implemented; Compiling code and Full software build untested (need the Clang 19 development libraries and a snapshot) |
| 3 | Windows PAL, CLI | portable package cross-built with llvm-mingw (`tools/package-windows.sh`), compile tests included; run under Wine (both compile tests, the app, the commands), not yet on Windows itself |
| 4 | Android (JNI bridge), GUI | JNI bridge and Kotlin wrapper written, untested; desktop app (Qt) working on Linux |
| 5 | macOS / iOS PAL, Swift wrapper | macOS built and run on Apple M2, disk-image package (`tools/package-macos.sh`) without the compile tests; iOS and the Swift wrapper not yet compiled |
| 6 | Stress test (one core, all cores, each core in turn, memory test) with checks and verdict; Vector maths benchmark; ISA uplift and Matrix maths removed | implemented; run on Linux x86-64 (AVX2; AVX-512 not yet run) |

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

Clang 19 comes from apt.llvm.org. Ubuntu 24.04 and later have everything else
in their own packages:

```sh
wget https://apt.llvm.org/llvm.sh && sudo bash llvm.sh 19     # clang-19, clang++-19, lld-19
sudo apt install cmake ninja-build libclang-19-dev llvm-19-dev \
                 qt6-base-dev qt6-svg-dev
```

Ubuntu 22.04 ships CMake 3.22, older than the 3.25 the build requires, and
names the Qt SVG package differently. Take CMake from Kitware's apt
repository, which replaces the system `cmake` package:

```sh
wget https://apt.llvm.org/llvm.sh && sudo bash llvm.sh 19
wget -qO- https://apt.kitware.com/keys/kitware-archive-latest.asc | gpg --dearmor |
  sudo tee /usr/share/keyrings/kitware-archive-keyring.gpg >/dev/null
echo 'deb [signed-by=/usr/share/keyrings/kitware-archive-keyring.gpg] https://apt.kitware.com/ubuntu/ jammy main' |
  sudo tee /etc/apt/sources.list.d/kitware.list >/dev/null
sudo apt update
sudo apt install cmake ninja-build libclang-19-dev llvm-19-dev \
                 qt6-base-dev libqt6svg6-dev
cmake --version     # must say 3.25 or later
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

This copies `prismark-gui` and the `prismark` runner to `/usr/local/bin`, the launcher entry to
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
back to `build/linux-clang/` and uses the runner built there; the
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

Windows is shipped as a portable zip, not an installer: unzip it and start
`prismark-gui.exe`. The folder holds everything the app and `prismark.exe`
need (Qt, its plugins and the C++ runtime), so nothing has to be installed;
it needs 64-bit Windows 10 or later. The compile tests come with it: Clang is
built into `prismark.exe` for Compiling code, and Full software build uses the
snapshot in `k1x\` and Clang 19, CMake and Ninja in `tools\`. Every push
builds it in CI (`windows-portable`, downloadable from the run). It is
cross-compiled on Linux with llvm-mingw and Qt for Windows:

```sh
tools/build-llvm-windows.sh <llvm-mingw> build/llvm-19-windows    # once: Clang and LLVM 19 libraries for Windows
K1_LLVM=build/llvm-19-windows K1X_SNAPSHOT=~/.local/share/prismark/k1x \
  tools/package-windows.sh <llvm-mingw> <Qt/6.8.3/llvm-mingw_64> <Qt/6.8.3/gcc_64>
```

The scripts list where to get llvm-mingw and Qt; the Windows build tools are
downloaded at pinned versions and checked. Packaging refuses to zip a folder
in which a DLL any binary needs is missing. `PRISMARK_COMPILE_TESTS=0` builds
the package without the compile tests.

Linux has a portable package too, a `.tar.gz` built the same way: unpack it
and start `./prismark-gui`, with nothing to install, so it also runs from a
live USB session. It carries Qt, every library a desktop distribution may
lack, and the compile tests' snapshot and tools (Clang 19, CMake, Ninja).
Build it on the oldest distribution it should run on (Ubuntu 22.04 covers
22.04 and later), with Clang 19 from apt.llvm.org and the official Qt 6:

```sh
aqt install-qt linux desktop 6.8.3 linux_gcc_64 -O Qt
K1X_SNAPSHOT=~/.local/share/prismark/k1x tools/package-linux.sh Qt/6.8.3/gcc_64
```

It refuses to pack a folder in which a program or plugin would take a
library from the system that is not on every desktop. It packages for the
machine it runs on, x86-64 or ARM64. For ARM64, Qt's own binaries need a newer
glibc than Ubuntu 22.04's, so Qt is built from source first
(`tools/build-qt-linux.sh build/qt-6.8.3`, then `tools/package-linux.sh
build/qt-6.8.3`). The ARM64 package covers Jetson Orin boards on JetPack 6 and
other ARM64 boards with Ubuntu 22.04, Debian 12 or later.

macOS is shipped as a disk image: open it and drag Prismark to Applications.
The app carries Qt and the runner (`Prismark.app/Contents/MacOS/prismark`,
which is also the command line) and runs on macOS 13 or later. It is signed
ad hoc, not notarised, so the first start is a right-click and Open. The
compile tests are not packed yet: Compiling code is not built in, and Full
software build needs the one-time setup with Homebrew's `llvm@19`, `cmake`
and `ninja`. It is built with Homebrew's Clang 19 and the official Qt 6.10
(6.8 does not link with the macOS 26 SDK), for the Mac it runs on:

```sh
brew install llvm@19 ninja
aqt install-qt mac desktop 6.10.3 clang_64 -O Qt
tools/package-macos.sh Qt/6.10.3/macos
```

It refuses to pack an app in which a program or library would take anything
from outside it but macOS itself. CI builds it for Apple silicon on every
push (`macos-package`) and opens the image to start the runner and the app.

CI builds both Linux packages in an Ubuntu 22.04 container on every push
(`linux-portable`), and fails if anything in them needs a glibc newer than
22.04's. It then runs each one on the newest Fedora (`fedora-check`): every
library must resolve, the runner must measure and the app must start. When a
`v*` tag is pushed, every package is attached to its GitHub release. To
repackage an earlier release, run the workflow by hand with that tag as `ref`.

## Running

```sh
./build/linux-clang/prismark                  # menu: full or quick run, choose tests, past runs, compare
./build/linux-clang/prismark run              # full run of all modes (about 30 min on a 4-core laptop)
./build/linux-clang/prismark run --quick      # smoke run, a few minutes; not for comparison
./build/linux-clang/prismark --mode short-task,from-rest --tests photo,script,wakeup
./build/linux-clang/prismark run --mode stress --stress all-cores --stress-time 30m   # or until-stopped
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
measured), 4 the stress test found wrong results, 130 stopped with Ctrl+C (what was measured is saved).

A stress test run with `--stress-time until-stopped` ends with Ctrl+C: that is
its normal end, so the run is saved as complete and the command exits 0.

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

On Linux, the power of an Intel or AMD processor is read from its RAPL energy
counter, which only root may read since Linux 5.10. Without it no power is
recorded, and the summary says `power none rapl-needs-root`. To measure power,
run `prismark` with `sudo`, or let every user read the counter until the next
restart (this also exposes it to other programs, which it is restricted to
keep from):

```sh
sudo chmod o+r /sys/class/powercap/intel-rapl:0/energy_uj
```

A run started with `sudo` (or `pkexec`), from the command line or the desktop
app, still saves to the results folder of the user who started it, and the
files and folders it creates there belong to that user.

### Compile tests setup

"Compiling code" (K1) runs on Linux, Windows, macOS and iPadOS; "Full software
build" (K1x) on Linux, Windows and macOS (it starts other programs, which
iPadOS does not allow). Phones do not offer either, and they are not shown
there at all.

The portable Windows package brings both ready to run, snapshot and tools
included. Everywhere else they need a one-time setup, in this order:

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
top, and press ▶ Run on a test card or "Run all". The Quick switch beside it
chooses a **full run** (the default: accurate and ranked; about 30 minutes for
all tests on a laptop) or a **quick run** (minutes, a first look). Each test is ranked on
its own, with your latest run highlighted against reference systems; the
details panel shows the machine state, capabilities, responsiveness curve and
scaling.

```sh
./build/linux-clang/prismark-gui              # or: prismark-gui --run [TEST] [--full], e.g. --run mc_k2
```

Once [installed](#install-adds-prismark-to-the-applications-menu), it is also
started from the applications menu like any other app.

- Built automatically when Qt Widgets and Svg are found (Qt 6, or Qt 5.15;
  the packages are in [Building](#building)); `-DPRISMARK_GUI=OFF` skips it. Its typefaces (Inter, Saira and IBM Plex Mono) are fetched at pinned versions and compiled in.
- **Isolation.** Runs execute in the `prismark` runner next to the app, as a
  separate process. During a run the main window closes and only a small
  measuring window remains, static except when a new phase begins (spec 7.1);
  closing it cancels the run (the partial result is kept).
- **Quick runs** skip the warm-up, use short series, compile every 8th unit in
  Compiling code and build once. Their numbers are rough, so they are not
  ranked: a test page shows your latest quick run under its ranking, with a
  link to its report, until a full run replaces it. The stress test has no
  quick run (the switch is hidden on its tab); it asks for its length instead.
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
  the app: add a file to add a system, delete it to remove one. They are result
  files from real machines; placeholders (example values typed in by hand) are
  still accepted and labelled as such. Users can add their own in
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
pal/                          platform layers: linux, darwin, windows, posix
kernels/                      one directory per kernel; compiled once, for the baseline ISA level
apps/cli, apps/gui            command-line runner, desktop app
apps/viewer                   static web viewer
bindings/android, bindings/swift   JNI bridge and Kotlin wrapper; Swift package
tools/k1x, tools/ci           compile-test snapshot preparation, checksum gate
analyzer/                     Python research tools
```

## License

Prismark is distributed under the Apache License 2.0. Results produced by
modified builds are marked as unverified.
