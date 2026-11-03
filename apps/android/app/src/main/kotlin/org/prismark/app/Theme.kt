/*
 * Colours and type of the mobile design (docs/design-canvas): flat blocks with ink seams, square corners, volt and
 * signal blue. The same values as the iOS app (apps/ios), which the design was drawn for first.
 *
 * Copyright 2026 The Prismark Authors. Apache-2.0.
 */
@file:OptIn(ExperimentalTextApi::class)

package org.prismark.app

import android.content.res.AssetManager
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.Immutable
import androidx.compose.runtime.remember
import androidx.compose.runtime.staticCompositionLocalOf
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.ExperimentalTextApi
import androidx.compose.ui.text.PlatformTextStyle
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.Font
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontVariation
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.TextUnit
import androidx.compose.ui.unit.em
import androidx.compose.ui.unit.sp

/* The same in light and dark. */
val Ink = Color(0xFF0C1430)
val Signal = Color(0xFF2D5BFF)
val Volt = Color(0xFFD6F53B)
val Fault = Color(0xFFFF4F3A)
val OnSignal = Color(0xFFF7F8F9)
val SignalLine = Color(0xFF9DB3FF)

/* These follow the system's light or dark mode. */
@Immutable
data class Palette(
    val ground: Color,
    val panel: Color,
    val raised: Color,
    val text: Color,
    val muted: Color,
    val line: Color,
    val steel: Color,
    val reference: Color,
    val yours: Color,
)

val LightPalette = Palette(
    ground = Ink, panel = Color(0xFFF7F8F9), raised = Color(0xFFE8ECEF), text = Ink, muted = Color(0xFF3B4660),
    line = Color(0xFFC9D0DA), steel = Color(0xFFC9D0DA), reference = Signal, yours = Ink,
)

val DarkPalette = Palette(
    ground = Color(0xFF070C1F), panel = Color(0xFF141D3B), raised = Color(0xFF1F2B52), text = Color(0xFFE8ECEF),
    muted = Color(0xFFAEB8D6), line = Color(0xFF2A3760), steel = Color(0xFF0F1730), reference = Color(0xFF5C80FF),
    yours = Volt,
)

/**
 * Saira with its width axis for names and numbers, Archivo for running text, IBM Plex Mono for labels and units.
 * The files are fetched at pinned versions by tools/build-android.sh into assets/fonts; without them the system
 * fonts stand in.
 */
class Fonts(private val assets: AssetManager?) {
    private val have = runCatching { assets?.list("fonts")?.toSet() }.getOrNull().orEmpty()
    private val cache = HashMap<String, FontFamily>()

    private fun variable(file: String, width: Int, weight: Int, fallback: FontFamily): FontFamily {
        val am = assets
        if (am == null || file !in have) return fallback
        return cache.getOrPut("$file/$width/$weight") {
            FontFamily(
                Font(
                    "fonts/$file", am, FontWeight(weight),
                    variationSettings = FontVariation.Settings(
                        FontVariation.weight(weight),
                        FontVariation.Setting("wdth", width.toFloat()),
                    ),
                ),
            )
        }
    }

    fun saira(width: Int, weight: Int) = variable("Saira.ttf", width, weight, FontFamily.SansSerif)
    fun archivo(weight: Int) = variable("Archivo.ttf", 100, weight, FontFamily.SansSerif)

    fun mono(semibold: Boolean): FontFamily {
        val am = assets
        val file = if (semibold) "IBMPlexMono-SemiBold.ttf" else "IBMPlexMono-Regular.ttf"
        if (am == null || file !in have) return FontFamily.Monospace
        return cache.getOrPut(file) { FontFamily(Font("fonts/$file", am, if (semibold) FontWeight.SemiBold else FontWeight.Normal)) }
    }
}

val LocalPalette = staticCompositionLocalOf { LightPalette }
val LocalFonts = staticCompositionLocalOf { Fonts(null) }

@Composable
fun PrismarkTheme(dark: Boolean, content: @Composable () -> Unit) {
    val context = LocalContext.current
    val fonts = remember { Fonts(context.applicationContext.assets) }
    CompositionLocalProvider(
        LocalPalette provides if (dark) DarkPalette else LightPalette,
        LocalFonts provides fonts,
        content = content,
    )
}

object Type {
    /** Names and numbers. Width 50 is condensed, 125 wide. */
    @Composable
    fun saira(size: TextUnit, width: Int = 100, weight: Int = 700, color: Color = LocalPalette.current.text) = TextStyle(
        fontFamily = LocalFonts.current.saira(width, weight), fontWeight = FontWeight(weight), fontSize = size,
        lineHeight = size * 1.05f, color = color, platformStyle = PlatformTextStyle(includeFontPadding = false),
    )

    @Composable
    fun archivo(size: TextUnit, weight: Int = 400, color: Color = LocalPalette.current.text) = TextStyle(
        fontFamily = LocalFonts.current.archivo(weight), fontWeight = FontWeight(weight), fontSize = size,
        lineHeight = size * 1.28f, color = color,
    )

    @Composable
    fun mono(size: TextUnit, semibold: Boolean = false, color: Color = LocalPalette.current.text) = TextStyle(
        fontFamily = LocalFonts.current.mono(semibold), fontSize = size, color = color,
    )

    /** Small capitals: TESTS · ONE CORE. */
    @Composable
    fun label(color: Color = LocalPalette.current.text, size: TextUnit = 11.sp) = TextStyle(
        fontFamily = LocalFonts.current.mono(false), fontSize = size, letterSpacing = 0.05.em, color = color,
    )
}
