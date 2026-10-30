/* Copyright 2026 The Prismark Authors. Apache-2.0. */
#include "theme.h"

#include <QApplication>
#include <QFontDatabase>
#include <QPalette>
#include <QStyleFactory>

namespace {

Theme g_theme;
QString g_family, g_display, g_mono;

/*
 * Flat blocks, square corners, no gradients: navy ink, paper, a volt accent for actions and the highlighted
 * result, and signal blue for structure and reference machines. Red is kept for wrong results and failures.
 */
const Theme kDark = {
    true,
    QColor(7, 12, 31),      /* bg */
    QColor(20, 29, 59),     /* card */
    QColor(31, 43, 82),     /* raised */
    QColor(42, 55, 96),     /* border */
    QColor(232, 236, 239),  /* text */
    QColor(174, 184, 214),  /* muted */
    QColor(133, 145, 181),  /* faint */
    QColor(214, 245, 59),   /* accent */
    QColor(12, 20, 48),     /* onAccent */
    QColor(214, 245, 59),   /* accentSoft */
    QColor(214, 245, 59),   /* link */
    QColor(214, 245, 59),   /* mine */
    QColor(92, 128, 255),   /* ref */
    QColor(31, 43, 82),     /* track */
    QColor(30, 63, 184),    /* warnBg */
    QColor(247, 248, 249),  /* warnText */
    QColor(214, 245, 59),   /* good */
    QColor(255, 111, 94),   /* bad */
};

const Theme kLight = {
    false,
    QColor(221, 226, 232),  /* bg */
    QColor(247, 248, 249),  /* card */
    QColor(232, 236, 239),  /* raised */
    QColor(201, 208, 218),  /* border */
    QColor(12, 20, 48),     /* text */
    QColor(30, 39, 66),     /* muted */
    QColor(52, 62, 90),     /* faint */
    QColor(214, 245, 59),   /* accent */
    QColor(12, 20, 48),     /* onAccent */
    QColor(214, 245, 59),   /* accentSoft */
    QColor(45, 91, 255),    /* link */
    QColor(12, 20, 48),     /* mine */
    QColor(45, 91, 255),    /* ref */
    QColor(232, 236, 239),  /* track */
    QColor(220, 228, 255),  /* warnBg */
    QColor(30, 63, 184),    /* warnText */
    QColor(59, 82, 0),      /* good */
    QColor(194, 38, 26),    /* bad */
};

QString addFont(const QString &file) {
  int id = QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/") + file);
  return id >= 0 ? QFontDatabase::applicationFontFamilies(id).value(0) : QString();
}

void loadFonts() {
  if (!g_family.isEmpty()) return;
  for (const char *w : {"Regular", "Medium", "SemiBold", "Bold"}) {
    QString f = addFont(QStringLiteral("Inter-%1.otf").arg(w));
    if (g_family.isEmpty()) g_family = f;
  }
  if (g_family.isEmpty()) g_family = QApplication::font().family();
  g_display = addFont(QStringLiteral("Saira.ttf"));
  for (const char *w : {"Regular", "Medium", "SemiBold"}) {
    QString f = addFont(QStringLiteral("IBMPlexMono-%1.ttf").arg(w));
    if (g_mono.isEmpty()) g_mono = f;
  }
}

}  // namespace

const Theme &theme() { return g_theme; }

QFont uiFont(double pt, int weight) {
  loadFonts();
  QFont f(g_family);
  f.setPointSizeF(pt);
  f.setWeight(static_cast<QFont::Weight>(weight));
  f.setHintingPreference(QFont::PreferVerticalHinting);
  return f;
}

QFont monoFont(double pt, int weight) {
  loadFonts();
  QFont f = g_mono.isEmpty() ? QFontDatabase::systemFont(QFontDatabase::FixedFont) : QFont(g_mono);
  f.setPointSizeF(pt);
  f.setWeight(static_cast<QFont::Weight>(weight));
  return f;
}

QFont displayFont(double pt, int weight, double width) {
  loadFonts();
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
  if (!g_display.isEmpty()) {
    QFont f(g_display);
    f.setPointSizeF(pt);
    f.setWeight(static_cast<QFont::Weight>(weight));
    f.setVariableAxis(QFont::Tag("wght"), weight);
    f.setVariableAxis(QFont::Tag("wdth"), width);
    return f;
  }
#else
  Q_UNUSED(width);
#endif
  return uiFont(pt, weight);
}

QFont labelFont(double pt) {
  QFont f = monoFont(pt, QFont::Medium);
  f.setCapitalization(QFont::AllUppercase);
  f.setLetterSpacing(QFont::PercentageSpacing, 106);
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
  p.setColor(QPalette::Link, t.link);
  p.setColor(QPalette::Disabled, QPalette::Text, t.faint);
  p.setColor(QPalette::Disabled, QPalette::ButtonText, t.faint);
  p.setColor(QPalette::Disabled, QPalette::WindowText, t.faint);
  QApplication::setPalette(p);

  auto c = [](const QColor &x) { return x.name(QColor::HexArgb); };
  qApp->setStyleSheet(QStringLiteral(R"(
    QToolTip { background: %1; color: %2; border: 1px solid %3; padding: 6px 8px; }
    QFrame#card { background: %4; border: none; }
    QScrollArea, QScrollArea > QWidget > QWidget { background: transparent; }
    QScrollBar:vertical { background: transparent; width: 10px; margin: 4px 2px; }
    QScrollBar::handle:vertical { background: %3; min-height: 30px; }
    QScrollBar::handle:vertical:hover { background: %5; }
    QScrollBar::add-line, QScrollBar::sub-line, QScrollBar::add-page, QScrollBar::sub-page { background: none; height: 0; }
    QPushButton { background: %1; color: %2; border: 1.5px solid transparent; padding: 9px 18px; font-weight: 600; min-height: 22px; }
    QPushButton:hover { border-color: %2; }
    QPushButton:pressed { background: %3; }
    QPushButton:disabled { color: %5; }
    QPushButton#primary { border: none; color: %7; font-weight: 700; padding: 10px 22px; background: %6; }
    QPushButton#primary:hover { background: %11; }
    QPushButton#ghost { background: transparent; border: 1.5px solid transparent; padding: 6px 10px; color: %8; }
    QPushButton#ghost:hover { background: %1; color: %2; }
    QToolButton#iconButton { background: %1; border: none; padding: 9px; }
    QToolButton#iconButton:hover { background: %3; }
    QToolButton#iconButton::menu-indicator { image: none; }
    QMenu { background: %4; color: %2; border: 1px solid %3; padding: 6px; }
    QMenu::item { padding: 8px 22px 8px 14px; }
    QMenu::item:selected { background: %9; color: %7; }
    QMenu::separator { height: 1px; background: %3; margin: 5px 8px; }
    QMainWindow { background: %10; }
    QDialog { background: %4; }
    QComboBox, QLineEdit, QPlainTextEdit { background: %1; color: %2; border: 1px solid %3; padding: 6px 8px; }
    QComboBox:focus, QLineEdit:focus, QPlainTextEdit:focus { border-color: %12; }
    QComboBox QAbstractItemView { background: %4; color: %2; border: 1px solid %3; selection-background-color: %9; selection-color: %7; }
    QCheckBox { spacing: 8px; }
  )")
                          .arg(c(t.raised), c(t.text), c(t.border), c(t.card), c(t.faint), c(t.accent), c(t.onAccent),
                               c(t.muted), c(t.accentSoft))
                          .arg(c(t.bg), c(t.dark ? t.accent.lighter(110) : t.accent.darker(108)), c(t.link)));
}
