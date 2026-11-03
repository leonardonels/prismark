/*
 * The run, kept outside the activity so that it survives the screen being rebuilt, and the result files.
 *
 * Copyright 2026 The Prismark Authors. Apache-2.0.
 */
package org.prismark.app

import android.content.Context
import android.os.Handler
import android.os.Looper
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import org.json.JSONArray
import org.json.JSONObject
import org.prismark.Prismark
import java.io.File
import java.time.Instant
import java.time.ZoneId
import java.time.ZonedDateTime
import java.time.format.DateTimeFormatter

/** A saved result: the document exactly as the core wrote it, and its file. */
class Run(val file: File, val json: String) {
    val doc: JSONObject by lazy { runCatching { JSONObject(json) }.getOrElse { JSONObject() } }
    val startedAt: ZonedDateTime? get() = runCatching { Instant.parse(doc.optString("started_utc")).atZone(ZoneId.systemDefault()) }.getOrNull()
    val started: String get() = startedAt?.format(DateTimeFormatter.ofPattern("d MMM yyyy, HH:mm")) ?: file.name
    val shortDate: String get() = startedAt?.format(DateTimeFormatter.ofPattern("d MMM"))?.uppercase() ?: "—"
    val time: String get() = startedAt?.format(DateTimeFormatter.ofPattern("yyyy, HH:mm")).orEmpty()
    val runId: String get() = doc.optString("run_id", "0000")
    val skipped: Boolean get() = (doc.optJSONArray("skipped_modes")?.length() ?: 0) > 0
    /** Why a part of the stress test did not run, if one did not. */
    val stressProblem: String get() = doc.optJSONArray("unavailable")?.let { a ->
        (0 until a.length()).mapNotNull { a.optJSONObject(it) }.firstOrNull { it.optString("mode") == "stress" }?.optString("reason")
    }.orEmpty()
    /** A run of the stress test, even one whose part could not run. */
    val stressed: Boolean get() = stressTests.any { it.ran(doc) } || stressProblem.isNotEmpty()
    val measured: Int get() = tests.count { runCatching { it.get(doc) }.getOrNull() != null }
    val quick: Boolean get() = doc.optJSONObject("config")?.optBoolean("quick") == true
    val complete: Boolean get() = doc.optBoolean("complete", false)

    /**
     * Measured while other programs were working (as pmk_describe's `busy`): the preflight saw more than
     * PMK_BUSY_LOAD. An unknown load (NaN, null in the file: /proc/stat closed to apps) never counts as busy.
     */
    val busy: Boolean get() = doc.optJSONObject("preflight")?.let { pre ->
        !pre.isNull("background_load") && pre.optDouble("background_load", Double.NaN).let { it.isFinite() && it > Prismark.BUSY_LOAD }
    } == true
}

object Session {
    var running by mutableStateOf(false)
        private set
    var phase by mutableStateOf("")
        private set
    var part by mutableStateOf(0)
        private set
    var parts by mutableStateOf(0)
        private set
    /** The run is a stress test, and if open, one without a set length: stopping it is its normal end. */
    var stressing by mutableStateOf(false)
        private set
    var stressOpen by mutableStateOf(false)
        private set
    var stressMinutes by mutableStateOf(0)
        private set
    var current by mutableStateOf<Run?>(null)
    var error by mutableStateOf<String?>(null)
    val warnings = mutableStateListOf<String>()
    val selected = mutableStateListOf<String>().apply { addAll(tests.map { it.id }) }
    var quick by mutableStateOf(false)
    val runs = mutableStateListOf<Run>()

    private val main = Handler(Looper.getMainLooper())

    fun resultsDir(context: Context) = File(context.filesDir, "runs").apply { mkdirs() }

    /** Reads the saved results, newest first. */
    fun load(context: Context) {
        val files = resultsDir(context).listFiles { f -> f.name.endsWith(".json") }?.sortedByDescending { it.lastModified() }.orEmpty()
        val read = files.map { f -> Run(f, runCatching { f.readText() }.getOrDefault("{}")) }
        main.post {
            runs.clear()
            runs.addAll(read)
        }
    }

