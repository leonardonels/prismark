#!/usr/bin/env python3
"""
Prepare the K1/K1x snapshot: a pinned subset of LLVM with every generated
source pre-built, the pinned aarch64 sysroot headers it needs, Clang's
resource headers, and a generated CMake project that cross-builds the same
translation units to aarch64 without running any host tool.

  K1  (in-process compile) reads manifest.json and root/ into memory.
  K1x (full build) copies the snapshot to a RAM-backed directory and runs
      `cmake` once, then `ninja -j n` from clean, timed.

Both use identical translation units and flags (-O2, --target=aarch64), and
the snapshot's hash is recorded in every result, so every host does the same
work.

Usage (once per machine; needs the pinned clang and lld, cmake and ninja;
about 4 GB of disk while preparing, 20-60 minutes):

  tools/k1x/prepare.py                       # pinned sysroot, output in the app's data folder
  tools/k1x/prepare.py --sysroot DIR --out DIR

By default the aarch64 sysroot is Chromium's pinned Debian bullseye arm64
sysroot, downloaded once and verified by SHA-256, so every host compiles
against identical headers.

The output layout:
  manifest.json       units (virtual paths), costs, hashes, versions
  root/               files mounted at /k1x: src/, gen/, sysroot/, resource/
  link-sysroot/       crt objects and libraries for linking (K1x only)
  build/CMakeLists.txt, build/toolchain.cmake

Copyright 2026 The Prismark Authors. Apache-2.0.
"""
import argparse
import concurrent.futures as cf
import hashlib
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import tarfile
import time
import urllib.request

LLVM_VERSION = "19.1.7"
LLVM_URL = f"https://github.com/llvm/llvm-project/releases/download/llvmorg-{LLVM_VERSION}/llvm-project-{LLVM_VERSION}.src.tar.xz"
# SHA-256 of the release tarball (verified 2026-10-04); prepare.py refuses any other content.
LLVM_SHA256 = "82401fea7b79d0078043f7598b835284d6650a75b93e64b6f761ea7b63097501"
# Chromium's pinned Debian bullseye arm64 sysroot (build/linux/sysroot_scripts/sysroots.json).
SYSROOT_SHA256 = "c7176a4c7aacbf46bda58a029f39f79a68008d3dee6518f154dcf5161a5486d8"
SYSROOT_URL = f"https://commondatastorage.googleapis.com/chrome-linux-sysroot/{SYSROOT_SHA256}"
VROOT = "/k1x"
STEPS = 7
TARGET = "aarch64-linux-gnu"

DEFAULT_LIBS = [
    "LLVMDemangle", "LLVMSupport", "LLVMTargetParser", "LLVMBinaryFormat", "LLVMBitstreamReader",
    "LLVMRemarks", "LLVMCore", "LLVMBitReader", "LLVMBitWriter", "LLVMAsmParser", "LLVMIRReader",
    "LLVMProfileData", "LLVMAnalysis", "LLVMTransformUtils", "LLVMTableGen", "LLVMCodeGenTypes",
]
DEFAULT_TOOLS = ["llvm-tblgen"]

# Flags that describe the host build rather than the work, dropped from every unit.
DROP_PREFIX = ("-march=", "-mtune=", "-mcpu=", "-fcolor-diagnostics", "-fdiagnostics-color", "-O", "-g",
               "--target=", "-target", "--sysroot", "-isysroot", "-W", "-pedantic", "-fdebug-prefix-map",
               "-ffile-prefix-map", "-fmacro-prefix-map")
DROP_WITH_VALUE = {"-o", "-MF", "-MT", "-MQ", "-target", "--sysroot", "-isysroot"}
DROP_EXACT = {"-c", "-MD", "-MMD", "-MP"}


def log(*a):
    print("[prepare]", *a, file=sys.stderr, flush=True)


def step(i, what):
    """Progress line the desktop app shows: 'step i/N: what'."""
    print(f"[prepare] step {i}/{STEPS}: {what}", file=sys.stderr, flush=True)


