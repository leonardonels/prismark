/*
 * The tests the app offers, and the reduction of a result document to them: each test is one kernel in one
 * mode, its value read from the statistics the C core computed (the document's "analysis" section), never
 * recomputed. The same definitions as the desktop app (apps/gui/metrics.cpp), without the tests phones do not
 * run: compiling code (K1, K1x) and the stress test.
 *
 * There is no overall score: every test stands on its own.
 *
 * Copyright 2026 The Prismark Authors. Apache-2.0.
 */
package org.prismark.app

import org.json.JSONArray
import org.json.JSONObject
import org.prismark.Prismark

data class Value(val v: Double, val lo: Double = Double.NaN, val hi: Double = Double.NaN, val unsettled: Boolean = false) {
    val hasRange get() = lo.isFinite() && hi.isFinite()
}

data class Group(val id: String, val label: String, val desc: String)

data class Test(
    val id: String,
    val group: String,
    val name: String,
    val sub: String,
    val unit: String,
    val higherBetter: Boolean,
    val mode: Int,
    val kernel: String,
    val get: (JSONObject) -> Value?,
)

val groups = listOf(
    Group("mc", "All cores", "Heavy jobs that use every core at once."),
    Group("st", "One core", "How fast a single core is: short everyday tasks, and long tasks once the core has warmed up."),
    Group("mem", "Memory & timing", "How long the CPU waits for memory, and how punctually it wakes up for tasks on a fixed schedule."),
    Group("resp", "Responsiveness", "How quickly the phone reacts when work arrives while it is resting. Do not touch the screen while these run."),
)

private fun series(d: JSONObject): List<JSONObject> = d.optJSONObject("analysis")?.optJSONArray("series").objects()

private fun JSONArray?.objects(): List<JSONObject> =
    if (this == null) emptyList() else (0 until length()).mapNotNull { optJSONObject(it) }

private fun JSONObject.num(key: String): Double = if (isNull(key)) Double.NaN else optDouble(key, Double.NaN)

private fun ci(o: JSONObject?, k: Double = 1.0): Value? {
    if (o == null || o.isNull("est")) return null
    val est = o.num("est")
    if (!est.isFinite()) return null
    return Value(est * k, o.num("lo") * k, o.num("hi") * k)
}

private fun fast(r: JSONObject) = r.optString("core_type") == "P"

private fun largestN(rows: List<JSONObject>) = rows.maxByOrNull { it.optInt("n") }

private fun throughput(kernel: String, mode: String): (JSONObject) -> Value? = { d ->
    val rows = series(d).filter {
        it.optString("kernel") == kernel && it.optString("mode") == mode && it.optString("tier") == "baseline" &&
            (!it.has("purpose") || it.isNull("purpose"))
    }
    val r = if (mode == "st_sustained") rows.firstOrNull(::fast) else largestN(rows)
    r?.let { row ->
        ci(row.optJSONObject("perf_steady"))?.copy(unsettled = row.has("steady_reached") && !row.optBoolean("steady_reached"))
    }
}

private fun medianOf(kernel: String, mode: String, start: String, k: Double): (JSONObject) -> Value? = { d ->
    series(d).firstOrNull {
        it.optString("kernel") == kernel && it.optString("mode") == mode && fast(it) &&
            (start.isEmpty() || it.optString("start") == start)
    }?.let { ci(it.optJSONObject("median"), k) }
}

private fun k7(all: Boolean): (JSONObject) -> Value? = { d ->
    val rows = series(d).filter { it.optString("kernel") == "K7" }
    val ws = rows.maxOfOrNull { it.optDouble("working_set_bytes", 0.0) } ?: 0.0
    val at = rows.filter { it.optDouble("working_set_bytes", 0.0) == ws }
    val r = if (all) largestN(at) else at.lastOrNull { it.optInt("n") == 1 }
    r?.let { ci(it.optJSONObject("median")) }
}

private fun periodic(condition: String): (JSONObject) -> Value? = { d ->
    d.optJSONObject("analysis")?.optJSONArray("periodic").objects()
        .firstOrNull { it.optString("condition") == condition && !it.isNull("p999_ns") }
        ?.let { Value(it.num("p999_ns") / 1e3) }
}

private fun rresp(w: Double): (JSONObject) -> Value? = { d ->
    d.optJSONObject("analysis")?.optJSONArray("cold_burst").objects()
        .firstOrNull { it.optDouble("W_ms", Double.NaN) == w && fast(it) }
        ?.let { ci(it.optJSONObject("R_resp"), 100.0) } /* shown as a percentage of busy-core speed */
}

