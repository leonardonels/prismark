/*
 * The tablet follows the desktop app: the tests, what the open test means with its ranking, and a report. The
 * reference machines are there from the start, before any run.
 *
 * Copyright 2026 The Prismark Authors. Apache-2.0.
 */
package org.prismark.app

import androidx.compose.foundation.background
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
import androidx.compose.foundation.layout.heightIn
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
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.min
import androidx.compose.ui.unit.sp
import org.json.JSONObject

@Composable
fun TabletTests() {
    val context = LocalContext.current
    var group by rememberSaveable { mutableStateOf("st") }
    var testId by rememberSaveable { mutableStateOf("st_k2") }
    var reportId by remember { mutableStateOf<String?>(null) }
    val test = allTests.first { it.id == testId }
    val latest = Session.runs.firstOrNull()
    val report: JSONObject = reportId?.let { id ->
        Session.runs.firstOrNull { it.runId == id }?.doc ?: References.all(context).firstOrNull { it.optString("run_id") == id }
    } ?: latest?.doc ?: DeviceInfo.doc

    BoxWithConstraints(Modifier.fillMaxSize()) {
        val wide = maxWidth > 900.dp
        Column(Modifier.fillMaxSize().padding(top = topInset() + 6.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
            TabletHeader(group, wide) { id ->
                group = id
                testId = allTests.first { it.group == id }.id
            }
            Row(Modifier.weight(1f).fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                TestList(group, testId, Modifier.width(if (wide) 300.dp else 270.dp).fillMaxHeight()) { testId = it }
                TestPage(test, report, reportId, showReport = !wide, Modifier.weight(1f).fillMaxHeight(), onPickRow = { reportId = it }, onPickTest = { id ->
                    testId = id
                    group = allTests.first { it.id == id }.group
                })
                if (wide) {
                    Report(report, inline = false, Modifier.width(300.dp).fillMaxHeight()) { id ->
                        testId = id
                        group = allTests.first { it.id == id }.group
                    }
                }
            }
        }
    }
}

@Composable
private fun TabletHeader(group: String, wide: Boolean, onPick: (String) -> Unit) {
    val p = LocalPalette.current
    val context = LocalContext.current
    Row(Modifier.fillMaxWidth().height(64.dp), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
        Logo(30.dp, if (wide) 16.sp else null, Modifier.fillMaxHeight())
        for (item in groups + stressGroup) {
            val on = item.id == group
            Box(Modifier.weight(1f).fillMaxHeight().background(if (on) p.panel else p.steel).tap { onPick(item.id) }) {
                Column(Modifier.fillMaxSize().padding(horizontal = 10.dp, vertical = 9.dp), verticalArrangement = Arrangement.SpaceBetween) {
                    BasicText(item.number, style = Type.mono(11.sp))
                    BasicText(item.label, style = Type.archivo(13.sp, 700), maxLines = 1, overflow = TextOverflow.Ellipsis)
                }
                if (on) Box(Modifier.align(Alignment.BottomStart).fillMaxWidth().height(5.dp).background(Volt))
            }
        }
        QuickSwitch(64.dp)
        Row(
            Modifier.fillMaxHeight().background(Volt).tap { Session.start(context, tests, Session.quick) }.padding(horizontal = 12.dp),
            verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            BasicText(if (Session.quick) "RUN ALL QUICK" else "RUN ALL FULL", style = Type.saira(14.sp, 125, 800, Ink), maxLines = 1, softWrap = false)
            if (wide) Chevrons(Ink)
        }
    }
}

@Composable
private fun TestList(group: String, testId: String, modifier: Modifier, onPick: (String) -> Unit) {
    val p = LocalPalette.current
    val about = (groups + stressGroup).first { it.id == group }
    val members = if (group == stressGroup.id) stressOffered() else tests.filter { it.group == group }

    BoxWithConstraints(modifier) {
        val full = maxHeight
        Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState())) {
            Column(Modifier.heightIn(min = full).fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                Column(Modifier.fillMaxWidth().background(p.panel).padding(14.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                    Mono("Tests · ${about.label}")
                    BasicText(about.desc, style = Type.archivo(13.sp, color = p.muted))
                }
                for (test in members) TestCard(test, test.id == testId) { onPick(test.id) }
                if (group == stressGroup.id) StressSettings(Modifier.fillMaxWidth().background(p.panel))
                GroupCard(about, members, Modifier.weight(1f).fillMaxWidth())
            }
        }
    }
}