    /**
     * Starts a run of the selected tests on a worker thread. The listener is called on that thread between
     * measurement windows; the screen is updated once per step, and in the quiet phases only between tests.
     */
    fun start(context: Context, chosen: List<Test>, quick: Boolean) {
        if (running || chosen.isEmpty()) return
        val (modes, kernels) = runRequest(chosen)
        val bits = chosen.fold(0) { all, test -> all or test.bit }
        val settings = StressChoice.read(context)
        val dir = resultsDir(context.applicationContext)
        running = true
        error = null
        warnings.clear()
        phase = "Starting"
        part = 0
        parts = 0
        stressing = bits != 0
        stressOpen = bits != 0 && settings.seconds == 0.0
        stressMinutes = (settings.seconds / 60).toInt()
        var lastPhase = ""
        val listener = Prismark.Listener { kind, ph, _, message, shown, total, _ ->
            when (kind) {
                0 -> if (ph != null && !(ph == lastPhase && ph in quietPhases)) {
                    lastPhase = ph
                    val name = phaseNames[ph] ?: ph
                    main.post { phase = name }
                }
                1 -> if (ph == "progress") main.post {
                    part = shown
                    parts = total
                }
                2 -> if (message != null) main.post { warnings += message }
            }
        }
        Thread({
            val result = runCatching {
                if (bits != 0) Prismark.runStress(bits, settings.seconds, settings.stopOnError, settings.maxTempC, listener)
                else Prismark.run(modes, kernels, quick, listener)
            }
            main.post {
                result.onSuccess { json ->
                    val id = runCatching { JSONObject(json).optString("run_id") }.getOrNull().orEmpty().ifEmpty { "run" }
                    val file = File(dir, "prismark-${System.currentTimeMillis()}-$id.json")
                    runCatching { file.writeText(json) }.onFailure { error = "The result could not be saved: ${it.message}" }
                    current = Run(file, json)
                    load(context.applicationContext)
                }.onFailure { error = it.message ?: it.toString() }
                running = false
            }
        }, "prismark-run").start()
    }
}

/** The other machines, from the same reduced file the website uses. */
object References {
    @Volatile private var docs: List<JSONObject>? = null

    fun all(context: Context): List<JSONObject> = docs ?: runCatching {
        val array = JSONArray(context.applicationContext.assets.open("references.json").bufferedReader().use { it.readText() })
        (0 until array.length()).mapNotNull { array.optJSONObject(it) }
    }.getOrDefault(emptyList()).also { docs = it }
}

class Rank(val name: String, val os: String, val value: Value, val yours: Boolean, val runId: String)

/** One test, every machine, best first. */
fun ranking(context: Context, test: Test, run: Run?): List<Rank> {
    val rows = References.all(context).mapNotNull { doc ->
        val value = runCatching { test.get(doc) }.getOrNull() ?: return@mapNotNull null
        val machine = doc.optJSONObject("machine")
        Rank(shortName(machine?.optJSONObject("cpu")?.optString("model").orEmpty()), machine?.optString("os").orEmpty(), value, false, doc.optString("run_id"))
    }.toMutableList()
    if (run != null) runCatching { test.get(run.doc) }.getOrNull()?.let { rows += Rank("This device", "", it, true, run.runId) }
    return if (test.higherBetter) rows.sortedByDescending { it.value.v } else rows.sortedBy { it.value.v }
}

fun shortName(model: String): String {
    var name = model.replace("Ryzen Threadripper PRO", "Threadripper")
    for (junk in listOf("(R)", "(TM)", "AMD ", "Intel ", " Processor", "Genuine ", "Apple ")) name = name.replace(junk, "")
    for (cut in listOf(" CPU", " w/", " 6-Core", " 64-Cores")) name = name.substringBefore(cut)
    return name.trim().ifEmpty { "?" }
}
