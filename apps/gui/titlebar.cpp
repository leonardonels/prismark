/* Copyright 2026 The Prismark Authors. Apache-2.0. */
#include "titlebar.h"

#include <QGuiApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWindow>
#include <QtGlobal>
#include <algorithm>

#include <QDir>
#include <QProcess>
#include <QSettings>
#ifdef Q_OS_MACOS
#include <CoreFoundation/CoreFoundation.h>
#include <objc/message.h>
#include <objc/runtime.h>
#endif
#ifdef Q_OS_WIN
#include <QAbstractNativeEventFilter>
#include <QCoreApplication>
#include <QSet>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dwmapi.h>
#endif

#include "theme.h"

namespace {

constexpr int kHeight = 32;
constexpr int kEdge = 6; /* the outer band that resizes a frameless window */

/* The strip's colours on the ink: drawn the same in both themes, a little brighter in light where the ink is too. */
struct Ink {
  QColor glyph, strong, line, fill, hover;
};
Ink ink() {
  if (theme().dark) return {QColor(174, 184, 214), QColor(232, 236, 239), QColor(31, 43, 82), QColor(20, 29, 59), QColor(31, 43, 82)};
  return {QColor(201, 208, 218), QColor(247, 248, 249), QColor(42, 55, 96), QColor(30, 39, 66), QColor(42, 55, 96)};
}
const QColor kCloseHover(194, 38, 26);

enum class Desk { Mac, Windows, Linux };
/* The desktop the strip is drawn for. PRISMARK_GUI_TITLEBAR=windows|gnome|kde draws another desktop's strip on this
   one, frameless, to look at it (screenshots, and checking the Windows and Linux strips on a Mac). */
Desk desk() {
  static const Desk d = [] {
    const QString want = qEnvironmentVariable("PRISMARK_GUI_TITLEBAR").toLower();
    if (want == QLatin1String("windows")) return Desk::Windows;
    if (want == QLatin1String("gnome") || want == QLatin1String("kde") || want == QLatin1String("linux")) return Desk::Linux;
#if defined(Q_OS_MACOS)
    return Desk::Mac;
#elif defined(Q_OS_WIN)
    return Desk::Windows;
#else
    return Desk::Linux;
#endif
  }();
  return d;
}

/* Which buttons the window manager shows, on which side. The app follows GNOME's and KDE Plasma's settings; other
   desktops, and Windows, get minimize, maximize and close on the right. PRISMARK_GUI_BUTTONS sets a layout in
   GNOME's words ("close,minimize:" puts them on the left) in place of the desktop's. */
struct Layout {
  QVector<int> left, right; /* TitleStrip::Button values */
};
Layout gnomeLayout(const QString &layout) {
  auto side = [](const QString &names) {
    QVector<int> out;
    for (const QString &item : names.split(',', Qt::SkipEmptyParts)) {
      const QString n = item.trimmed();
      if (n == QLatin1String("minimize")) out << 0;
      else if (n == QLatin1String("maximize")) out << 1;
      else if (n == QLatin1String("close")) out << 2;
    }
    return out;
  };
  return {side(layout.section(':', 0, 0)), side(layout.section(':', 1))}; /* left of the colon goes left */
}
Layout buttonLayout() {
  const Layout standard{{}, {0, 1, 2}};
  if (desk() != Desk::Linux) return standard;
  if (qEnvironmentVariableIsSet("PRISMARK_GUI_BUTTONS")) return gnomeLayout(qEnvironmentVariable("PRISMARK_GUI_BUTTONS"));
  QString de = qEnvironmentVariable("XDG_CURRENT_DESKTOP").toUpper();
  if (qEnvironmentVariable("PRISMARK_GUI_TITLEBAR").toLower() == QLatin1String("gnome")) de = QStringLiteral("GNOME");
  if (qEnvironmentVariable("PRISMARK_GUI_TITLEBAR").toLower() == QLatin1String("kde")) de = QStringLiteral("KDE");
  if (de.contains("KDE")) {
    /* kwinrc: letters, I minimize, A maximize, X close (the others are buttons the app has no use for) */
    QSettings kwin(QDir::homePath() + "/.config/kwinrc", QSettings::IniFormat);
    kwin.beginGroup("org.kde.kdecoration2");
    auto letters = [](const QString &s) {
      QVector<int> out;
      for (QChar c : s)
        if (c == 'I') out << 0;
        else if (c == 'A') out << 1;
        else if (c == 'X') out << 2;
      return out;
    };
    return {letters(kwin.value("ButtonsOnLeft", "MS").toString()), letters(kwin.value("ButtonsOnRight", "HIAX").toString())};
  }
  if (de.contains("GNOME") || de.contains("UNITY") || de.contains("BUDGIE")) {
    QString layout = QStringLiteral("appmenu:close"); /* GNOME's default */
    QProcess p;
    p.start(QStringLiteral("gsettings"), {"get", "org.gnome.desktop.wm.preferences", "button-layout"});
    if (p.waitForFinished(2000) && p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0) {
      QString out = QString::fromUtf8(p.readAllStandardOutput()).trimmed();
      out.remove('\'');
      if (out.contains(':')) layout = out;
    }
    return gnomeLayout(layout);
  }
  return standard;
}
const Layout &buttons() {
  static const Layout l = buttonLayout();
  return l;
}

bool fixedSize(const QWidget *w) { return w->minimumSize() == w->maximumSize(); }

/* Resizes a frameless window from its outer band: the window's own events, before any widget sees them. */
class EdgeResizer : public QObject {
 public:
  EdgeResizer(QWidget *widget) : QObject(widget), widget_(widget) {}