/** One test: tap to open it, or run it on its own. */
@Composable
private fun TestCard(test: Test, current: Boolean, onOpen: () -> Unit) {
    val p = LocalPalette.current
    val context = LocalContext.current
    val value = Session.runs.firstNotNullOfOrNull { run -> runCatching { test.get(run.doc) }.getOrNull() }
    val text = if (current) Ink else p.text

    Row(Modifier.fillMaxWidth().height(112.dp).background(if (current) Volt else p.panel), verticalAlignment = Alignment.CenterVertically) {
        Column(Modifier.weight(1f).fillMaxHeight().tap(onClick = onOpen).padding(start = 14.dp, top = 10.dp, bottom = 10.dp), verticalArrangement = Arrangement.SpaceBetween) {
            Column(verticalArrangement = Arrangement.spacedBy(2.dp)) {
                BasicText(test.name, style = Type.archivo(15.sp, 700, text), maxLines = 2)
                BasicText(test.sub, style = Type.archivo(12.sp, color = text), maxLines = 2)
            }
            if (value != null) {
                Row(verticalAlignment = Alignment.Bottom, horizontalArrangement = Arrangement.spacedBy(5.dp)) {
                    BasicText(format(value.v), style = Type.saira(30.sp, 50, 600, text), maxLines = 1)
                    BasicText(test.unit, style = Type.mono(10.sp, color = text), maxLines = 1)
                }
            } else {
                BasicText("No result yet", style = Type.mono(11.sp, color = if (current) Ink else p.muted))
            }
        }
        Box(
            Modifier.padding(10.dp).size(width = 72.dp, height = 56.dp).background(if (current) Ink else p.raised)
                .tap { Session.start(context, listOf(test), Session.quick) },
            contentAlignment = Alignment.Center,
        ) {
            BasicText(if (test.bit > 0) "RUN" else if (Session.quick) "RUN\nQUICK" else "RUN\nFULL", style = Type.saira(12.sp, 125, 800, if (current) Volt else p.text))
        }
    }
}

