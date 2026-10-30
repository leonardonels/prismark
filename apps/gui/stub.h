/*
 * The app's dialogs, as the design draws them: a stub torn off the pass. A coloured strip with a code (ERR /
 * RUNNER, RUN / CANCEL, SYS / POWER), a heading, one sentence, what else the question needs (a code block, the
 * run's progress, a checkbox), and below a dashed rule its buttons side by side. The same in light and dark.
 *
 * Copyright 2026 The Prismark Authors. Apache-2.0.
 */
#pragma once

#include <QDialog>
#include <QPoint>

class QAbstractButton;
class QHBoxLayout;
class QLabel;
class QPushButton;
class QVBoxLayout;

class Stub : public QDialog {
  Q_OBJECT
 public:
  /* The strip: red for errors, blue for the run, volt for the system, the app and your results. */
  enum Kind { Error, Run, System };
  /* Primary: volt, the button Enter presses. Secondary: a quiet panel. Outline: the one that ends something. */
  enum Role { Primary, Secondary, Outline };

  Stub(QWidget *parent, Kind kind, const QString &code, const QString &title, const QString &sentence = QString());
  void setTag(const QString &tag);     /* the strip's right-hand side: "Part 07 of 12", "optional" */
  void setCode(const QString &mono);   /* a block in mono: an exit status, a path */
  void setDetails(const QString &mono); /* a block shown by a Show details button */
  void setStrip(double done, double now, double left); /* the run so far: done, the current part, still to come */
  QAbstractButton *addCheck(const QString &text);
  QVBoxLayout *body() const { return body_; } /* for what else a dialog needs, under the sentence */
  void setWide(int width);             /* wider than the usual 460 */
  QPushButton *addButton(const QString &text, Role role);
  /* the button that closed it; null when closed with Escape */
  QAbstractButton *clicked() const { return clicked_; }

  /* A message with OK. */
  static void tell(QWidget *parent, Kind kind, const QString &code, const QString &title, const QString &sentence,
                   const QString &mono = QString());
  /* A question; true for yes. destructive: yes ends something, so no is the volt button and Enter's. */
  static bool ask(QWidget *parent, Kind kind, const QString &code, const QString &title, const QString &sentence,
                  const QString &yes, const QString &no, bool destructive = false);

 protected:
  void showEvent(QShowEvent *e) override;
  void mousePressEvent(QMouseEvent *e) override;
  void mouseMoveEvent(QMouseEvent *e) override;

 private:
  void fit();
  QWidget *head_ = nullptr;
  QVBoxLayout *body_ = nullptr;
  QHBoxLayout *buttons_ = nullptr;
  QAbstractButton *clicked_ = nullptr;
  QPoint drag_;
};
