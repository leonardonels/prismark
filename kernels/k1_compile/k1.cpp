/*
 * K1 — compile, in-process. Clang/LLVM linked as a library compiles the K1x
 * translation units at -O2 to aarch64 object files, entirely in memory: the
 * snapshot (sources, pre-generated headers, sysroot and Clang resource
 * headers) is loaded into an in-memory file system and objects are written
 * to memory, so no subprocess and no file-system access is timed. Each
 * thread has its own compiler instance and file-system view; the file
 * contents are shared read-only. Units are ordered largest first (LPT).
 *
 * The object files are deterministic for a given Clang version and target,
 * so the checksum is identical on every host.
 *
 * Snapshot layout (tools/k1x/prepare.py):
 *   manifest.json   {"schema": "prismark-k1x/1", "root": "/k1x",
 *                    "units": [{"file": ..., "args": [...], "cost_us": ...}]}
 *   root/...        the files, mounted at "root" in the virtual file system
 *
 * Copyright 2026 The Prismark Authors. Apache-2.0.
 */
#include <algorithm>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <clang/Basic/Diagnostic.h>
#include <clang/Basic/DiagnosticOptions.h>
#include <clang/CodeGen/CodeGenAction.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/CompilerInvocation.h>
#include <clang/Frontend/TextDiagnosticBuffer.h>
#include <clang/Frontend/Utils.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/VirtualFileSystem.h>
#include <llvm/Support/raw_ostream.h>

extern "C" {
#include "kernels.h"
}

namespace {

constexpr const char *kSchema = "prismark-k1x/1";
constexpr double kWorkPerBuild = 1e6; /* task work is scaled so one full build is 1e6 units */

struct Unit {
  std::string file;
  std::vector<std::string> args;
  double cost_us = 1;
  uint64_t work = 1;
};

struct K1 {
  std::string root; /* virtual mount point, e.g. "/k1x" */
  std::vector<Unit> units;
  std::vector<std::pair<std::string, std::unique_ptr<llvm::MemoryBuffer>>> files;
  uint64_t hash = 0;
};

struct Scratch {
  llvm::IntrusiveRefCntPtr<llvm::vfs::InMemoryFileSystem> fs;
};

void init_llvm() {
  static std::once_flag once;
  std::call_once(once, [] {
    LLVMInitializeAArch64TargetInfo();
    LLVMInitializeAArch64Target();
    LLVMInitializeAArch64TargetMC();
    LLVMInitializeAArch64AsmPrinter();
    LLVMInitializeAArch64AsmParser(); /* units with inline assembly need it to emit objects */
  });
}

void fail(const pmk_tk_args *a, const std::string &msg) { snprintf(a->err, a->errlen, "%s", msg.c_str()); }

bool load_manifest(K1 &k, const std::string &dir, const pmk_tk_args *a) {
  auto buf = llvm::MemoryBuffer::getFile(dir + "/manifest.json");
  if (!buf) return fail(a, "no manifest.json in the K1 snapshot " + dir), false;
  auto doc = llvm::json::parse((*buf)->getBuffer());
  if (!doc) {
    fail(a, "manifest.json: " + llvm::toString(doc.takeError()));
    return false;
  }
  const llvm::json::Object *o = doc->getAsObject();
  if (!o || o->getString("schema") != kSchema) return fail(a, "manifest.json: unexpected schema"), false;
  k.root = o->getString("root").value_or("/k1x").str();
  const llvm::json::Array *units = o->getArray("units");
  if (!units || units->empty()) return fail(a, "manifest.json: no units"), false;
  for (const llvm::json::Value &v : *units) {
    const llvm::json::Object *u = v.getAsObject();
    const llvm::json::Array *args = u ? u->getArray("args") : nullptr;
    if (!u || !args) return fail(a, "manifest.json: malformed unit"), false;
    Unit unit;
    unit.file = u->getString("file").value_or("").str();
    unit.cost_us = u->getNumber("cost_us").value_or(1.0);
    for (const llvm::json::Value &arg : *args)
      if (auto s = arg.getAsString()) unit.args.push_back(s->str());
    k.units.push_back(std::move(unit));
  }
  /* LPT: largest units first, ties broken by name so the order is identical everywhere. */
  std::sort(k.units.begin(), k.units.end(), [](const Unit &x, const Unit &y) {
    return x.cost_us != y.cost_us ? x.cost_us > y.cost_us : x.file < y.file;
  });
  double total = 0;
  for (const Unit &u : k.units) total += u.cost_us;
  for (Unit &u : k.units) {
    u.work = (uint64_t)(u.cost_us / total * kWorkPerBuild + 0.5);
    if (!u.work) u.work = 1;
  }
  k.hash = pmk_hash_bytes((*buf)->getBufferStart(), (*buf)->getBufferSize(), 1);
  return true;
}

bool load_files(K1 &k, const std::string &dir, const pmk_tk_args *a) {
  std::string base = dir + "/root";
  std::error_code ec;
  std::vector<std::string> paths;
  for (llvm::sys::fs::recursive_directory_iterator it(base, ec), end; it != end && !ec; it.increment(ec)) {
    if (it->type() == llvm::sys::fs::file_type::regular_file) paths.push_back(it->path());
  }
  if (ec) return fail(a, "reading the K1 snapshot: " + ec.message()), false;
  std::sort(paths.begin(), paths.end()); /* fixed order for the input hash */
  for (const std::string &p : paths) {
    auto buf = llvm::MemoryBuffer::getFile(p, /*IsText=*/false, /*RequiresNullTerminator=*/true);
    if (!buf) return fail(a, "cannot read " + p), false;
    std::string rel = p.substr(base.size());
    std::replace(rel.begin(), rel.end(), '\\', '/');
    k.hash = pmk_hash_bytes(rel.data(), rel.size(), k.hash);
    k.hash = pmk_hash_bytes((*buf)->getBufferStart(), (*buf)->getBufferSize(), k.hash);
    k.files.emplace_back(k.root + rel, std::move(*buf));
  }
  return !k.files.empty() || (fail(a, "the K1 snapshot has no files"), false);
}

}  // namespace