def default_data_dir():
    if sys.platform == "win32":
        base = os.environ.get("LOCALAPPDATA", os.path.expanduser("~"))
    elif sys.platform == "darwin":
        base = os.path.expanduser("~/Library/Application Support")
    else:
        base = os.environ.get("XDG_DATA_HOME") or os.path.expanduser("~/.local/share")
    return os.path.join(base, "prismark")


def fetch_sysroot(work):
    """Downloads and unpacks the pinned sysroot once; returns its directory."""
    dest = os.path.join(work, "sysroot-" + SYSROOT_SHA256[:12])
    if os.path.isfile(os.path.join(dest, ".complete")):
        return dest
    tarball = os.path.join(work, "sysroot.tar.xz")
    if not os.path.exists(tarball) or sha256_file(tarball) != SYSROOT_SHA256:
        log("downloading the pinned aarch64 sysroot")
        urllib.request.urlretrieve(SYSROOT_URL, tarball)
    digest = sha256_file(tarball)
    if digest != SYSROOT_SHA256:
        sys.exit(f"sysroot hash mismatch: {digest} (pinned {SYSROOT_SHA256})")
    shutil.rmtree(dest, ignore_errors=True)
    os.makedirs(dest)
    with tarfile.open(tarball) as t:
        t.extractall(dest, filter="tar") if hasattr(tarfile, "data_filter") else t.extractall(dest)
    open(os.path.join(dest, ".complete"), "w").close()
    return dest


def run(cmd, **kw):
    log(" ".join(shlex.quote(str(c)) for c in cmd))
    subprocess.run(cmd, check=True, **kw)


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def fetch_llvm(work, src_arg):
    if src_arg:
        return os.path.abspath(src_arg), None
    tarball = os.path.join(work, os.path.basename(LLVM_URL))
    if not os.path.exists(tarball):
        log("downloading", LLVM_URL)
        urllib.request.urlretrieve(LLVM_URL, tarball)
    digest = sha256_file(tarball)
    if LLVM_SHA256 and digest != LLVM_SHA256:
        sys.exit(f"LLVM tarball hash mismatch: {digest} (pinned {LLVM_SHA256})")
    src = os.path.join(work, f"llvm-project-{LLVM_VERSION}.src")
    if not os.path.isfile(os.path.join(src, ".complete")):
        # Only the parts the build reads; the rest (clang tests among them) holds symlinks
        # that point outside the tree and gigabytes the snapshot never uses.
        top = f"llvm-project-{LLVM_VERSION}.src/"
        keep = tuple(top + d for d in ("llvm/", "cmake/", "third-party/"))
        log("extracting llvm/, cmake/ and third-party/")
        with tarfile.open(tarball) as t:
            members = [m for m in t.getmembers() if m.name.startswith(keep) and not m.name.startswith(top + "llvm/test/")]
            if hasattr(tarfile, "data_filter"):
                t.extractall(work, members=members, filter="data")
            else:
                t.extractall(work, members=members)
        open(os.path.join(src, ".complete"), "w").close()
    return src, digest


def host_build(src, host, clang, clangxx, libs, tools, jobs):
    """Configures LLVM natively and builds the selected targets, which generates every .inc they need."""
    if not os.path.exists(os.path.join(host, "build.ninja")):
        run(["cmake", "-G", "Ninja", "-S", os.path.join(src, "llvm"), "-B", host,
             "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
             f"-DCMAKE_C_COMPILER={clang}", f"-DCMAKE_CXX_COMPILER={clangxx}",
             "-DLLVM_TARGETS_TO_BUILD=AArch64", "-DLLVM_ENABLE_PROJECTS=",
             "-DLLVM_INCLUDE_TESTS=OFF", "-DLLVM_INCLUDE_BENCHMARKS=OFF", "-DLLVM_INCLUDE_EXAMPLES=OFF",
             "-DLLVM_ENABLE_ZLIB=OFF", "-DLLVM_ENABLE_ZSTD=OFF", "-DLLVM_ENABLE_LIBXML2=OFF",
             "-DLLVM_ENABLE_TERMINFO=OFF", "-DLLVM_ENABLE_LIBEDIT=OFF", "-DLLVM_ENABLE_LIBPFM=OFF",
             "-DLLVM_ENABLE_ASSERTIONS=OFF", "-DLLVM_APPEND_VC_REV=OFF"])
    run(["ninja", "-C", host, f"-j{jobs}"] + libs + tools)


