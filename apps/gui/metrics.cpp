/* Copyright 2026 The Prismark Authors. Apache-2.0. */
#include "metrics.h"

#include <QDateTime>
#include <QRegularExpression>
#include <QLocale>
#include <algorithm>

namespace {

QJsonArray series(const QJsonObject &d) { return d["analysis"].toObject()["series"].toArray(); }

std::optional<Value> ci(const QJsonValue &v, double k = 1) {
  QJsonObject o = v.toObject();
  if (!o.contains("est") || o["est"].isNull()) return std::nullopt;
  return Value{o["est"].toDouble() * k, o["lo"].toDouble(NAN) * k, o["hi"].toDouble(NAN) * k};
}

bool fast(const QJsonObject &r) { return r["core_type"].toString() == "P"; }

QJsonObject largestN(const QVector<QJsonObject> &rows) {
  QJsonObject best;
  for (const QJsonObject &r : rows)
    if (best.isEmpty() || r["n"].toInt() > best["n"].toInt()) best = r;
  return best;
}

/* Marks an all-cores value measured on fewer threads than the machine has, so it is not shown as all cores. */
void markThreads(std::optional<Value> &v, const QJsonObject &doc, const QJsonObject &row) {
  int all = doc["machine"].toObject()["cpu"].toObject()["cores"].toArray().size(), n = row["n"].toInt();
  if (v && all > 0 && n > 0 && n < all) {
    v->threads = n;
    v->ofThreads = all;
  }
}

QVector<QJsonObject> filter(const QJsonArray &a, const std::function<bool(const QJsonObject &)> &f) {
  QVector<QJsonObject> out;
  for (const QJsonValue &v : a)
    if (f(v.toObject())) out.push_back(v.toObject());
  return out;
}

std::function<std::optional<Value>(const QJsonObject &)> throughput(QString kernel, QString mode) {
  return [=](const QJsonObject &d) -> std::optional<Value> {
    auto rows = filter(series(d), [&](const QJsonObject &r) {
      return r["kernel"].toString() == kernel && r["mode"].toString() == mode && r["tier"].toString() == "baseline" &&
             r["purpose"].isNull();
    });
    QJsonObject r;
    if (mode == "st_sustained") {
      for (const auto &x : rows)
        if (fast(x)) { r = x; break; }
    } else {
      r = largestN(rows);
    }
    if (r.isEmpty()) return std::nullopt;
    auto v = ci(r["perf_steady"]);
    if (v) v->unsettled = r.contains("steady_reached") && !r["steady_reached"].toBool();
    if (mode == "mc_threaded") markThreads(v, d, r);
    if (v && r["clamped_windows"].toInt() > 0) {
      v->clamped = r["clamped_windows"].toInt();
      v->clampedMhz = r["clamped_lowest_mhz"].toDouble(NAN);
    }
    return v;
  };
}

std::function<std::optional<Value>(const QJsonObject &)> medianOf(QString kernel, QString mode, QString start, double k) {
  return [=](const QJsonObject &d) -> std::optional<Value> {
    for (const QJsonValue &v : series(d)) {
      QJsonObject r = v.toObject();
      if (r["kernel"].toString() == kernel && r["mode"].toString() == mode && fast(r) &&
          (start.isEmpty() || r["start"].toString() == start))
        return ci(r["median"], k);
    }
    return std::nullopt;
  };
}

std::function<std::optional<Value>(const QJsonObject &)> k7(bool all) {
  return [=](const QJsonObject &d) -> std::optional<Value> {
    auto rows = filter(series(d), [](const QJsonObject &r) { return r["kernel"].toString() == "K7"; });
    if (rows.isEmpty()) return std::nullopt;
    double ws = 0;
    for (const auto &r : rows) ws = std::max(ws, r["working_set_bytes"].toDouble());
    QVector<QJsonObject> at;
    for (const auto &r : rows)
      if (r["working_set_bytes"].toDouble() == ws) at.push_back(r);
    QJsonObject r;
    if (all) r = largestN(at);
    else
      for (const auto &x : at)
        if (x["n"].toInt() == 1) r = x;
    return r.isEmpty() ? std::nullopt : ci(r["median"]);
  };
}

std::function<std::optional<Value>(const QJsonObject &)> periodic(QString condition) {
  return [=](const QJsonObject &d) -> std::optional<Value> {
    for (const QJsonValue &v : d["analysis"].toObject()["periodic"].toArray()) {
      QJsonObject r = v.toObject();
      if (r["condition"].toString() == condition && !r["p999_ns"].isNull()) return Value{r["p999_ns"].toDouble() / 1e3};
    }
    return std::nullopt;
  };
}

/* A stress-test part's score: the speed in its 10th minute; none for a part that ran less than 10 minutes. */
std::function<std::optional<Value>(const QJsonObject &)> stress(QString part) {
  return [=](const QJsonObject &d) -> std::optional<Value> {
    for (const QJsonValue &v : d["analysis"].toObject()["stress"].toArray()) {
      QJsonObject r = v.toObject();
      if (r["part"].toString() == part) return ci(r["score"]);
    }
    return std::nullopt;
  };
}

/* Files from before ABI 4 may also hold series measured with changed power settings; only "as configured" counts. */
bool asConfigured(const QJsonObject &r) { return !r.contains("cell") || r["cell"].toString() == "as_shipped"; }

std::function<std::optional<Value>(const QJsonObject &)> rresp(double w) {
  return [=](const QJsonObject &d) -> std::optional<Value> {
    for (const QJsonValue &v : d["analysis"].toObject()["cold_burst"].toArray()) {
      QJsonObject r = v.toObject();
      if (r["W_ms"].toDouble() == w && fast(r) && asConfigured(r))
        return ci(r["R_resp"], 100); /* shown as a percentage of busy-core speed */
    }
    return std::nullopt;
  };
}

}  // namespace