extern "C" {

static void *k1_create(const pmk_tk_args *a) {
  if (!a->data_dir || !*a->data_dir) {
    fail(a, "no K1/K1x snapshot given (prepare one with tools/k1x/prepare.py, pass --k1-data)");
    return nullptr;
  }
  auto k = std::make_unique<K1>();
  if (!load_manifest(*k, a->data_dir, a) || !load_files(*k, a->data_dir, a)) return nullptr;
  if (a->size == PMK_SIZE_BURST) { /* every 8th unit in LPT order, as K1x's quick build (src/engine/k1x.c) */
    std::vector<Unit> sub;
    for (size_t i = 0; i < k->units.size(); i += PMK_K1_QUICK_STRIDE) sub.push_back(k->units[i]);
    k->units = std::move(sub);
  }
  init_llvm();
  return k.release();
}

static void k1_destroy(void *p) { delete static_cast<K1 *>(p); }
static size_t k1_ntasks(const void *p) { return static_cast<const K1 *>(p)->units.size(); }
static uint64_t k1_task_work(const void *p, size_t i) { return static_cast<const K1 *>(p)->units[i].work; }

static void *k1_scratch_new(const void *p) {
  const K1 *k = static_cast<const K1 *>(p);
  auto *s = new Scratch;
  s->fs = llvm::makeIntrusiveRefCnt<llvm::vfs::InMemoryFileSystem>();
  for (const auto &f : k->files)
    s->fs->addFile(f.first, 0, llvm::MemoryBuffer::getMemBuffer(f.second->getMemBufferRef(), false));
  s->fs->setCurrentWorkingDirectory(k->root);
  return s;
}

static void k1_scratch_free(void *p) { delete static_cast<Scratch *>(p); }

static uint64_t k1_task(const void *p, void *sp, size_t i) {
  const K1 *k = static_cast<const K1 *>(p);
  Scratch *s = static_cast<Scratch *>(sp);
  const Unit &u = k->units[i];
  std::vector<const char *> argv;
  for (const std::string &arg : u.args) argv.push_back(arg.c_str());

  auto diag_opts = llvm::makeIntrusiveRefCnt<clang::DiagnosticOptions>();
  auto *diag_buf = new clang::TextDiagnosticBuffer;
  auto diags = clang::CompilerInstance::createDiagnostics(diag_opts.get(), diag_buf, true);
  /* A unit that does not compile is a broken snapshot, not a result: say so once, loudly. */
  auto fail_unit = [&](const char *stage) -> uint64_t {
    static std::once_flag once;
    std::call_once(once, [&] {
      std::string msg;
      for (auto it = diag_buf->err_begin(); it != diag_buf->err_end() && msg.size() < 2000; ++it)
        msg += "  " + it->second + "\n";
      fprintf(stderr, "K1: %s failed for %s\n%s", stage, u.file.c_str(), msg.c_str());
    });
    return 0;
  };
  clang::CreateInvocationOptions opts;
  opts.Diags = diags;
  opts.VFS = s->fs;
  std::shared_ptr<clang::CompilerInvocation> inv = clang::createInvocation(argv, opts);
  if (!inv) return fail_unit("the driver");

  clang::CompilerInstance ci;
  ci.setInvocation(std::move(inv));
  ci.setDiagnostics(diags.get());
  ci.createFileManager(s->fs);
  llvm::SmallVector<char, 0> obj;
  ci.setOutputStream(std::make_unique<llvm::raw_svector_ostream>(obj));
  clang::EmitObjAction action;
  if (!ci.ExecuteAction(action) || obj.empty()) return fail_unit("compiling");
  return pmk_hash_bytes(obj.data(), obj.size(), obj.size());
}

static uint64_t k1_input_hash(const void *p) { return static_cast<const K1 *>(p)->hash; }

extern const pmk_tk PMK_SYM(pmk_k1_tk);
const pmk_tk PMK_SYM(pmk_k1_tk) = {
    "K1",
    nullptr,
    "in-process compile (Clang, aarch64, -O2)",
    "builds/h",
    kWorkPerBuild / 3600.0,
    k1_create,
    k1_destroy,
    k1_ntasks,
    k1_task_work,
    k1_scratch_new,
    k1_scratch_free,
    k1_task,
    k1_input_hash,
};

}  // extern "C"
