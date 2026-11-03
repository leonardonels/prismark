/*
 * The pieces every screen shares: flat buttons that take a tap anywhere on the block, the side column, the screen
 * stack, and what the screens say about this device.
 *
 * Copyright 2026 The Prismark Authors. Apache-2.0.
 */
package org.prismark.app

import android.app.ActivityManager
import android.content.Context
import android.content.Intent
import android.os.Build
import androidx.activity.compose.BackHandler
import androidx.compose.animation.AnimatedContent
import androidx.compose.animation.ContentTransform
import androidx.compose.animation.slideInHorizontally
import androidx.compose.animation.slideOutHorizontally
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.asPaddingValues
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.navigationBars
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBars
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.text.BasicText
import androidx.compose.runtime.Composable
import androidx.compose.runtime.compositionLocalOf
import androidx.compose.runtime.getValue
import androidx.compose.runtime.setValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clipToBounds
import androidx.compose.ui.draw.rotate
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.PathEffect
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.layout.layout
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.Constraints
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.TextUnit
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.core.content.FileProvider
import org.json.JSONObject
import kotlin.math.ceil
import kotlin.math.roundToInt

/** The top and bottom of the screen that the system's bars cover. */
@Composable
fun topInset(): Dp = WindowInsets.statusBars.asPaddingValues().calculateTopPadding()

@Composable
fun bottomInset(): Dp = WindowInsets.navigationBars.asPaddingValues().calculateBottomPadding()

/** A whole block that takes the tap, without a ripple: buttons here are flat. */
@Composable
fun Modifier.tap(enabled: Boolean = true, onClick: () -> Unit): Modifier = clickable(
    enabled = enabled, indication = null, interactionSource = remember { MutableInteractionSource() },
    role = Role.Button, onClick = onClick,
)

/** Text on its side, reading top to bottom, taking the space it covers. */
fun Modifier.onItsSide(): Modifier = layout { measurable, _ ->
    val placeable = measurable.measure(Constraints())
    layout(placeable.height, placeable.width) {
        placeable.place((placeable.height - placeable.width) / 2, (placeable.width - placeable.height) / 2)
    }
}.rotate(90f)

@Composable
fun Mono(text: String, color: Color = LocalPalette.current.text, modifier: Modifier = Modifier) =
    BasicText(text.uppercase(), style = Type.label(color), modifier = modifier)

/** The square check: an outline when off, an ink box with a volt centre when on. */
@Composable
fun Check(on: Boolean) {
    val p = LocalPalette.current
    Box(
        Modifier.size(24.dp).background(if (on) Ink else Color.Transparent).border(2.dp, if (on) Ink else p.muted),
        contentAlignment = Alignment.Center,
    ) { if (on) Box(Modifier.size(10.dp).background(Volt)) }
}

@Composable
fun Chevrons(color: Color) = Canvas(Modifier.size(width = 30.dp, height = 16.dp)) { chevrons(color) }

private fun DrawScope.chevrons(color: Color) {
    val w = size.height * 0.5f
    for (i in 0 until 3) {
        val x = i * (size.width - w) / 2
        val path = Path().apply {
            moveTo(x, 0f)
            lineTo(x + w, size.height / 2)
            lineTo(x, size.height)
        }
        drawPath(path, color, style = androidx.compose.ui.graphics.drawscope.Stroke(width = size.height * 0.2f))
    }
}

/** One chevron, pointing right or left. */
@Composable
fun Chevron(color: Color, left: Boolean = false) = Canvas(Modifier.size(width = 9.dp, height = 15.dp)) {
    val path = Path().apply {
        moveTo(if (left) size.width else 0f, 0f)
        lineTo(if (left) 0f else size.width, size.height / 2)
        lineTo(if (left) size.width else 0f, size.height)
    }
    drawPath(path, color, style = androidx.compose.ui.graphics.drawscope.Stroke(width = 2.5f * density))
}

