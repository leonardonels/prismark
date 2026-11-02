/*
 * Colours and type of docs/design.md: flat blocks, square corners, navy ink, paper, volt and signal blue.
 * Nothing here animates.
 *
 * Copyright 2026 The Prismark Authors. Apache-2.0.
 */
@file:OptIn(ExperimentalTextApi::class)

package org.prismark.app

import android.content.Context
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.Immutable
import androidx.compose.runtime.remember
import androidx.compose.runtime.staticCompositionLocalOf
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.ExperimentalTextApi
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.Font
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontVariation
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.TextUnit
import androidx.compose.ui.unit.em
import androidx.compose.ui.unit.sp

@Immutable
data class Palette(
    val dark: Boolean,
    val bg: Color,
    val card: Color,
    val raised: Color,
    val border: Color,
    val text: Color,
    val muted: Color,
    val faint: Color,
    val accent: Color,
    val onAccent: Color,
    val link: Color,
    val good: Color,
    val bad: Color,
)

val DarkPalette = Palette(
    dark = true,
    bg = Color(0xFF070C1F), card = Color(0xFF141D3B), raised = Color(0xFF1F2B52), border = Color(0xFF2A3760),
    text = Color(0xFFE8ECEF), muted = Color(0xFFAEB8D6), faint = Color(0xFF8591B5),
    accent = Color(0xFFD6F53B), onAccent = Color(0xFF0C1430), link = Color(0xFFD6F53B),
    good = Color(0xFFD6F53B), bad = Color(0xFFFF6F5E),
)

val LightPalette = Palette(
    dark = false,
    bg = Color(0xFFDDE2E8), card = Color(0xFFF7F8F9), raised = Color(0xFFE8ECEF), border = Color(0xFFC9D0DA),
    text = Color(0xFF0C1430), muted = Color(0xFF1E2742), faint = Color(0xFF343E5A),
    accent = Color(0xFFD6F53B), onAccent = Color(0xFF0C1430), link = Color(0xFF2D5BFF),
    good = Color(0xFF3B5200), bad = Color(0xFFC2261A),
)

/** Signal blue, fixed outside the theme (run dialogs). */
val SignalBlue = Color(0xFF2D5BFF)

/**
 * Saira for headings (wide) and scores (condensed), Inter for running text, IBM Plex Mono for units, ids and
 * labels. The files are fetched at pinned versions by tools/build-android.sh into assets/fonts; without them
 * the system fonts stand in.
 */
@Immutable
class Fonts(val text: FontFamily, val heading: FontFamily, val score: FontFamily, val mono: FontFamily)

private fun loadFonts(context: Context): Fonts {
    val am = context.assets
    val have = runCatching { am.list("fonts")?.toSet() }.getOrNull().orEmpty()
    fun family(vararg fonts: Pair<String, () -> Font>): FontFamily? {
        val list = fonts.filter { it.first in have }.map { it.second() }
        return if (list.isEmpty()) null else FontFamily(list)
    }
    val inter = family(
        "Inter-Regular.otf" to { Font("fonts/Inter-Regular.otf", am, FontWeight.Normal) },
        "Inter-Medium.otf" to { Font("fonts/Inter-Medium.otf", am, FontWeight.Medium) },
        "Inter-SemiBold.otf" to { Font("fonts/Inter-SemiBold.otf", am, FontWeight.SemiBold) },
        "Inter-Bold.otf" to { Font("fonts/Inter-Bold.otf", am, FontWeight.Bold) },
    ) ?: FontFamily.SansSerif
    fun saira(width: Float, weight: Int) = family(
        "Saira.ttf" to {
            Font(
                "fonts/Saira.ttf", am, FontWeight(weight),
                variationSettings = FontVariation.Settings(
                    FontVariation.weight(weight),
                    FontVariation.Setting("wdth", width),
                ),
            )
        },
    )
    val mono = family(
        "IBMPlexMono-Regular.ttf" to { Font("fonts/IBMPlexMono-Regular.ttf", am, FontWeight.Normal) },
        "IBMPlexMono-Medium.ttf" to { Font("fonts/IBMPlexMono-Medium.ttf", am, FontWeight.Medium) },
        "IBMPlexMono-SemiBold.ttf" to { Font("fonts/IBMPlexMono-SemiBold.ttf", am, FontWeight.SemiBold) },
    ) ?: FontFamily.Monospace
    return Fonts(
        text = inter,
        heading = saira(118f, 700) ?: inter,
        score = saira(80f, 600) ?: inter,
        mono = mono,
    )
}

val LocalPalette = staticCompositionLocalOf { DarkPalette }
val LocalFonts = staticCompositionLocalOf { Fonts(FontFamily.SansSerif, FontFamily.SansSerif, FontFamily.SansSerif, FontFamily.Monospace) }

@Composable
fun PrismarkTheme(dark: Boolean, content: @Composable () -> Unit) {
    val context = LocalContext.current
    val fonts = remember { loadFonts(context.applicationContext) }
    CompositionLocalProvider(
        LocalPalette provides if (dark) DarkPalette else LightPalette,
        LocalFonts provides fonts,
        content = content,
    )
}

object Type {
    @Composable fun heading(size: TextUnit = 26.sp) =
        TextStyle(fontFamily = LocalFonts.current.heading, fontWeight = FontWeight.Bold, fontSize = size, color = LocalPalette.current.text)

    @Composable fun body(color: Color = LocalPalette.current.text, size: TextUnit = 15.sp) =
        TextStyle(fontFamily = LocalFonts.current.text, fontSize = size, lineHeight = size * 1.4f, color = color)

    @Composable fun strong(color: Color = LocalPalette.current.text, size: TextUnit = 16.sp) =
        TextStyle(fontFamily = LocalFonts.current.text, fontWeight = FontWeight.SemiBold, fontSize = size, color = color)

    @Composable fun score(color: Color = LocalPalette.current.text, size: TextUnit = 32.sp) =
        TextStyle(fontFamily = LocalFonts.current.score, fontWeight = FontWeight.SemiBold, fontSize = size, color = color)

    /** Small capitals with tracking: TESTS · ALL CORES. */
    @Composable fun label(color: Color = LocalPalette.current.muted, size: TextUnit = 11.sp) =
        TextStyle(
            fontFamily = LocalFonts.current.mono, fontWeight = FontWeight.Medium, fontSize = size,
            letterSpacing = 0.12.em, color = color,
        )

    @Composable fun mono(color: Color = LocalPalette.current.muted, size: TextUnit = 13.sp) =
        TextStyle(fontFamily = LocalFonts.current.mono, fontSize = size, color = color)
}
