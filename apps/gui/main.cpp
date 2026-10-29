/*
 * prismark-gui: desktop front-end. Launch it directly; it finds the
 * `prismark` runner next to itself.
 *
 *   prismark-gui                 open the app
 *   prismark-gui --run [TEST] [--full]   start a quick (or full) run at once: all tests, or one test
 *                                        id such as mc_k2
 *
 * PRISMARK_GUI_SCREENSHOT=<file.png> renders the window once, saves it and
 * exits (used to check the layout headless, with QT_QPA_PLATFORM=offscreen).
 *
 * Copyright 2026 The Prismark Authors. Apache-2.0.
 */
#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QPainter>
#include <QSvgRenderer>
#include <QTimer>

#include "mainwindow.h"
#include "widgets.h"

int main(int argc, char **argv) {
  QApplication app(argc, argv);
  /* The portable packages carry the compile tests' build tools (cmake, ninja, clang-19) in tools/bin: first in
     PATH, so the checks before a run find them and the runner inherits them. */
  const QString tools = QDir::toNativeSeparators(QCoreApplication::applicationDirPath() + "/tools/bin");
  if (QFileInfo(tools).isDir()) qputenv("PATH", (tools + QDir::listSeparator()).toLocal8Bit() + qgetenv("PATH"));
  QApplication::setOrganizationName("Prismark");
  QApplication::setApplicationName("Prismark");
  QApplication::setApplicationVersion(PMK_VERSION);
  MainWindow w;
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
  /* PRISMARK_GUI_ICONSET=<dir.iconset> writes the macOS app icon's sizes for iconutil and exits (used by
   * tools/package-macos.sh). The tile sits in the middle 80 % of the canvas, as macOS icons do. */
  QByteArray iconset = qgetenv("PRISMARK_GUI_ICONSET");
  if (!iconset.isEmpty()) {
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