def target_config(src, host, dest, clang, clangxx, sysroot):
    """Configures LLVM for the aarch64 target against the sysroot (configure only).

    The host build's llvm/Config headers describe the host C library; the units must be compiled
    with headers that describe the target, so the feature checks run again with the cross compiler.
    """
    if os.path.isfile(os.path.join(dest, "include", "llvm", "Config", "config.h")):
        return
    lld = shutil.which("ld.lld") or shutil.which("ld.lld-19") or (
        "/usr/lib/llvm-19/bin/ld.lld" if os.path.exists("/usr/lib/llvm-19/bin/ld.lld") else None)
    cmd = ["cmake", "-G", "Ninja", "-S", os.path.join(src, "llvm"), "-B", dest, "-DCMAKE_BUILD_TYPE=Release",
           "-DCMAKE_SYSTEM_NAME=Linux", "-DCMAKE_SYSTEM_PROCESSOR=aarch64",
           f"-DCMAKE_C_COMPILER={clang}", f"-DCMAKE_CXX_COMPILER={clangxx}",
           f"-DCMAKE_C_COMPILER_TARGET={TARGET}", f"-DCMAKE_CXX_COMPILER_TARGET={TARGET}",
           f"-DCMAKE_SYSROOT={sysroot}", f"-DLLVM_HOST_TRIPLE={TARGET}", "-DLLVM_TARGETS_TO_BUILD=AArch64",
           f"-DLLVM_TABLEGEN={host}/bin/llvm-tblgen", f"-DLLVM_NATIVE_TOOL_DIR={host}/bin",
           "-DLLVM_ENABLE_PROJECTS=", "-DLLVM_INCLUDE_TESTS=OFF", "-DLLVM_INCLUDE_BENCHMARKS=OFF",
           "-DLLVM_INCLUDE_EXAMPLES=OFF", "-DLLVM_ENABLE_ZLIB=OFF", "-DLLVM_ENABLE_ZSTD=OFF",
           "-DLLVM_ENABLE_LIBXML2=OFF", "-DLLVM_ENABLE_TERMINFO=OFF", "-DLLVM_ENABLE_LIBEDIT=OFF",
           "-DLLVM_ENABLE_LIBPFM=OFF", "-DLLVM_ENABLE_ASSERTIONS=OFF", "-DLLVM_APPEND_VC_REV=OFF"]
    if lld:
        cmd += [f"-DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld", f"-DCMAKE_SHARED_LINKER_FLAGS=-fuse-ld=lld"]
    else:
        # Without a cross linker, checks compile but do not link: header-based checks stay exact.
        cmd += ["-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY"]
        log("WARNING: no lld found; target feature checks are compile-only")
    run(cmd, stdout=subprocess.DEVNULL)


GEN_SUFFIXES = (".h", ".inc", ".def", ".gen", ".td")


def make_gen(host, target, gen):
    """Generated sources for the snapshot: host tablegen output, target llvm/Config headers."""
    shutil.rmtree(gen, ignore_errors=True)
    for root, dirs, files in os.walk(host):
        dirs[:] = [d for d in dirs if d not in ("CMakeFiles", "bin", "lib64") or root != host]
        for f in files:
            if f.endswith(GEN_SUFFIXES):
                src = os.path.join(root, f)
                dst = os.path.join(gen, os.path.relpath(src, host))
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                shutil.copyfile(src, dst)
    cfg_src = os.path.join(target, "include", "llvm", "Config")
    for f in os.listdir(cfg_src):
        shutil.copyfile(os.path.join(cfg_src, f), os.path.join(gen, "include", "llvm", "Config", f))