 protected:
  bool eventFilter(QObject *o, QEvent *e) override {
    auto *win = qobject_cast<QWindow *>(o);
    if (!win || (e->type() != QEvent::MouseMove && e->type() != QEvent::MouseButtonPress)) return false;
    auto *me = static_cast<QMouseEvent *>(e);
    const Qt::Edges edges = edgesAt(me->pos());
    if (e->type() == QEvent::MouseButtonPress) {
      if (edges && me->button() == Qt::LeftButton) {
        win->startSystemResize(edges);
        return true;
      }
      return false;
    }
    if (me->buttons() != Qt::NoButton) return false;
    if (edges) {
      const bool diagonal = (edges & (Qt::LeftEdge | Qt::RightEdge)) && (edges & (Qt::TopEdge | Qt::BottomEdge));
      Qt::CursorShape shape = (edges == Qt::LeftEdge || edges == Qt::RightEdge) ? Qt::SizeHorCursor
                              : !diagonal                                       ? Qt::SizeVerCursor
                              : (edges == (Qt::TopEdge | Qt::LeftEdge) || edges == (Qt::BottomEdge | Qt::RightEdge))
                                  ? Qt::SizeFDiagCursor
                                  : Qt::SizeBDiagCursor;
      win->setCursor(shape);
      overridden_ = true;
    } else if (overridden_) { /* back to the cursor of what is under the mouse */
      QWidget *under = widget_->childAt(me->pos());
      win->setCursor((under ? under : widget_)->cursor());
      overridden_ = false;
    }
    return false;
  }

