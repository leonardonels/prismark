/*
 * Painted widgets. Everything paints only when Qt asks (no timers, no
 * animation), so an idle window costs nothing and never wakes a core during
 * a measurement.
 *
 * Copyright 2026 The Prismark Authors. Apache-2.0.
 */
#pragma once

#include <array>

#include <QAbstractButton>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

#include "metrics.h"

class QPainter;

/* The Prismark logo (apps/gui/icons/prismark.svg), drawn from vectors into the largest square that fits r. */
void paintLogo(QPainter &p, const QRectF &r);

struct RankRow {
  QString runId, name, detail;
  QStringList tags; /* "placeholder", "quick", "partial", "latest", "reference", "did not settle" */
  bool yours = false;
  Value value;
};

class RankingView : public QWidget {
  Q_OBJECT
 public:
  explicit RankingView(QWidget *parent = nullptr);
  void setRows(QVector<RankRow> rows, bool higherBetter, const QString &unit, const QString &selected);
  QSize sizeHint() const override;
  QSize minimumSizeHint() const override;

 signals:
  void runSelected(const QString &id);

 protected:
  void paintEvent(QPaintEvent *) override;
  void mousePressEvent(QMouseEvent *) override;
  void mouseMoveEvent(QMouseEvent *) override;
  void leaveEvent(QEvent *) override;
  void keyPressEvent(QKeyEvent *) override;

 private:
  double speed(double v) const;
  int rowAt(int y) const;
  QVector<RankRow> rows_;
  bool higher_ = true;
  QString unit_, selected_;
  int hover_ = -1;
};

struct ChartLine {
  QString name;
  QVector<std::array<double, 4>> pts; /* x, value, lo, hi */
};

class ChartView : public QWidget {
  Q_OBJECT
 public:
  explicit ChartView(QWidget *parent = nullptr);
  void setData(QVector<ChartLine> lines, bool logX, const QString &xLabel, double yMin, double yMax, bool ideal);
  QSize sizeHint() const override { return {320, 200}; }
  bool hasHeightForWidth() const override { return true; }
  int heightForWidth(int w) const override { return std::max(170, w * 9 / 16); }

 protected:
  void paintEvent(QPaintEvent *) override;

 private:
  QVector<ChartLine> lines_;
  bool logX_ = false, ideal_ = false;
  QString xLabel_;
  double yMin_ = NAN, yMax_ = NAN;
};

/* One test in the left column: name, what it measures, your latest score, and a Run button. */
class TestCard : public QAbstractButton {
  Q_OBJECT
 public:
  TestCard(const QString &id, QWidget *parent = nullptr);
  void setContent(const QString &name, const QString &sub, const QString &score, const QString &unit,
                  const QString &note);
  void setRunLabel(const QString &label); /* "Run quick" or "Run full" */
  QString id() const { return id_; }
  QSize sizeHint() const override { return {300, 92}; }

 signals:
  void runRequested(const QString &id);

 protected:
  void paintEvent(QPaintEvent *) override;
  void mousePressEvent(QMouseEvent *) override;
  void mouseMoveEvent(QMouseEvent *) override;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
  void enterEvent(QEnterEvent *) override;
#else
  void enterEvent(QEvent *) override;
#endif
  void leaveEvent(QEvent *) override;

 private:
  QRectF runRect() const;
  QString id_, name_, sub_, score_, unit_, note_, runLabel_ = tr("Run");
  bool hover_ = false, hoverRun_ = false;
};

/* Pill-shaped segmented control. */
class Segmented : public QWidget {
  Q_OBJECT
 public:
  explicit Segmented(QWidget *parent = nullptr);
  void setItems(const QStringList &items);
  void setCurrent(int i);
  int current() const { return cur_; }
  QSize sizeHint() const override;

 signals:
  void changed(int i);

 protected:
  void paintEvent(QPaintEvent *) override;
  void mousePressEvent(QMouseEvent *) override;

 private:
  QVector<QRectF> rects() const;
  QStringList items_;
  int cur_ = 0;
};

/* A switch with a label. */
class Toggle : public QAbstractButton {
  Q_OBJECT
 public:
  Toggle(const QString &text, QWidget *parent = nullptr);
  QSize sizeHint() const override;

 protected:
  void paintEvent(QPaintEvent *) override;
};

/* Static step indicator for the measuring page: an arc for step/steps, redrawn only when set. */
class StepRing : public QWidget {
  Q_OBJECT
 public:
  explicit StepRing(QWidget *parent = nullptr);
  void setProgress(int step, int steps, const QString &centre);
  QSize sizeHint() const override { return {170, 170}; }

 protected:
  void paintEvent(QPaintEvent *) override;

 private:
  int step_ = 0, steps_ = 0;
  QString centre_;
};

QColor seriesColor(int i);
