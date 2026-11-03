/*
 * Saved runs, one run, one test. Each test stands on its own: there is no total.
 *
 * Copyright 2026 The Prismark Authors. Apache-2.0.
 */
package org.prismark.app

import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.items
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
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import org.json.JSONObject

private class OpenTest(val test: Test, val run: Run)

/** A group's tests in this run. Each core in turn counts only where it ran: not every phone offers it. */
private fun membersOf(group: String, run: Run) = allTests.filter { it.group == group && (it.part != "each_core" || it.ran(run.doc)) }

@Composable
fun ResultPage(page: Any) {
    when (page) {
        is Run -> ResultsScreen(page)
        is OpenTest -> TestScreen(page.test, page.run)
    }
}

/** Your saved runs, each one a stub with its own barcode. Newest first. */
@Composable
fun RunsScreen() {
    val p = LocalPalette.current
    val pages = LocalPages.current

    Column(Modifier.fillMaxSize(), verticalArrangement = Arrangement.spacedBy(6.dp)) {
        Row(
            Modifier.fillMaxWidth().background(p.panel).padding(top = topInset() + 16.dp, start = 20.dp, end = 20.dp, bottom = 14.dp),
            verticalAlignment = Alignment.Bottom,
        ) {
            BasicText("Your runs", style = Type.saira(36.sp, 112, 700), modifier = Modifier.weight(1f))
            Mono("${Session.runs.size} saved", p.muted)
        }
        if (Session.runs.isEmpty()) {
            BasicText(
                "No runs yet. Your results will be kept here.", style = Type.archivo(16.sp, color = p.muted),
                modifier = Modifier.fillMaxSize().background(p.panel).padding(20.dp),
            )
        } else {
            LazyVerticalGrid(
                GridCells.Adaptive(280.dp), Modifier.fillMaxSize(),
                horizontalArrangement = Arrangement.spacedBy(6.dp), verticalArrangement = Arrangement.spacedBy(6.dp),
            ) {
                items(Session.runs, key = { it.file.name }) { run ->
                    RunStub(run, latest = run == Session.runs.firstOrNull()) { pages.push(run) }
                }
            }
        }
    }
}

@Composable
private fun RunStub(run: Run, latest: Boolean, onOpen: () -> Unit) {
    val p = LocalPalette.current
    val text = if (latest) Ink else p.text

    Row(Modifier.fillMaxWidth().height(170.dp).background(if (latest) Volt else p.panel).tap(onClick = onOpen)) {
        Column(Modifier.weight(1f).fillMaxHeight().padding(14.dp), verticalArrangement = Arrangement.SpaceBetween) {
            Row(Modifier.fillMaxWidth()) {
                BasicText(run.time, style = Type.mono(11.sp, color = text), modifier = Modifier.weight(1f))
                BasicText(if (run.stressed && run.measured == 0) "" else "${run.measured} of ${tests.size}", style = Type.mono(11.sp, color = text))
            }
            BasicText(run.shortDate, style = Type.saira(62.sp, 50, 600, text), maxLines = 1)
            Row(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                Tag(if (run.stressed && run.measured == 0) "STRESS" else if (run.quick) "QUICK" else "FULL", latest)
                if (!run.complete) Tag("PARTIAL", latest)
                if (run.busy) Tag("BUSY", latest)
                if (run.skipped) Tag("SKIPPED", latest)
            }
        }
        Tear(text, vertical = true)
        Barcode(run.runId, text, Modifier.width(68.dp).fillMaxHeight().padding(12.dp), vertical = true)
    }
}

@Composable
private fun Tag(text: String, latest: Boolean) {
    val p = LocalPalette.current
    BasicText(
        text, style = Type.mono(11.sp, color = if (latest) Volt else p.text),
        modifier = Modifier.background(if (latest) Ink else p.raised).padding(horizontal = 7.dp, vertical = 3.dp),
    )
}

/** Saves the result file where the person chooses. Android asks where before anything is written. */
@Composable
fun rememberSaveFile(run: Run): () -> Unit {
    val context = LocalContext.current
    val launcher = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("application/json")) { uri ->
        if (uri != null) runCatching { context.contentResolver.openOutputStream(uri)?.use { it.write(run.json.toByteArray()) } }
            .onFailure { Session.error = "The result file was not saved: ${it.message}" }
    }
    return { launcher.launch(run.file.name) }
}