def owner(output):
    m = re.search(r"CMakeFiles/([^/]+)\.dir/", output)
    return m.group(1) if m else None


def link_inputs(host, tool):
    """Libraries (in link order) and object-library owners of a host executable, from its link command."""
    out = subprocess.run(["ninja", "-C", host, "-t", "commands", f"bin/{tool}"], check=True,
                         capture_output=True, text=True).stdout.strip().splitlines()[-1]
    libs = []
    for tok in shlex.split(out):
        base = os.path.basename(tok)
        m = re.match(r"lib(LLVM\w+)\.a$", base)
        name = m.group(1) if m else (owner(tok) if tok.endswith(".o") else None)
        if name and name != tool and name not in libs:
            libs.append(name)
    return libs


def search_dirs(clang, sysroot, lang):
    """The include search list the real driver uses for this target and sysroot."""
    p = subprocess.run([clang, f"--target={TARGET}", f"--sysroot={sysroot}", "-E", "-x", lang, "-", "-v"],
                       input="", capture_output=True, text=True)
    lines = p.stderr.splitlines()
    try:
        start = lines.index("#include <...> search starts here:") + 1
        end = lines.index("End of search list.")
    except ValueError:
        sys.exit(f"cannot read the include search list of {clang}:\n{p.stderr}")
    return [os.path.normpath(l.strip().split(" (")[0]) for l in lines[start:end]]


class Mapper:
    """Maps real paths into the virtual root and back."""

    def __init__(self, roots):
        self.roots = sorted(((os.path.normpath(r), v) for r, v in roots), key=lambda x: -len(x[0]))

    def virt(self, path):
        p = os.path.normpath(path)
        for real, v in self.roots:
            if p == real or p.startswith(real + os.sep):
                return v + p[len(real):].replace(os.sep, "/")
        return None


def unit_args(entry, mapper, isystem, lang):
    """Rewrites a host compile command into the pinned, portable form (virtual paths)."""
    args = entry.get("arguments") or shlex.split(entry["command"])
    out, skip = [], False
    for i in range(1, len(args)):
        a = args[i]
        if skip:
            skip = False
            continue
        if a in DROP_WITH_VALUE:
            skip = True
            continue
        if a in DROP_EXACT or a == entry["file"] or os.path.normpath(os.path.join(entry["directory"], a)) == entry["file"]:
            continue
        if a.startswith(DROP_PREFIX) and not a.startswith("-Wno-"):
            continue
        if a.startswith("-I"):
            path = a[2:] or args[i + 1]
            v = mapper.virt(os.path.join(entry["directory"], path))
            if v is None:
                sys.exit(f"include path outside the snapshot: {path}")
            out.append("-I" + v)
            if not a[2:]:
                skip = True
            continue
        out.append(a)
    driver = "clang" if lang == "c" else "clang++"
    head = [driver, f"--target={TARGET}", "-nostdinc"] + [x for d in isystem for x in ("-isystem", d)]
    src = mapper.virt(entry["file"])
    return head + ["-O2", "-w"] + out + ["-c", src]


def to_real(args, vroot_real):
    return [a.replace(VROOT, vroot_real) for a in args]


def deps_of(clang_c, clangxx, real_args):
    exe = clang_c if real_args[0] == "clang" else clangxx
    p = subprocess.run([exe] + real_args[1:] + ["-M", "-MF", "-", "-o", os.devnull], capture_output=True, text=True)
    if p.returncode:
        raise RuntimeError(p.stderr[-2000:])
    text = p.stdout.replace("\\\n", " ")
    return [t for t in text.split(":", 1)[1].split()]


