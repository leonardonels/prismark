/*
 * Picking tests and measuring. While a run measures, nothing on the screen moves.
 *
 * Copyright 2026 The Prismark Authors. Apache-2.0.
 */
package org.prismark.app

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
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
import androidx.compose.foundation.layout.wrapContentSize
import androidx.compose.foundation.text.BasicText
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.draw.rotate
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import org.prismark.Prismark

@Composable
fun RunScreen() {
    Column(Modifier.fillMaxSize(), verticalArrangement = Arrangement.spacedBy(6.dp)) {
        MachinePass(Modifier.weight(1f).fillMaxWidth())
        for (group in groups) GroupRow(group)
        RunBar()
    }
}

@Composable
fun RunPage(page: Any) {
    if (page is Group) GroupScreen(page)
}

/** The blue pass: which processor this is. */
@Composable
fun MachinePass(modifier: Modifier = Modifier) {
    val context = LocalContext.current
    val machine = remember(DeviceInfo.doc) { Machine(context, DeviceInfo.doc) }

    Column(
        modifier.background(Signal).drawBehind { passDots() }.padding(top = topInset() + 12.dp, start = 20.dp, end = 20.dp, bottom = 14.dp),
        verticalArrangement = Arrangement.SpaceBetween,
    ) {
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
            Logo(24.dp, 14.sp, Modifier.height(40.dp))
            Spacer(Modifier.weight(1f))
            BasicText("REV. ${runCatching { Prismark.version }.getOrDefault("?")}", style = Type.mono(11.sp, color = OnSignal))
        }
        Column {
            BasicText("Processor:", style = Type.mono(12.sp, color = OnSignal))
            ChipName(machine, 130f)
            Box(Modifier.fillMaxWidth().padding(top = 6.dp).height(1.dp).background(SignalLine))
            BasicText(machine.summary, style = Type.archivo(13.sp, 600, OnSignal), modifier = Modifier.padding(top = 6.dp))
            DeviceInfo.error?.let { BasicText("The core could not describe this device: $it", style = Type.archivo(12.sp, 600, OnSignal)) }
        }
    }
}

/** One test group: the check picks them all, the rest opens the group. */
@Composable
private fun GroupRow(group: Group) {
    val p = LocalPalette.current
    val pages = LocalPages.current
    val members = tests.filter { it.group == group.id }
    val count = members.count { it.id in Session.selected }
    val on = count > 0
    val text = if (on) Ink else p.text

    Row(Modifier.fillMaxWidth().height(56.dp).background(if (on) Volt else p.panel), verticalAlignment = Alignment.CenterVertically) {
        Box(
            Modifier.size(56.dp).tap {
                if (on) Session.selected.removeAll(members.map { it.id }) else Session.selected.addAll(members.map { it.id })
            },
            contentAlignment = Alignment.Center,
        ) { Check(on) }
        Row(Modifier.weight(1f).height(56.dp).tap { pages.push(group) }.padding(end = 14.dp), verticalAlignment = Alignment.CenterVertically) {
            Column(Modifier.weight(1f)) {
                BasicText(group.label, style = Type.archivo(16.sp, 700, text))
                BasicText("$count of ${members.size} tests", style = Type.mono(11.sp, color = text))
            }
            Chevron(text)
        }
    }
}

/** Quick runs give a first look and are not ranked. */
@Composable
fun QuickSwitch(height: Dp, bottom: Dp = 0.dp) {
    val p = LocalPalette.current
    val on = Session.quick
    Column(
        Modifier.width(96.dp).height(height).background(p.panel).tap { Session.quick = !on }.padding(12.dp).padding(bottom = bottom),
        verticalArrangement = Arrangement.SpaceBetween,
    ) {
        Mono("Quick")
        Row(Modifier.background(if (on) Volt else p.line).padding(horizontal = 5.dp, vertical = 3.dp), verticalAlignment = Alignment.CenterVertically) {
            val fg = if (on) Ink else p.text
            if (on) BasicText("ON", style = Type.mono(10.sp, true, fg), modifier = Modifier.padding(end = 6.dp))
            Box(Modifier.size(12.dp).background(fg))
            if (!on) BasicText("OFF", style = Type.mono(10.sp, true, fg), modifier = Modifier.padding(start = 6.dp))
        }
    }
}

/** Both reach the screen's edges, like the rows above. */
@Composable
private fun RunBar() {
    val p = LocalPalette.current
    val context = LocalContext.current
    val chosen = tests.filter { it.id in Session.selected }
    val bottom = bottomInset()
    val height = 60.dp + bottom
    val ready = chosen.isNotEmpty()

    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
        QuickSwitch(height, bottom)
        Row(
            Modifier.weight(1f).height(height).background(if (ready) Volt else p.raised)
                .tap(enabled = ready) { Session.start(context, chosen, Session.quick) }
                .padding(start = 14.dp, end = 14.dp, bottom = bottom),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            BasicText(
                if (ready) "RUN ${chosen.size} TEST${if (chosen.size == 1) "" else "S"}" else "PICK A TEST",
                style = Type.saira(16.sp, 125, 800, if (ready) Ink else p.muted), modifier = Modifier.weight(1f), maxLines = 1,
            )
            if (ready) Chevrons(Ink)
        }
    }
}

