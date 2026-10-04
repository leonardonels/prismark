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
#include <QPainter>
#include <QTimer>

#include "mainwindow.h"
#include "widgets.h"

int main(int argc, char **argv) {
  QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
  QApplication app(argc, argv);
  QApplication::setOrganizationName("Prismark");
  QApplication::setApplicationName("Prismark");
  QApplication::setApplicationVersion(PMK_VERSION);
  MainWindow w;
  QPixmap icon(256, 256);
  icon.fill(Qt::transparent);
  {
    QPainter p(&icon);
    paintLogo(p, QRectF(16, 16, 224, 224));
  }
  QApplication::setWindowIcon(QIcon(icon));
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