/** The logo: a beam enters a prism and leaves as three bars. Drawn on a 64 by 64 grid. */
@Composable
fun PrismMark(modifier: Modifier = Modifier) = Canvas(modifier) {
    val unit = size.width / 64
    drawLine(Ink, Offset(0f, 37 * unit), Offset(21.5f * unit, 34 * unit), strokeWidth = 3 * unit)
    drawPath(Path().apply {
        moveTo(12 * unit, 52 * unit)
        lineTo(32 * unit, 12 * unit)
        lineTo(52 * unit, 52 * unit)
        close()
    }, Ink)
    drawRect(Ink, Offset(42 * unit, 25 * unit), Size(22 * unit, 5 * unit))
    drawRect(Signal, Offset(46.5f * unit, 34 * unit), Size(17.5f * unit, 5 * unit))
    drawRect(Ink, Offset(51 * unit, 43 * unit), Size(13 * unit, 5 * unit))
}

/** The look someone picked over the system's, kept between launches. Null follows the system. */
object Appearance {
    var dark by mutableStateOf<Boolean?>(null)
        private set

    fun load(context: Context) {
        val saved = context.getSharedPreferences("prismark", Context.MODE_PRIVATE).getString("appearance", null)
        dark = saved?.let { it == "dark" }
    }

    /** Coming back to the system's own look follows the system again. */
    fun swap(context: Context, systemDark: Boolean) {
        val next = !(dark ?: systemDark)
        dark = next.takeIf { it != systemDark }
        context.getSharedPreferences("prismark", Context.MODE_PRIVATE).edit()
            .putString("appearance", dark?.let { if (it) "dark" else "light" }).apply()
    }
}