val tests = listOf(
    Test("mc_k2", "mc", "3D rendering", "All cores share one image", "M samples/s", true,
        Prismark.MC_THREADED, "K2", throughput("K2", "mc_threaded")),
    Test("mc_k12", "mc", "Vector maths", "All cores, widest vectors (NEON)", "GFLOP/s", true,
        Prismark.MC_THREADED, "K12", throughput("K12", "mc_threaded")),
    Test("st_k2", "st", "3D rendering, one core", "One core, after warming up", "M samples/s", true,
        Prismark.ST_SUSTAINED, "K2", throughput("K2", "st_sustained")),
    Test("st_k12", "st", "Vector maths, one core", "One core, widest vectors", "GFLOP/s", true,
        Prismark.ST_SUSTAINED, "K12", throughput("K12", "st_sustained")),
    Test("b_k3", "st", "Compression", "Compress a small file", "ms", false,
        Prismark.ST_BURST, "K3", medianOf("K3", "st_burst", "", 1e-6)),
    Test("b_k4", "st", "Opening a photo", "Decode and resize a JPEG", "ms", false,
        Prismark.ST_BURST, "K4", medianOf("K4", "st_burst", "", 1e-6)),
    Test("b_k5", "st", "Reading JSON", "Parse web-style text data", "ms", false,
        Prismark.ST_BURST, "K5", medianOf("K5", "st_burst", "", 1e-6)),
    Test("b_k6", "st", "Starting a script", "Launch Lua and run a script", "ms", false,
        Prismark.ST_BURST, "K6", medianOf("K6", "st_burst", "", 1e-6)),
    Test("m_lat1", "mem", "Memory delay", "One read from main memory", "ns", false,
        Prismark.MC_INSTANCES, "K7", k7(false)),
    Test("m_latn", "mem", "Memory delay, busy", "All cores reading at once", "ns", false,
        Prismark.MC_INSTANCES, "K7", k7(true)),
    Test("j_idle", "mem", "Timer, idle", "How late a 1 ms timer fires", "µs", false,
        Prismark.PERIODIC, "K10", periodic("idle")),
    Test("j_load", "mem", "Timer, busy", "The same, other cores busy", "µs", false,
        Prismark.PERIODIC, "K10", periodic("loaded")),
    Test("r_1ms", "resp", "Wake-up, 1 ms task", "A short task started from rest", "%", true,
        Prismark.COLD_BURST, "K9", rresp(1.0)),
    Test("r_10ms", "resp", "Wake-up, 10 ms task", "A longer task started from rest", "%", true,
        Prismark.COLD_BURST, "K9", rresp(10.0)),
    Test("c_k4", "resp", "Photo from rest", "Decode a JPEG after a pause", "ms", false,
        Prismark.COLD_BURST, "K4", medianOf("K4", "cold_burst", "cold", 1e-6)),
    Test("c_k6", "resp", "Script from rest", "Launch Lua after a pause", "ms", false,
        Prismark.COLD_BURST, "K6", medianOf("K6", "cold_burst", "cold", 1e-6)),
)

/** What one run of the selected tests asks the core for: the union of their modes and kernels. */
fun runRequest(selected: Collection<Test>): Pair<Int, String> =
    selected.fold(0) { m, t -> m or t.mode } to selected.map { it.kernel }.distinct().joinToString(",")

/** The phase names the core reports, as the desktop app shows them. */
val phaseNames = mapOf(
    "preflight" to "Checking the phone is idle",
    "st_burst" to "One core, short tasks",
    "st_sustained" to "One core, after warming up",
    "mc_threaded" to "All cores, working together",
    "mc_instances" to "All cores, each on its own",
    "cold_burst" to "Started from rest",
    "periodic" to "Timer punctuality",
    "cooldown" to "Cooling down",
    "done" to "Computing the results",
)

/** Phases that measure waits and wake-ups: the screen must not change at all while they run. */
val quietPhases = setOf("cold_burst", "periodic")

/**
 * What the platform could not tell the core on this device. Each is recorded in the result as null (or, for the
 * clock, 0 MHz, the format's existing "unknown") and shown as unknown; none of them stops a run.
 */
data class Unknown(val what: String, val why: String)

fun unknowns(d: JSONObject): List<Unknown> {
    val out = mutableListOf<Unknown>()
    val machine = d.optJSONObject("machine")
    val pre = d.optJSONObject("preflight")
    val state = d.optJSONObject("state")
    val temps = listOfNotNull(
        pre?.num("idle_temp_c"),
        state?.optJSONObject("start")?.optJSONObject("temp_c")?.num("cpu"),
        state?.optJSONObject("end")?.optJSONObject("temp_c")?.num("cpu"),
    )
    if (temps.none { it.isFinite() })
        out += Unknown("Temperature", "Android does not let apps read the CPU's temperature sensors on this phone.")
    val cores = machine?.optJSONObject("cpu")?.optJSONArray("cores").objects()
    val clocks = series(d).map { it.num("mhz") }
    if (cores.all { it.optInt("max_mhz") <= 0 } && clocks.none { it.isFinite() && it > 0 })
        out += Unknown("Clock speed", "The CPU's clock is not readable by apps on this phone.")
    if (pre != null && !pre.optBoolean("skipped") && !pre.num("background_load").isFinite())
        out += Unknown("Background load", "Android does not let apps read /proc/stat, so whether other apps were working could not be checked. The run is not marked busy.")
    val caps = machine?.optJSONObject("capabilities")
    if (caps != null && !caps.optBoolean("pinning"))
        out += Unknown("Core placement", "Threads cannot be pinned to a core here; Android chose the cores, and each series records where it ran.")
    if (caps != null && !caps.optBoolean("perf_counters"))
        out += Unknown("Cycle counter", "Hardware counters are closed to apps, so cycle counts are not recorded.")
    return out
}

fun format(v: Double): String = when {
    !v.isFinite() -> "—"
    kotlin.math.abs(v) >= 100 -> "%.0f".format(v)
    kotlin.math.abs(v) >= 10 -> "%.1f".format(v)
    else -> "%.2f".format(v)
}
