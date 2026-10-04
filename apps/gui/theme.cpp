/* Copyright 2026 The Prismark Authors. Apache-2.0. */
#include "theme.h"

#include <QApplication>
#include <QFontDatabase>
#include <QPalette>
#include <QStyleFactory>

namespace {

Theme g_theme;
QString g_family;

/*
 * Amber on warm greys: charcoal and paper backgrounds, one amber accent for what is yours or selected, and no
 * gradients. The placeholder warning is blue so it never reads as the accent; colour otherwise means data.
 */
const Theme kDark = {
    true,
    QColor(20, 19, 17),     /* bg */
    QColor(28, 26, 23),     /* card */
    QColor(36, 34, 30),     /* raised */
    QColor(52, 49, 43),     /* border */
    QColor(235, 232, 225),  /* text */
    QColor(167, 162, 151),  /* muted */
    QColor(113, 108, 98),   /* faint */
    QColor(232, 163, 60),   /* accent */
    QColor(29, 20, 5),      /* onAccent */
    QColor(58, 44, 21),     /* accentSoft */
    QColor(91, 87, 79),     /* ref */
    QColor(40, 38, 31),     /* track */
    QColor(44, 58, 74),     /* warnBg */
    QColor(169, 200, 238),  /* warnText */
    QColor(92, 207, 152),   /* good */
    QColor(240, 138, 118),  /* bad */
};

const Theme kLight = {
    false,
    QColor(245, 243, 238),  /* bg */
    QColor(255, 253, 249),  /* card */
    QColor(249, 247, 242),  /* raised */
    QColor(224, 220, 210),  /* border */
    QColor(28, 26, 22),     /* text */
    QColor(98, 93, 83),     /* muted */
    QColor(157, 151, 140),  /* faint */
    QColor(178, 107, 0),    /* accent */
    QColor(255, 255, 255),  /* onAccent */
    QColor(251, 238, 216),  /* accentSoft */
    QColor(189, 183, 171),  /* ref */
    QColor(236, 232, 223),  /* track */
    QColor(227, 236, 248),  /* warnBg */
    QColor(36, 69, 110),    /* warnText */
    QColor(31, 138, 91),    /* good */
    QColor(179, 64, 46),    /* bad */
};

void loadFonts() {
  if (!g_family.isEmpty()) return;
  for (const char *w : {"Regular", "Medium", "SemiBold", "Bold"}) {
    int id = QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/Inter-%1.otf").arg(w));
    if (id >= 0 && g_family.isEmpty()) g_family = QFontDatabase::applicationFontFamilies(id).value(0);
  }
  if (g_family.isEmpty()) g_family = QApplication::font().family();
}

}  // namespace

const Theme &theme() { return g_theme; }

QFont uiFont(double pt, int weight) {
  loadFonts();
  QFont f(g_family);
  f.setPointSizeF(pt);
  f.setWeight(weight);
  f.setHintingPreference(QFont::PreferVerticalHinting);
  return f;
}

QFont monoFont(double pt) {
  QFont f = QFontDatabase::systemFont(QFontDatabase::FixedFont);
  f.setPointSizeF(pt);
  return f;
}

void applyTheme(bool dark) {
  g_theme = dark ? kDark : kLight;
  const Theme &t = g_theme;
  QApplication::setStyle(QStyleFactory::create("Fusion"));
  QApplication::setFont(uiFont(10));

  QPalette p;
  p.setColor(QPalette::Window, t.bg);
  p.setColor(QPalette::Base, t.card);
  p.setColor(QPalette::AlternateBase, t.track);
  p.setColor(QPalette::Text, t.text);
  p.setColor(QPalette::WindowText, t.text);
  p.setColor(QPalette::Button, t.raised);
  p.setColor(QPalette::ButtonText, t.text);
  p.setColor(QPalette::Mid, t.ref);
  p.setColor(QPalette::Highlight, t.accent);
  p.setColor(QPalette::HighlightedText, t.onAccent);
  p.setColor(QPalette::ToolTipBase, t.raised);
  p.setColor(QPalette::ToolTipText, t.text);
  p.setColor(QPalette::PlaceholderText, t.muted);
  p.setColor(QPalette::Link, t.accent);
  p.setColor(QPalette::Disabled, QPalette::Text, t.faint);
  p.setColor(QPalette::Disabled, QPalette::ButtonText, t.faint);
  p.setColor(QPalette::Disabled, QPalette::WindowText, t.faint);
  QApplication::setPalette(p);

  auto c = [](const QColor &x) { return x.name(QColor::HexArgb); };
  qApp->setStyleSheet(QStringLiteral(R"(
    QToolTip { background: %1; color: %2; border: 1px solid %3; border-radius: 6px; padding: 6px 8px; }
    QFrame#card { background: %4; border: 1px solid %3; border-radius: 16px; }
    QScrollArea, QScrollArea > QWidget > QWidget { background: transparent; }
    QScrollBar:vertical { background: transparent; width: 10px; margin: 4px 2px; }
    QScrollBar::handle:vertical { background: %3; border-radius: 3px; min-height: 30px; }
    QScrollBar::handle:vertical:hover { background: %5; }
    QScrollBar::add-line, QScrollBar::sub-line, QScrollBar::add-page, QScrollBar::sub-page { background: none; height: 0; }
    QPushButton { background: %1; color: %2; border: 1px solid %3; border-radius: 10px; padding: 8px 16px; font-weight: 500; }
    QPushButton:hover { border-color: %6; }
    QPushButton:pressed { background: %3; }
    QPushButton:disabled { color: %5; }
    QPushButton#primary { border: none; color: %7; font-weight: 600; padding: 9px 20px; background: %6; }
    QPushButton#primary:hover { background: %11; }
    QPushButton#ghost { background: transparent; border: 1px solid transparent; padding: 6px 10px; color: %8; }
    QPushButton#ghost:hover { background: %1; border-color: %3; color: %2; }
    QToolButton#iconButton { background: transparent; border: 1px solid transparent; border-radius: 10px; padding: 6px; }
    QToolButton#iconButton:hover { background: %1; border-color: %3; }
    QToolButton#iconButton::menu-indicator { image: none; }
    QMenu { background: %1; color: %2; border: 1px solid %3; border-radius: 10px; padding: 6px; }
    QMenu::item { padding: 7px 22px 7px 14px; border-radius: 6px; }
    QMenu::item:selected { background: %9; }
    QMenu::separator { height: 1px; background: %3; margin: 5px 8px; }
    QDialog { background: %10; }
    QComboBox, QLineEdit, QPlainTextEdit { background: %1; color: %2; border: 1px solid %3; border-radius: 8px; padding: 6px 8px; }
    QComboBox QAbstractItemView { background: %1; color: %2; border: 1px solid %3; selection-background-color: %9; }
    QCheckBox { spacing: 8px; }
  )")
                          .arg(c(t.raised), c(t.text), c(t.border), c(t.card), c(t.faint), c(t.accent), c(t.onAccent),
                               c(t.muted), c(t.accentSoft))
                          .arg(c(t.bg), c(t.dark ? t.accent.lighter(112) : t.accent.darker(112))));
}