/** The logo on its volt block. A hidden touch: tapping it swaps dark and light, whatever the system is set to. */
@Composable
fun Logo(mark: Dp, name: TextUnit?, modifier: Modifier = Modifier) {
    val context = LocalContext.current
    val systemDark = isSystemInDarkTheme()
    Row(
        modifier.background(Volt).tap { Appearance.swap(context, systemDark) }.semantics { contentDescription = "Prismark. Swap dark and light" }
            .padding(horizontal = 10.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        PrismMark(Modifier.size(mark))
        if (name != null) BasicText("PRISMARK", style = Type.saira(name, 125, 800, Ink))
    }
}

/** The processor's name: as big as there is room for, its brand small below. */
@Composable
fun ChipName(machine: Machine, size: Float) {
    BasicText(machine.chip, style = Type.saira(chipSize(machine.chip, size, 7).sp, 50, 600, Ink), maxLines = 1, softWrap = false)
    if (machine.brand.isNotEmpty()) BasicText(machine.brand, style = Type.saira(maxOf(13f, size * 0.125f).sp, 125, 800, OnSignal), maxLines = 1)
}

/** Hands the result file to the system's share sheet, which also offers saving it to a folder. */
fun shareResult(context: Context, run: Run) {
    runCatching {
        val file = FileProvider.getUriForFile(context, "${context.packageName}.files", run.file)
        val send = Intent(Intent.ACTION_SEND).setType("application/json").putExtra(Intent.EXTRA_STREAM, file).addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
        context.startActivity(Intent.createChooser(send, "Share the result file"))
    }.onFailure { Session.error = "The result file could not be shared: ${it.message}" }
}

/** The share sign: three dots joined by two lines. */
@Composable
fun ShareMark(color: Color) = Canvas(Modifier.size(20.dp)) {
    val a = Offset(size.width * 0.8f, size.height * 0.16f)
    val b = Offset(size.width * 0.2f, size.height * 0.5f)
    val c = Offset(size.width * 0.8f, size.height * 0.84f)
    drawLine(color, a, b, strokeWidth = 2.dp.toPx())
    drawLine(color, b, c, strokeWidth = 2.dp.toPx())
    for (dot in listOf(a, b, c)) drawCircle(color, 3.dp.toPx(), dot)
}

/** The pass's printed dots. */
fun DrawScope.passDots() {
    val step = 24.dp.toPx()
    var x = step / 2
    while (x < size.width) {
        var y = step / 2
        while (y < size.height) {
            drawCircle(Color(0xFF5C80FF), 1.2.dp.toPx(), Offset(x, y))
            y += step
        }
        x += step
    }
}

/** The dashed tear of a stub. */
@Composable
fun Tear(color: Color, vertical: Boolean = false) = Canvas(
    if (vertical) Modifier.width(2.dp).fillMaxHeight() else Modifier.fillMaxWidth().height(2.dp),
) {
    drawLine(
        color, Offset.Zero, if (vertical) Offset(0f, size.height) else Offset(size.width, 0f), strokeWidth = 2.dp.toPx(),
        pathEffect = PathEffect.dashPathEffect(floatArrayOf(6.dp.toPx(), 5.dp.toPx())),
    )
}

/** Bars drawn from a run's own id, so every run has its own barcode. */
@Composable
fun Barcode(id: String, color: Color, modifier: Modifier = Modifier, vertical: Boolean = false) = Canvas(modifier.clipToBounds()) {
    val digits = id.mapNotNull { Character.digit(it, 16).takeIf { d -> d >= 0 } }.ifEmpty { listOf(5, 3, 9, 1, 7) }
    val unit = 1.dp.toPx()
    val end = if (vertical) size.height else size.width
    var at = 0f
    var i = 0
    while (at < end) {
        val digit = digits[i % digits.size]
        val bar = (1 + (digit + i) % 4) * unit
        val gap = (1 + (digit * 3 + i) % 3) * unit
        if (vertical) drawRect(color, Offset(0f, at), Size(size.width, bar)) else drawRect(color, Offset(at, 0f), Size(bar, size.height))
        at += bar + gap
        i++
    }
}

/** Run, Results and Device, reaching the screen's edges so the phone's own corners finish it. */
@Composable
fun SideColumn(current: Int, firstName: String, onPick: (Int) -> Unit, modifier: Modifier = Modifier) {
    val p = LocalPalette.current
    val top = topInset() + 16.dp
    val bottom = bottomInset() + 16.dp
    Column(modifier, verticalArrangement = Arrangement.spacedBy(6.dp)) {
        listOf(firstName, "RESULTS", "DEVICE").forEachIndexed { index, name ->
            val on = index == current
            Box(
                Modifier.weight(if (index == 0) 1.15f else 1f).fillMaxWidth().background(if (on) p.panel else p.steel)
                    .tap { onPick(index) }.semantics { contentDescription = name },
            ) {
                if (on) Box(Modifier.width(6.dp).fillMaxHeight().background(Volt))
                Column(
                    Modifier.fillMaxSize().padding(top = if (index == 0) top else 14.dp, bottom = if (index == 2) bottom else 14.dp),
                    horizontalAlignment = Alignment.CenterHorizontally,
                    verticalArrangement = Arrangement.SpaceBetween,
                ) {
                    BasicText("0${index + 1}", style = Type.mono(11.sp, color = p.text))
                    BasicText(name, style = Type.saira(19.sp, 125, 800), modifier = Modifier.onItsSide())
                }
            }
        }
    }
}

/* ---------- screens opened on top of a section ---------- */

class Pages {
    val path = mutableStateListOf<Any>()
    fun push(page: Any) { path.add(page) }
    fun pop() { if (path.isNotEmpty()) path.removeAt(path.lastIndex) }
}

val LocalPages = compositionLocalOf { Pages() }

/** A section's first screen, and the screens on top of it. The system's back gesture goes back a screen. */
@Composable
fun PageStack(destination: @Composable (Any) -> Unit, root: @Composable () -> Unit) {
    val pages = remember { Pages() }
    BackHandler(enabled = pages.path.isNotEmpty()) { pages.pop() }
    CompositionLocalProvider(LocalPages provides pages) {
        AnimatedContent(
            targetState = pages.path.toList(),
            modifier = Modifier.fillMaxSize().clipToBounds(),
            transitionSpec = {
                if (targetState.size >= initialState.size) {
                    ContentTransform(slideInHorizontally { it }, slideOutHorizontally { -it / 4 }, targetContentZIndex = targetState.size.toFloat())
                } else {
                    ContentTransform(slideInHorizontally { -it / 4 }, slideOutHorizontally { it }, targetContentZIndex = targetState.size.toFloat())
                }
            },
            label = "pages",
        ) { path ->
            Box(Modifier.fillMaxSize().background(LocalPalette.current.ground)) {
                if (path.isEmpty()) root() else destination(path.last())
            }
        }
    }
}

/** The back button, a square 44 points wide. On a volt header it is ink, like the share button beside it. */
@Composable
fun BackButton(onVolt: Boolean = false) {
    val pages = LocalPages.current
    Box(
        Modifier.size(44.dp).background(if (onVolt) Ink else LocalPalette.current.raised).tap { pages.pop() }.semantics { contentDescription = "Back" },
        contentAlignment = Alignment.Center,
    ) { Chevron(if (onVolt) Volt else LocalPalette.current.text, left = true) }
}

/* ---------- this device ---------- */

/** What the core and the system say about this device. */
class Machine(context: Context, info: JSONObject) {
    private val cpu = info.optJSONObject("machine")?.optJSONObject("cpu")
    private val list = cpu?.optJSONArray("cores").let { a -> if (a == null) emptyList() else (0 until a.length()).mapNotNull { a.optJSONObject(it) } }
    private val types = list.map { it.optString("type") }

    /** The core clusters, fastest first: how many, and what to call them. The core ranks up to four by their speed. */
    val clusters: List<Pair<String, Int>> = types.distinct().sortedBy { clusterOrder.indexOf(it) }.let { ranked ->
        val slowest = list.filter { it.optString("type") == ranked.lastOrNull() }.maxOfOrNull { it.optInt("max_mhz") } ?: 0
        val names = when (ranked.size) {
            1 -> listOf("Core")
            // Small cores have never come near this clock. A slower cluster this fast is big too, just not the biggest.
            2 -> if (slowest >= BIG_CORE_MHZ) listOf("Prime", "Fast") else listOf("Fast", "Efficient")
            3 -> listOf("Prime", "Fast", "Efficient")
            else -> listOf("Prime", "Fast", "Middle", "Efficient")
        }
        ranked.mapIndexed { index, type -> names[index] to types.count { it == type } }
    }
    val cores: String = if (clusters.size == 1) "${types.size} cores" else clusters.joinToString(" + ") { "${it.second} ${it.first.lowercase()}" }.ifEmpty { "Unknown" }

    // Older Android has no chip name, and the core then only knows an ARM part number.
    private val name: String = cpu?.optString("model")?.takeIf { it.isNotBlank() && !it.startsWith("arm implementer") } ?: Build.HARDWARE

    /** "Snapdragon 8 Gen 5" is drawn as a big 8 Gen 5, and SNAPDRAGON small below, as iPhones draw BIONIC. */
    val chip: String = name.substringAfter(' ', name)
    val brand: String = name.substringBefore(' ', "").uppercase()

    // Some makers already start the model with their own name.
    val model: String = (if (Build.MODEL.startsWith(Build.MANUFACTURER, ignoreCase = true)) Build.MODEL else "${Build.MANUFACTURER} ${Build.MODEL}")
        .replaceFirstChar { it.uppercase() }
    val system: String = "Android ${Build.VERSION.RELEASE}"
    val cache: String? = cpu?.optDouble("llc_bytes", 0.0)?.takeIf { it > 0 }?.let { "${(it / 1_048_576).roundToInt()} MB" }
    val memory: String = run {
        val out = ActivityManager.MemoryInfo()
        (context.getSystemService(Context.ACTIVITY_SERVICE) as ActivityManager).getMemoryInfo(out)
        // The system keeps part of the memory for itself, so the total is short of the size the phone is sold with.
        val advertised = if (Build.VERSION.SDK_INT >= 34) out.advertisedMem else 0L
        val gb = (if (advertised > 0) advertised else out.totalMem) / 1_073_741_824.0
        "${memorySizes.firstOrNull { it >= gb } ?: ceil(gb).toInt()} GB"
    }
    val summary: String = "$cores · $memory · $system"

    private companion object {
        val clusterOrder = listOf("P", "M", "M1", "M2", "E")
        const val BIG_CORE_MHZ = 2800
        val memorySizes = listOf(1, 2, 3, 4, 6, 8, 10, 12, 16, 18, 24, 32, 48, 64)
    }
}

/** A long chip name must still fit on one line. */
fun chipSize(name: String, base: Float, fits: Int = 5): Float = if (name.length <= fits) base else base * fits / name.length