const QVector<Group> &groups() {
  static const QVector<Group> g = {
      {"mc", "All cores",
       "Heavy jobs that use every core at once, measured after a minute of full load has warmed the CPU up."},
      {"st", "One core",
       "How fast a single core is: short everyday tasks, and long tasks once the core has warmed up."},
      {"mem", "Memory && timing",
       "How long the CPU waits for main memory, and how punctually it wakes up for tasks that run on a fixed "
       "schedule (audio, games, control loops)."},
      {"resp", "Responsiveness",
       "How quickly the computer reacts when work arrives while it is resting. Idle CPUs sleep and slow their "
       "clock to save power, and waking up costs time."},
      {"stress", "Stress test",
       "Ten minutes or more of full load, as in a long render or a game: how well the speed holds up as the "
       "computer heats up, and whether every result stays correct. Run each test on its own; Run all leaves them out."},
  };
  return g;
}

const QVector<Metric> &metrics() {
  static const QString kSettle =
      "In a full run the processor first works at full load for a minute, so it is as hot (and, if it throttles, as "
      "slow) as long use makes it; then the test is measured for 20 seconds (compiling: 60). Every computer gets "
      "exactly the same warm-up and measurement. Quick runs skip the warm-up and measure a few seconds.";
  static const QString kStressHow =
      "Full load without a pause for the length you choose: 10 minutes, 30 minutes, or until you stop it. Speed, "
      "clock, temperature and power are recorded every 5 seconds, and every result is checked against the first. "
      "The score is the speed in the 10th minute, so runs of any length compare; a run stopped before 10 minutes "
      "keeps its curve but has no score.";
  static const QString kBurstHow =
      "The task is repeated on a core that is already running, until its time is known to within about 1 %.";
  static const QString kRestHow =
      "Before each task the processor rests for a random 50–500 ms. The time is counted from the moment the task "
      "was due, so the wake-up delay is included. Keep your hands off the keyboard and mouse while it runs: input "
      "skips the test.";
  static const QVector<Metric> m = {
      /* All cores */
      {"mc_k2", "mc", "3D rendering", "3D rendering on all cores", "All cores share one image", "M samples/s",
       "How much rendering work the whole processor gets done when every core helps with the same image, as video "
       "and 3D rendering software do.",
       "A small ray tracer draws a fixed scene over and over, every core working on each frame. " + kSettle,
       "Higher is better. Measured in millions of light samples traced per second.", "scaling:K2", true,
       {"mc_threaded"}, "K2", throughput("K2", "mc_threaded")},
      {"mc_k1", "mc", "Compiling code", "Compiling code on all cores", "All cores compile C++ in memory", "builds/h",
       "How quickly the processor compiles a fixed set of C++ source files using every core: the heart of a "
       "programmer’s build.",
       "Prismark contains the Clang compiler and compiles the files entirely in memory, with no disk access and no "
       "other programs, so only the processor is measured. Needs a one-time setup. " + kSettle,
       "Higher is better. Measured in complete builds per hour.", "scaling:K1", true, {"mc_threaded"}, "K1",
       throughput("K1", "mc_threaded")},
      {"mc_k1x", "mc", "Full software build", "Full software build with standard tools", "Real build: CMake + Ninja",
       "s",
       "How long a complete build of a fixed part of the LLVM project takes with the usual tools (CMake and Ninja), "
       "including starting programs, reading files and linking.",
       "The same source files as “Compiling code”, built from a memory disk on all cores, three times. Needs a "
       "one-time setup.",
       "Lower is better, in seconds. Compare with “Compiling code on all cores”: the difference is time spent by the "
       "build tools rather than by compiling.",
       "ratio:R_build", false, {"mc_threaded"}, "K1x",
       [](const QJsonObject &d) -> std::optional<Value> {
         auto r = largestN(filter(series(d), [](const QJsonObject &x) { return x["kernel"].toString() == "K1x"; }));
         if (r.isEmpty()) return std::nullopt;
         auto v = ci(r["median"], 1e-9);
         markThreads(v, d, r);
         return v;
       }},
      /* One core */
      {"mc_k12", "mc", "Vector maths", "Vector maths on all cores", "All cores, widest vectors (AVX / NEON)",
       "GFLOP/s",
       "How many double-precision operations per second every core together gets done in dense matrix maths, as "
       "scientific, engineering and simulation software do. It uses the widest vector instructions the processor has "
       "(AVX-512, AVX2 or NEON), so it shows the processor's full floating-point power, and it is the hottest load "
       "in Prismark.",
       "Matrix multiplication in double precision, in tiles shared by every core, with fused multiply-adds as wide as "
       "the processor allows; every width gives exactly the same result. " + kSettle,
       "Higher is better, in billions of floating-point operations per second (GFLOP/s). Each row says which vector "
       "instructions it ran with.",
       "scaling:K12", true, {"mc_threaded"}, "K12", throughput("K12", "mc_threaded")},
      {"st_k2", "st", "3D rendering, one core", "3D rendering on one core", "One core, after warming up", "M samples/s",
       "How fast a single core is on a long task once it has warmed up: what one core can keep up.",
       "The renderer runs alone on the fastest core. " + kSettle,
       "Higher is better. Measured in millions of light samples traced per second.", "", true, {"st_sustained"}, "K2",
       throughput("K2", "st_sustained")},
      {"st_k1", "st", "Compiling, one core", "Compiling code on one core", "One core, after warming up", "builds/h",
       "How fast a single core compiles C++ once it has warmed up.",
       "The built-in compiler runs alone on the fastest core. Needs a one-time setup. " + kSettle,
       "Higher is better. Measured in complete builds per hour.", "", true, {"st_sustained"}, "K1",
       throughput("K1", "st_sustained")},
      {"st_k12", "st", "Vector maths, one core", "Vector maths on one core", "One core, widest vectors",
       "GFLOP/s",
       "How many double-precision operations per second a single core gets done in dense matrix maths, with the "
       "widest vector instructions it has (AVX-512, AVX2 or NEON).",
       "The matrix multiplication of “Vector maths on all cores”, on the fastest core alone. " + kSettle,
       "Higher is better, in billions of floating-point operations per second (GFLOP/s).", "", true,
       {"st_sustained"}, "K12", throughput("K12", "st_sustained")},
      {"b_k3", "st", "Compression", "Compressing a file", "Compress a small file", "ms",
       "How long one core takes to compress a small file with zstd, a common compression format. Short tasks like "
       "this are most of what everyday programs do.",
       kBurstHow, "Lower is better, in milliseconds.", "", false, {"st_burst"}, "K3",
       medianOf("K3", "st_burst", "", 1e-6)},
      {"b_k4", "st", "Opening a photo", "Opening a photo", "Decode and resize a JPEG", "ms",
       "How long one core takes to open a JPEG photo and resize it, as a photo viewer or a web page does.", kBurstHow,
       "Lower is better, in milliseconds.", "", false, {"st_burst"}, "K4", medianOf("K4", "st_burst", "", 1e-6)},
      {"b_k5", "st", "Reading JSON", "Reading structured data (JSON)", "Parse web-style text data", "ms",
       "How long one core takes to read JSON documents, the text format most websites and online services exchange.",
       kBurstHow, "Lower is better, in milliseconds.", "", false, {"st_burst"}, "K5",
       medianOf("K5", "st_burst", "", 1e-6)},
      {"b_k6", "st", "Starting a script", "Starting a small script", "Launch Lua and run a script", "ms",
       "How long it takes to start the Lua scripting language, load a script and run it, as when a plugin or a game "
       "script starts.",
       kBurstHow, "Lower is better, in milliseconds.", "", false, {"st_burst"}, "K6",
       medianOf("K6", "st_burst", "", 1e-6)},
      /* Memory & timing */
      {"m_lat1", "mem", "Memory delay", "Memory delay", "One read from main memory", "ns",
       "How long the processor waits for one piece of data from main memory (RAM) when it is in none of the caches. "
       "Programs that jump around large amounts of data, such as databases and games, wait for this often.",
       "The processor follows a long chain of links through a block of memory larger than all its caches; each step "
       "has to wait for the previous one to arrive.",
       "Lower is better, in nanoseconds (billionths of a second).", "", false, {"mc_instances"}, "K7", k7(false)},
      {"m_latn", "mem", "Memory delay, busy", "Memory delay while every core uses memory", "All cores reading at once",
       "ns",
       "The same wait while every other core is also reading main memory, so they all compete for it, as under heavy "
       "multitasking.",
       "The same chain of links, followed while every other core runs its own copy.",
       "Lower is better, in nanoseconds. Compare with “Memory delay”: the difference is the cost of competition.", "",
       false, {"mc_instances"}, "K7", k7(true)},
      {"j_idle", "mem", "Timer, idle", "Timer punctuality on an idle computer", "How late a 1 ms timer fires", "µs",
       "How late a task that asks to wake up every millisecond actually wakes up. It matters for audio, games and "
       "machine control, where late means a glitch.",
       "A task wakes up every millisecond for a minute (quick runs: shorter). The result is the lateness of its worst "
       "1-in-1000 wake-ups, because an average would hide the rare long delays that cause glitches. Keep your hands "
       "off the keyboard and mouse while it runs.",
       "Lower is better, in microseconds (millionths of a second).", "", false, {"periodic"}, "K10", periodic("idle")},
      {"j_load", "mem", "Timer, busy", "Timer punctuality while other cores are busy", "The same, other cores busy",
       "µs", "The same timer while every other core is kept busy reading memory.",
       "As on an idle computer, with every other core running a memory-heavy task.",
       "Lower is better, in microseconds. Compare with “Timer punctuality on an idle computer”.", "", false,
       {"periodic"}, "K10", periodic("loaded")},
      /* Responsiveness */
      {"r_1ms", "resp", "Wake-up, 1 ms task", "Reacting from rest: a 1 ms task", "A short task started from rest", "%",
       "An idle processor sleeps and slows down to save power. This shows how fast a short task runs when it arrives "
       "during that rest, compared with the same task on a processor that is already busy. You feel it as the small "
       "lag when you click something.",
       "Before each task the processor rests for a random 50–500 ms; then a task that takes 1 ms on a busy core is "
       "started and timed from the moment it was due. Keep your hands off the keyboard and mouse while it runs: input "
       "skips the test.",
       "Higher is better. 100 % means no penalty; 50 % means the task took twice as long because the processor first "
       "had to wake up and speed up.",
       "wake", true, {"cold_burst"}, "K9", rresp(1)},
      {"r_10ms", "resp", "Wake-up, 10 ms task", "Reacting from rest: a 10 ms task", "A longer task started from rest",
       "%",
       "The same comparison for a task ten times longer. A longer task hides more of the wake-up delay, so this is "
       "usually closer to 100 %.",
       "As for the 1 ms task: a random rest of 50–500 ms, then a task that takes 10 ms on a busy core, timed from the "
       "moment it was due. Keep your hands off the keyboard and mouse while it runs.",
       "Higher is better. 100 % means no penalty.", "wake", true, {"cold_burst"}, "K9", rresp(10)},
      {"c_k4", "resp", "Photo from rest", "Opening a photo from rest", "Decode a JPEG after a pause", "ms",
       "How long opening a photo takes when the request arrives while the processor rests, wake-up delay included.",
       kRestHow, "Lower is better, in milliseconds. Compare with “Opening a photo” under One core, measured on a busy core.",
       "resp:K4", false, {"cold_burst"}, "K4", medianOf("K4", "cold_burst", "cold", 1e-6)},
      {"c_k6", "resp", "Script from rest", "Starting a small script from rest", "Launch Lua after a pause", "ms",
       "How long starting a small script takes when the request arrives while the processor rests, wake-up delay "
       "included.",
       kRestHow, "Lower is better, in milliseconds. Compare with “Starting a small script” under One core.",
       "resp:K6", false, {"cold_burst"}, "K6", medianOf("K6", "cold_burst", "cold", 1e-6)},
      /* Stress test */
      {"s_st", "stress", "One core", "One core under stress", "One core at full load for 10 minutes or more",
       "GFLOP/s",
       "How fast one core does vector maths after minutes of full load, and whether it stays correct. A single busy "
       "core keeps its highest clock only while the cooling and the power limits allow it.",
       kStressHow, "Higher is better, in GFLOP/s in the 10th minute.", "stress:one_core", true, {"stress"}, "K12",
       stress("one_core")},
      {"s_mc", "stress", "All cores", "All cores under stress", "Every core at full load for 10 minutes or more",
       "GFLOP/s",
       "How fast every core together does vector maths after minutes of full load, when the processor is as hot as "
       "long use makes it, and whether every result stays correct. Wide vector instructions are the hottest load a "
       "processor gets; laptops in particular slow down here as their power limits and cooling take over.",
       kStressHow, "Higher is better, in GFLOP/s in the 10th minute.", "stress:all_cores", true, {"stress"}, "K12",
       stress("all_cores")},
      {"s_cc", "stress", "Each core in turn", "Each core in turn (core cycling)",
       "One core at a time, every core in turn", "GFLOP/s",
       "Vector maths on a single core at its highest boost clock, moving to the next physical core every 10 seconds "
       "and around again. Each core reaches its own top clock only when it runs alone, so this is where a single "
       "weak or degraded core, or a too aggressive per-core tuning (such as AMD's Curve Optimizer), shows up.",
       kStressHow + " The score is the slowest core's speed.",
       "Higher is better, in GFLOP/s: the slowest core's speed. The test page lists every core, and any core that "
       "computed a wrong result.",
       "stress:each_core", true, {"stress"}, "K12", stress("each_core")},
      {"s_mem", "stress", "Memory", "Memory under stress", "Most of the free memory, tested on every core", "GB/s",
       "A memory test: every core writes about three quarters of the free memory and checks every word on the next "
       "pass, over and over, with four patterns in turn (an address pattern, a walking one-bit, and their opposites, "
       "so every bit is tested both ways). It shows whether any memory comes back wrong when kept busy and warm, and "
       "how much data the memory moves meanwhile.",
       kStressHow + " Close other programs first: the test covers the memory they leave free.",
       "Higher is better, in gigabytes per second (read and written) in the 10th minute. Any wrong word fails the "
       "test.", "stress:memory", true, {"stress"}, "K11", stress("memory")},
  };
  return m;
}

