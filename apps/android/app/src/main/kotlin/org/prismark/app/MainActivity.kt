/*
 * Prismark for Android: pick tests, run them, read each result on its own.
 *
 * While a run measures, the screen is static (spec 7.1, docs/design.md "Quiet while measuring"): no animation,
 * no timer, no progress bar; the text changes only between steps, and not at all in the phases that measure
 * waits and wake-ups. A touch during a run is reported to the core, which skips those phases as it does for
 * keyboard and mouse input on a desktop.
 *
 * Copyright 2026 The Prismark Authors. Apache-2.0.
 */
package org.prismark.app

import android.os.Build
import android.os.Bundle
import android.view.MotionEvent
import android.view.WindowManager
import androidx.activity.ComponentActivity
import androidx.activity.compose.BackHandler
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.BasicText
import androidx.compose.foundation.verticalScroll
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.json.JSONObject
import org.prismark.Prismark
import java.io.File

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        setContent {
            PrismarkTheme(dark = isSystemInDarkTheme()) { App() }
        }
    }

    /* Every touch during a run reaches the core before anything else handles it. */
    override fun dispatchTouchEvent(ev: MotionEvent): Boolean {
        if (Session.running && ev.actionMasked == MotionEvent.ACTION_DOWN) Prismark.notifyInput()
        return super.dispatchTouchEvent(ev)
    }
}

private enum class Screen { Pick, Results }

@Composable
private fun App() {
    val p = LocalPalette.current
    var screen by remember { mutableStateOf(Screen.Pick) }
    val activity = LocalContext.current as ComponentActivity

    // Keep the screen on while measuring: a locked phone moves the app to the small cores.
    LaunchedEffect(Session.running) {
        if (Session.running) activity.window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        else activity.window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        if (!Session.running && Session.current != null) screen = Screen.Results
    }
    BackHandler(enabled = Session.running) { /* a run ends with Cancel, not by leaving the screen */ }
    BackHandler(enabled = !Session.running && screen == Screen.Results) { screen = Screen.Pick }

    Box(Modifier.fillMaxSize().background(p.bg).safeDrawingPadding()) {
        when {
            Session.running -> RunningScreen()
            screen == Screen.Results && Session.current != null -> ResultsScreen(Session.current!!) { screen = Screen.Pick }
            else -> PickScreen(onOpen = { Session.current = it; screen = Screen.Results })
        }
    }
}

/* ---------- components (flat, square, no ripple) ---------- */

@Composable
private fun Label(text: String, color: Color = LocalPalette.current.muted) = BasicText(text.uppercase(), style = Type.label(color))

@Composable
private fun Panel(modifier: Modifier = Modifier, color: Color = LocalPalette.current.card, content: @Composable () -> Unit) {
    Column(modifier.fillMaxWidth().background(color).padding(16.dp)) { content() }
}

@Composable
private fun FlatButton(text: String, primary: Boolean, modifier: Modifier = Modifier, enabled: Boolean = true, onClick: () -> Unit) {
    val p = LocalPalette.current
    val bg = if (!enabled) p.raised else if (primary) p.accent else Color.Transparent
    val fg = if (!enabled) p.faint else if (primary) p.onAccent else p.text
    Box(
        modifier
            .background(bg)
            .then(if (primary && enabled) Modifier else Modifier.border(1.dp, p.border))
            .clickable(enabled = enabled, indication = null, interactionSource = remember { MutableInteractionSource() }, onClick = onClick)
            .padding(horizontal = 18.dp, vertical = 14.dp),
        contentAlignment = Alignment.Center,
    ) { BasicText(text, style = Type.strong(fg, 15.sp)) }
}