 private:
  Qt::Edges edgesAt(const QPoint &p) const {
    if (fixedSize(widget_) || widget_->isMaximized() || widget_->isFullScreen()) return {};
    Qt::Edges e;
    if (p.x() < kEdge) e |= Qt::LeftEdge;
    if (p.x() >= widget_->width() - kEdge) e |= Qt::RightEdge;
    if (p.y() < kEdge) e |= Qt::TopEdge;
    if (p.y() >= widget_->height() - kEdge) e |= Qt::BottomEdge;
    return e;
  }
  QWidget *widget_;
  bool overridden_ = false;
};

#ifdef Q_OS_WIN
/* A frameless window maximized by Windows would cover the taskbar: it is given the screen's work area instead. */
class WorkArea : public QAbstractNativeEventFilter {
 public:
  QSet<HWND> windows;
  bool nativeEventFilter(const QByteArray &, void *message, qintptr *) override {
    auto *msg = static_cast<MSG *>(message);
    if (msg->message != WM_GETMINMAXINFO || !windows.contains(msg->hwnd)) return false;
    MONITORINFO mi = {sizeof(mi)};
    if (!GetMonitorInfoW(MonitorFromWindow(msg->hwnd, MONITOR_DEFAULTTONEAREST), &mi)) return false;
    auto *mmi = reinterpret_cast<MINMAXINFO *>(msg->lParam);
    mmi->ptMaxPosition.x = mi.rcWork.left - mi.rcMonitor.left;
    mmi->ptMaxPosition.y = mi.rcWork.top - mi.rcMonitor.top;
    mmi->ptMaxSize.x = mi.rcWork.right - mi.rcWork.left;
    mmi->ptMaxSize.y = mi.rcWork.bottom - mi.rcWork.top;
    return false; /* Qt adds the window's own limits */
  }
};
WorkArea *workArea() {
  static WorkArea *w = [] {
    auto *f = new WorkArea;
    QCoreApplication::instance()->installNativeEventFilter(f);
    return f;
  }();
  return w;
}
#endif

void paintGlyph(QPainter &p, int button, bool maximized, const QRectF &r, double size) {
  const QPointF c = r.center();
  const double h = size / 2;
  QPen pen = p.pen();
  pen.setCapStyle(Qt::FlatCap);
  pen.setJoinStyle(Qt::MiterJoin);
  p.setPen(pen);
  p.setBrush(Qt::NoBrush);
  if (button == 0) {
    p.drawLine(QPointF(c.x() - h, c.y()), QPointF(c.x() + h, c.y()));
  } else if (button == 1 && !maximized) {
    p.drawRect(QRectF(c.x() - h + 0.8, c.y() - h + 0.8, size - 1.6, size - 1.6));
  } else if (button == 1) { /* restore: two squares, the back one showing at the top right */
    const double s = size * 0.72;
    p.drawRect(QRectF(c.x() - h + 0.6, c.y() + h - s - 0.6, s, s));
    QPainterPath back;
    back.moveTo(c.x() - h + size - s, c.y() + h - s - 0.6);
    back.lineTo(c.x() - h + size - s, c.y() - h + 0.6);
    back.lineTo(c.x() + h - 0.6, c.y() - h + 0.6);
    back.lineTo(c.x() + h - 0.6, c.y() + h - size + s);
    back.lineTo(c.x() - h + s + 0.6, c.y() + h - size + s);
    p.drawPath(back);
  } else {
    p.drawLine(QPointF(c.x() - h, c.y() - h), QPointF(c.x() + h, c.y() + h));
    p.drawLine(QPointF(c.x() + h, c.y() - h), QPointF(c.x() - h, c.y() + h));
  }
}

}  // namespace

bool useOwnTitleBar(QWidget *window) {
  const QString platform = QGuiApplication::platformName();
  if (platform == QLatin1String("offscreen") || platform == QLatin1String("minimal")) return false;
  if (desk() == Desk::Mac) {
#if defined(Q_OS_MACOS) && QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
    window->setWindowFlag(Qt::ExpandedClientAreaHint);
    window->setWindowFlag(Qt::NoTitleBarBackgroundHint);
    window->setAttribute(Qt::WA_ContentsMarginsRespectsSafeArea, false); /* the strip goes under the buttons */
    return true;
#else
    return false;
#endif
  }
  window->setWindowFlag(Qt::FramelessWindowHint);
  window->winId(); /* the native window, for the resize band */
  window->windowHandle()->installEventFilter(new EdgeResizer(window));
#ifdef Q_OS_WIN
  HWND hwnd = reinterpret_cast<HWND>(window->winId());
  /* minimize and maximize boxes: the taskbar button minimizes the window, Win+arrows work */
  SetWindowLongPtrW(hwnd, GWL_STYLE, GetWindowLongPtrW(hwnd, GWL_STYLE) | WS_MINIMIZEBOX | WS_MAXIMIZEBOX);
  /* the rounded corners of Windows 11 (ignored by Windows 10) */
  const DWORD round = 2; /* DWMWCP_ROUND */
  DwmSetWindowAttribute(hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &round, sizeof(round));
  workArea()->windows.insert(hwnd);
#endif
  return true;
}

/* ---------- TitleStrip ---------- */

TitleStrip::TitleStrip(QWidget *parent) : QWidget(parent) {
  setMouseTracking(true);
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  setFixedHeight(kHeight);
}

QSize TitleStrip::sizeHint() const { return {400, kHeight}; }

