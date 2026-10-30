/*
 * prismark-gui: desktop front-end. Launch it directly; it finds the
 * `prismark` runner next to itself.
 *
 *   prismark-gui                 open the app
 *   prismark-gui --run [TEST] [--full]   start a quick (or full) run at once: all tests, or one test
 *                                        id such as mc_k2
 *
 * PRISMARK_GUI_ICONSET and PRISMARK_GUI_DMG_BACKGROUND draw the macOS icon and disk-image background, and
 * PRISMARK_GUI_ICO the Windows icon (below).
 *
 * PRISMARK_GUI_SCREENSHOT=<file.png> renders the window once, saves it and
 * exits (used to check the layout headless, with QT_QPA_PLATFORM=offscreen).
 *
 * Copyright 2026 The Prismark Authors. Apache-2.0.
 */
#include <QApplication>
#include <QBuffer>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QPainter>
#include <QPolygonF>
#include <QStandardPaths>
#include <QSvgRenderer>
#include <QTimer>

#include "mainwindow.h"
#include "setup.h"
#include "theme.h"
#include "widgets.h"

#if defined(Q_OS_LINUX)
/* On Wayland the desktop shows a window's icon from the .desktop file named by its app id, not the icon the app
 * sets: without one, GNOME shows its generic icon. An installed Prismark has prismark.desktop (apps/gui/gui.cmake);
 * the portable folder gets one in ~/.local/share/applications pointing at itself, rewritten when the folder
 * moves. NoDisplay keeps it out of the app menu, where it would outlive a deleted folder: it only names the icon. */
void registerDesktopEntry(const QIcon &icon) {
  QGuiApplication::setDesktopFileName(QStringLiteral("prismark"));
  const QString name = QStringLiteral("applications/prismark.desktop");
  const QString own = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/" + name;
  const QString found = QStandardPaths::locate(QStandardPaths::GenericDataLocation, name);
  if (!found.isEmpty() && found != own) return; /* installed: the system's own entry */
  const QString png = dataDir() + "/prismark.png";
  auto quoted = [](QString s) { /* an argument of Exec: quoted, then escaped as any string value */
    for (const char *c : {"\\", "\"", "`", "$"}) s.replace(QLatin1String(c), QLatin1String("\\") + QLatin1String(c));
    return ("\"" + s.replace('%', QLatin1String("%%")) + "\"").replace('\\', QLatin1String("\\\\"));
  };
  /* with the version, so a new one also redraws the icon */
  const QByteArray entry = QStringLiteral("[Desktop Entry]\nType=Application\nName=Prismark\n"
                                          "Comment=Vendor-neutral CPU benchmark\nExec=%1\nIcon=%2\nTerminal=false\n"
                                          "NoDisplay=true\nX-Prismark-Version=%3\n")
                               .arg(quoted(QCoreApplication::applicationFilePath()), png, PMK_VERSION)
                               .toUtf8();
  QFile f(own);
  if (f.open(QIODevice::ReadOnly) && f.readAll() == entry && QFileInfo::exists(png)) return;
  f.close();
  QDir().mkpath(dataDir());
  icon.pixmap(256, 256).save(png);
  QDir().mkpath(QFileInfo(own).path());
  if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) f.write(entry);
}
#endif

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
   * image's window (laid out for 740 x 560 points; tools/package-macos.sh places the icons on it), and exits.
   * The Finder may open the window larger than asked (it does on macOS 27), so the image is larger too and blue
   * to its edges: never a white margin. Signal blue keeps the Finder's icon labels legible both in light mode
   * (black) and in dark mode (white). */
  QByteArray background = qgetenv("PRISMARK_GUI_DMG_BACKGROUND");
  if (!background.isEmpty()) {
    for (int k : {1, 2}) {
      QImage im(1200 * k, 900 * k, QImage::Format_ARGB32_Premultiplied);
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
      p.drawText(QPointF(40, 48), QStringLiteral("Prismark %1 · macOS").arg(PMK_VERSION));
      p.setFont(px(displayFont(20, QFont::Bold, 112), 30));
      p.drawText(QPointF(40, 92), QStringLiteral("Drag Prismark to Applications"));
      /* The arrow from the app (centred at x 200) to the Applications folder (x 540), at the icons' height. */
      p.setPen(Qt::NoPen);
      p.setBrush(volt);
      p.drawRect(QRectF(284, 226, 150, 8));
      p.drawPolygon(QPolygonF({QPointF(430, 212), QPointF(456, 230), QPointF(430, 248)}));
      p.setBrush(QColor(247, 248, 249, 90));
      p.drawRect(QRectF(40, 342, 660, 1));
      p.setPen(paper);
      p.setFont(label);
      p.drawText(QPointF(40, 380), QStringLiteral("First open"));
      p.setFont(px(uiFont(10), 13));
      p.drawText(QRectF(40, 392, 360, 100), Qt::TextWordWrap,
                 QStringLiteral("The app is not notarised by Apple, so macOS may stop it the first time. Then "
                                "open System Settings › Privacy & Security and click Open Anyway."));
      p.end();
      im.save(QString::fromLocal8Bit(background) + (k == 2 ? "@2x.png" : ".png"));
    }
    return 0;
  }
  /* PRISMARK_GUI_ICO=<file.ico> writes the Windows icon (apps/gui/icons/prismark.ico, built into prismark-gui.exe
   * for Explorer and the taskbar) and exits: every size Windows asks for, PNG-compressed, drawn as the window icon
   * below. The Windows build is cross-compiled and cannot run the app, so the file is kept in the sources: run
   * this again after changing the icon's SVG. */
  QByteArray ico = qgetenv("PRISMARK_GUI_ICO");
  if (!ico.isEmpty()) {
    QSvgRenderer appIcon(QStringLiteral(":/prismark-app.svg")), smallIcon(QStringLiteral(":/prismark-app-small.svg"));
    const int sizes[] = {16, 20, 24, 32, 40, 48, 64, 128, 256};
    QVector<QByteArray> pngs;
    for (int s : sizes) {
      QImage im(s, s, QImage::Format_ARGB32_Premultiplied);
      im.fill(Qt::transparent);
      QPainter p(&im);
      p.setRenderHint(QPainter::Antialiasing);
      (s <= 32 ? smallIcon : appIcon).render(&p);
      p.end();
      QBuffer b;
      b.open(QIODevice::WriteOnly);
      im.save(&b, "PNG");
      pngs << b.data();
    }
    QFile f(QString::fromLocal8Bit(ico));
    if (!f.open(QIODevice::WriteOnly)) return 1;
    QDataStream out(&f);
    out.setByteOrder(QDataStream::LittleEndian);
    out << quint16(0) << quint16(1) << quint16(pngs.size()); /* an icon, with this many images */
    quint32 offset = 6 + 16 * pngs.size();
    for (int i = 0; i < pngs.size(); i++) {
      quint8 side = sizes[i] >= 256 ? 0 : quint8(sizes[i]); /* 0 is 256 */
      out << side << side << quint8(0) << quint8(0) << quint16(1) << quint16(32) << quint32(pngs[i].size()) << offset;
      offset += pngs[i].size();
    }
    for (const QByteArray &png : pngs) out.writeRawData(png.constData(), png.size());
    return f.error() == QFile::NoError ? 0 : 1;
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
#if defined(Q_OS_LINUX)
  registerDesktopEntry(icon);
#endif
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
