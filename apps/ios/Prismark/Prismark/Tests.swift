// The tests the app offers, and how each result is read from a result document: the same definitions as the
// Android app (Tests.kt). There is no overall score: every test stands on its own.
import Foundation

typealias Doc = [String: Any]

struct Value {
    let v: Double
    var lo = Double.nan
    var hi = Double.nan
    var unsettled = false
    var hasRange: Bool { lo.isFinite && hi.isFinite }
}

struct TestGroup: Identifiable, Hashable {
    let id: String
    let number: String
    let label: String
    let about: String
}

struct Test: Identifiable, Hashable {
    let id: String
    let group: String
    let name: String
    let sub: String
    let unit: String
    let higherBetter: Bool
    let mode: PrismarkModes
    let kernel: String
    let read: (Doc) -> Value?
    /// A part of the stress test: its name in the result, and its PMK_STRESS_* bit.
    var part: String? = nil
    var bit: UInt32 = 0

    /// Whether a run holds this test: a stress test under 10 minutes has a verdict but no score.
    func ran(_ doc: Doc) -> Bool { read(doc) != nil || part.flatMap { stressRow(doc, $0) } != nil }

    static func == (a: Test, b: Test) -> Bool { a.id == b.id }
    func hash(into hasher: inout Hasher) { hasher.combine(id) }
}

private func series(_ d: Doc) -> [Doc] { (d["analysis"] as? Doc)?["series"] as? [Doc] ?? [] }
func num(_ o: Doc?, _ key: String) -> Double { (o?[key] as? NSNumber)?.doubleValue ?? .nan }
private func int(_ o: Doc, _ key: String) -> Int { (o[key] as? NSNumber)?.intValue ?? 0 }
private func fast(_ r: Doc) -> Bool { r["core_type"] as? String == "P" }

func ci(_ o: Doc?, _ k: Double = 1) -> Value? {
    let est = num(o, "est")
    guard est.isFinite else { return nil }
    return Value(v: est * k, lo: num(o, "lo") * k, hi: num(o, "hi") * k)
}

private func throughput(_ kernel: String, _ mode: String) -> (Doc) -> Value? {
    { d in
        let rows = series(d).filter {
            $0["kernel"] as? String == kernel && $0["mode"] as? String == mode && $0["tier"] as? String == "baseline"
                && $0["purpose"] as? String == nil
        }
        let row = mode == "st_sustained" ? rows.first(where: fast) : rows.max { int($0, "n") < int($1, "n") }
        guard let row, var value = ci(row["perf_steady"] as? Doc) else { return nil }
        value.unsettled = row["steady_reached"] as? Bool == false
        return value
    }
}

private func medianOf(_ kernel: String, _ mode: String, start: String? = nil, _ k: Double) -> (Doc) -> Value? {
    { d in
        let row = series(d).first {
            $0["kernel"] as? String == kernel && $0["mode"] as? String == mode && fast($0)
                && (start == nil || $0["start"] as? String == start)
        }
        return ci(row?["median"] as? Doc, k)
    }
}

private func memory(all: Bool) -> (Doc) -> Value? {
    { d in
        let rows = series(d).filter { $0["kernel"] as? String == "K7" }
        let largest = rows.map { num($0, "working_set_bytes") }.max() ?? 0
        let at = rows.filter { num($0, "working_set_bytes") == largest }
        let row = all ? at.max { int($0, "n") < int($1, "n") } : at.last { int($0, "n") == 1 }
        return ci(row?["median"] as? Doc)
    }
}

private func periodic(_ condition: String) -> (Doc) -> Value? {
    { d in
        let rows = (d["analysis"] as? Doc)?["periodic"] as? [Doc] ?? []
        let ns = num(rows.first { $0["condition"] as? String == condition && $0["p999_ns"] is NSNumber }, "p999_ns")
        return ns.isFinite ? Value(v: ns / 1e3) : nil
    }
}

private func wakeUp(_ ms: Double) -> (Doc) -> Value? {
    { d in
        let rows = (d["analysis"] as? Doc)?["cold_burst"] as? [Doc] ?? []
        return ci(rows.first { num($0, "W_ms") == ms && fast($0) }?["R_resp"] as? Doc, 100)
    }
}

let groups = [
    TestGroup(id: "mc", number: "01", label: "All cores", about: "Heavy jobs that use every core at once."),
    TestGroup(id: "st", number: "02", label: "One core", about: "How fast a single core is: short everyday tasks, and long tasks once the core has warmed up."),
    TestGroup(id: "mem", number: "03", label: "Memory & timing", about: "How long the CPU waits for memory, and how punctually it wakes up for tasks on a fixed schedule."),
    TestGroup(id: "resp", number: "04", label: "Responsiveness", about: "How quickly it reacts when work arrives while it is resting. Do not touch the screen while these run."),
]

