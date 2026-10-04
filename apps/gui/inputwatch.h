/*
 * Watches for keyboard and mouse activity in the desktop session while a
 * test that needs an idle machine runs, so the runner can skip it.
 *
 * Linux (GNOME, X11 or Wayland): Mutter's IdleMonitor user-active watch over
 * D-Bus. It is event-driven, so watching costs no wake-ups. Elsewhere on
 * Linux: the freedesktop ScreenSaver idle time, polled every 2 s. Windows and
 * macOS need nothing here: the runner reads the last-input time itself.
 *
 * Copyright 2026 The Prismark Authors. Apache-2.0.
 */
#pragma once

#include <QObject>
#include <QString>

class QTimer;

class InputWatch : public QObject {
  Q_OBJECT
 public:
  explicit InputWatch(QObject *parent = nullptr);
  ~InputWatch() override;
  /* Starts watching; returns the method name recorded with results, or empty if none is available. */
  QString start();
  void stop();

 signals:
  void activity();

 private slots:
  void onWatchFired(uint id);
  void poll();

 private:
  bool addWatch();
  uint watch_ = 0;
  bool active_ = false;
  QTimer *pollTimer_ = nullptr;
  qint64 lastIdle_ = -1;
};
