/*
 * Visual theme of the desktop app: colour tokens for dark and light, the
 * embedded typefaces (Inter for text, Saira for scores and headings, IBM Plex
 * Mono for units and IDs), and the stylesheet for standard widgets. Painted
 * widgets read the same tokens, so everything changes together.
 *
 * Copyright 2026 The Prismark Authors. Apache-2.0.
 */
#pragma once

#include <QColor>
#include <QFont>
#include <QString>

struct Theme {
  bool dark;
  QColor bg, card, raised, border, text, muted, faint;
  /* accent: the volt fill for actions and the highlighted result; onAccent: text and marks drawn on it;
     accentSoft: a selected fill (text on it is onAccent too) */
  QColor accent, onAccent, accentSoft;
  QColor link; /* the accent as text or a thin line on a panel, where volt would not read */
  QColor mine; /* your results' bars on a panel */
  QColor ref, track;
  QColor warnBg, warnText, good, bad;
};

const Theme &theme();
void applyTheme(bool dark);

/* Inter at a size (points) and weight; falls back to the system font when Inter is missing. */
QFont uiFont(double pt, int weight = QFont::Normal);
/* IBM Plex Mono: units, IDs, clocks, conditions. */
QFont monoFont(double pt, int weight = QFont::Normal);
/* Saira at a width (50 condensed … 125 expanded): scores condensed, headings wide. Without Saira, or before
   Qt 6.7 (no font axes), Inter at the same size and weight. */
QFont displayFont(double pt, int weight = QFont::Bold, double width = 100);
/* Mono capitals with tracking, for small labels above a block. */
QFont labelFont(double pt = 8);
