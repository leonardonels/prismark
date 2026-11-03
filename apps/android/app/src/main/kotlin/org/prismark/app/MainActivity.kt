/*
 * Prismark for Android: pick tests, run them, read each result on its own. The screens follow the iOS app: a side
 * column on a phone, and on a tablet the desktop app's tests, explanation and ranking side by side.
 *
 * While a run measures, the screen is static (spec 7.1, docs/design.md "Quiet while measuring"): no animation,
 * no timer, no progress bar; the text changes only between steps, and not at all in the phases that measure
 * waits and wake-ups. A touch during a run is reported to the core, which skips those phases as it does for
 * keyboard and mouse input on a desktop.
 *
 * Copyright 2026 The Prismark Authors. Apache-2.0.
 */
package org.prismark.app

import android.content.pm.ActivityInfo
import android.os.Bundle
import android.view.MotionEvent
import android.view.WindowManager
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.foundation.background
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.text.BasicText
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.json.JSONObject
import org.prismark.Prismark

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        // A phone stays upright, as on iPhone; a tablet turns freely.
        if (resources.configuration.smallestScreenWidthDp < 600) requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_PORTRAIT
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

/** What the core says about this device, read once. */
object DeviceInfo {
    var doc by mutableStateOf(JSONObject())
        private set
    var error by mutableStateOf<String?>(null)
        private set

    fun load() {
        runCatching { JSONObject(Prismark.info()) }.onSuccess { doc = it }.onFailure { error = it.message ?: it.toString() }
    }
}

private enum class Section { Run, Results, Device }

@Composable
private fun App() {
    val p = LocalPalette.current
    val context = LocalContext.current
    val activity = context as ComponentActivity
    var section by rememberSaveable { mutableStateOf(Section.Run) }

    LaunchedEffect(Unit) {
        withContext(Dispatchers.IO) {
            DeviceInfo.load()
            Session.load(context)
        }
    }

    BoxWithConstraints(Modifier.fillMaxSize().background(p.ground)) {
        val tablet = maxWidth >= 600.dp

        // Keep the screen on while measuring: a locked phone moves the app to the small cores.
        LaunchedEffect(Session.running) {
            if (Session.running) activity.window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
            else activity.window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
            if (!Session.running && Session.current != null && !tablet) section = Section.Results
        }

        if (Session.running) {
            MeasuringScreen()
        } else {
            Row(Modifier.fillMaxSize(), horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                Box(Modifier.weight(1f).fillMaxHeight()) {
                    when (section) {
                        Section.Run -> if (tablet) TabletTests() else PageStack(destination = { RunPage(it) }) { RunScreen() }
                        Section.Results -> PageStack(destination = { ResultPage(it) }) { RunsScreen() }
                        Section.Device -> DeviceScreen(tablet)
                    }
                }
                SideColumn(
                    current = section.ordinal,
                    firstName = if (tablet) "TESTS" else "RUN",
                    onPick = { section = Section.values()[it] },
                    modifier = Modifier.width(66.dp).fillMaxHeight(),
                )
            }
        }

        Session.error?.let { message ->
            Box(Modifier.fillMaxWidth().background(Fault).padding(horizontal = 16.dp, vertical = 12.dp)) {
                BasicText(message, style = Type.archivo(14.sp, 600, Ink))
            }
        }
    }
}
