/* Copyright 2026 The Prismark Authors. Apache-2.0. */
#include "inputwatch.h"

#include <QTimer>

#ifdef PMK_HAVE_DBUS
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>

namespace {
const char *kMutter = "org.gnome.Mutter.IdleMonitor", *kMutterPath = "/org/gnome/Mutter/IdleMonitor/Core";
const char *kSaver = "org.freedesktop.ScreenSaver", *kSaverPath = "/org/freedesktop/ScreenSaver";
}  // namespace
#endif

InputWatch::InputWatch(QObject *parent) : QObject(parent) {}

InputWatch::~InputWatch() { stop(); }

bool InputWatch::addWatch() {
#ifdef PMK_HAVE_DBUS
  QDBusInterface mon(kMutter, kMutterPath, kMutter, QDBusConnection::sessionBus());
  if (!mon.isValid()) return false;
  QDBusReply<uint> id = mon.call("AddUserActiveWatch");
  if (!id.isValid()) return false;
  watch_ = id.value();
  return true;
#else
  return false;
#endif
}

QString InputWatch::start() {
  stop();
  active_ = true;
#ifdef PMK_HAVE_DBUS
  auto bus = QDBusConnection::sessionBus();
  if (bus.connect(kMutter, kMutterPath, kMutter, "WatchFired", this, SLOT(onWatchFired(uint))) && addWatch())
    return QStringLiteral("gnome-idle-monitor");
  bus.disconnect(kMutter, kMutterPath, kMutter, "WatchFired", this, SLOT(onWatchFired(uint)));
  QDBusInterface saver(kSaver, kSaverPath, kSaver, bus);
  QDBusReply<uint> idle = saver.isValid() ? saver.call("GetSessionIdleTime") : QDBusReply<uint>();
  if (idle.isValid()) {
    lastIdle_ = idle.value();
    pollTimer_ = new QTimer(this);
    connect(pollTimer_, &QTimer::timeout, this, &InputWatch::poll);
    pollTimer_->start(2000);
    return QStringLiteral("screensaver-idle-poll-2s");
  }
#endif
  active_ = false;
  return QString();
}

void InputWatch::stop() {
  active_ = false;
#ifdef PMK_HAVE_DBUS
  if (watch_) {
    QDBusInterface mon(kMutter, kMutterPath, kMutter, QDBusConnection::sessionBus());
    if (mon.isValid()) mon.call(QDBus::NoBlock, "RemoveWatch", watch_);
    watch_ = 0;
  }
  QDBusConnection::sessionBus().disconnect(kMutter, kMutterPath, kMutter, "WatchFired", this, SLOT(onWatchFired(uint)));
#endif
  delete pollTimer_;
  pollTimer_ = nullptr;
}

void InputWatch::onWatchFired(uint id) {
  if (!active_ || id != watch_) return;
  emit activity();
  /* A user-active watch fires once; re-arm it shortly, so continued activity is reported too. */
  watch_ = 0;
  QTimer::singleShot(1000, this, [this] {
    if (active_) addWatch();
  });
}

void InputWatch::poll() {
#ifdef PMK_HAVE_DBUS
  QDBusInterface saver(kSaver, kSaverPath, kSaver, QDBusConnection::sessionBus());
  QDBusReply<uint> idle = saver.call("GetSessionIdleTime");
  if (!idle.isValid()) return;
  qint64 now = idle.value();
  if (lastIdle_ >= 0 && now < lastIdle_) emit activity(); /* the idle time restarted: there was input */
  lastIdle_ = now;
#endif
}