/** The square switch that says ON or OFF (docs/design.md). */
@Composable
private fun Toggle(on: Boolean, onChange: (Boolean) -> Unit) {
    val p = LocalPalette.current
    Row(
        Modifier.background(if (on) p.accent else p.raised)
            .clickable(indication = null, interactionSource = remember { MutableInteractionSource() }) { onChange(!on) }
            .padding(4.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        if (on) BasicText("ON", style = Type.label(p.onAccent), modifier = Modifier.padding(horizontal = 8.dp))
        Box(Modifier.size(20.dp).background(if (on) p.onAccent else p.faint))
        if (!on) BasicText("OFF", style = Type.label(p.muted), modifier = Modifier.padding(horizontal = 8.dp))
    }
}

@Composable
private fun Check(on: Boolean) {
    val p = LocalPalette.current
    Box(
        Modifier.size(22.dp).background(if (on) p.onAccent else Color.Transparent).border(2.dp, if (on) p.onAccent else p.faint),
        contentAlignment = Alignment.Center,
    ) { if (on) Box(Modifier.size(10.dp).background(p.accent)) }
}

/* ---------- pick ---------- */

@Composable
private fun PickScreen(onOpen: (Run) -> Unit) {
    val p = LocalPalette.current
    val context = LocalContext.current
    var info by remember { mutableStateOf<JSONObject?>(null) }
    var infoError by remember { mutableStateOf<String?>(null) }
    var saved by remember { mutableStateOf(emptyList<File>()) }
    LaunchedEffect(Unit) {
        withContext(Dispatchers.IO) {
            runCatching { JSONObject(Prismark.info()) }.onSuccess { info = it }.onFailure { infoError = it.message ?: it.toString() }
            saved = Session.saved(context)
        }
    }

    Column(Modifier.fillMaxSize()) {
        Column(Modifier.weight(1f).verticalScroll(rememberScrollState()).padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            Label("Prismark · ${runCatching { Prismark.version }.getOrDefault("?")}")
            BasicText("Tests", style = Type.heading())
            BasicText(
                "Each test is measured and shown on its own; there is no overall score. Close other apps, keep the " +
                    "phone on a table, and do not touch it while a run measures.",
                style = Type.body(p.muted),
            )
            Panel {
                Label("This phone")
                Spacer(Modifier.height(6.dp))
                BasicText("${Build.MANUFACTURER} ${Build.MODEL}", style = Type.strong())
                val soc = if (Build.VERSION.SDK_INT >= 31) Build.SOC_MODEL.takeIf { it.isNotBlank() && it != Build.UNKNOWN } else null
                val cpu = info?.optJSONObject("machine")?.optJSONObject("cpu")
                val cores = cpu?.optJSONArray("cores")
                val types = cores?.let { a -> (0 until a.length()).map { a.getJSONObject(it).optString("type") } }
                    ?.groupingBy { it }?.eachCount()?.entries?.joinToString(" + ") { "${it.value} ${it.key}" }
                BasicText(
                    listOfNotNull(soc, types?.let { "$it cores" }, "Android ${Build.VERSION.RELEASE}").joinToString(" · "),
                    style = Type.body(p.muted, 14.sp),
                )
                infoError?.let { BasicText("The core could not describe this phone: $it", style = Type.body(p.bad, 14.sp)) }
            }
            Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                Column(Modifier.weight(1f)) {
                    BasicText("Quick run", style = Type.strong())
                    BasicText("A few minutes, short series. Not for comparison.", style = Type.body(p.muted, 13.sp))
                }
                Toggle(Session.quick) { Session.quick = it }
            }
            for (g in groups) {
                Spacer(Modifier.height(4.dp))
                Label("Tests · ${g.label}")
                BasicText(g.desc, style = Type.body(p.muted, 13.sp))
                for (t in tests.filter { it.group == g.id }) {
                    val on = t.id in Session.selected
                    Row(
                        Modifier.fillMaxWidth().background(if (on) p.accent else p.card)
                            .clickable(indication = null, interactionSource = remember { MutableInteractionSource() }) {
                                if (on) Session.selected.remove(t.id) else Session.selected.add(t.id)
                            }
                            .padding(14.dp),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Column(Modifier.weight(1f)) {
                            BasicText(t.name, style = Type.strong(if (on) p.onAccent else p.text))
                            BasicText(t.sub, style = Type.body(if (on) p.onAccent else p.muted, 13.sp))
                        }
                        Check(on)
                    }
                }
            }
            if (saved.isNotEmpty()) {
                Spacer(Modifier.height(4.dp))
                Label("Your runs")
                for (f in saved.take(20)) {
                    val run = remember(f) { Run(f, runCatching { f.readText() }.getOrDefault("{}")) }
                    Row(
                        Modifier.fillMaxWidth().background(p.card)
                            .clickable(indication = null, interactionSource = remember { MutableInteractionSource() }) { onOpen(run) }
                            .padding(14.dp),
                    ) {
                        BasicText(run.started, style = Type.body(), modifier = Modifier.weight(1f))
                        Label(listOfNotNull(if (run.quick) "quick" else "full", if (!run.complete) "partial" else null,
                            if (run.busy) "busy" else null).joinToString(" · "))
                    }
                }
            }
        }
        val chosen = tests.filter { it.id in Session.selected }
        Column(Modifier.fillMaxWidth().background(p.card).padding(16.dp)) {
            Session.error?.let { BasicText(it, style = Type.body(p.bad, 14.sp)); Spacer(Modifier.height(8.dp)) }
            FlatButton(
                if (chosen.isEmpty()) "Pick a test" else "Run ${chosen.size} test${if (chosen.size == 1) "" else "s"}",
                primary = true, enabled = chosen.isNotEmpty() && info != null, modifier = Modifier.fillMaxWidth(),
            ) { Session.start(context, chosen, Session.quick) }
        }
    }
}

