/*
 * Visual theme of the desktop app: colour tokens for dark and light, the
 * embedded Inter typeface, and the stylesheet for standard widgets. Painted
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
  QColor accent, accent2, accentSoft, ref, track;
  QColor warnBg, warnText, good, bad;
};

const Theme &theme();
void applyTheme(bool dark);

/* Inter at a size (points) and weight; falls back to the system font when Inter is missing. */
QFont uiFont(double pt, int weight = QFont::Normal);
QFont monoFont(double pt);
