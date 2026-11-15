/*
 * The window's own title bar: a strip of the ink ground across the top of the
 * window, with where you are in the app on the left, the device on the right
 * and the window controls of the desktop it runs on. macOS keeps its own red,
 * yellow and green buttons on the strip; Windows gets the app's minimize,
 * maximize and close; GNOME and other Linux desktops get the buttons their
 * window manager is set to show, in its order.
 *
 * Copyright 2026 The Prismark Authors. Apache-2.0.
 */
#pragma once

#include <QStringList>
#include <QVector>
#include <QWidget>

/* Takes the system's title bar off a top-level window before it is first shown. Windows and Linux: the window
   becomes frameless, and is moved from the strip and resized from its outer 6 px here. macOS (Qt 6.9 and later):
   the content runs up under the system's buttons. Returns false where the system's title bar stays (macOS with an
   older Qt, or a platform with no window manager such as offscreen): then no strip should be added. */
bool useOwnTitleBar(QWidget *window);

class TitleStrip : public QWidget {
  Q_OBJECT
 public:
  explicit TitleStrip(QWidget *parent = nullptr);
  /* Where you are, from the page down: the first part is drawn brighter. */
  void setPath(const QStringList &path);
  /* The processor and the system, in the chip by the controls; empty hides the chip. */
  void setDevice(const QString &device);
  QSize sizeHint() const override;
  QSize minimumSizeHint() const override { return sizeHint(); }

 protected:
  void paintEvent(QPaintEvent *) override;
  void mousePressEvent(QMouseEvent *e) override;
  void mouseMoveEvent(QMouseEvent *e) override;
  void mouseReleaseEvent(QMouseEvent *e) override;
  void mouseDoubleClickEvent(QMouseEvent *e) override;
  void leaveEvent(QEvent *) override;
  void showEvent(QShowEvent *e) override;
  bool eventFilter(QObject *o, QEvent *e) override;

 private:
  enum Button { Minimize, Maximize, Close };
  struct Slot {
    Button button;
    QRectF rect;
  };
  QVector<Slot> places() const;
  int slotAt(const QPointF &p) const;
  void press(Button b);
  void toggleMaximized();
  double leftInset() const; /* past macOS's own buttons, or the buttons placed on the left */
  QStringList path_;
  QString device_;
  int hover_ = -1, down_ = -1;
  bool watching_ = false;
};