void TitleStrip::setPath(const QStringList &path) {
  if (path == path_) return;
  path_ = path;
  update();
}

void TitleStrip::setDevice(const QString &device) {
  if (device == device_) return;
  device_ = device;
  update();
}

QVector<TitleStrip::Slot> TitleStrip::places() const {
  QVector<Slot> out;
  if (desk() == Desk::Mac || !window()) return out;
  const bool canMaximize = !fixedSize(window());
  auto keep = [&](const QVector<int> &side) {
    QVector<Button> b;
    for (int i : side)
      if (i != Maximize || canMaximize) b << Button(i);
    return b;
  };
  if (desk() == Desk::Windows) { /* flat, the full height, 46 px wide as Windows' own */
    const QVector<Button> right = keep(buttons().right);
    double x = width() - 46.0 * right.size();
    for (Button b : right) {
      out.push_back({b, QRectF(x, 0, 46, height())});
      x += 46;
    }
    return out;
  }
  const double size = 24, gap = 4, y = (height() - size) / 2; /* Linux: small tiles */
  double x = y;
  for (Button b : keep(buttons().left)) {
    out.push_back({b, QRectF(x, y, size, size)});
    x += size + gap;
  }
  const QVector<Button> right = keep(buttons().right);
  x = width() - y - right.size() * size - (right.size() - 1) * gap;
  for (Button b : right) {
    out.push_back({b, QRectF(x, y, size, size)});
    x += size + gap;
  }
  return out;
}

int TitleStrip::slotAt(const QPointF &p) const {
  const QVector<Slot> s = places();
  for (int i = 0; i < s.size(); i++)
    if (s[i].rect.contains(p)) return i;
  return -1;
}

double TitleStrip::leftInset() const {
  if (desk() == Desk::Mac) return window() && window()->isFullScreen() ? 16 : 84; /* past the system's buttons */
  double x = 16;
  for (const Slot &s : places())
    if (s.rect.center().x() < width() / 2.0) x = std::max(x, s.rect.right() + 16);
  return x;
}

void TitleStrip::paintEvent(QPaintEvent *) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  const Ink k = ink();
  p.fillRect(rect(), theme().bg);

  /* the controls */
  const QVector<Slot> s = places();
  double rightEnd = width();
  const bool maximized = window() && window()->isMaximized();
  for (int i = 0; i < s.size(); i++) {
    const Slot &sl = s[i];
    const bool hot = i == hover_ && (down_ < 0 || down_ == i);
    QColor bg = desk() == Desk::Windows ? QColor(Qt::transparent) : k.fill;
    if (hot) bg = sl.button == Close ? kCloseHover : k.hover;
    if (bg.alpha()) p.fillRect(sl.rect, bg);
    p.setPen(QPen(hot ? QColor(Qt::white) : k.glyph, desk() == Desk::Windows ? 1.6 : 1.8));
    paintGlyph(p, sl.button, maximized, sl.rect, desk() == Desk::Windows ? 10 : 9);
    if (sl.rect.center().x() > width() / 2.0) rightEnd = std::min(rightEnd, sl.rect.left());
  }

  /* the device chip, by the controls */
  QFont label = labelFont(8);
  p.setFont(label);
  QFontMetricsF fm(label);
  const double left = leftInset();
  double textEnd = rightEnd - 16;
  if (!device_.isEmpty()) {
    const QString d = device_.toUpper();
    const double w = 10 + 6 + 8 + fm.horizontalAdvance(d) + 10;
    const double right = rightEnd - 8;
    if (right - w > left + 120) { /* only where it leaves room for the path */
      QRectF chip(right - w, (height() - 20) / 2.0, w, 20);
      p.setPen(QPen(k.line, 1));
      p.setBrush(Qt::NoBrush);
      p.drawRect(chip.adjusted(0.5, 0.5, -0.5, -0.5));
      p.fillRect(QRectF(chip.left() + 10, chip.center().y() - 3, 6, 6), theme().accent);
      p.setPen(k.glyph);
      p.drawText(QRectF(chip.left() + 24, chip.top(), w - 24, chip.height()), Qt::AlignVCenter | Qt::AlignLeft, d);
      textEnd = chip.left() - 16;
    }
  }

  /* where you are: the page brighter, then its parts */
  double x = left;
  for (int i = 0; i < path_.size() && x < textEnd; i++) {
    if (i) {
      p.setPen(k.glyph);
      p.drawText(QRectF(x, 0, textEnd - x, height()), Qt::AlignVCenter | Qt::AlignLeft, QStringLiteral("/"));
      x += fm.horizontalAdvance(QStringLiteral("/")) + 10;
    }
    const QString part = fm.elidedText(path_[i].toUpper(), Qt::ElideRight, std::max(0.0, textEnd - x));
    p.setPen(i == 0 ? k.strong : k.glyph);
    p.drawText(QRectF(x, 0, textEnd - x, height()), Qt::AlignVCenter | Qt::AlignLeft, part);
    x += fm.horizontalAdvance(part) + 10;
  }
}

