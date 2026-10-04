/*
 * What the compile tests (K1, K1x) need, and the one-time preparation of the
 * K1/K1x snapshot.
 *
 * Copyright 2026 The Prismark Authors. Apache-2.0.
 */
#pragma once

#include <QDialog>
#include <QJsonObject>
#include <QProcess>
#include <QStringList>

class QLabel;
class QPlainTextEdit;
class QPushButton;
class StepRing;

struct CompileStatus {
  bool k1Built = false;        /* the runner was built with in-process Clang */
  QString snapshot;            /* prepared snapshot directory, or empty */
  QStringList missingPrepare;  /* tools missing to prepare a snapshot */
  QStringList missingBuild;    /* tools missing to run K1x */
  bool canRunK1() const { return k1Built && !snapshot.isEmpty(); }
  bool canRunK1x() const { return !snapshot.isEmpty() && missingBuild.isEmpty(); }
};

QString dataDir();                 /* ~/.local/share/prismark and platform equivalents */
QString defaultSnapshotDir();
QString prepareScript();           /* tools/k1x/prepare.py, installed or in the source tree; empty if absent */
CompileStatus compileStatus(const QJsonObject &info, const QString &configuredSnapshot);
/* Commands that install what is missing, for this platform. */
QString installInstructions(const CompileStatus &s);

/* Runs prepare.py with progress; accepted when the snapshot is ready. */
class PrepareDialog : public QDialog {
  Q_OBJECT
 public:
  explicit PrepareDialog(QWidget *parent = nullptr);
  ~PrepareDialog() override;
  int exec() override;

 private:
  void onOutput();
  void onFinished(int code);
  QProcess proc_;
  QByteArray buf_;
  StepRing *ring_ = nullptr;
  QLabel *step_ = nullptr;
  QPlainTextEdit *log_ = nullptr;
  QPushButton *button_ = nullptr;
  bool done_ = false;
};