/** One run: each test on its own, a group at a time. */
@Composable
private fun ResultsScreen(run: Run) {
    val p = LocalPalette.current
    val pages = LocalPages.current
    val save = rememberSaveFile(run)
    val values = remember(run) { allTests.associateWith { runCatching { it.get(run.doc) }.getOrNull() } }
    var group by remember(run) {
        mutableStateOf(if (tests.any { it.group == "st" && it.ran(run.doc) }) "st" else allTests.firstOrNull { it.ran(run.doc) }?.group ?: if (run.stressed) stressGroup.id else "st")
    }
    val shown = allTests.filter { it.group == group && it.ran(run.doc) }

    Column(Modifier.fillMaxSize(), verticalArrangement = Arrangement.spacedBy(6.dp)) {
        Column(
            Modifier.fillMaxWidth().background(Volt).padding(top = topInset() + 12.dp, start = 16.dp, end = 16.dp, bottom = 14.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                BackButton(onVolt = true)
                Spacer(Modifier.weight(1f))
                Mono(if (run.stressed && run.measured == 0) "Stress test" else "${if (run.quick) "Quick" else "Full"} · ${run.measured} test${if (run.measured == 1) "" else "s"}", Ink, Modifier.padding(end = 8.dp))
                Box(
                    Modifier.size(44.dp).background(Ink).tap(onClick = save).semantics { contentDescription = "Save the result file" },
                    contentAlignment = Alignment.Center,
                ) { BasicText("↓", style = Type.archivo(20.sp, 700, Volt)) }
            }
            BasicText(run.started, style = Type.saira(24.sp, 112, 700, Ink), maxLines = 1, overflow = TextOverflow.Ellipsis)
        }
        Row(Modifier.fillMaxWidth().horizontalScroll(rememberScrollState()), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            for (item in if (run.stressed && run.measured == 0) listOf(stressGroup) else groups) {
                val on = item.id == group
                Box(
                    Modifier.height(48.dp).background(if (on) p.panel else p.steel)
                        .drawBehind {
                            val edge = 5.dp.toPx()
                            if (on) drawRect(Volt, Offset(0f, size.height - edge), Size(size.width, edge))
                        }
                        .tap { group = item.id },
                    contentAlignment = Alignment.Center,
                ) {
                    BasicText(item.label, style = Type.archivo(14.sp, 700), modifier = Modifier.padding(horizontal = 14.dp))
                }
            }
        }
        if (shown.isEmpty()) {
            BasicText(
                if (group == stressGroup.id && run.stressProblem.isNotEmpty()) "Not run: ${run.stressProblem}." else if (run.skipped) "Not measured in this run. A touch during the tests that measure waking up discarded them." else "Not measured in this run: cancelled, or not available here.",
                style = Type.archivo(15.sp, color = p.muted), modifier = Modifier.fillMaxSize().background(p.panel).padding(20.dp),
            )
        } else {
            // The tiles, then a card filling what they leave. On a small screen or with large text it all scrolls instead.
            BoxWithConstraints(Modifier.weight(1f).fillMaxWidth()) {
                val rows = shown.chunked(2)
                val tile = with(LocalDensity.current) { 150.sp.toDp() }
                val left = maxHeight - (tile + 6.dp) * rows.size
                Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                    for (row in rows) {
                        Row(Modifier.fillMaxWidth().height(tile), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                            for (test in row) {
                                val value = values[test]
                                val open = { pages.push(OpenTest(test, run)) }
                                if (value != null) ScoreTile(test, value, Modifier.weight(1f).fillMaxHeight(), open)
                                else StressTile(test, stressRow(run.doc, test.part.orEmpty()) ?: JSONObject(), Modifier.weight(1f).fillMaxHeight(), open)
                            }
                            if (row.size == 1) MissingTile(membersOf(group, run).size - shown.size, Modifier.weight(1f).fillMaxHeight())
                        }
                    }
                    if (left >= 100.dp) GroupCard((groups + stressGroup).first { it.id == group }, shown.size, membersOf(group, run).size, run, left) else Spacer(Modifier.height(bottomInset()))
                }
            }
        }
    }
}