const Metric *findMetric(const QString &id) {
  for (const Metric &m : metrics())
    if (m.id == id) return &m;
  return nullptr;
}

bool summarize(const QJsonObject &doc, const QString &file, RunSummary &s) {
  if (doc["schema"].toString() != "prismark/1") return false;
  s = RunSummary();
  s.id = doc["run_id"].toString();
  s.file = file;
  s.machine = doc["machine"].toObject();
  /* a display name given to the file (references/README.md), else the CPU model */
  s.name = doc["name"].toString(s.machine["cpu"].toObject()["model"].toString());
  if (s.name.isEmpty()) s.name = "Unknown CPU";
  QDateTime t = QDateTime::fromString(doc["started_utc"].toString(), Qt::ISODate);
  if (t.isValid()) s.date = t.toLocalTime().toString("yyyy-MM-dd HH:mm");
  s.complete = doc["complete"].toBool();
  s.verified = doc["verified"].toBool();
  QJsonObject cfg = doc["config"].toObject();
  s.quick = cfg.contains("quick") ? cfg["quick"].toBool() : cfg["sustained_max_s"].toDouble(600) < 30; /* older */
  s.tiers = doc["tiers"].toObject();
  s.harness = doc["harness"].toObject();
  s.frontend = doc["frontend"].toObject();
  s.state = doc["state"].toObject()["start"].toObject();
  s.unavailable = doc["unavailable"].toArray();
  for (const QJsonValue &v : series(doc)) {
    QJsonObject r = v.toObject();
    if (r.contains("vector_isa")) s.vectorIsa = r["vector_isa"].toString();
    if (r["clamped_windows"].toInt() > 0) {
      s.clampedSeries++;
      double mhz = r["clamped_lowest_mhz"].toDouble(NAN);
      if (!(s.clampedMhz <= mhz)) s.clampedMhz = mhz;
    }
    if (r["purpose"].toString() != "warmup") continue;
    if (auto t = ci(r["R_throttle"])) s.hot[r["mode"].toString()] = *t;
  }
  for (const Metric &m : metrics()) {
    auto v = m.get(doc);
    if (v && std::isfinite(v->v)) s.metrics[m.id] = *v;
  }
  QJsonObject an = doc["analysis"].toObject();
  for (const QJsonValue &v : an["cold_burst"].toArray()) {
    QJsonObject r = v.toObject();
    if (r["core_type"].toString() != "P" || !asConfigured(r)) continue;
    if (auto c = ci(r["R_resp"], 100)) s.cold.push_back({r["W_ms"].toDouble(), *c});
  }
  for (const QJsonValue &v : an["scaling"].toArray()) {
    QJsonObject r = v.toObject();
    if (auto c = ci(r["S"])) s.scaling.push_back({r["kernel"].toString(), r["n"].toInt(), *c});
  }
  /* Clock and power by thread count, from the windows of each all-cores series (older results have no power). */
  for (const QJsonValue &v : doc["results"].toArray()) {
    QJsonObject r = v.toObject(), p = r["params"].toObject();
    if (r["mode"].toString() != "mc_threaded" || r["tier"].toString() != "baseline" || p.contains("purpose") ||
        !r.contains("mhz"))
      continue;
    int from = r["steady"].toObject()["from_window"].toInt();
    auto median = [from](const QJsonArray &a) {
      QVector<double> x;
      for (int i = from; i < a.size(); i++)
        if (a[i].isDouble() && a[i].toDouble() > 0) x.push_back(a[i].toDouble());
      if (x.isEmpty()) return double(NAN);
      std::sort(x.begin(), x.end());
      return x.size() % 2 ? x[x.size() / 2] : (x[x.size() / 2 - 1] + x[x.size() / 2]) / 2;
    };
    QVector<RunSummary::LoadPt::Clock> by;
    QJsonObject dom = r["mhz_by_domain"].toObject();
    QJsonArray idle = r["idle_domains"].toArray(); /* older results list only the domains in use */
    for (auto it = dom.begin(); it != dom.end(); ++it)
      by.push_back({it.key(), median(it.value().toArray()), idle.contains(it.key())});
    s.load.push_back({r["kernel"].toString(), p["threads"].toInt(), median(r["samples"].toArray()),
                      median(r["mhz"].toArray()), median(r["power_w"].toArray()), r["unit"].toString(), by});
  }
  for (const QJsonValue &v : an["ratios"].toArray()) {
    QJsonObject r = v.toObject();
    if (auto c = ci(r["value"]))
      s.ratios.push_back({r["name"].toString(), r["kernel"].toString(), r["variant"].toString(), r["n"].toInt(), *c});
  }
  for (const QJsonValue &v : an["checksums"].toArray())
    if (!v.toObject()["consistent"].toBool()) s.checksumsOk = false;
  /* The stress test: its figures, and its windows over time for the chart. */
  for (const QJsonValue &v : an["stress"].toArray()) {
    QJsonObject r = v.toObject();
    RunSummary::Stress &x = s.stress[r["part"].toString()];
    x.unit = r["unit"].toString();
    if (r.contains("vector_isa") && s.vectorIsa.isEmpty()) s.vectorIsa = r["vector_isa"].toString();
    x.threads = r["n"].toInt();
    x.bytes = r["bytes_tested"].toDouble(NAN);
    x.durationS = r["duration_s"].toDouble(NAN);
    x.stopped = r["stopped"].toBool();
    if (auto c = ci(r["first_minute"])) x.first = *c;
    if (auto c = ci(r["last_minute"])) x.last = *c;
    if (auto c = ci(r["hold_up"])) x.holdUp = *c;
    x.tempMax = r["temp_max_c"].toDouble(NAN);
    x.mhz = r["mhz"].toDouble(NAN);
    x.watts = r["power_w"].toDouble(NAN);
    x.clamped = r["clamped_windows"].toInt();
    x.errors = qint64(r["checksum_errors"].toDouble() + r["wrong_tasks"].toDouble());
    x.verdict = r["verdict"].toString(x.errors ? "failed" : "stable"); /* older results have no verdict */
    x.stopReason = r["stop_reason"].toString();
    x.firstErrorS = r["first_error_s"].toDouble(NAN);
    x.firstErrorCpu = r["first_error_cpu"].toInt(-1);
    QJsonObject byCpu = r["wrong_by_cpu"].toObject();
    for (auto it = byCpu.begin(); it != byCpu.end(); ++it) x.wrongByCpu[it.key().toInt()] = qint64(it.value().toDouble());
    for (const QJsonValue &cv : r["cores"].toArray()) {
      QJsonObject o = cv.toObject();
      x.cores.push_back({o["cpu"].toInt(), o["core_type"].toString(), ci(o["perf"]).value_or(Value{}),
                         o["mhz"].toDouble(NAN), qint64(o["wrong"].toDouble())});
    }
  }
  for (const QJsonValue &v : doc["results"].toArray()) {
    QJsonObject r = v.toObject(), p = r["params"].toObject();
    if (r["mode"].toString() != "stress" || !s.stress.contains(p["part"].toString())) continue;
    RunSummary::Stress &x = s.stress[p["part"].toString()];
    x.windowS = p["window_ms"].toDouble(5000) / 1e3;
    auto nums = [](const QJsonArray &a) {
      QVector<double> out;
      for (const QJsonValue &e : a) out.push_back(e.isDouble() ? e.toDouble() : NAN);
      return out;
    };
    x.perf = nums(r["samples"].toArray());
    x.mhzT = nums(r["mhz"].toArray());
    x.tempT = nums(r["temp_c"].toArray());
    x.powerT = nums(r["power_w"].toArray());
  }
  return true;
}

