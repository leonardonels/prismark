/*
 * The stress test: minutes of full load, run on its own, with the desktop app's settings (apps/gui/mainwindow.cpp).
 * A run under 10 minutes has a verdict and no score.
 *
 * Copyright 2026 The Prismark Authors. Apache-2.0.
 */
package org.prismark.app

import android.content.Context
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.BasicText
import androidx.compose.foundation.verticalScroll
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import org.json.JSONObject

/** The settings of the next stress test, remembered between runs. */
class StressChoice(val seconds: Double, val stopOnError: Boolean, val maxTempC: Double) {
    companion object {
        fun read(context: Context): StressChoice {
            val p = context.getSharedPreferences("prismark", Context.MODE_PRIVATE)
            return StressChoice(p.getFloat("stress_s", 600f).toDouble(), p.getBoolean("stress_stop_on_error", true), p.getFloat("stress_max_temp", 0f).toDouble())
        }
    }
}

/** Moving from core to core means nothing where the system places the threads. */
fun stressOffered(): List<Test> {
    val pinned = DeviceInfo.doc.optJSONObject("machine")?.optJSONObject("capabilities")?.optBoolean("pinning") == true
    return stressTests.filter { pinned || it.part != "each_core" }
}

/** The row under the tests. Not picked with them: it runs on its own. */
@Composable
fun StressRow() {
    val p = LocalPalette.current
    val pages = LocalPages.current
    Row(
        Modifier.fillMaxWidth().height(56.dp).background(p.panel).tap { pages.push(stressGroup) }.padding(start = 56.dp, end = 14.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(Modifier.weight(1f)) {
            BasicText(stressGroup.label, style = Type.archivo(16.sp, 700))
            BasicText("Runs on its own, 10 min or more", style = Type.mono(11.sp))
        }
        Chevron(p.text)
    }
}

/** A row of choices, one of them on. */
@Composable
private fun Choices(name: String, items: List<Pair<String, Float>>, key: String, default: Float) {
    val p = LocalPalette.current
    val context = LocalContext.current
    val prefs = context.getSharedPreferences("prismark", Context.MODE_PRIVATE)
    var current by remember { mutableStateOf(prefs.getFloat(key, default)) }
    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
        Mono(name)
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            for ((label, value) in items) {
                val on = value == current
                Box(
                    Modifier.weight(1f).height(44.dp).background(if (on) Volt else p.raised).tap {
                        current = value
                        prefs.edit().putFloat(key, value).apply()
                    },
                    contentAlignment = Alignment.Center,
                ) { BasicText(label, style = Type.archivo(13.sp, 700, if (on) Ink else p.text), maxLines = 1) }
            }
        }
    }
}

/** The length, the temperature limit, and whether to stop at the first wrong result. */
@Composable
fun StressSettings(modifier: Modifier = Modifier) {
    val p = LocalPalette.current
    val context = LocalContext.current
    val prefs = context.getSharedPreferences("prismark", Context.MODE_PRIVATE)
    var stop by remember { mutableStateOf(prefs.getBoolean("stress_stop_on_error", true)) }
    Column(modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
        Choices("Length", listOf("10 min" to 600f, "30 min" to 1800f, "Until stopped" to 0f), "stress_s", 600f)
        Choices("Stop at a temperature", listOf("Never" to 0f, "85°" to 85f, "90°" to 90f, "95°" to 95f, "100°" to 100f), "stress_max_temp", 0f)
        BasicText("Only where the phone lets apps read the temperature.", style = Type.archivo(12.sp, color = p.muted))
        Row(
            Modifier.fillMaxWidth().height(44.dp).tap {
                stop = !stop
                prefs.edit().putBoolean("stress_stop_on_error", stop).apply()
            },
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            Check(stop)
            BasicText("Stop at the first wrong result", style = Type.archivo(14.sp, 600))
        }
    }
}