def cost_of(clang_c, clangxx, real_args):
    exe = clang_c if real_args[0] == "clang" else clangxx
    t0 = time.perf_counter()
    p = subprocess.run([exe] + real_args[1:] + ["-o", os.devnull], capture_output=True, text=True)
    if p.returncode:
        raise RuntimeError(f"{real_args[-1]} does not compile for {TARGET}:\n{p.stderr[-3000:]}")
    return (time.perf_counter() - t0) * 1e6


def copy_link_sysroot(sysroot, dest):
    """Crt objects and libraries the cross linker needs; headers stay in root/sysroot."""
    for rel in ("lib", f"lib/{TARGET}", f"usr/lib/{TARGET}", "usr/lib/gcc"):
        s = os.path.join(sysroot, rel)
        if os.path.isdir(s) and not os.path.islink(s):
            for dirpath, dirs, files in os.walk(s):
                for f in files:
                    if f.endswith((".o", ".a", ".so")) or ".so." in f:
                        src = os.path.join(dirpath, f)
                        dst = os.path.join(dest, os.path.relpath(src, sysroot))
                        os.makedirs(os.path.dirname(dst), exist_ok=True)
                        shutil.copy2(src, dst, follow_symlinks=True)
                if rel in ("lib",):
                    break  # only the top level of lib/
        elif os.path.islink(s):
            os.makedirs(os.path.dirname(os.path.join(dest, rel)), exist_ok=True)
            if not os.path.lexists(os.path.join(dest, rel)):
                os.symlink(os.readlink(s), os.path.join(dest, rel))


CMAKE_HEADER = """# Generated by tools/k1x/prepare.py. K1X_ROOT is the snapshot's root/ directory.
cmake_minimum_required(VERSION 3.20)
project(k1x LANGUAGES C CXX)
if(NOT K1X_ROOT)
  message(FATAL_ERROR "pass -DK1X_ROOT=<snapshot>/root")
endif()
"""

TOOLCHAIN = """# Generated by tools/k1x/prepare.py: cross-build to {target} with the pinned Clang.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
find_program(K1X_CC NAMES {cc} REQUIRED)
find_program(K1X_CXX NAMES {cxx} REQUIRED)
set(CMAKE_C_COMPILER ${{K1X_CC}})
set(CMAKE_CXX_COMPILER ${{K1X_CXX}})
set(CMAKE_C_COMPILER_TARGET {target})
set(CMAKE_CXX_COMPILER_TARGET {target})
get_filename_component(_snap "${{CMAKE_CURRENT_LIST_DIR}}/.." ABSOLUTE)
set(CMAKE_SYSROOT "${{_snap}}/link-sysroot")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
# Units carry their complete flags; nothing else may be added.
set(CMAKE_C_FLAGS_RELEASE "" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS_RELEASE "" CACHE STRING "" FORCE)
"""


def _terminate(signum, frame):
    raise KeyboardInterrupt