QString formatValue(double v) {
  if (!std::isfinite(v)) return QStringLiteral("–");
  double a = std::fabs(v);
  if (a >= 1000) return QString::number(v, 'f', 0);
  if (a >= 100) return QString::number(v, 'f', 1);
  return QString::number(v, 'g', 3);
}

QString plainKernel(const QString &kernel, const QString &variant) {
  static const QMap<QString, QString> names = {
      {"K1", "Compiling code"},    {"K1x", "Full software build"}, {"K2", "3D rendering"},
      {"K3", "Compression"},       {"K4", "Opening a photo"},     {"K5", "Reading JSON"},
      {"K6", "Starting a script"}, {"K7", "Memory delay"},        {"K8", "Matrix maths"},
      {"K9", "Wake-up test"},      {"K10", "Timer punctuality"},   {"K11", "Memory bandwidth"},
      {"K12", "Vector maths"}};
  QString n = names.value(kernel, kernel);
  if (variant == "fp32") n += ", decimals";
  else if (variant == "int8") n += ", small integers";
  else if (variant.startsWith("n=")) n += ", " + variant.mid(2) + (variant == "n=1" ? " thread" : " threads");
  else if (!variant.isEmpty()) n += " " + variant;
  return n;
}

QString plainMode(const QString &mode) {
  static const QMap<QString, QString> names = {
      {"st_burst", "one core, short task"},           {"st_sustained", "one core, after warming up"},
      {"mc_threaded", "all cores, working together"}, {"mc_instances", "all cores, each on its own"},
      {"cold_burst", "started from rest"},            {"periodic", "timer punctuality"},
      {"stress", "stress test"},                      {"all", "every test"}};
  return names.value(mode, mode);
}