/** A boarding card under the tiles, as on the tablet's test list: how far this run got in the group. */
@Composable
private fun GroupCard(group: Group, measured: Int, members: Int, run: Run, height: Dp) {
    val place = if (group.id == stressGroup.id) "Full load" else "${group.number} of 04"
    // Too short for a whole card: the count and the barcode side by side, as tall as there is room for.
    if (height < 230.dp) {
        val bottom = bottomInset() + 14.dp
        val room = (height - bottom - 44.dp).coerceAtLeast(20.dp)
        Column(
            Modifier.fillMaxWidth().height(height).background(Signal).padding(start = 14.dp, end = 14.dp, top = 12.dp, bottom = bottom),
            verticalArrangement = Arrangement.spacedBy(6.dp),
        ) {
            Row(Modifier.fillMaxWidth()) {
                Mono(group.label, OnSignal, Modifier.weight(1f))
                Mono(place, OnSignal)
            }
            Row(Modifier.fillMaxWidth().height(room), horizontalArrangement = Arrangement.spacedBy(14.dp), verticalAlignment = Alignment.CenterVertically) {
                BasicText("$measured/$members", style = Type.saira(with(LocalDensity.current) { (room * 1.2f).toSp() }, 50, 600, Ink), maxLines = 1, softWrap = false)
                Barcode(run.runId, Ink, Modifier.weight(1f).fillMaxHeight())
            }
        }
        return
    }
    Column(
        Modifier.fillMaxWidth().height(height).background(Signal).padding(start = 14.dp, end = 14.dp, top = 14.dp, bottom = bottomInset() + 14.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        Row(Modifier.fillMaxWidth()) {
            Mono(group.label, OnSignal, Modifier.weight(1f))
            Mono(place, OnSignal)
        }
        Spacer(Modifier.weight(1f))
        val big = (height.value * 0.34f).coerceAtMost(150f).sp
        BasicText("$measured/$members", style = Type.saira(big, 50, 600, Ink), maxLines = 1, softWrap = false, modifier = Modifier.capitalsOnly(big))
        BasicText("Measured in this run", style = Type.archivo(13.sp, 600, OnSignal))
        Tear(OnSignal)
        Barcode(run.runId, Ink, Modifier.fillMaxWidth().height((height * 0.16f).coerceAtMost(72.dp)))
    }
}

/** Beside a lone tile, in a run that left tests out. */
@Composable
private fun MissingTile(count: Int, modifier: Modifier) {
    Box(modifier.background(Signal).drawBehind { passDots() }.padding(12.dp), contentAlignment = Alignment.BottomStart) {
        Mono("$count not measured", OnSignal)
    }
}

@Composable
private fun ScoreTile(test: Test, value: Value, modifier: Modifier, onOpen: () -> Unit) {
    val p = LocalPalette.current
    Column(modifier.background(p.panel).tap(onClick = onOpen).padding(12.dp), verticalArrangement = Arrangement.SpaceBetween) {
        Row(Modifier.fillMaxWidth()) {
            BasicText(test.unit, style = Type.mono(11.sp, color = p.muted), modifier = Modifier.weight(1f))
            BasicText(if (test.higherBetter) "↑" else "↓", style = Type.mono(11.sp, color = p.muted))
        }
        BasicText(format(value.v), style = Type.saira(66.sp, 50, 600), maxLines = 1, modifier = Modifier.capitalsOnly(66.sp))
        BasicText(test.name, style = Type.archivo(14.sp, 700), maxLines = 2, modifier = Modifier.height(38.dp))
    }
}

/** One test: your score, then every machine. */
@Composable
private fun TestScreen(test: Test, run: Run) {
    val p = LocalPalette.current
    val context = LocalContext.current
    val rows = remember(test, run) { ranking(context, test, run) }
    val value = remember(test, run) { runCatching { test.get(run.doc) }.getOrNull() }
    val rank = rows.indexOfFirst { it.yours } + 1

    Column(Modifier.fillMaxSize(), verticalArrangement = Arrangement.spacedBy(6.dp)) {
        Column(
            Modifier.fillMaxWidth().background(p.panel).padding(top = topInset() + 12.dp, start = 20.dp, end = 20.dp, bottom = 14.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            BackButton()
            BasicText(test.name, style = Type.saira(28.sp, 112, 700), maxLines = 2)
            BasicText(if (test.bit > 0) "${test.sub}. A score needs 10 minutes." else "${test.sub}. ${if (test.higherBetter) "Higher" else "Lower"} is better.", style = Type.archivo(14.sp, color = p.muted))
        }
        val row = test.part?.let { stressRow(run.doc, it) }
        if (row != null) {
            StressBlock(test, row)
            Column(Modifier.fillMaxSize().background(p.panel).verticalScroll(rememberScrollState()).padding(bottom = bottomInset() + 8.dp)) {
                StressFacts(test, run, row)
            }
        } else {
            Row(Modifier.fillMaxWidth().background(Volt).padding(horizontal = 20.dp, vertical = 16.dp), verticalAlignment = Alignment.Bottom) {
                BasicText(value?.let { format(it.v) } ?: "—", style = Type.saira(96.sp, 50, 600, Ink), maxLines = 1, modifier = Modifier.weight(1f).capitalsOnly(96.sp))
                Column(horizontalAlignment = Alignment.End, verticalArrangement = Arrangement.spacedBy(6.dp)) {
                    BasicText(test.unit, style = Type.mono(12.sp, true, Ink))
                    BasicText(
                        when {
                            value == null -> "Not measured"
                            value.hasRange -> "95 % range\n${format(value.lo)}–${format(value.hi)}"
                            else -> "No range:\nmay be noise"
                        },
                        style = Type.mono(12.sp, color = Ink),
                    )
                    if (value != null && !run.quick && run.complete) {
                        BasicText(
                            "RANK %02d / %d".format(rank, rows.size), style = Type.mono(12.sp, color = Volt),
                            modifier = Modifier.background(Ink).padding(horizontal = 7.dp, vertical = 3.dp),
                        )
                    }
                }
            }
            Column(Modifier.fillMaxSize().background(p.panel).verticalScroll(rememberScrollState()).padding(bottom = bottomInset() + 8.dp)) {
                val top = rows.maxOfOrNull { it.value.v } ?: 1.0
                rows.forEachIndexed { index, row -> RankRow(index + 1, row, top, value?.v, highlighted = false, onTap = null) }
            }
        }
    }
}

/** One machine in a ranking. */
@Composable
fun RankRow(number: Int, row: Rank, top: Double, yours: Double?, highlighted: Boolean, onTap: (() -> Unit)?) {
    val p = LocalPalette.current
    val change = if (yours == null || yours == 0.0) null else (row.value.v / yours - 1) * 100
    val delta = if (change == null || kotlin.math.abs(change) < 0.05) "—" else (if (change > 0) "+" else "−") + "%.1f%%".format(kotlin.math.abs(change))

    Column(Modifier.fillMaxWidth().background(if (row.yours || highlighted) p.raised else Color.Transparent).then(if (onTap != null) Modifier.tap(onClick = onTap) else Modifier)) {
        Box(Modifier.fillMaxWidth().height(1.dp).background(p.line))
        Row(Modifier.fillMaxWidth().height(54.dp).padding(horizontal = 16.dp), verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            BasicText("%02d".format(number), style = Type.mono(12.sp), modifier = Modifier.width(22.dp))
            Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(5.dp)) {
                Row(verticalAlignment = Alignment.Bottom, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                    BasicText(row.name, style = Type.archivo(14.sp, if (row.yours) 700 else 500), maxLines = 1, overflow = TextOverflow.Ellipsis, modifier = Modifier.weight(1f, fill = false))
                    BasicText(row.os, style = Type.mono(10.sp, color = p.muted), maxLines = 1)
                }
                Box(Modifier.fillMaxWidth((row.value.v / top).toFloat().coerceIn(0.01f, 1f)).height(7.dp).background(if (row.yours) p.yours else p.reference))
            }
            BasicText(format(row.value.v), style = Type.saira(20.sp, 75, 600), modifier = Modifier.width(46.dp), maxLines = 1)
            BasicText(delta, style = Type.mono(11.sp), modifier = Modifier.width(50.dp), maxLines = 1)
        }
    }
}
