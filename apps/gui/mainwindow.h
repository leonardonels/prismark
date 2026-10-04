/*
 * The Prismark desktop app. Header with the test groups and Run all; test
 * cards on the left (each with its own Run button); the ranking of the
 * selected test in the middle; details of the selected result on the right.
 *
 * Runs execute in the bundled `prismark` runner as a separate process, never
 * in the GUI process, and measure the computer as it is configured: the app
 * never changes power settings. While one is in progress the window
 * shows a static page that changes only when a new phase begins (spec 7.1),
 * and keyboard or mouse activity is reported to the runner, which skips the
 * tests that need an idle machine.
 *
 * Copyright 2026 The Prismark Authors. Apache-2.0.
 */
#pragma once

#include <QButtonGroup>
#include <QJsonObject>
#include <QMainWindow>
#include <QProcess>

#include "metrics.h"

class QLabel;
class QPushButton;
class QStackedWidget;
class QVBoxLayout;
class InputWatch;
class RankingView;
class Segmented;
class StepRing;
class TestCard;
class Toggle;

class MainWindow : public QMainWindow {
  Q_OBJECT
 public:
  MainWindow();
  ~MainWindow() override;
  /* Starts a run as if its Run button were pressed; empty id: all tests. */
  void startRun(const QString &metricId, bool quick) { runTest(metricId, quick); }

 private slots:
  void runTest(const QString &metricId, bool quick);
  void cancelRun();
  void onRunOutput();
  void onRunFinished(int code, QProcess::ExitStatus status);
  void onInputActivity();
  void openResult();
  void compareRuns();
  void prepareCompileTests();
  void chooseSnapshot();
  void showGlossary();
  void showDataFolders();
  void showAbout();
  void removeSelected();

 private:
  void buildUi();
  QWidget *buildMeasuringWindow();
  QWidget *buildMenuButton();
  void loadInfo();
  void loadAll();
  bool loadFile(const QString &path, bool reference);
  void refresh();
  void refreshTests();
  void refreshRanking();
  void refreshDetails();
  void refreshContext(); /* the chart or comparison under the ranking, for the highlighted result */
  void selectMetric(const QString &id);
  void refreshRunLabels(); /* Run buttons say whether they start a quick or a full run */
  QString cliPath() const;
  QString resultsDir() const;
  QString referencesDir() const;
  const RunSummary *selectedRun() const;
  /* Pre-run checks; false when the user cancelled. */
  bool confirmCompileTests(bool needK1, bool needK1x, bool all, QStringList &args);

  QVector<RunSummary> runs_;
  QJsonObject info_; /* `prismark info` */
  QString group_ = "mc", metric_ = "mc_k2", selected_;

  Segmented *groups_ = nullptr;
  Toggle *quick_ = nullptr, *allRuns_ = nullptr;
  QPushButton *runAll_ = nullptr;
  QLabel *banner_ = nullptr, *testsTitle_ = nullptr, *groupDesc_ = nullptr, *title_ = nullptr, *desc_ = nullptr,
         *how_ = nullptr, *read_ = nullptr;
  QWidget *context_ = nullptr;
  QVBoxLayout *contextLayout_ = nullptr;
  bool techOpen_ = false; /* the report's technical details are expanded */
  QVBoxLayout *cardsLayout_ = nullptr;
  QButtonGroup cards_;
  RankingView *ranking_ = nullptr;
  QWidget *details_ = nullptr;
  QVBoxLayout *detailsLayout_ = nullptr;

  /* measuring window: the only window shown while a run is in progress */
  QWidget *measure_ = nullptr;
  StepRing *ring_ = nullptr;
  QLabel *fzPhase_ = nullptr, *fzMsg_ = nullptr, *fzHands_ = nullptr, *fzStatus_ = nullptr, *fzWarn_ = nullptr;
  QProcess *proc_ = nullptr;
  InputWatch *input_ = nullptr;
  QByteArray procBuf_;
  QString procOut_, procErrTail_, phase_;
  bool inputDuringQuiet_ = false;
  int warnings_ = 0;
};
