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
import org.json.JSONObject
import org.prismark.Prismark
import java.io.File

/** A saved result: the document exactly as the core wrote it, and its file. */
class Run(val file: File, val json: String) {
    val doc: JSONObject by lazy { runCatching { JSONObject(json) }.getOrElse { JSONObject() } }
    val started: String get() = doc.optString("started_utc").replace('T', ' ').removeSuffix("Z").let { if (it.isEmpty()) file.name else "$it UTC" }
    val quick: Boolean get() = doc.optJSONObject("config")?.optBoolean("quick") == true
    val complete: Boolean get() = doc.optBoolean("complete", false)
}

object Session {
    var running by mutableStateOf(false)
        private set
    var phase by mutableStateOf("")
        private set
    var step by mutableStateOf("")
        private set
    var current by mutableStateOf<Run?>(null)
    var error by mutableStateOf<String?>(null)
    val warnings = mutableStateListOf<String>()
    val selected = mutableStateListOf<String>().apply { addAll(tests.map { it.id }) }
    var quick by mutableStateOf(true)

    private val main = Handler(Looper.getMainLooper())

    fun resultsDir(context: Context) = File(context.filesDir, "runs").apply { mkdirs() }

    fun saved(context: Context): List<File> =
        resultsDir(context).listFiles { f -> f.name.endsWith(".json") }?.sortedByDescending { it.lastModified() }.orEmpty()

    /**
     * Starts a run of the selected tests on a worker thread. The listener is called on that thread between
     * measurement windows; the screen is updated once per step, and not at all during the quiet phases.
     */
    fun start(context: Context, chosen: List<Test>, quick: Boolean) {
        if (running || chosen.isEmpty()) return
        val (modes, kernels) = runRequest(chosen)
        val dir = resultsDir(context.applicationContext)
        running = true
        error = null
        warnings.clear()
        phase = "Starting"
        step = ""
        var lastPhase = ""
        val listener = Prismark.Listener { kind, ph, _, message, _, _, _ ->
            when (kind) {
                0 -> if (ph != null && !(ph == lastPhase && ph in quietPhases)) {
                    lastPhase = ph
                    val name = phaseNames[ph] ?: ph
                    main.post { phase = name }
                }
                1 -> if (ph == "progress" && message != null && lastPhase !in quietPhases) main.post { step = message }
                2 -> if (message != null) main.post { warnings += message }
            }
        }
        Thread({
            val result = runCatching { Prismark.run(modes, kernels, quick, listener) }
            main.post {
                result.onSuccess { json ->
                    val id = runCatching { JSONObject(json).optString("run_id") }.getOrNull().orEmpty().ifEmpty { "run" }
                    val file = File(dir, "prismark-${System.currentTimeMillis()}-$id.json")
                    runCatching { file.writeText(json) }.onFailure { error = "The result could not be saved: ${it.message}" }
                    current = Run(file, json)
                }.onFailure { error = it.message ?: it.toString() }
                running = false
            }
        }, "prismark-run").start()
    }
}