/** Pick the tests of one group. */
@Composable
private fun GroupScreen(group: Group) {
    val p = LocalPalette.current
    val members = tests.filter { it.group == group.id }

    Column(Modifier.fillMaxSize(), verticalArrangement = Arrangement.spacedBy(6.dp)) {
        Column(
            Modifier.fillMaxWidth().background(p.panel).padding(top = topInset() + 12.dp, start = 20.dp, end = 20.dp, bottom = 16.dp),
            verticalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                BackButton()
                Spacer(Modifier.weight(1f))
                Mono("Tests · ${group.number} of 04", p.muted)
            }
            BasicText(group.label, style = Type.saira(if (group.label.length > 10) 28.sp else 36.sp, 112, 700), maxLines = 1)
            BasicText(group.desc, style = Type.archivo(14.sp, color = p.muted))
        }
        for (test in members) {
            val on = test.id in Session.selected
            val text = if (on) Ink else p.text
            Row(
                Modifier.fillMaxWidth().weight(1f, fill = false).height(84.dp).background(if (on) Volt else p.panel)
                    .tap { if (on) Session.selected.remove(test.id) else Session.selected.add(test.id) }
                    .padding(horizontal = 16.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(14.dp),
            ) {
                Check(on)
                Column {
                    BasicText(test.name, style = Type.archivo(16.sp, 700, text))
                    BasicText(test.sub, style = Type.archivo(13.sp, color = text))
                }
            }
        }
    }
}

/** Quiet while measuring: no animation and no timer. The text changes only between steps. */
@Composable
fun MeasuringScreen() {
    var asking by remember { mutableStateOf(false) }

    Box(Modifier.fillMaxSize().background(Signal)) {
        Column(
            Modifier.fillMaxSize().padding(top = topInset() + 12.dp, start = 20.dp, end = 20.dp, bottom = bottomInset() + 16.dp),
            verticalArrangement = Arrangement.spacedBy(16.dp),
        ) {
            Row(Modifier.fillMaxWidth()) {
                BasicText("RUN / MEASURING", style = Type.mono(12.sp, true, OnSignal), modifier = Modifier.weight(1f))
                BasicText(if (Session.quick) "QUICK RUN" else "FULL RUN", style = Type.mono(12.sp, true, OnSignal))
            }
            Row(Modifier.fillMaxWidth().weight(1f).background(Volt).padding(14.dp), verticalAlignment = Alignment.Top) {
                // As wide as the capitals alone, not the font's whole line, which would leave the words no room.
                val number = 220.dp
                Box(Modifier.size(width = number * 0.69f, height = number * 0.72f), contentAlignment = Alignment.Center) {
                    BasicText(
                        "%02d".format(Session.part), style = Type.saira(with(LocalDensity.current) { number.toSp() }, 50, 600, Ink), maxLines = 1, softWrap = false,
                        modifier = Modifier.wrapContentSize(unbounded = true).rotate(90f),
                    )
                }
                Column(Modifier.weight(1f).fillMaxHeight().padding(start = 14.dp), verticalArrangement = Arrangement.spacedBy(8.dp, Alignment.Bottom)) {
                    BasicText("NOW", style = Type.mono(11.sp, color = Ink))
                    BasicText(Session.phase, style = Type.saira(25.sp, 100, 700, Ink))
                    if (Session.parts > 0) BasicText("Step ${Session.part} of ${Session.parts}", style = Type.mono(12.sp, color = Ink))
                }
            }
            BasicText("Leave it on a table and do not touch it.", style = Type.archivo(19.sp, 600, OnSignal))
            BasicText(
                "Tests that measure waking up can take up to half an hour in a full run. The step number moves only between tests, so the screen cannot disturb them. Any touch, even on Cancel run, discards them.",
                style = Type.archivo(14.sp, color = OnSignal),
            )
            Box(
                Modifier.fillMaxWidth().height(60.dp).border(2.dp, OnSignal).tap { asking = true },
                contentAlignment = Alignment.Center,
            ) { BasicText("Cancel run", style = Type.archivo(16.sp, 600, OnSignal)) }
        }
        if (asking) {
            Box(Modifier.fillMaxSize().background(Ink.copy(alpha = 0.78f)).tap { })
            CancelStub(
                onCancel = {
                    asking = false
                    Prismark.cancel()
                },
                onKeep = { asking = false },
                modifier = Modifier.align(Alignment.BottomCenter),
            )
        }
    }
}

/** A dialog is a stub torn off the pass: a code, one sentence, two buttons. */
@Composable
private fun CancelStub(onCancel: () -> Unit, onKeep: () -> Unit, modifier: Modifier = Modifier) {
    val p = LocalPalette.current
    Column(modifier.fillMaxWidth().background(p.panel)) {
        Row(Modifier.fillMaxWidth().background(Signal).padding(horizontal = 20.dp, vertical = 14.dp)) {
            BasicText("RUN / CANCEL", style = Type.mono(12.sp, true, OnSignal), modifier = Modifier.weight(1f))
            BasicText("Step ${Session.part} of ${Session.parts}", style = Type.mono(12.sp, true, OnSignal))
        }
        Column(Modifier.padding(20.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
            BasicText("Cancel the run?", style = Type.saira(30.sp, 112, 700))
            BasicText("The tests finished so far are kept as a partial result. It is not ranked.", style = Type.archivo(15.sp, color = p.muted))
        }
        Tear(p.line)
        Row(
            Modifier.fillMaxWidth().padding(start = 20.dp, end = 20.dp, top = 14.dp, bottom = bottomInset() + 16.dp),
            horizontalArrangement = Arrangement.spacedBy(6.dp),
        ) {
            Box(Modifier.weight(1f).height(56.dp).border(2.dp, p.text).tap(onClick = onCancel), contentAlignment = Alignment.Center) {
                BasicText("Cancel run", style = Type.archivo(15.sp, 600), maxLines = 1, overflow = TextOverflow.Ellipsis)
            }
            Box(Modifier.weight(1f).height(56.dp).background(Volt).tap(onClick = onKeep), contentAlignment = Alignment.Center) {
                BasicText("Keep measuring", style = Type.archivo(15.sp, 700, Ink), maxLines = 1, overflow = TextOverflow.Ellipsis)
            }
        }
    }
}