QString plainRatio(const QString &name) {
  static const QMap<QString, QString> names = {
      {"R_build", "Full build vs. in-memory compile"},
      {"R_resp", "Speed from rest vs. busy"},
      {"U_ISA", "Gain from newest instructions"}};
  return names.value(name, name);
}

QString glossaryHtml() {
  struct Term {
    const char *term, *meaning;
  };
  static const Term terms[] = {
      {"Test", "One workload (rendering, compressing, compiling…) measured in one way. Every test is ranked on its "
               "own; Prismark never adds them up into a single score, because each answers a different question."},
      {"Quick run / full run",
       "A quick run takes minutes and gives a first look; its numbers are rough and are never compared with full "
       "runs. A full run warms the CPU up before the long tests and repeats the short ones until they are precise; "
       "about half an hour on a laptop, less on faster computers."},
      {"Core, thread", "A core is one processing unit of the CPU. Many CPUs run two threads per core. “All cores” "
                       "tests use every thread the operating system offers."},
      {"Warming up, when hot",
       "A cool CPU runs faster for a while, then heats up and may slow down (throttling). In a full run, long tests "
       "follow a minute of full load, so they measure the computer as it is in sustained use. “When hot” shows how "
       "much slower it got during that minute."},
      {"From rest (cold start)",
       "To save power an idle CPU sleeps and lowers its clock. Work that arrives then waits for it to wake up and "
       "speed up again. These tests pause for 50–500 ms before each task to measure that delay. Keep your hands off "
       "the keyboard and mouse while they run."},
      {"Wake-up (%)",
       "Speed of a task started from rest, as a percentage of the same task on a busy core. 100 % = no penalty."},
      {"Timer punctuality, 1-in-1000",
       "A task asks to wake every millisecond and we record how late each wake-up is. Averages hide rare long "
       "delays, so the result is the lateness of the worst 1 in 1000 wake-ups (the 99.9th percentile, p99.9)."},
      {"Memory delay", "Time for one read from main memory when the data is in no cache. Lower is better."},
      {"Instruction set, new instructions",
       "The vocabulary of operations a CPU understands. Newer CPUs add instructions (AVX2, AVX-512 on Intel/AMD; "
       "SVE2 on Arm) that do more per step. Prismark builds each test for a common baseline (x86-64-v2 or "
       "Armv8.2-A) and, separately, for the newest level the CPU supports."},
      {"Uncertainty (95 % range)",
       "Measurements vary a little from run to run. The thin line on each bar shows the range the true value lies "
       "in with 95 % confidence. If two bars’ ranges overlap, treat them as equal."},
      {"+/− %", "How much faster (+) or slower (−) a result is than the highlighted one, for this test."},
      {"Placeholder", "Made-up example values shown for comparison until real reference machines have been "
                      "measured. They are not measurements."},
      {"Power policy (governor)",
       "The operating system’s rule for choosing the CPU clock speed, such as “powersave” or “performance”. It "
       "affects results, so it is recorded with every run."},
  };
  QString h = QStringLiteral("<table cellspacing='0' cellpadding='5'>");
  for (const Term &t : terms)
    h += QStringLiteral("<tr><td valign='top' style='white-space:nowrap; padding-right:14px'><b>%1</b></td><td>%2</td></tr>")
             .arg(QString::fromUtf8(t.term).toHtmlEscaped(), QString::fromUtf8(t.meaning).toHtmlEscaped());
  return h + QStringLiteral("</table>");
}

