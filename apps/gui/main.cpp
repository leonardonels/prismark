/*
 * prismark-gui: desktop front-end. Launch it directly; it finds the
 * `prismark` runner next to itself.
 *
 *   prismark-gui                 open the app
 *   prismark-gui --run [TEST] [--full]   start a quick (or full) run at once: all tests, or one test
 *                                        id such as mc_k2
 *
 * PRISMARK_GUI_ICONSET and PRISMARK_GUI_DMG_BACKGROUND draw the macOS icon and disk-image background (below).
 *
 * PRISMARK_GUI_SCREENSHOT=<file.png> renders the window once, saves it and
 * exits (used to check the layout headless, with QT_QPA_PLATFORM=offscreen).
 *
 * Copyright 2026 The Prismark Authors. Apache-2.0.
 */
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QPainter>
#include <QPolygonF>
#include <QSvgRenderer>
#include <QTimer>

#include "mainwindow.h"
#include "setup.h"
#include "theme.h"
#include "widgets.h"

int main(int argc, char **argv) {
  QApplication app(argc, argv);
  /* PRISMARK_GUI_ICONSET=<dir.iconset> writes the macOS app icon's sizes for iconutil and exits (used by
   * the macOS build). The tile sits in the middle 80 % of the canvas, as macOS icons do. */
  QByteArray iconset = qgetenv("PRISMARK_GUI_ICONSET");
  if (!iconset.isEmpty()) {
    QSvgRenderer appIcon(QStringLiteral(":/prismark-app.svg")), smallIcon(QStringLiteral(":/prismark-app-small.svg"));
    QDir().mkpath(QString::fromLocal8Bit(iconset));
    for (int s : {16, 32, 128, 256, 512})
      for (int k : {1, 2}) {
        int px = s * k;
        QImage im(px, px, QImage::Format_ARGB32_Premultiplied);
        im.fill(Qt::transparent);
        QPainter p(&im);
        p.setRenderHint(QPainter::Antialiasing);
        (px <= 32 ? smallIcon : appIcon).render(&p, QRectF(px * 0.1, px * 0.1, px * 0.8, px * 0.8));
        p.end();
        im.save(QStringLiteral("%1/icon_%2x%2%3.png").arg(QString::fromLocal8Bit(iconset)).arg(s).arg(k == 2 ? "@2x" : ""));
      }
    return 0;
  }
  /* PRISMARK_GUI_DMG_BACKGROUND=<base> writes <base>.png and <base>@2x.png, the background of the macOS disk
   * image's window (640 x 480 points; tools/package-macos.sh places the icons on it), and exits. Signal blue
   * keeps the Finder's icon labels legible both in light mode (black) and in dark mode (white). */
  QByteArray background = qgetenv("PRISMARK_GUI_DMG_BACKGROUND");
  if (!background.isEmpty()) {
    for (int k : {1, 2}) {
      QImage im(640 * k, 480 * k, QImage::Format_ARGB32_Premultiplied);
      im.setDevicePixelRatio(k);
      im.setDotsPerMeterX(qRound(72 * k / 0.0254)); /* 72 dpi per point: the Finder sizes the background by it */
      im.setDotsPerMeterY(qRound(72 * k / 0.0254));
      im.fill(QColor(45, 91, 255));
      QPainter p(&im);
      p.setRenderHint(QPainter::Antialiasing);
      auto px = [](QFont f, int size) { f.setPixelSize(size); return f; };
      const QColor paper(247, 248, 249), volt(214, 245, 59);
      p.setPen(paper);
      QFont label = px(monoFont(10, QFont::Medium), 11);
      label.setCapitalization(QFont::AllUppercase);
      label.setLetterSpacing(QFont::PercentageSpacing, 110);
      p.setFont(label);
      p.drawText(QPointF(32, 40), QStringLiteral("Prismark %1 · macOS").arg(PMK_VERSION));
      p.setFont(px(displayFont(20, QFont::Bold, 112), 28));
      p.drawText(QPointF(32, 80), QStringLiteral("Drag Prismark to Applications"));
      /* The arrow from the app (centred at x 170) to the Applications folder (x 470), at the icons' height. */
      p.setPen(Qt::NoPen);
      p.setBrush(volt);
      p.drawRect(QRectF(246, 196, 128, 8));
      p.drawPolygon(QPolygonF({QPointF(370, 182), QPointF(396, 200), QPointF(370, 218)}));
      p.setBrush(QColor(247, 248, 249, 90));
      p.drawRect(QRectF(32, 300, 576, 1));
      p.setPen(paper);
      p.setFont(label);
      p.drawText(QPointF(32, 334), QStringLiteral("First open"));
      p.setFont(px(uiFont(10), 13));
      p.drawText(QRectF(32, 344, 300, 80), Qt::TextWordWrap,
                 QStringLiteral("The app is not notarised by Apple, so macOS may stop it the first time. Then "
                                "open System Settings › Privacy & Security and click Open Anyway."));
      p.end();
      im.save(QString::fromLocal8Bit(background) + (k == 2 ? "@2x.png" : ".png"));
    }
    return 0;
  }
#if defined(Q_OS_MACOS)
  /* An app opened from the Finder gets only the system PATH, without Homebrew. Homebrew's LLVM 19 is keg-only and
     has no clang++-19, so links under the names the compile tests use (clang-19, clang++-19, llvm-ar, llvm-ranlib)
     go to <data>/llvm19/bin, ahead of Homebrew's own bin. */
  {
    QStringList add;
    for (const QString &brew : {QStringLiteral("/opt/homebrew"), QStringLiteral("/usr/local")}) {
      const QString llvm = brew + "/opt/llvm@19/bin";
      if (QFileInfo(llvm + "/clang-19").isExecutable()) {
        const QString links = dataDir() + "/llvm19/bin";
        QDir().mkpath(links);
        const char *names[][2] = {{"clang-19", "clang-19"}, {"clang++-19", "clang-19"}, {"llvm-ar", "llvm-ar"},
                                  {"llvm-ranlib", "llvm-ranlib"}};
        for (const auto &n : names) {
          const QString link = links + "/" + n[0], to = llvm + "/" + n[1];
          if (QFileInfo(link).symLinkTarget() != to) {
            QFile::remove(link);
            QFile::link(to, link);
          }
        }
        add << links;
      }
      if (QFileInfo(brew + "/bin").isDir()) add << brew + "/bin";
    }
    const QStringList path = QString::fromLocal8Bit(qgetenv("PATH")).split(':', Qt::SkipEmptyParts);
    for (auto it = add.rbegin(); it != add.rend(); ++it)
      if (!path.contains(*it)) qputenv("PATH", (*it + ":").toLocal8Bit() + qgetenv("PATH"));
  }
  /* In Prismark.app the compile tests' build tools are in Contents/Resources/tools/bin. */
  const QString tools = QCoreApplication::applicationDirPath() + "/../Resources/tools/bin";
#else
  /* The portable packages carry the compile tests' build tools (cmake, ninja, clang-19) in tools/bin: first in
     PATH, so the checks before a run find them and the runner inherits them. */
  const QString tools = QDir::toNativeSeparators(QCoreApplication::applicationDirPath() + "/tools/bin");
#endif
  if (QFileInfo(tools).isDir())
    qputenv("PATH", (QDir::toNativeSeparators(QFileInfo(tools).canonicalFilePath()) + QDir::listSeparator()).toLocal8Bit() +
                        qgetenv("PATH"));
  QApplication::setOrganizationName("Prismark");
  QApplication::setApplicationName("Prismark");
  QApplication::setApplicationVersion(PMK_VERSION);
  /* The window and taskbar icon, drawn from vectors at each size the system may ask for; 32 px and below use
   * the simpler version (prism and one bar), which stays legible. */
  QIcon icon;
  QSvgRenderer appIcon(QStringLiteral(":/prismark-app.svg")), smallIcon(QStringLiteral(":/prismark-app-small.svg"));
  for (int s : {16, 24, 32, 48, 64, 128, 256, 512}) {
    QPixmap pm(s, s);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    (s <= 32 ? smallIcon : appIcon).render(&p);
    p.end();
    icon.addPixmap(pm);
  }
  QApplication::setWindowIcon(icon);
  MainWindow w;
  w.show();
  QStringList args = QApplication::arguments();
  int run = args.indexOf("--run");
  if (run > 0) {
    QString test = run + 1 < args.size() && !args[run + 1].startsWith("-") ? args[run + 1] : QString();
    bool full = args.contains("--full");
    QTimer::singleShot(0, &w, [&w, test, full] { w.startRun(test, !full); });
  }
  QByteArray shot = qgetenv("PRISMARK_GUI_SCREENSHOT");
  if (!shot.isEmpty()) {
    QTimer::singleShot(qEnvironmentVariableIntValue("PRISMARK_GUI_SCREENSHOT_MS") > 0
                           ? qEnvironmentVariableIntValue("PRISMARK_GUI_SCREENSHOT_MS") : 400, [&] {
      QWidget *target = QApplication::activeModalWidget();
      if (!target) /* the window the user sees: the main one, or the measuring one during a run */
        for (QWidget *tw : QApplication::topLevelWidgets())
          if (tw->isVisible() && (!target || tw->windowTitle().contains("measuring"))) target = tw;
      (target ? target : static_cast<QWidget *>(&w))->grab().save(QString::fromLocal8Bit(shot));
      app.quit();
    });
  }
  return app.exec();
}