/** Pick the settings, then run one part. */
@Composable
fun StressScreen() {
    val p = LocalPalette.current
    val context = LocalContext.current
    Column(Modifier.fillMaxSize(), verticalArrangement = Arrangement.spacedBy(6.dp)) {
        Column(
            Modifier.fillMaxWidth().background(p.panel).padding(top = topInset() + 12.dp, start = 20.dp, end = 20.dp, bottom = 16.dp),
            verticalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            BackButton()
            BasicText(stressGroup.label, style = Type.saira(36.sp, 112, 700), maxLines = 1)
            BasicText(stressGroup.desc, style = Type.archivo(14.sp, color = p.muted))
        }
        Column(
            Modifier.weight(1f).fillMaxWidth().verticalScroll(rememberScrollState()).padding(bottom = bottomInset() + 8.dp),
            verticalArrangement = Arrangement.spacedBy(6.dp),
        ) {
            StressSettings(Modifier.fillMaxWidth().background(p.panel))
            for (test in stressOffered()) {
                Row(
                    Modifier.fillMaxWidth().height(72.dp).background(p.panel).tap { Session.start(context, listOf(test), false) }
                        .semantics { contentDescription = "Run ${test.name}" }.padding(horizontal = 16.dp),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(14.dp),
                ) {
                    Column(Modifier.weight(1f)) {
                        BasicText(test.name, style = Type.archivo(16.sp, 700))
                        BasicText(test.sub, style = Type.archivo(13.sp))
                    }
                    Box(Modifier.size(width = 64.dp, height = 44.dp).background(Volt), contentAlignment = Alignment.Center) {
                        BasicText("RUN", style = Type.saira(13.sp, 125, 800, Ink))
                    }
                }
            }
            BasicText(
                "It gets warm and uses a lot of battery, and the phone slows down if it gets too hot. The result is kept like any other.",
                style = Type.archivo(13.sp, color = p.muted), modifier = Modifier.fillMaxWidth().background(p.raised).padding(16.dp),
            )
        }
    }
}

private fun verdict(row: JSONObject) = when (row.optString("verdict")) {
    "failed" -> "Failed"
    "too_hot" -> "Too hot"
    else -> "Stable"
}

private fun JSONObject.number(key: String): Double = if (isNull(key)) Double.NaN else optDouble(key, Double.NaN)

/** A stress test under 10 minutes: its verdict, without a score. */
@Composable
fun StressTile(test: Test, row: JSONObject, modifier: Modifier, onOpen: () -> Unit) {
    val p = LocalPalette.current
    Column(modifier.background(p.panel).tap(onClick = onOpen).padding(12.dp), verticalArrangement = Arrangement.SpaceBetween) {
        BasicText("Under 10 min, no score", style = Type.mono(11.sp, color = p.muted), maxLines = 1)
        BasicText(
            verdict(row), style = Type.saira(66.sp, 50, 600, if (row.optString("verdict") == "failed") Fault else p.text),
            maxLines = 1, modifier = Modifier.capitalsOnly(66.sp),
        )
        BasicText(test.name, style = Type.archivo(14.sp, 700), maxLines = 2, modifier = Modifier.height(38.dp))
    }
}

/** The score, or the verdict alone when the run was too short for one. */
@Composable
fun StressBlock(test: Test, row: JSONObject) {
    val score = row.optJSONObject("score")?.number("est")?.takeIf { it.isFinite() }
    val size = if (score == null) 72.sp else 96.sp
    Row(
        Modifier.fillMaxWidth().background(if (row.optString("verdict") == "failed") Fault else Volt).padding(horizontal = 20.dp, vertical = 16.dp),
        verticalAlignment = Alignment.Bottom,
    ) {
        BasicText(score?.let { format(it) } ?: verdict(row), style = Type.saira(size, 50, 600, Ink), maxLines = 1, modifier = Modifier.weight(1f).capitalsOnly(size))
        Column(horizontalAlignment = Alignment.End, verticalArrangement = Arrangement.spacedBy(6.dp)) {
            if (score != null) {
                BasicText(test.unit, style = Type.mono(12.sp, true, Ink))
                BasicText(verdict(row).uppercase(), style = Type.mono(12.sp, color = Volt), modifier = Modifier.background(Ink).padding(horizontal = 7.dp, vertical = 3.dp))
            } else {
                BasicText("Under 10 min:\nno score", style = Type.mono(12.sp, color = Ink))
            }
        }
    }
}