def main():
    import signal
    signal.signal(signal.SIGTERM, _terminate)
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--sysroot", default="auto",
                    help="aarch64 Linux sysroot, or 'auto' for the pinned one (default)")
    ap.add_argument("--out", default=os.path.join(default_data_dir(), "k1x"),
                    help="snapshot directory to create (default: the app's data folder)")
    ap.add_argument("--force", action="store_true", help="replace an existing snapshot")
    ap.add_argument("--work", help="work directory for the LLVM source and host build (default: <out>.work)")
    ap.add_argument("--llvm-src", help="existing llvm-project source tree (skips the download)")
    ap.add_argument("--clang", default="clang-19")
    ap.add_argument("--clangxx", default="clang++-19")
    ap.add_argument("--libs", nargs="*", default=DEFAULT_LIBS)
    ap.add_argument("--tools", nargs="*", default=DEFAULT_TOOLS)
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) // 2),
                    help="parallel cost measurements (fewer is more accurate)")
    ap.add_argument("--max-threads", type=int, default=64, help="largest n the granularity rule must hold for")
    a = ap.parse_args()

    out = os.path.abspath(a.out)
    work = os.path.abspath(a.work or out + ".work")
    os.makedirs(work, exist_ok=True)
    if os.path.exists(out):
        if not a.force:
            sys.exit(f"{out} exists; pass --force to replace it")
        shutil.rmtree(out)
    for tool in (a.clang, a.clangxx, "cmake", "ninja"):
        if not shutil.which(tool):
            sys.exit(f"{tool} not found in PATH")

    step(1, "aarch64 sysroot")
    sysroot = fetch_sysroot(work) if a.sysroot == "auto" else os.path.abspath(a.sysroot)

    clang_version = subprocess.run([a.clang, "--version"], capture_output=True, text=True, check=True).stdout.splitlines()[0]
    resource = subprocess.run([a.clang, "-print-resource-dir"], capture_output=True, text=True, check=True).stdout.strip()
    step(2, f"LLVM {LLVM_VERSION} sources")
    src, digest = fetch_llvm(work, a.llvm_src)
    host = os.path.join(work, "host")
    step(3, "host build of the subset (generates the tablegen sources)")
    host_build(src, host, a.clang, a.clangxx, a.libs, a.tools, os.cpu_count() or 4)

    target = os.path.join(work, "target-config")
    target_config(src, host, target, a.clang, a.clangxx, sysroot)
    gen = os.path.join(work, "gen")
    make_gen(host, target, gen)

    targets = list(a.libs)
    tool_libs = {}
    for t in a.tools:
        tool_libs[t] = link_inputs(host, t)
        for lib in tool_libs[t]:
            if lib not in targets:
                log(f"adding {lib}, needed to link {t}")
                targets.append(lib)

    mapper = Mapper([(src, VROOT + "/src"), (gen, VROOT + "/gen"), (sysroot, VROOT + "/sysroot"),
                     (resource, VROOT + "/resource")])
    isys = {lang: [] for lang in ("c", "c++")}
    for lang in isys:
        for d in search_dirs(a.clang if lang == "c" else a.clangxx, sysroot, lang):
            v = mapper.virt(d)
            if v is None:
                sys.exit(f"system include directory outside the sysroot and resource dir: {d}")
            isys[lang].append(v)

    with open(os.path.join(host, "compile_commands.json")) as f:
        db = json.load(f)
    units = []
    for e in db:
        tgt = owner(e.get("output") or "")
        if tgt not in targets and tgt not in a.tools:
            continue
        # Generated headers come from gen/ (target config), not from the host build tree.
        if "command" in e:
            e["command"] = e["command"].replace(host + "/", gen + "/")
        if "arguments" in e:
            e["arguments"] = [x.replace(host + "/", gen + "/") for x in e["arguments"]]
        e["directory"] = e["directory"].replace(host, gen)
        e["file"] = os.path.normpath(os.path.join(e["directory"], e["file"]))
        lang = "c" if e["file"].endswith(".c") else "c++"
        units.append({"target": tgt, "lang": lang, "args": unit_args(e, mapper, isys[lang], lang)})
    if not units:
        sys.exit("no translation units selected")
    log(f"{len(units)} translation units in {len(targets)} libraries and {len(a.tools)} tools")

    vreal = os.path.join(out, "root")
    files = set()

    def real_of(u):
        return [x.replace(VROOT + "/src", src).replace(VROOT + "/gen", gen)
                 .replace(VROOT + "/sysroot", sysroot).replace(VROOT + "/resource", resource) for x in u["args"]]

    step(4, f"header closures of {len(units)} translation units")
    with cf.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        for u, deps in zip(units, pool.map(lambda u: deps_of(a.clang, a.clangxx, real_of(u)), units)):
            for d in deps:
                v = mapper.virt(d if os.path.isabs(d) else os.path.join(gen, d))
                if v is None:
                    sys.exit(f"{u['args'][-1]} depends on a file outside the snapshot: {d}")
                files.add(v)

    step(5, f"compile cost of {len(units)} units ({a.jobs} parallel)")
    with cf.ThreadPoolExecutor(max_workers=a.jobs) as pool:
        for u, c in zip(units, pool.map(lambda u: cost_of(a.clang, a.clangxx, real_of(u)), units)):
            u["cost_us"] = round(c)
    total = sum(u["cost_us"] for u in units)
    largest = max(u["cost_us"] for u in units)
    ratio = total / largest
    need = 4 * a.max_threads
    log(f"granularity: sum T_i / T_max = {ratio:.0f} (required >= {need} for n_max = {a.max_threads})")
    if ratio < need:
        log("WARNING: the granularity rule is not met; add libraries or lower --max-threads")

    step(6, f"copying {len(files)} files into the snapshot")
    inv = [(v, r) for r, v in mapper.roots]
    for v in sorted(files):
        real = next(r + v[len(vv):] for vv, r in sorted(inv, key=lambda x: -len(x[0])) if v.startswith(vv))
        dst = os.path.join(vreal, v[len(VROOT) + 1:])
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copyfile(real, dst)
    copy_link_sysroot(sysroot, os.path.join(out, "link-sysroot"))

    step(7, "K1x build project and manifest")
    # K1x build project: same units, VROOT replaced by ${K1X_ROOT}.
    bdir = os.path.join(out, "build")
    os.makedirs(bdir)
    lines = [CMAKE_HEADER]
    for tgt in targets + a.tools:
        mine = [u for u in units if u["target"] == tgt]
        if not mine:
            continue
        kind = "add_executable" if tgt in a.tools else "add_library"
        extra = "" if tgt in a.tools else " STATIC"
        srcs = " ".join('"' + u["args"][-1].replace(VROOT, "${K1X_ROOT}") + '"' for u in mine)
        lines.append(f"{kind}({tgt}{extra} {srcs})")
        for u in mine:
            opts = [x.replace(VROOT, "${K1X_ROOT}") for x in u["args"][1:-2]]  # drop driver, -c, file
            lines.append('set_source_files_properties("{}" PROPERTIES COMPILE_OPTIONS "{}")'.format(
                u["args"][-1].replace(VROOT, "${K1X_ROOT}"), ";".join(o.replace('"', '\\"') for o in opts)))
        if tgt in a.tools:
            lines.append(f"target_link_libraries({tgt} {' '.join(tool_libs[tgt])} pthread dl m)")
    with open(os.path.join(bdir, "CMakeLists.txt"), "w") as f:
        f.write("\n".join(lines) + "\n")
    cc_name = os.path.basename(a.clang)
    with open(os.path.join(bdir, "toolchain.cmake"), "w") as f:
        f.write(TOOLCHAIN.format(target=TARGET, cc=cc_name, cxx=os.path.basename(a.clangxx)))

    manifest = {
        "schema": "prismark-k1x/1",
        "root": VROOT,
        "llvm_version": LLVM_VERSION,
        "llvm_source_sha256": digest,
        "sysroot": {"sha256": SYSROOT_SHA256 if a.sysroot == "auto" else None,
                    "source": SYSROOT_URL if a.sysroot == "auto" else sysroot},
        "clang": clang_version,
        "target": TARGET,
        "libraries": targets,
        "tools": a.tools,
        "granularity": {"sum_over_max": ratio, "required": need, "n_max": a.max_threads},
        "total_cost_us": total,
        "units": [{"file": u["args"][-1], "target": u["target"], "args": u["args"], "cost_us": u["cost_us"]}
                  for u in sorted(units, key=lambda u: (-u["cost_us"], u["args"][-1]))],
    }
    with open(os.path.join(out, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=1)
    log(f"snapshot ready: {out} ({len(units)} units, {len(files)} files, total cost {total / 1e6:.0f} s)")
    log(f"run with: prismark --k1-data {out}")


if __name__ == "__main__":
    main()