let tests = [
    Test(id: "mc_k2", group: "mc", name: "3D rendering", sub: "All cores share one image", unit: "M samples/s", higherBetter: true, mode: .mcThreaded, kernel: "K2", read: throughput("K2", "mc_threaded")),
    Test(id: "mc_k12", group: "mc", name: "Vector maths", sub: "All cores, widest vectors", unit: "GFLOP/s", higherBetter: true, mode: .mcThreaded, kernel: "K12", read: throughput("K12", "mc_threaded")),
    Test(id: "st_k2", group: "st", name: "3D rendering, one core", sub: "One core, after warming up", unit: "M samples/s", higherBetter: true, mode: .stSustained, kernel: "K2", read: throughput("K2", "st_sustained")),
    Test(id: "st_k12", group: "st", name: "Vector maths, one core", sub: "One core, widest vectors", unit: "GFLOP/s", higherBetter: true, mode: .stSustained, kernel: "K12", read: throughput("K12", "st_sustained")),
    Test(id: "b_k3", group: "st", name: "Compression", sub: "Compress a small file", unit: "ms", higherBetter: false, mode: .stBurst, kernel: "K3", read: medianOf("K3", "st_burst", 1e-6)),
    Test(id: "b_k4", group: "st", name: "Opening a photo", sub: "Decode and resize a JPEG", unit: "ms", higherBetter: false, mode: .stBurst, kernel: "K4", read: medianOf("K4", "st_burst", 1e-6)),
    Test(id: "b_k5", group: "st", name: "Reading JSON", sub: "Parse web-style text data", unit: "ms", higherBetter: false, mode: .stBurst, kernel: "K5", read: medianOf("K5", "st_burst", 1e-6)),
    Test(id: "b_k6", group: "st", name: "Starting a script", sub: "Launch Lua and run a script", unit: "ms", higherBetter: false, mode: .stBurst, kernel: "K6", read: medianOf("K6", "st_burst", 1e-6)),
    Test(id: "m_lat1", group: "mem", name: "Memory delay", sub: "One read from main memory", unit: "ns", higherBetter: false, mode: .mcInstances, kernel: "K7", read: memory(all: false)),
    Test(id: "m_latn", group: "mem", name: "Memory delay, busy", sub: "All cores reading at once", unit: "ns", higherBetter: false, mode: .mcInstances, kernel: "K7", read: memory(all: true)),
    Test(id: "j_idle", group: "mem", name: "Timer, idle", sub: "How late a 1 ms timer fires", unit: "µs", higherBetter: false, mode: .periodic, kernel: "K10", read: periodic("idle")),
    Test(id: "j_load", group: "mem", name: "Timer, busy", sub: "The same, other cores busy", unit: "µs", higherBetter: false, mode: .periodic, kernel: "K10", read: periodic("loaded")),
    Test(id: "r_1ms", group: "resp", name: "Wake-up, 1 ms task", sub: "A short task started from rest", unit: "%", higherBetter: true, mode: .coldBurst, kernel: "K9", read: wakeUp(1)),
    Test(id: "r_10ms", group: "resp", name: "Wake-up, 10 ms task", sub: "A longer task started from rest", unit: "%", higherBetter: true, mode: .coldBurst, kernel: "K9", read: wakeUp(10)),
    Test(id: "c_k4", group: "resp", name: "Photo from rest", sub: "Decode a JPEG after a pause", unit: "ms", higherBetter: false, mode: .coldBurst, kernel: "K4", read: medianOf("K4", "cold_burst", start: "cold", 1e-6)),
    Test(id: "c_k6", group: "resp", name: "Script from rest", sub: "Launch Lua after a pause", unit: "ms", higherBetter: false, mode: .coldBurst, kernel: "K6", read: medianOf("K6", "cold_burst", start: "cold", 1e-6)),
]

func stressRow(_ d: Doc, _ part: String) -> Doc? {
    ((d["analysis"] as? Doc)?["stress"] as? [Doc])?.first { $0["part"] as? String == part }
}

private func stress(_ part: String) -> (Doc) -> Value? {
    { ci(stressRow($0, part)?["score"] as? Doc) }
}

/// Minutes of full load, run on their own: not one more benchmark, so not among the tests above.
let stressGroup = TestGroup(id: "stress", number: "05", label: "Stress test", about: "Ten minutes or more of full load, as in a long game: how well the speed holds up as the device heats up, and whether every result stays correct. Each part runs on its own.")

let stressTests = [
    Test(id: "s_st", group: "stress", name: "One core", sub: "One core at full load", unit: "GFLOP/s", higherBetter: true, mode: .stress, kernel: "K12", read: stress("one_core"), part: "one_core", bit: 1 << 0),
    Test(id: "s_mc", group: "stress", name: "All cores", sub: "Every core at full load", unit: "GFLOP/s", higherBetter: true, mode: .stress, kernel: "K12", read: stress("all_cores"), part: "all_cores", bit: 1 << 1),
    Test(id: "s_cc", group: "stress", name: "Each core in turn", sub: "One core at a time, every core in turn", unit: "GFLOP/s", higherBetter: true, mode: .stress, kernel: "K12", read: stress("each_core"), part: "each_core", bit: 1 << 3),
    Test(id: "s_mem", group: "stress", name: "Memory", sub: "Most of the memory the app may use, written and checked", unit: "GB/s", higherBetter: true, mode: .stress, kernel: "K11", read: stress("memory"), part: "memory", bit: 1 << 2),
]

let allTests = tests + stressTests

let phaseNames = [
    "stress": "Full load",
    "preflight": "Checking it is idle",
    "st_burst": "One core, short tasks",
    "st_sustained": "One core, after warming up",
    "mc_threaded": "All cores, working together",
    "mc_instances": "All cores, each on its own",
    "cold_burst": "Started from rest",
    "periodic": "Timer punctuality",
    "cooldown": "Cooling down",
    "done": "Computing the results",
]

/// Phases that measure waits and wake-ups: the screen must not change at all while they run.
let quietPhases: Set<String> = ["cold_burst", "periodic"]

func format(_ v: Double) -> String {
    guard v.isFinite else { return "—" }
    if abs(v) >= 100 { return String(format: "%.0f", v) }
    if abs(v) >= 10 { return String(format: "%.1f", v) }
    return String(format: "%.2f", v)
}