/* ---------- running: static ---------- */

@Composable
private fun RunningScreen() {
    val p = LocalPalette.current
    Column(Modifier.fillMaxSize().padding(16.dp), verticalArrangement = Arrangement.spacedBy(16.dp)) {
        Row(Modifier.fillMaxWidth().background(SignalBlue).padding(horizontal = 12.dp, vertical = 8.dp)) {
            BasicText("RUN / MEASURING", style = Type.label(Color.White))
        }
        BasicText("Measuring", style = Type.heading())
        BasicText(
            "Leave the phone still and do not touch the screen: a touch skips the tests that measure waking up. " +
                "The screen stays on and does not change while a test measures.",
            style = Type.body(p.muted),
        )
        Panel {
            Label("Now")
            Spacer(Modifier.height(6.dp))
            BasicText(Session.phase, style = Type.strong())
            if (Session.step.isNotEmpty()) BasicText(Session.step, style = Type.mono())
        }
        Spacer(Modifier.weight(1f))
        FlatButton("Cancel run", primary = false, modifier = Modifier.fillMaxWidth()) { Prismark.cancel() }
    }
}

/* ---------- results: each test on its own ---------- */

@Composable
private fun ResultsScreen(run: Run, onBack: () -> Unit) {
    val p = LocalPalette.current
    val context = LocalContext.current
    var exported by remember(run) { mutableStateOf<String?>(null) }
    val export = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("application/json")) { uri ->
        if (uri != null) exported = runCatching {
            context.contentResolver.openOutputStream(uri)?.use { it.write(run.json.toByteArray()) }
            "Saved."
        }.getOrElse { "Not saved: ${it.message}" }
    }
    val doc = run.doc
    val values = remember(run) { tests.associateWith { runCatching { it.get(doc) }.getOrNull() } }
    val gone = remember(run) { unknowns(doc) }

    Column(Modifier.fillMaxSize()) {
        Column(Modifier.weight(1f).verticalScroll(rememberScrollState()).padding(16.dp), verticalArrangement = Arrangement.spacedBy(12.dp)) {
            Label(listOfNotNull("Your run", if (run.quick) "quick" else null, if (!run.complete) "partial" else null,
                if (run.busy) "busy" else null).joinToString(" · "))
            BasicText("Results", style = Type.heading())
            BasicText(run.started, style = Type.mono())
            if (!run.complete)
                BasicText("The tests finished so far are kept as a partial result.", style = Type.body(p.muted))
            if (run.quick)
                BasicText("A quick run: short series, not for comparison.", style = Type.body(p.muted))
            if (run.busy)
                BasicText(
                    "Other apps were working when this run started, so its results may be lower than this phone can do.",
                    style = Type.body(p.muted),
                )
            for (g in groups) {
                val shown = tests.filter { it.group == g.id && values[it] != null }
                if (shown.isEmpty()) continue
                Spacer(Modifier.height(4.dp))
                Label("Tests · ${g.label}")
                for (t in shown) TestCard(t, values[t]!!)
            }
            val missing = tests.filter { values[it] == null && it.id in Session.selected }
            if (missing.isNotEmpty()) {
                Spacer(Modifier.height(4.dp))
                Label("Not measured")
                Panel {
                    BasicText(missing.joinToString(", ") { it.name }, style = Type.body())
                    BasicText(
                        "Cancelled, skipped after a touch, or not available on this phone. The result file says which.",
                        style = Type.body(p.muted, 13.sp),
                    )
                }
            }
            if (gone.isNotEmpty()) {
                Spacer(Modifier.height(4.dp))
                Label("Unknown on this phone")
                Panel {
                    for (u in gone) {
                        BasicText(u.what, style = Type.strong(size = 15.sp))
                        BasicText(u.why, style = Type.body(p.muted, 13.sp))
                        Spacer(Modifier.height(8.dp))
                    }
                    BasicText("Recorded as unknown in the result file; the tests ran without them.", style = Type.body(p.faint, 13.sp))
                }
            }
            if (Session.warnings.isNotEmpty() && run === Session.current) {
                Spacer(Modifier.height(4.dp))
                Label("Messages from the run")
                Panel { for (w in Session.warnings.distinct().take(30)) BasicText(w, style = Type.mono(size = 12.sp)) }
            }
            Spacer(Modifier.height(4.dp))
            Label("Result file")
            BasicText(run.file.name, style = Type.mono(size = 12.sp))
            exported?.let { BasicText(it, style = Type.body(p.muted, 13.sp)) }
        }
        Row(Modifier.fillMaxWidth().background(p.card).padding(16.dp), horizontalArrangement = Arrangement.spacedBy(12.dp)) {
            FlatButton("Back", primary = false, modifier = Modifier.weight(1f), onClick = onBack)
            FlatButton("Save JSON", primary = true, modifier = Modifier.weight(1f)) { export.launch(run.file.name) }
        }
    }
}

@Composable
private fun TestCard(t: Test, v: Value) {
    val p = LocalPalette.current
    Panel {
        Row(verticalAlignment = Alignment.Bottom) {
            Column(Modifier.weight(1f)) {
                BasicText(t.name, style = Type.strong())
                BasicText(t.sub, style = Type.body(p.muted, 13.sp))
            }
            Spacer(Modifier.width(12.dp))
            BasicText(format(v.v), style = Type.score())
            Spacer(Modifier.width(6.dp))
            BasicText(t.unit, style = Type.mono(), modifier = Modifier.padding(bottom = 6.dp))
        }
        Spacer(Modifier.height(4.dp))
        val range = if (v.hasRange) "95 % range ${format(v.lo)}–${format(v.hi)} ${t.unit}" else "No range: may be noise"
        BasicText(
            listOfNotNull(range, if (t.higherBetter) "higher is better" else "lower is better",
                if (v.unsettled) "had not settled" else null).joinToString(" · "),
            style = Type.mono(size = 12.sp),
        )
    }
}
