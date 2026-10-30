# Packaging

[README](../README.md) · [Measuring](measuring.md) · [Command line](command-line.md) · [Desktop app](desktop-app.md) · [Building](building.md) · [Packaging](packaging.md) · [Development](development.md) · [Design](design.md)

How the release packages are made. Each script builds for the machine it runs on (Windows is cross-built on Linux) and refuses to pack anything that would need a file from outside the package.

## Windows

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

## Linux

Linux is shipped as packages to install: a `.deb` for Ubuntu and Debian and
an `.rpm` for Fedora, for x86-64 and ARM64. Install with the system's own tool,
which also brings in the few libraries every desktop has:

```sh
sudo apt install ./prismark_0.4.2_amd64.deb        # Ubuntu 22.04 or later, Debian 12 or later
sudo dnf install ./prismark-0.4.2-1.x86_64.rpm     # Fedora
```

Prismark then appears in the desktop's menu; `prismark` and `prismark-gui`
are in `/usr/bin`. Both packages hold the same files, in `/opt/prismark`: the
app, the runner, Qt, every library a desktop distribution may lack, and the
compile tests' snapshot and tools (Clang 19, CMake, Ninja). They are built on
the oldest distribution they should run on (Ubuntu 22.04), with Clang 19 from
apt.llvm.org, the official Qt 6, `dpkg-deb` and `rpmbuild` (`apt install rpm`):

```sh
aqt install-qt linux desktop 6.8.3 linux_gcc_64 -O Qt
K1X_SNAPSHOT=~/.local/share/prismark/k1x tools/package-linux.sh Qt/6.8.3/gcc_64
```

It refuses to pack a folder in which a program or plugin would take a
library from the system that is not on every desktop, and makes the packages
depend on exactly the system libraries the rest use. It packages for the
machine it runs on, x86-64 or ARM64. For ARM64, Qt's own binaries need a newer
glibc than Ubuntu 22.04's, so Qt is built from source first
(`tools/build-qt-linux.sh build/qt-6.8.3`, then `tools/package-linux.sh
build/qt-6.8.3`). The ARM64 `.deb` covers Jetson Orin boards on JetPack 6 and
other ARM64 boards with Ubuntu 22.04, Debian 12 or later.

## macOS

macOS is shipped as a disk image: it opens on a window that shows the install,
and Prismark goes to Applications with a drag.
Nothing else is needed, no Homebrew or Xcode: the app carries Qt, the runner
(`Prismark.app/Contents/MacOS/prismark`, also the command line) and the
compile tests with everything they need: Clang built into the runner, and the
snapshot and Clang 19, CMake and Ninja in `Contents/Resources`. It is signed
ad hoc, not notarised, so macOS may stop the first start: System Settings ›
Privacy & Security › Open Anyway. It is
compiled with Homebrew's Clang 19 and built against LLVM's own release of
LLVM 19 (Homebrew's is built for the machine's own macOS release) and the
official Qt 6.10 (6.8 does not link with the macOS 26 SDK):

```sh
brew install llvm@19 ninja
aqt install-qt mac desktop 6.10.3 clang_64 -O Qt
curl -LO https://github.com/llvm/llvm-project/releases/download/llvmorg-19.1.7/LLVM-19.1.7-macOS-ARM64.tar.xz
tar -xJf LLVM-19.1.7-macOS-ARM64.tar.xz
K1_LLVM=LLVM-19.1.7-macOS-ARM64 K1X_SNAPSHOT=~/Library/Application\ Support/prismark/k1x \
  tools/package-macos.sh Qt/6.10.3/macos
```

It refuses to pack an app in which a program or library would take anything
from outside it but macOS itself, and writes the oldest macOS every part
supports into the app. CI builds it for Apple silicon on every push
(`macos-package`, with the Windows job's snapshot) and opens the image to
start the runner, the app and the bundled tools.

## CI and releases

CI builds the Linux packages in an Ubuntu 22.04 container on every push
(`linux-packages`), and fails if anything in them needs a glibc newer than
22.04's. It then installs each one as a user would (`linux-install-check`):
the `.rpm` with dnf on the newest Fedora and the `.deb` with apt on Ubuntu
24.04, on both architectures; every library must resolve, the runner must
measure and the app must start. When a
`v*` tag is pushed, every package is attached to its GitHub release. To
repackage an earlier release, run the workflow by hand with that tag as `ref`.