/** The speed over time, and what else the run recorded. */
@Composable
fun StressFacts(test: Test, run: Run, row: JSONObject) {
    val p = LocalPalette.current
    val samples = remember(run, test.part) { speeds(run.doc, test.part) }
    Column(Modifier.fillMaxWidth().background(p.panel).padding(20.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
        if (samples.size > 1) {
            Mono("Speed over time, ${test.unit}")
            StressChart(samples, Modifier.fillMaxWidth().height(96.dp))
        }
        for ((name, value) in facts(row, test)) Fact(name, value)
        row.optJSONArray("cores")?.let { cores ->
            if (cores.length() > 0) Mono("Each core", modifier = Modifier.padding(top = 8.dp))
            for (i in 0 until cores.length()) {
                val core = cores.optJSONObject(i) ?: continue
                val wrong = core.optLong("wrong")
                Fact("CPU ${core.optInt("cpu")}", "${format(core.optJSONObject("perf")?.number("est") ?: Double.NaN)} ${test.unit}" + if (wrong > 0) ", $wrong wrong" else "")
            }
        }
    }
}

@Composable
private fun Fact(name: String, value: String) {
    Row(horizontalArrangement = Arrangement.spacedBy(10.dp), verticalAlignment = Alignment.CenterVertically) {
        BasicText(name, style = Type.mono(12.sp, color = LocalPalette.current.muted), modifier = Modifier.width(96.dp))
        BasicText(value, style = Type.archivo(14.sp, 600))
    }
}

/** One series only: core cycling keeps one for each core, listed instead. */
private fun speeds(doc: JSONObject, part: String?): List<Double> {
    val results = doc.optJSONArray("results") ?: return emptyList()
    val series = (0 until results.length()).mapNotNull { results.optJSONObject(it) }
        .filter { it.optString("mode") == "stress" && it.optJSONObject("params")?.optString("part") == part }
    val samples = series.singleOrNull()?.optJSONArray("samples") ?: return emptyList()
    return (0 until samples.length()).map { samples.optDouble(it) }.filter { it.isFinite() }
}

private fun facts(row: JSONObject, test: Test): List<Pair<String, String>> = buildList {
    val seconds = row.number("duration_s")
    if (seconds.isFinite()) add("Ran for" to "%d:%02d".format(seconds.toInt() / 60, seconds.toInt() % 60))
    val first = row.optJSONObject("first_minute")?.number("est") ?: Double.NaN
    if (first.isFinite()) add("First minute" to "${format(first)} ${test.unit}")
    val last = row.optJSONObject("last_minute")?.number("est") ?: Double.NaN
    if (last.isFinite()) add("Last minute" to "${format(last)} ${test.unit}")
    val hold = row.optJSONObject("hold_up")?.number("est") ?: Double.NaN
    if (hold.isFinite()) add("Holds up" to "%.0f %% of its start".format(hold * 100))
    val hot = row.number("temp_max_c")
    if (hot.isFinite()) add("Hottest" to "%.0f °C".format(hot))
    val mhz = row.number("mhz")
    if (mhz.isFinite() && mhz > 0) add("Clock" to "%.0f MHz".format(mhz))
    val bytes = row.number("bytes_tested")
    if (bytes.isFinite()) add("Tested" to "%.1f GB".format(bytes / 1_073_741_824))
    add("Wrong" to "${row.optLong("wrong_tasks") + row.optLong("checksum_errors")} results")
    when (row.optString("stop_reason")) {
        "user" -> add("Ended" to "You stopped it")
        "error" -> add("Ended" to "At the first wrong result")
        "temperature" -> add("Ended" to "At the temperature limit")
    }
}

/** Flat bars, at most one for each 60th of the run. */
@Composable
private fun StressChart(values: List<Double>, modifier: Modifier) {
    val color = LocalPalette.current.reference
    Canvas(modifier) {
        val per = maxOf(1, values.size / 60)
        val bars = values.chunked(per) { it.average() }
        val top = bars.maxOrNull() ?: return@Canvas
        if (top <= 0) return@Canvas
        val width = size.width / bars.size
        bars.forEachIndexed { index, bar ->
            val height = size.height * (bar / top).toFloat()
            drawRect(color, Offset(index * width, size.height - height), Size(maxOf(1f, width - 1.dp.toPx()), height))
        }
    }
}
