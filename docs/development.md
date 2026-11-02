# Development

[README](../README.md) · [Measuring](measuring.md) · [Command line](command-line.md) · [Desktop app](desktop-app.md) · [Building](building.md) · [Packaging](packaging.md) · [Development](development.md) · [Design](design.md)

Where the project stands and how the source is laid out.

## Status

| Phase | Content | State |
| --- | --- | --- |
| 1 | Linux core, all six modes, 3D rendering, Compression, Wake-up test, Timer punctuality | implemented; gate 1 needs repeat runs on an x86-64 and an ARM64 machine |
| 2 | Compiling code, Full software build, Opening a photo, Reading JSON, Starting a script, Memory delay, statistics in the core, analyzer | implemented; Compiling code and Full software build run on all ten reference machines (Linux, Windows and macOS, `references/`) |
| 3 | Windows PAL, CLI | portable package cross-built with llvm-mingw (`tools/package-windows.sh`), compile tests included; run on Windows itself (Windows 11, builds 26200 and 26300) with the desktop app on three reference machines (Ryzen 5 7500F, Ryzen 7 8845HS, i9-14900K), both compile tests included; the command-line runner on Windows itself not confirmed (earlier run under Wine) |
| 4 | Android (JNI bridge), GUI | JNI bridge and Kotlin wrapper written, untested; desktop app (Qt) working on Linux and Windows (the Windows references were measured with it) |
| 5 | macOS / iOS PAL, Swift wrapper | macOS built and run on Apple M2, disk-image package (`tools/package-macos.sh`) with the compile tests; iOS and the Swift wrapper not yet compiled |
| 6 | Stress test (one core, all cores, each core in turn, memory test) with checks and verdict; Vector maths benchmark; ISA uplift and Matrix maths removed | implemented; run on Linux x86-64 (AVX2; AVX-512 not yet run on a test machine); Vector maths also on macOS ARM64 (NEON, `apple-m2-macos`) |

On Linux x86-64 everything has been run end to end; so have all tests but the
stress test and Vector maths on Windows x86-64 (with results of version 0.2.x),
and all tests but the stress test on macOS ARM64. No reference holds a stress
test, and none comes from Linux on ARM64. The release gates (repeat runs on an x86-64 and an ARM64 machine) are still to be passed.

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

## Analyzer

```sh
cd analyzer
uv run prismark-analyze plot ../prismark-<id>.json      # responsiveness, scaling, latency, perf(t), jitter
uv run prismark-analyze check ../prismark-<id>.json     # recompute the core's medians from raw samples
uv run prismark-analyze table run1.json run2.json       # CSV of medians across runs
```