QString plainReason(const QString &reason) {
  if (reason.contains("snapshot") || reason.contains("k1-data")) return QStringLiteral("needs the one-time setup (Menu › Prepare compile tests)");
  if (reason == "not built") return QStringLiteral("not included in this build of Prismark");
  if (reason.startsWith("skipped: keyboard")) return QStringLiteral("skipped: the keyboard or mouse was used while it ran");
  if (reason.contains("no max-level tier") || reason.contains("supports no max-level"))
    return QStringLiteral("this processor has no newer instruction set to compare with");
  return reason;
}

QString plainText(QString text) {
  /* Kernel ids in messages from the runner ("K3", "K1x") become test names. */
  static const QRegularExpression id(QStringLiteral("\\bK(10|[1-9])(x?)\\b"));
  QString out;
  qsizetype last = 0;
  for (auto it = id.globalMatch(text); it.hasNext();) {
    QRegularExpressionMatch mt = it.next();
    out += text.mid(last, mt.capturedStart() - last) + plainKernel(mt.captured(0));
    last = mt.capturedEnd();
  }
  out += text.mid(last);
  /* Core types: "core P" is the fast kind, "core E" the efficient one. */
  out.replace(QRegularExpression(QStringLiteral("\\bcore P\\b")), QStringLiteral("fast core"));
  out.replace(QRegularExpression(QStringLiteral("\\bcore E\\b")), QStringLiteral("efficient core"));
  return out;
}