void TitleStrip::mousePressEvent(QMouseEvent *e) {
  if (e->button() != Qt::LeftButton) return QWidget::mousePressEvent(e);
  down_ = slotAt(e->pos());
  if (down_ >= 0) {
    update();
    return;
  }
  if (QWindow *w = window()->windowHandle()) w->startSystemMove(); /* the rest of the strip moves the window */
}

void TitleStrip::mouseMoveEvent(QMouseEvent *e) {
  const int h = slotAt(e->pos());
  if (h != hover_) {
    hover_ = h;
    update();
  }
}

void TitleStrip::mouseReleaseEvent(QMouseEvent *e) {
  const int was = down_;
  down_ = -1;
  if (was >= 0 && slotAt(e->pos()) == was) press(places()[was].button);
  update();
}

void TitleStrip::mouseDoubleClickEvent(QMouseEvent *e) {
  if (e->button() != Qt::LeftButton || slotAt(e->pos()) >= 0) return;
#ifdef Q_OS_MACOS
  if (desk() != Desk::Mac) return toggleMaximized();
  /* what System Settings › Desktop & Dock says a double-click on a title bar does: zoom, minimize or nothing */
  QString action = QStringLiteral("Maximize");
  if (CFPropertyListRef v = CFPreferencesCopyAppValue(CFSTR("AppleActionOnDoubleClick"), kCFPreferencesAnyApplication)) {
    if (CFGetTypeID(v) == CFStringGetTypeID()) {
      char buf[32];
      if (CFStringGetCString(static_cast<CFStringRef>(v), buf, sizeof buf, kCFStringEncodingUTF8)) action = QString::fromUtf8(buf);
    }
    CFRelease(v);
  }
  if (action == QLatin1String("Minimize")) window()->showMinimized();
  else if (action != QLatin1String("None")) toggleMaximized();
#else
  toggleMaximized();
#endif
}

void TitleStrip::leaveEvent(QEvent *) {
  if (hover_ < 0) return;
  hover_ = -1;
  update();
}

void TitleStrip::showEvent(QShowEvent *e) {
  QWidget::showEvent(e);
#ifdef Q_OS_MACOS
  /* the window's title stays in the Dock and the Window menu, not over the strip: titleVisibility = hidden */
  id view = desk() == Desk::Mac ? reinterpret_cast<id>(window()->winId()) : nullptr;
  if (view) {
    auto send = reinterpret_cast<id (*)(id, SEL)>(objc_msgSend);
    if (id nsWindow = send(view, sel_registerName("window")))
      reinterpret_cast<void (*)(id, SEL, long)>(objc_msgSend)(nsWindow, sel_registerName("setTitleVisibility:"), 1);
  }
#endif
  if (!watching_ && window() != this) { /* the restore glyph and macOS's full screen follow the window */
    window()->installEventFilter(this);
    watching_ = true;
  }
}

bool TitleStrip::eventFilter(QObject *o, QEvent *e) {
  if (o == window() && e->type() == QEvent::WindowStateChange) {
    hover_ = down_ = -1;
    update();
  }
  return QWidget::eventFilter(o, e);
}

void TitleStrip::press(Button b) {
  QWidget *w = window();
  if (b == Minimize) w->showMinimized();
  else if (b == Maximize) toggleMaximized();
  else w->close();
}

void TitleStrip::toggleMaximized() {
  QWidget *w = window();
  if (fixedSize(w)) return;
  if (w->isMaximized()) w->showNormal();
  else w->showMaximized();
}
