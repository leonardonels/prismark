/* Copyright 2026 The Prismark Authors. Apache-2.0. */
#include "theme.h"

#include <QApplication>
#include <QFontDatabase>
#include <QPalette>
#include <QStyleFactory>

namespace {

Theme g_theme;
QString g_family;

const Theme kDark = {
    true,
    QColor(11, 13, 19),     /* bg */
    QColor(20, 23, 32),     /* card */
    QColor(28, 32, 44),     /* raised */
    QColor(38, 43, 58),     /* border */
    QColor(236, 238, 245),  /* text */
    QColor(145, 152, 172),  /* muted */
    QColor(92, 99, 118),    /* faint */
    QColor(124, 108, 255),  /* accent */
    QColor(78, 163, 255),   /* accent2 (gradient end) */
    QColor(40, 37, 78),     /* accentSoft */
    QColor(70, 77, 96),     /* ref */
    QColor(30, 34, 46),     /* track */
    QColor(58, 46, 14),     /* warnBg */
    QColor(246, 214, 128),  /* warnText */
    QColor(92, 207, 152),   /* good */
    QColor(240, 138, 118),  /* bad */
};

const Theme kLight = {
    false,
    QColor(242, 244, 249),
    QColor(255, 255, 255),
    QColor(246, 247, 251),
    QColor(222, 226, 236),
    QColor(20, 23, 33),
    QColor(92, 99, 120),
    QColor(150, 156, 174),
    QColor(98, 84, 240),
    QColor(40, 130, 240),
    QColor(232, 229, 255),
    QColor(178, 184, 200),
    QColor(232, 235, 243),
    QColor(255, 243, 210),
    QColor(110, 78, 0),
    QColor(31, 138, 91),
    QColor(179, 64, 46),
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
  p.setColor(QPalette::HighlightedText, Qt::white);
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
    QPushButton#primary { border: none; color: white; font-weight: 600; padding: 9px 20px;
      background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 %6, stop:1 %7); }
    QPushButton#primary:hover { background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 %7, stop:1 %6); }
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
                          .arg(c(t.raised), c(t.text), c(t.border), c(t.card), c(t.faint), c(t.accent), c(t.accent2),
                               c(t.muted), c(t.accentSoft))
                          .arg(c(t.bg)));
}