bool isSimple(const QString &metricId) {
  static const QStringList simple = {
      "mc_k2", "mc_k1",   /* All cores: rendering, and compiling (desktop only, one-time setup); the full build is
                             for experts */
      "b_k4", "st_k2",    /* One core: an everyday short task, and one core after warming up */
      "m_lat1", "j_idle", /* Memory & timing: how long a read from main memory takes, and how punctual a timer is */
      /* None from Responsiveness or Stress test: waking from rest depends on power settings more than on the
       * processor, and a stress test takes ten minutes or more. Those tabs appear only in Advanced. */
  };
  return simple.contains(metricId);
}

QString isaName(const QString &level) {
  static const QMap<QString, QString> names = {
      {"x86-64-v2", "SSE4.2"},   {"x86-64-v3", "AVX2"}, {"x86-64-v4", "AVX-512"},
      {"armv8.2-a", "NEON"},     {"armv8.2-a+dotprod+fp16", "NEON + dot product"},
      {"armv9-a+sve2", "SVE2"}};
  return names.value(level, level);
}

bool offeredOn(const QString &kernel, const QString &os) {
  bool phone = os == "android" || os == "ios";
  if (kernel == "K1") return !phone;
  if (kernel == "K1x") return !phone && os != "ipados";
  return true;
}
