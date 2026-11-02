# Building

[README](../README.md) · [Measuring](measuring.md) · [Command line](command-line.md) · [Desktop app](desktop-app.md) · [Building](building.md) · [Packaging](packaging.md) · [Development](development.md) · [Design](design.md)

How to build Prismark from source, install it from the build, and set up the compile tests. The released packages need none of this: see [Packaging](packaging.md).

Requires CMake ≥ 3.25, Ninja, network access on first configure (zstd, Lua and
stb are fetched at pinned versions and verified by SHA-256), and the pinned
upstream Clang (currently 19). The packages differ by distribution; each block
below installs everything: compiler, build tools, the Clang 19 development
libraries that build "Compiling code" in, and Qt for the desktop app.

## Ubuntu (x86-64 or arm64)

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

## Fedora

Clang 19 comes from Fedora's own versioned packages (installed under
`/usr/lib64/llvm19`, with `clang-19` and `ld.lld-19` in `/usr/bin`); no extra
repository is needed:

```sh
sudo dnf install cmake ninja-build clang19 lld19 clang19-devel llvm19-devel \
                 qt6-qtbase-devel qt6-qtsvg-devel
```

## Build and test (all distributions)

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

## Android

The Android app (`apps/android`, Kotlin and Jetpack Compose) runs the core in
its own process through the JNI bridge (`bindings/android`). One script builds
the library with the `android-arm64` preset, fetches the pinned fonts and builds
the APK with Gradle:

```sh
export ANDROID_HOME=~/Android/Sdk                  # the SDK, with platform 35
export ANDROID_NDK=$ANDROID_HOME/ndk/27.2.12479018 # the NDK CI uses
tools/build-android.sh                             # build/android/prismark-<version>-android-arm64-debug.apk
adb install build/android/prismark-*-android-arm64-debug.apk
```

It needs CMake 3.25 or later, Ninja and a JDK 17 or later. CI builds the same
APK (job `android`, artifact `android-arm64-debug-apk`). A debug APK is signed
with the key of the machine that built it: uninstall one from elsewhere before
installing another. Phones run ARM64 only; Compiling code, Full software build
and the stress test are not offered on them. The preset also builds the command
line for Android (`build/android-arm64/prismark`), which runs from `adb shell`
after `adb push` to `/data/local/tmp`.

What Android does not let an app read is recorded as unknown (null in the
result) and does not stop a run: the CPU temperature where SELinux hides the
thermal zones, the clock where cpufreq is closed (0 MHz, as on Windows), the
background load (`/proc/stat` is closed to apps since Android 8), pinning where
the app's cpuset or the vendor refuses it, and the cycle counter.
An unknown background load never marks a run as busy; where it can be read and
is above 5 % (`PMK_BUSY_LOAD`), the run warns, continues and is marked busy.

## Install (adds Prismark to the applications menu)

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

### Developer install (follows the latest build)

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

## Uninstall

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

## macOS

A build from source on macOS finds Homebrew's tools for the compile tests
(`brew install llvm@19 lld cmake ninja python`) even when the app is opened
from the Finder, whose `PATH` has no Homebrew.

## Compile tests setup

"Compiling code" (K1) runs on Linux, Windows, macOS and iPadOS; "Full software
build" (K1x) on Linux, Windows and macOS (it starts other programs, which
iPadOS does not allow). Phones do not offer either, and they are not shown
there at all.

The released packages (the Windows zip, the Linux `.deb` and `.rpm`, the macOS
disk image) bring both ready to run, snapshot and tools included. A build from
source needs a one-time setup, in this order:

1. **Install the packages** for your distribution from [the packages above](#ubuntu-x86-64-or-arm64)
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