/** A boarding card under the tests: it grows to fill whatever they leave, and says how far the group has got. */
@Composable
private fun GroupCard(group: Group, members: List<Test>, modifier: Modifier) {
    val run = Session.runs.firstOrNull()
    val measured = if (run == null) 0 else members.count { it.ran(run.doc) }

    BoxWithConstraints(modifier.background(Signal)) {
        val tall = maxHeight
        if (tall >= 150.dp) {
            Column(Modifier.fillMaxSize().padding(start = 14.dp, end = 14.dp, top = 14.dp, bottom = bottomInset() + 14.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Row(Modifier.fillMaxWidth()) {
                    Mono(group.label, OnSignal, Modifier.weight(1f))
                    Mono(if (group.id == stressGroup.id) "Full load" else "${group.number} of 04", OnSignal)
                }
                Spacer(Modifier.weight(1f))
                val big = min(tall * 0.34f, 150.dp).value.sp
                BasicText("$measured/${members.size}", style = Type.saira(big, 50, 600, Ink), maxLines = 1, modifier = Modifier.capitalsOnly(big))
                BasicText(if (run == null) "No run yet" else "Measured in your latest run", style = Type.archivo(13.sp, 600, OnSignal))
                Tear(OnSignal)
                Barcode(run?.runId ?: "5a3c91e7b20d48f6a1c7e93b", Ink, Modifier.fillMaxWidth().height(min(tall * 0.16f, 72.dp)))
            }
        }
    }
}

@Composable
private fun TestPage(test: Test, report: JSONObject, reportId: String?, showReport: Boolean, modifier: Modifier, onPickRow: (String) -> Unit, onPickTest: (String) -> Unit) {
    val p = LocalPalette.current
    val context = LocalContext.current
    val text = testTexts[test.id]
    val rows = remember(test.id, Session.runs.firstOrNull()) { ranking(context, test, Session.runs.firstOrNull()) }
    val top = rows.maxOfOrNull { it.value.v } ?: 1.0
    val compared = remember(test.id, report) { runCatching { test.get(report) }.getOrNull()?.v }

    Column(modifier.background(p.panel).verticalScroll(rememberScrollState()).padding(bottom = bottomInset() + 20.dp)) {
        Column(Modifier.fillMaxWidth().padding(start = 20.dp, end = 20.dp, top = 20.dp, bottom = 8.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            BasicText(text?.title ?: test.name, style = Type.saira(30.sp, 112, 700), maxLines = 2)
            BasicText(text?.about.orEmpty(), style = Type.archivo(15.sp))
            Section("How it is measured", text?.how.orEmpty())
            Section("Reading the result", text?.reading.orEmpty())
            if (test.bit == 0) {
                Mono("Ranking", modifier = Modifier.padding(top = 8.dp))
                Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                    Legend("Your runs", p.yours)
                    Legend("Reference", p.reference)
                }
            }
        }
        test.part?.let { part ->
            val run = Session.runs.firstOrNull { stressRow(it.doc, part) != null }
            val row = run?.let { stressRow(it.doc, part) }
            if (run != null && row != null) {
                StressBlock(test, row)
                StressFacts(test, run, row)
            } else {
                BasicText("No result yet. Run it from the list: it takes as long as you set there.", style = Type.archivo(14.sp, color = p.muted), modifier = Modifier.padding(horizontal = 20.dp))
            }
        }
        rows.forEachIndexed { index, row ->
            RankRow(index + 1, row, top, compared, highlighted = row.runId == reportId, onTap = { onPickRow(row.runId) })
        }
        if (showReport) Report(report, inline = true, Modifier.fillMaxWidth(), onPickTest)
    }
}

@Composable
private fun Section(title: String, text: String) {
    Column(Modifier.padding(top = 8.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
        Mono(title)
        BasicText(text, style = Type.archivo(14.sp))
    }
}

@Composable
private fun Legend(text: String, color: androidx.compose.ui.graphics.Color) {
    Row(
        Modifier.background(LocalPalette.current.raised).padding(horizontal = 10.dp, vertical = 6.dp),
        verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp),
    ) {
        Box(Modifier.size(8.dp).background(color))
        Mono(text)
    }
}

/** The report of one result: which machine, what it scored, and the file if it is yours. */
@Composable
private fun Report(doc: JSONObject, inline: Boolean, modifier: Modifier, onPickTest: (String) -> Unit) {
    val p = LocalPalette.current
    val machine = doc.optJSONObject("machine")
    val cpu = machine?.optJSONObject("cpu")
    val model = shortName(cpu?.optString("model").orEmpty())
    val id = doc.optString("run_id").takeIf { it.isNotEmpty() }
    val run = Session.runs.firstOrNull { it.runId == id }
    val measured = allTests.filter { runCatching { it.get(doc) }.getOrNull() != null }
    val quick = doc.optJSONObject("config")?.optBoolean("quick") == true

    Column(modifier, verticalArrangement = Arrangement.spacedBy(if (inline) 0.dp else 6.dp)) {
        val tag = if (id == null) "NO RUN YET" else if (run == null) "REFERENCE" else "YOUR RUN"
        if (inline) {
            Box(Modifier.fillMaxWidth().height(1.dp).background(p.line))
            Column(Modifier.fillMaxWidth().padding(start = 20.dp, end = 20.dp, top = 20.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                    Mono("Report", p.muted, Modifier.weight(1f))
                    Tags(tag, run != null, quick)
                }
                BasicText(model, style = Type.saira(30.sp, 112, 700), maxLines = 1, overflow = TextOverflow.Ellipsis)
            }
        } else {
            Column(Modifier.fillMaxWidth().background(Signal).padding(16.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
                Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                    BasicText("Processor:", style = Type.mono(12.sp, color = OnSignal), modifier = Modifier.weight(1f))
                    Tags(tag, run != null, quick)
                }
                val big = chipSize(model, 92f, 6).sp
                BasicText(model, style = Type.saira(big, 50, 600, Ink), maxLines = 1, softWrap = false, modifier = Modifier.capitalsOnly(big))
            }
        }

        val results: @Composable () -> Unit = {
            Column(Modifier.fillMaxWidth().background(p.panel).padding(if (inline) 20.dp else 16.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
                Mono("Results")
                if (measured.isEmpty()) {
                    BasicText("No results yet. Run a test, or pick a machine in the ranking to see its report.", style = Type.archivo(14.sp, color = p.muted))
                }
                for (test in measured) {
                    Row(Modifier.fillMaxWidth().heightIn(min = 24.dp).tap { onPickTest(test.id) }, verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                        BasicText(test.name, style = Type.archivo(13.sp, 500), maxLines = 1, overflow = TextOverflow.Ellipsis, modifier = Modifier.weight(1f))
                        BasicText(format(test.get(doc)!!.v), style = Type.archivo(13.sp, 700))
                        BasicText(test.unit, style = Type.mono(10.sp, color = p.muted))
                    }
                }
                Box(Modifier.fillMaxWidth().height(1.dp).background(p.line))
                Mono(if (id == null || run != null) "This device" else "This machine")
                Line("Processor", cpu?.optString("model").orEmpty().ifEmpty { "Unknown" })
                Line("System", machine?.optString("os").orEmpty())
            }
        }
        if (inline) {
            results()
        } else {
            Column(Modifier.weight(1f).fillMaxWidth().background(p.panel).verticalScroll(rememberScrollState())) { results() }
        }

        if (run != null) {
            val save = rememberSaveFile(run)
            val context = LocalContext.current
            Column(Modifier.fillMaxWidth().background(Volt).padding(start = 14.dp, end = 14.dp, top = 14.dp, bottom = if (inline) 14.dp else bottomInset() + 14.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                BasicText(run.started, style = Type.mono(11.sp, color = Ink))
                Barcode(run.runId, Ink, Modifier.fillMaxWidth().height(36.dp))
                Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                    Box(Modifier.weight(1f).height(44.dp).background(Ink).tap(onClick = save), contentAlignment = Alignment.Center) {
                        BasicText("Save", style = Type.archivo(14.sp, 700, Volt))
                    }
                    Box(Modifier.weight(1f).height(44.dp).background(Ink).tap { shareResult(context, run) }, contentAlignment = Alignment.Center) {
                        BasicText("Share", style = Type.archivo(14.sp, 700, Volt))
                    }
                }
            }
        }
    }
}

@Composable
private fun Tags(first: String, strong: Boolean, quick: Boolean) {
    Row(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
        BasicText(first, style = Type.mono(11.sp, color = Ink), modifier = Modifier.background(if (strong) Volt else OnSignal).padding(horizontal = 7.dp, vertical = 3.dp))
        if (quick) BasicText("QUICK", style = Type.mono(11.sp, color = Ink), modifier = Modifier.background(OnSignal).padding(horizontal = 7.dp, vertical = 3.dp))
    }
}

@Composable
private fun Line(label: String, value: String) {
    Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
        BasicText(label, style = Type.mono(12.sp, color = LocalPalette.current.muted), modifier = Modifier.width(76.dp))
        BasicText(value, style = Type.archivo(13.sp, 600))
    }
}
