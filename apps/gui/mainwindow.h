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
#include <QDateTime>
#include <QJsonObject>
#include <QMainWindow>
#include <QProcess>

#include "metrics.h"

class QLabel;
class QPushButton;
class QStackedWidget;
class QTimer;
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
  void saveAsReference(); /* the selected run, as the user's own reference system */
  void removeReference(); /* the selected reference, when it is one of the user's own */

 private:
  void buildUi();
  QWidget *buildMeasuringWindow();
  QWidget *buildMenuButton();
  void loadInfo();
  void moveOldDataFolders();
  void loadAll();
  bool loadFile(const QString &path, bool reference);
  void refresh();
  void refreshTests();
  void refreshRanking();
  void refreshDetails();
  void refreshContext(); /* the chart or comparison under the ranking, for the highlighted result */
  void selectMetric(const QString &id);
  /* Simple shows only the tests isSimple() picks; Advanced shows every test. */
  bool visible(const Metric &m) const { return advanced_ || isSimple(m.id); }
  QString firstVisible(const QString &group) const;
  QVector<QString> visibleGroups() const; /* tabs with at least one visible test */
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
  bool advanced_ = false;
  Segmented *level_ = nullptr;
  QLabel *moreTests_ = nullptr;
  QVBoxLayout *cardsLayout_ = nullptr;
  QButtonGroup cards_;
  RankingView *ranking_ = nullptr;
  QWidget *details_ = nullptr;
  QVBoxLayout *detailsLayout_ = nullptr;

  /* measuring window: the only window shown while a run is in progress */
  QWidget *measure_ = nullptr;
  StepRing *ring_ = nullptr;
  QLabel *fzState_ = nullptr, *fzPhase_ = nullptr, *fzMsg_ = nullptr, *fzTime_ = nullptr, *fzHands_ = nullptr,
         *fzStatus_ = nullptr, *fzWarn_ = nullptr, *fzStall_ = nullptr;
  QProcess *proc_ = nullptr;
  InputWatch *input_ = nullptr;
  QByteArray procBuf_;
  QString procOut_, procErrTail_, phase_, kernel_;
  bool inputDuringQuiet_ = false, cancelling_ = false;
  int warnings_ = 0;
  int part_ = 0, parts_ = 0; /* overall position: the runner's "run" events */
  int runStep_ = 0, runSteps_ = 0; /* steps of the whole run begun and planned: its "progress" events */
  QDateTime runStart_, phaseStart_;
  /* Single-shot, restarted by every runner event: fires only when the runner has been silent too long, so the
     window stays still while a run is healthy. killTimer_ ends a cancelled run that stopped answering. */
  QTimer *stallTimer_ = nullptr, *killTimer_ = nullptr;
  void onRunEvent(); /* any event arrived: the runner is alive */
  void onStall();
  void showPhase(int step, int steps);
};
