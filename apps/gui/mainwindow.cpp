/* Copyright 2026 The Prismark Authors. Apache-2.0. */
#include "mainwindow.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLineEdit>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSettings>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

#include "inputwatch.h"
#include "setup.h"
#include "theme.h"
#include "widgets.h"

namespace {

QString esc(const QString &s) { return s.toHtmlEscaped(); }

QLabel *text(const QString &s, double pt, int weight = QFont::Normal, const QColor &color = QColor()) {
  auto *l = new QLabel(s);
  l->setFont(uiFont(pt, weight));
  l->setWordWrap(true);
  l->setTextFormat(Qt::RichText);
  if (color.isValid()) l->setStyleSheet(QStringLiteral("color:%1").arg(color.name()));
  return l;
}

QLabel *sectionLabel(const QString &s) {
  auto *l = text(s.toUpper(), 8.5, QFont::DemiBold, theme().faint);
  l->setContentsMargins(0, 14, 0, 4);
  return l;
}

/* A rounded pill, e.g. "quick" or "✓ pinning". */
QLabel *pill(const QString &s, const QColor &fg, const QColor &bg) {
  auto *l = new QLabel(s);
  l->setFont(uiFont(8.5, QFont::Medium));
  l->setStyleSheet(QStringLiteral("color:%1; background:%2; border-radius:9px; padding:3px 9px;").arg(fg.name(), bg.name()));
  return l;
}

QWidget *pillRow(const QVector<QLabel *> &pills) {
  auto *w = new QWidget;
  auto *h = new QHBoxLayout(w);
  h->setContentsMargins(0, 0, 0, 0);
  h->setSpacing(6);
  for (QLabel *p : pills) h->addWidget(p);
  h->addStretch();
  return w;
}

QFrame *card() {
  auto *f = new QFrame;
  f->setObjectName("card");
  return f;
}

class Logo : public QWidget {
 public:
  Logo() { setFixedSize(42, 42); }

 protected:
  void paintEvent(QPaintEvent *) override {
    QPainter p(this);
    paintLogo(p, QRectF(rect()).adjusted(2, 2, -2, -2));
  }
};

/* Menu icon (three lines), painted in the theme colour. */
QIcon gearIcon() {
  QPixmap pm(44, 44);
  pm.fill(Qt::transparent);
  QPainter p(&pm);
  p.setRenderHint(QPainter::Antialiasing);
  p.setPen(QPen(theme().text, 3.6, Qt::SolidLine, Qt::RoundCap));
  for (int y : {12, 22, 32}) p.drawLine(QPointF(9, y), QPointF(35, y));
  return QIcon(pm);
}

/* The small window shown alone during a run; closing it asks to cancel. */
class MeasureWindow : public QWidget {
 public:
  std::function<bool()> onClose; /* returns true when the window may close */

 protected:
  void closeEvent(QCloseEvent *e) override {
    if (onClose && !onClose()) e->ignore();
    else e->accept();
  }
};

const QStringList kQuietPhases = {"cold_burst", "periodic"};

QString phaseTitle(const QString &phase) {
  static const QMap<QString, QString> names = {
      {"preflight", "Checking the machine is idle"}, {"st_burst", "One core, short tasks"},
      {"st_sustained", "One core, after warming up"}, {"mc_threaded", "All cores, working together"},
      {"mc_instances", "All cores, each on its own"}, {"cold_burst", "Started from rest"},
      {"periodic", "Timer punctuality"},              {"cooldown", "Cooling down"},
      {"stress", "Stress test"},
      {"done", "Computing the results"}};
  return names.value(phase, phase);
}

/* How long a step of this phase normally takes, so a long silence is not mistaken for a hang. */
QString phaseHint(const QString &phase, const QString &kernel) {
  if (kernel == "K1x") return QObject::tr("Each build takes a few minutes.");
  static const QMap<QString, QString> hints = {
      {"preflight", "Usually under a minute."},
      {"st_burst", "Each step takes seconds to a minute."},
      {"st_sustained", "A one-minute warm-up, then about 25 seconds per step (compiling: about a minute)."},
      {"mc_threaded", "A one-minute warm-up, then about 25 seconds per step (compiling: about a minute)."},
      {"mc_instances", "Each step takes up to a few minutes."},
      {"cold_burst", "Each step takes up to a few minutes."},
      {"periodic", "Each step takes about a minute."},
      {"stress", "Each part runs for the length you chose; the score comes from the 10th minute."},
      {"cooldown", "Waits for the CPU to cool down: up to 2 minutes."},
      {"done", "Measuring is finished; usually a few seconds."}};
  return QObject::tr(qPrintable(hints.value(phase)));
}

/*
 * One slim chart of a stack sharing the x axis (only the last one names it): one quantity, one or more lines of
 * the same unit. Its axis is fitted to the values (never below 0) in four grid steps of a tidy size (1, 2, 2.5 or
 * 5 x a power of ten), so a change of a few percent shows.
 */
void addStrip(QVBoxLayout *to, const QVector<ChartLine> &lines, const QString &unit, const QString &xLabel) {
  double lo = INFINITY, hi = -INFINITY;
  for (const ChartLine &l : lines)
    for (const auto &pt : l.pts)
      if (std::isfinite(pt[1])) lo = std::min(lo, pt[1]), hi = std::max(hi, pt[1]);
  if (!std::isfinite(lo)) return;
  double span = std::max(hi - lo, std::abs(hi) * 0.05), raw = span * 1.6 / 4, mag = std::pow(10, std::floor(std::log10(raw)));
  double step = mag * (raw <= mag ? 1 : raw <= 2 * mag ? 2 : raw <= 2.5 * mag ? 2.5 : raw <= 5 * mag ? 5 : 10);
  double y0 = std::max(0.0, std::floor((lo - span * 0.3) / step) * step);
  while (y0 + 4 * step < hi) y0 += step;
  auto *ch = new ChartView;
  ch->setData(lines, false, xLabel, y0, y0 + 4 * step, false);
  ch->setYUnit(unit);
  ch->setAspect(0.3, xLabel.isEmpty() ? 150 : 170);
  to->addWidget(ch);
}

/* No event for this long: something is probably wrong. Above the longest silent step of each phase. */
int stallMinutes(const QString &phase, const QString &kernel) {
  if (kernel == "K1x") return 20;
  return 8;
}

/* The runner found an energy meter (package power) that only administrators may read. */
bool powerNeedsAdmin(const QJsonObject &info) {
  return info["machine"].toObject()["capabilities"].toObject()["power_note"].toString() == "emi-needs-admin";
}

/* Starts this app again with administrator rights (Windows asks the user first); true if it started. */
bool restartAsAdmin() {
#ifdef Q_OS_WIN
  std::wstring exe = QDir::toNativeSeparators(QCoreApplication::applicationFilePath()).toStdWString();
  SHELLEXECUTEINFOW sei = {};
  sei.cbSize = sizeof sei;
  sei.lpVerb = L"runas";
  sei.lpFile = exe.c_str();
  sei.nShow = SW_SHOWNORMAL;
  return ShellExecuteExW(&sei); /* false also when the user declines */
#else
  return false;
#endif
}

}  // namespace

MainWindow::MainWindow() {
  QSettings s;
  applyTheme(s.value("dark", true).toBool());
  if (const Metric *m = findMetric(s.value("metric").toString())) {
    metric_ = m->id;
    group_ = m->group;
  }
  advanced_ = s.value("advanced", false).toBool();
  input_ = new InputWatch(this);
  connect(input_, &InputWatch::activity, this, &MainWindow::onInputActivity);
  buildUi();
  loadInfo();
  moveOldDataFolders();
  loadAll();
  for (auto it = runs_.rbegin(); it != runs_.rend(); ++it)
    if (!it->reference) {
      selected_ = it->id;
      break;
    }
  refresh();
  resize(1440, 900);
  setMinimumSize(1180, 720);
  setWindowTitle(QStringLiteral("Prismark"));
}

MainWindow::~MainWindow() {
  if (proc_ && proc_->state() != QProcess::NotRunning) {
    proc_->write("cancel\n");
    proc_->closeWriteChannel();
    if (!proc_->waitForFinished(8000)) proc_->kill();
  }
  delete measure_;
}

/* ---------- layout ---------- */

QWidget *MainWindow::buildMenuButton() {
  auto *b = new QToolButton;
  b->setObjectName("iconButton");
  b->setIcon(gearIcon());
  b->setIconSize(QSize(22, 22));
  b->setToolTip(tr("Menu"));
  b->setPopupMode(QToolButton::InstantPopup);
  auto *m = new QMenu(b);
  m->addAction(tr("Open result…"), this, &MainWindow::openResult)->setShortcut(QKeySequence::Open);
  m->addAction(tr("Compare two results…"), this, &MainWindow::compareRuns);
  m->addSeparator();
  m->addAction(tr("Prepare compile tests…"), this, &MainWindow::prepareCompileTests);
  m->addAction(tr("Use another compile-test snapshot…"), this, &MainWindow::chooseSnapshot);
  QAction *admin = m->addAction(tr("Restart as administrator to measure power"), this, [this] {
    if (!proc_ && restartAsAdmin()) qApp->quit();
  });
  connect(m, &QMenu::aboutToShow, admin, [this, admin] { admin->setVisible(powerNeedsAdmin(info_)); });
  m->addSeparator();
  QAction *dark = m->addAction(tr("Dark theme"));
  dark->setCheckable(true);
  dark->setChecked(theme().dark);
  connect(dark, &QAction::toggled, this, [this, b](bool on) {
    QSettings().setValue("dark", on);
    applyTheme(on);
    b->setIcon(gearIcon());
    measure_->setStyleSheet(QStringLiteral("#measureWindow { background:%1; }").arg(theme().card.name()));
    for (QWidget *w : QApplication::allWidgets()) w->update();
    refresh();
  });
  m->addAction(tr("What do these terms mean?"), this, &MainWindow::showGlossary)->setShortcut(QKeySequence::HelpContents);
  m->addAction(tr("Data folders…"), this, &MainWindow::showDataFolders);
  m->addAction(tr("About Prismark"), this, &MainWindow::showAbout);
  m->addSeparator();
  m->addAction(tr("Quit"), qApp, &QApplication::quit)->setShortcut(QKeySequence::Quit);
  b->setMenu(m);
  return b;
}

void MainWindow::buildUi() {
  auto *page = new QWidget;
  setCentralWidget(page);
  auto *outer = new QVBoxLayout(page);
  outer->setContentsMargins(24, 18, 24, 20);
  outer->setSpacing(14);

  /* header */
  auto *head = new QHBoxLayout;
  head->setSpacing(14);
  head->addWidget(new Logo);
  auto *brand = new QVBoxLayout;
  brand->setSpacing(0);
  brand->addWidget(text(QStringLiteral("Prismark"), 18, QFont::Bold));
  QLabel *sub = text(tr("Each test is ranked on its own · there is no overall score"), 9, QFont::Normal, theme().muted);
  sub->setWordWrap(false);
  brand->addWidget(sub);
  head->addLayout(brand);
  head->addStretch();
  groups_ = new Segmented; /* items set in refresh(): Simple shows fewer tabs */
  connect(groups_, &Segmented::changed, this, [this](int i) {
    QVector<QString> shown = visibleGroups();
    shown.removeAll(QStringLiteral("stress"));
    if (i < 0 || i >= shown.size()) return;
    group_ = shown[i];
    metric_ = firstVisible(group_);
    refresh();
  });
  head->addWidget(groups_);
  /* The stress test in a bubble of its own: minutes of load, run on its own, not one more benchmark tab. */
  stressTab_ = new Segmented;
  stressTab_->setItems({tr("Stress test")});
  connect(stressTab_, &Segmented::changed, this, [this] {
    group_ = QStringLiteral("stress");
    metric_ = firstVisible(group_);
    refresh();
  });
  head->addWidget(stressTab_);
  head->addStretch();
  quick_ = new Toggle(tr("Quick"));
  quick_->setToolTip(tr("On: quick runs, a few minutes, for a first look. Their numbers are rough, so they are not "
                        "ranked: each test page shows your latest one under its ranking.\n"
                        "Off: full runs, accurate and ranked; all tests take about 30 minutes on a laptop."));
  quick_->setChecked(QSettings().value("quick", false).toBool());
  QSizePolicy keep = quick_->sizePolicy(); /* hidden on the stress tab without moving the tabs */
  keep.setRetainSizeWhenHidden(true);
  quick_->setSizePolicy(keep);
  connect(quick_, &Toggle::toggled, this, [this](bool on) {
    QSettings().setValue("quick", on);
    refreshRunLabels();
  });
  head->addWidget(quick_);
  runAll_ = new QPushButton;
  runAll_->setObjectName("primary");
  runAll_->setCursor(Qt::PointingHandCursor);
  runAll_->setFont(uiFont(10.5, QFont::DemiBold));
  connect(runAll_, &QPushButton::clicked, this, [this] { runTest(QString(), quick_->isChecked()); });
  head->addWidget(runAll_);
  head->addWidget(buildMenuButton());
  outer->addLayout(head);

  banner_ = text(tr("Reference systems show <b>placeholder values</b>, not measurements, until real reference results "
                    "are added."),
                 9.5, QFont::Normal, theme().warnText);
  banner_->setStyleSheet(QStringLiteral("color:%1; background:%2; border-radius:10px; padding:8px 14px;")
                             .arg(theme().warnText.name(), theme().warnBg.name()));
  outer->addWidget(banner_);

  auto *body = new QHBoxLayout;
  body->setSpacing(16);
  outer->addLayout(body, 1);

  /* tests column */
  auto *left = new QWidget;
  auto *ll = new QVBoxLayout(left);
  ll->setContentsMargins(0, 0, 4, 0);
  ll->setSpacing(8);
  level_ = new Segmented;
  level_->setItems({tr("Simple"), tr("Advanced")});
  level_->setCurrent(advanced_ ? 1 : 0);
  level_->setToolTip(tr("Simple: a few tests that run everywhere and are easy to relate to.\nAdvanced: every test."));
  connect(level_, &Segmented::changed, this, [this](int i) {
    advanced_ = i == 1;
    QSettings().setValue("advanced", advanced_);
    refresh();
  });
  testsTitle_ = sectionLabel(QString());
  testsTitle_->setContentsMargins(4, 0, 0, 2);
  ll->addWidget(testsTitle_);
  groupDesc_ = text(QString(), 9, QFont::Normal, theme().muted);
  groupDesc_->setContentsMargins(4, 0, 4, 6);
  ll->addWidget(groupDesc_);
  cardsLayout_ = new QVBoxLayout;
  cardsLayout_->setSpacing(10);
  ll->addLayout(cardsLayout_);
  moreTests_ = text(QString(), 9, QFont::Normal, theme().faint);
  moreTests_->setContentsMargins(4, 2, 4, 0);
  moreTests_->setTextInteractionFlags(Qt::LinksAccessibleByMouse);
  connect(moreTests_, &QLabel::linkActivated, this, [this] { level_->setCurrent(1); emit level_->changed(1); });
  ll->addWidget(moreTests_);
  ll->addStretch();
  auto *ls = new QScrollArea;
  ls->setWidget(left);
  ls->setWidgetResizable(true);
  ls->setFrameShape(QFrame::NoFrame);
  /* the tests column, with the Simple/Advanced switch pinned to its bottom left */
  auto *leftCol = new QWidget;
  leftCol->setFixedWidth(340);
  auto *lc = new QVBoxLayout(leftCol);
  lc->setContentsMargins(0, 0, 0, 0);
  lc->setSpacing(10);
  lc->addWidget(ls, 1);
  lc->addWidget(level_, 0, Qt::AlignLeft | Qt::AlignBottom);
  body->addWidget(leftCol);
  cards_.setExclusive(true);

  /* test page: what the test is, the ranking, and the chart that explains the highlighted result */
  QFrame *center = card();
  auto *cl = new QVBoxLayout(center);
  cl->setContentsMargins(4, 4, 4, 12);
  cl->setSpacing(6);
  auto *testPage = new QWidget;
  auto *pl = new QVBoxLayout(testPage);
  pl->setContentsMargins(20, 16, 20, 8);
  pl->setSpacing(6);
  title_ = text(QString(), 17, QFont::Bold);
  desc_ = text(QString(), 10.5);
  pl->addWidget(title_);
  pl->addWidget(desc_);
  auto section = [&](const QString &head, QLabel *&body) {
    QLabel *h = sectionLabel(head);
    h->setContentsMargins(0, 8, 0, 0);
    pl->addWidget(h);
    body = text(QString(), 10, QFont::Normal, theme().muted);
    pl->addWidget(body);
  };
  section(tr("How it is measured"), how_);
  section(tr("Reading the result"), read_);
  pl->addWidget(sectionLabel(tr("Ranking")));
  auto *legend = new QHBoxLayout;
  legend->setContentsMargins(0, 0, 0, 4);
  legend->addWidget(pill(tr("● Your runs"), theme().accent, theme().accentSoft));
  legend->addWidget(pill(tr("● Reference"), theme().muted, theme().track));
  legend->addStretch();
  QLabel *ciNote = text(tr("thin line on a bar = measurement uncertainty"), 8.5, QFont::Normal, theme().faint);
  ciNote->setWordWrap(false);
  ciNote->setToolTip(tr("The true value lies within this range with 95 % confidence. Overlapping ranges: treat as equal."));
  legend->addWidget(ciNote);
  pl->addLayout(legend);
  ranking_ = new RankingView;
  connect(ranking_, &RankingView::runSelected, this, [this](const QString &id) {
    selected_ = id;
    refreshRanking();
    refreshDetails();
  });
  pl->addWidget(ranking_);
  quickNote_ = text(QString(), 9, QFont::Normal, theme().muted);
  quickNote_->setTextInteractionFlags(Qt::LinksAccessibleByMouse);
  connect(quickNote_, &QLabel::linkActivated, this, [this](const QString &id) {
    selected_ = id;
    refreshRanking();
    refreshDetails();
  });
  quickNote_->hide();
  pl->addWidget(quickNote_);
  context_ = new QWidget;
  contextLayout_ = new QVBoxLayout(context_);
  contextLayout_->setContentsMargins(0, 8, 0, 0);
  contextLayout_->setSpacing(4);
  pl->addWidget(context_);
  pl->addStretch();
  auto *ps = new QScrollArea;
  ps->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  ps->setWidget(testPage);
  ps->setWidgetResizable(true);
  ps->setFrameShape(QFrame::NoFrame);
  cl->addWidget(ps, 1);
  auto *foot = new QHBoxLayout;
  foot->setContentsMargins(20, 0, 20, 0);
  allRuns_ = new Toggle(tr("All my runs"));
  allRuns_->setToolTip(tr("Show every saved run, not only the latest one per test"));
  connect(allRuns_, &Toggle::toggled, this, [this] { refreshRanking(); });
  foot->addWidget(allRuns_);
  foot->addStretch();
  QLabel *rel = text(tr("+/− % vs. the highlighted result"), 8.5, QFont::Normal, theme().faint);
  rel->setWordWrap(false);
  foot->addWidget(rel);
  foot->addSpacing(14);
  QLabel *help = text(QStringLiteral("<a href='#' style='color:%1; text-decoration:none'>%2</a>")
                          .arg(theme().accent.name(), tr("What do these terms mean?")),
                      8.5, QFont::DemiBold);
  help->setWordWrap(false);
  help->setCursor(Qt::PointingHandCursor);
  connect(help, &QLabel::linkActivated, this, &MainWindow::showGlossary);
  foot->addWidget(help);
  cl->addLayout(foot);
  body->addWidget(center, 3);

  /* details card */
  QFrame *right = card();
  auto *rl = new QVBoxLayout(right);
  rl->setContentsMargins(4, 4, 4, 4);
  details_ = new QWidget;
  detailsLayout_ = new QVBoxLayout(details_);
  detailsLayout_->setContentsMargins(18, 16, 18, 16);
  detailsLayout_->setSpacing(4);
  auto *ds = new QScrollArea;
  ds->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  ds->setWidget(details_);
  ds->setWidgetResizable(true);
  ds->setFrameShape(QFrame::NoFrame);
  rl->addWidget(ds);
  right->setMinimumWidth(380);
  body->addWidget(right, 2);

  measure_ = buildMeasuringWindow();
}

QWidget *MainWindow::buildMeasuringWindow() {
  auto *win = new MeasureWindow;
  win->setWindowTitle(tr("Prismark — measuring"));
  win->setObjectName("measureWindow");
  win->setAttribute(Qt::WA_StyledBackground);
  win->setStyleSheet(QStringLiteral("#measureWindow { background:%1; }").arg(theme().card.name()));
  win->setFixedWidth(620);
  win->onClose = [this] {
    if (!proc_) return true;
    if (!stressOpen_ &&
        QMessageBox::question(measure_, tr("Cancel the run?"),
                              tr("Closing this window cancels the run. The partial result is kept.")) != QMessageBox::Yes)
      return false;
    cancelRun(); /* a stress test without a set length asks itself */
    return false; /* the window closes when the runner has stopped */
  };
  auto *l = new QVBoxLayout(win);
  l->setContentsMargins(36, 32, 36, 28);
  l->setSpacing(10);
  auto *top = new QHBoxLayout;
  top->setSpacing(24);
  ring_ = new StepRing;
  top->addWidget(ring_);
  auto *tv = new QVBoxLayout;
  tv->addStretch();
  fzState_ = text(tr("MEASURING"), 9, QFont::DemiBold, theme().accent);
  tv->addWidget(fzState_);
  fzPhase_ = text(QString(), 19, QFont::Bold);
  tv->addWidget(fzPhase_);
  fzMsg_ = text(QString(), 10, QFont::Normal, theme().muted);
  tv->addWidget(fzMsg_);
  fzTime_ = text(QString(), 9, QFont::Normal, theme().muted);
  tv->addWidget(fzTime_);
  tv->addStretch();
  top->addLayout(tv, 1);
  l->addLayout(top);
  fzStall_ = text(QString(), 10, QFont::Medium, theme().warnText);
  fzStall_->setStyleSheet(QStringLiteral("color:%1; background:%2; border-radius:10px; padding:10px 14px;")
                              .arg(theme().warnText.name(), theme().warnBg.name()));
  fzStall_->hide();
  l->addWidget(fzStall_);
  fzHands_ = text(QString(), 10, QFont::Medium, theme().warnText);
  fzHands_->setStyleSheet(QStringLiteral("color:%1; background:%2; border-radius:10px; padding:10px 14px;")
                              .arg(theme().warnText.name(), theme().warnBg.name()));
  l->addWidget(fzHands_);
  l->addWidget(text(tr("The benchmark runs as a separate process. This window changes only when a new step begins, "
                       "so it does not wake the CPU: a still window is normal. If the runner stops unexpectedly, "
                       "this window closes and says so. Leave the machine alone until the run ends."),
                    9.5, QFont::Normal, theme().muted));
  fzStatus_ = text(QString(), 8.5, QFont::Normal, theme().faint);
  l->addWidget(fzStatus_);
  fzWarn_ = text(QString(), 9, QFont::Normal, theme().bad);
  l->addWidget(fzWarn_);
  fzCancel_ = new QPushButton(tr("Cancel run"));
  fzCancel_->setCursor(Qt::PointingHandCursor);
  connect(fzCancel_, &QPushButton::clicked, this, &MainWindow::cancelRun);
  l->addWidget(fzCancel_, 0, Qt::AlignLeft);

  stallTimer_ = new QTimer(this);
  stallTimer_->setSingleShot(true);
  connect(stallTimer_, &QTimer::timeout, this, &MainWindow::onStall);
  killTimer_ = new QTimer(this);
  killTimer_->setSingleShot(true);
  connect(killTimer_, &QTimer::timeout, this, [this] {
    if (proc_) proc_->kill();
  });
  return win;
}


/* ---------- data ---------- */

QString MainWindow::resultsDir() const {
  QString d = dataDir() + "/results/runs"; /* shared with the command line, which saves here too */
  makeUserDir(d);
  return d;
}

QString MainWindow::referencesDir() const {
  QString d = dataDir() + "/results/references";
  makeUserDir(d);
  return d;
}

/* Older versions kept runs directly in results/ and references in a references/ folder beside it. */
void MainWindow::moveOldDataFolders() {
  auto move = [](const QString &from, const QString &to) {
    for (const QFileInfo &fi : QDir(from).entryInfoList({"*.json"}, QDir::Files)) {
      QString dest = QDir(to).filePath(fi.fileName());
      if (!QFileInfo::exists(dest) && !QFile::rename(fi.filePath(), dest))
        qWarning().noquote() << "could not move" << fi.filePath() << "to" << dest;
    }
  };
  move(dataDir() + "/results", resultsDir());
  move(dataDir() + "/references", referencesDir());
  QDir().rmdir(dataDir() + "/references"); /* only if now empty */
}

QString MainWindow::cliPath() const {
#ifdef Q_OS_WIN
  const QString exe = "prismark.exe";
#else
  const QString exe = "prismark";
#endif
  QString local = QCoreApplication::applicationDirPath() + "/" + exe;
  if (QFileInfo(local).isExecutable()) return local;
  return QStandardPaths::findExecutable("prismark");
}

void MainWindow::loadInfo() {
  QProcess p;
  p.start(cliPath(), {"info"});
  if (p.waitForFinished(15000)) info_ = QJsonDocument::fromJson(p.readAllStandardOutput()).object();
}

bool MainWindow::loadFile(const QString &path, bool reference) {
  RunSummary s;
  if (reference) {
    if (!readReference(path, s)) return false;
  } else {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject() || !summarize(doc.object(), path, s)) return false;
  }
  s.reference = reference;
  for (RunSummary &r : runs_)
    if (r.id == s.id && r.reference == reference) {
      r = s;
      return true;
    }
  runs_.push_back(s);
  return true;
}

void MainWindow::loadAll() {
  runs_ = bundledReferences();
  for (const QFileInfo &fi : QDir(referencesDir()).entryInfoList({"*.json"}, QDir::Files)) loadFile(fi.filePath(), true);
  QFileInfoList mine = QDir(resultsDir()).entryInfoList({"prismark-*.json"}, QDir::Files, QDir::Time | QDir::Reversed);
  for (const QFileInfo &fi : mine) loadFile(fi.filePath(), false); /* oldest first: the last is the latest */
}

const RunSummary *MainWindow::selectedRun() const {
  for (const RunSummary &r : runs_)
    if (r.id == selected_) return &r;
  return nullptr;
}

/* ---------- views ---------- */

QString MainWindow::firstVisible(const QString &group) const {
  for (const Metric &m : metrics())
    if (m.group == group && visible(m)) return m.id;
  return QString();
}

QVector<QString> MainWindow::visibleGroups() const {
  QVector<QString> out;
  for (const Group &g : groups())
    if (!firstVisible(g.id).isEmpty()) out << g.id;
  return out;
}

void MainWindow::refresh() {
  QVector<QString> shown = visibleGroups();
  if (!shown.contains(group_)) group_ = shown.value(0);
  if (const Metric *m = findMetric(metric_); !m || !visible(*m) || m->group != group_) metric_ = firstVisible(group_);
  banner_->setVisible(std::any_of(runs_.begin(), runs_.end(), [](const RunSummary &r) { return r.placeholder; }));
  QVector<QString> main = shown;
  main.removeAll(QStringLiteral("stress"));
  QStringList names;
  for (const QString &id : main)
    for (const Group &g : groups())
      if (g.id == id) names << QString(g.label).replace("&&", "&");
  groups_->setItems(names);
  groups_->setCurrent(int(main.indexOf(group_))); /* -1 while the stress test is shown: no tab highlighted */
  stressTab_->setVisible(shown.contains(QStringLiteral("stress")));
  stressTab_->setCurrent(group_ == "stress" ? 0 : -1);
  refreshTests();
  refreshRanking();
  refreshDetails();
}

void MainWindow::refreshTests() {
  for (QAbstractButton *b : cards_.buttons()) {
    cards_.removeButton(b);
    b->deleteLater();
  }
  for (const Group &g : groups())
    if (g.id == group_) {
      testsTitle_->setText(QStringLiteral("%1 · %2").arg(tr("TESTS"), QString(g.label).replace("&&", "&").toUpper()));
      groupDesc_->setText(esc(g.desc));
    }
  int hidden = 0;
  for (const Metric &m : metrics()) {
    if (m.group != group_) continue;
    if (!visible(m)) {
      hidden++;
      continue;
    }
    const RunSummary *latest = nullptr, *quick = nullptr; /* the card shows full runs: quick ones are not ranked */
    for (const RunSummary &r : runs_)
      if (!r.reference && r.metrics.contains(m.id)) (r.quick ? quick : latest) = &r;
    auto *c = new TestCard(m.id);
    QString note = quick ? tr("Quick run only · not ranked") : tr("No result yet");
    if (m.group == "stress" && !latest) /* a stress test under 10 minutes: its verdict, without a score */
      for (const RunSummary &r : runs_)
        if (!r.reference && r.stress.contains(m.context.section(':', 1))) {
          const QString v = r.stress[m.context.section(':', 1)].verdict;
          note = (v == "failed" ? tr("Failed") : v == "too_hot" ? tr("Stopped hot") : tr("Stable")) +
                 tr(" · under 10 min, no score");
        }
    if (m.kernels == "K1" || m.kernels == "K1x") {
      CompileStatus st = compileStatus(info_, QSettings().value("k1_data").toString());
      if (m.kernels == "K1" ? !st.canRunK1() : !st.canRunK1x()) note = tr("Needs one-time setup · press Run");
    }
    c->setContent(m.name, m.sub, latest ? formatValue(latest->metrics[m.id].v) : QString(), m.unit, note);
    c->setToolTip(QStringLiteral("<p>%1</p>").arg(esc(m.desc)));
    c->setChecked(m.id == metric_);
    connect(c, &TestCard::clicked, this, [this, id = m.id] {
      metric_ = id;
      refreshRanking();
      refreshDetails();
    });
    /* a stress test has no quick run: its length is asked for instead */
    connect(c, &TestCard::runRequested, this,
            [this](const QString &id) { runTest(id, quick_->isChecked() && findMetric(id)->group != "stress"); });
    cards_.addButton(c);
    cardsLayout_->addWidget(c);
  }
  const QString link =
      QStringLiteral("<a href='#' style='color:%1; text-decoration:none'>%2</a>").arg(theme().accent.name(), tr("Advanced"));
  moreTests_->setText(hidden == 1 ? tr("1 more test on this tab in %1").arg(link)
                      : hidden    ? tr("%1 more tests on this tab in %2").arg(hidden).arg(link)
                                  : QString());
  moreTests_->setVisible(hidden > 0);
  refreshRunLabels();
}

void MainWindow::refreshRunLabels() {
  bool quick = quick_->isChecked();
  runAll_->setText(quick ? tr("▶  Run all quick") : tr("▶  Run all full"));
  runAll_->setToolTip(advanced_ ? tr("Runs every test but the stress tests, which run from their own cards.")
                                : tr("Runs the tests of the Simple view. Switch to Advanced to run every test."));
  bool stress = group_ == "stress";
  quick_->setVisible(!stress); /* a stress test has no quick run */
  for (QAbstractButton *b : cards_.buttons())
    static_cast<TestCard *>(b)->setRunLabel(stress ? tr("Run") : quick ? tr("Run quick") : tr("Run full"));
}

void MainWindow::refreshRanking() {
  const Metric *m = findMetric(metric_);
  if (!m) return;
  QSettings().setValue("metric", metric_);
  title_->setText(esc(m->title));
  desc_->setText(esc(m->desc));
  how_->setText(esc(m->how));
  read_->setText(esc(m->read));
  QVector<RankRow> rows;
  /* Quick runs are a first look: rough numbers, so they stay out of the ranking; the latest is offered below it. */
  const RunSummary *latest = nullptr, *quick = nullptr;
  for (const RunSummary &r : runs_)
    if (!r.reference && r.metrics.contains(m->id)) (r.quick ? quick : latest) = &r;
  if (quick && latest && quick->date < latest->date) quick = nullptr; /* a full run since: the quick one is old news */
  /* A stress test shorter than 10 minutes has no score, so it is not ranked either, but its verdict matters most
     when it failed: the latest such run is offered below the ranking too. */
  const QString part = m->group == "stress" ? m->context.section(':', 1) : QString();
  if (!part.isEmpty())
    for (const RunSummary &r : runs_)
      if (!r.reference && r.stress.contains(part) && !r.metrics.contains(m->id) && (!latest || latest->date < r.date))
        quick = &r;
  for (const RunSummary &r : runs_) {
    if (!r.metrics.contains(m->id) || r.quick) continue;
    if (!r.reference && !allRuns_->isChecked() && &r != latest) continue;
    RankRow row;
    row.runId = r.id;
    row.yours = !r.reference;
    row.name = r.reference ? r.name : tr("This PC · %1").arg(r.name);
    row.detail = r.reference ? QString("%1 · %2").arg(r.machine["os"].toString(), r.machine["isa"].toString()) : r.date;
    if (m->kernels == "K12" && !r.vectorIsa.isEmpty()) row.detail += QStringLiteral(" · ") + r.vectorIsa;
    if (r.placeholder) row.tags << "placeholder";
    if (r.reference && !r.placeholder) row.tags << "reference";
    if (&r == latest && allRuns_->isChecked()) row.tags << "latest";
    if (r.state["ac_online"] == QJsonValue(false)) row.tags << tr("on battery");
    if (!r.complete) row.tags << "partial";
    row.value = r.metrics[m->id];
    if (row.value.unsettled) row.tags << "did not settle";
    if (row.value.ofThreads) row.tags << tr("%1 of %2 threads").arg(row.value.threads).arg(row.value.ofThreads);
    if (row.value.clamped) row.tags << tr("throttled");
    if (m->group == "stress") {
      const RunSummary::Stress x = r.stress.value(m->context.section(':', 1));
      if (x.stopped) row.tags << tr("until stopped, %1 min").arg(std::floor(x.durationS / 60));
      else if (std::isfinite(x.durationS)) row.tags << tr("%1 min").arg(std::round(x.durationS / 60));
      if (x.verdict == "failed") row.tags << tr("failed");
      else if (x.verdict == "too_hot") row.tags << tr("stopped hot");
    }
    rows.push_back(row);
  }
  bool shown = std::any_of(rows.begin(), rows.end(), [&](const RankRow &x) { return x.runId == selected_; }) ||
               (quick && quick->id == selected_); /* a quick run opened from the note below */
  if (!shown && (latest || quick)) { /* follow the test: select your result that this page shows */
    selected_ = (latest ? latest : quick)->id;
    QTimer::singleShot(0, this, [this] { refreshDetails(); });
  }
  if (quick) {
    const QString link = QStringLiteral("<a href='%1' style='color:%2; text-decoration:none'>%3</a>")
                             .arg(quick->id, theme().accent.name(),
                                  quick->id == selected_ ? tr("Shown on the right") : tr("Show its report"));
    if (!part.isEmpty() && !quick->metrics.contains(m->id)) {
      const RunSummary::Stress x = quick->stress.value(part);
      QString v = x.verdict == "failed"    ? QStringLiteral("<b style='color:%1'>%2</b>").arg(theme().bad.name(), tr("failed"))
                  : x.verdict == "too_hot" ? tr("stopped at the temperature limit")
                                           : tr("stable");
      quickNote_->setText(tr("Your stress test of %1: %2 after %3:%4, not ranked (a score needs 10 minutes). %5")
                              .arg(quick->date, v)
                              .arg(int(x.durationS) / 60)
                              .arg(int(x.durationS) % 60, 2, 10, QChar('0'))
                              .arg(link));
    } else {
      const Value &v = quick->metrics[m->id];
      quickNote_->setText(tr("Your quick run of %1: %2 %3, a rough first look, not ranked. %4")
                              .arg(quick->date, formatValue(v.v), esc(m->unit), link));
    }
  }
  quickNote_->setVisible(quick != nullptr);
  ranking_->setRows(rows, m->higherBetter, m->unit, selected_);
  refreshContext();
}

void MainWindow::refreshContext() {
  while (QLayoutItem *it = contextLayout_->takeAt(0)) {
    if (it->widget()) {
      it->widget()->hide(); /* deleted later: until then it must not show through the new content */
      it->widget()->deleteLater();
    }
    delete it;
  }
  const Metric *m = findMetric(metric_);
  const RunSummary *r = selectedRun();
  context_->hide();
  if (!m || !r || m->context.isEmpty()) return;
  const Theme &t = theme();
  const QString who = r->reference ? r->name : tr("your run of %1").arg(r->date);
  auto show = [&](const QString &head, const QString &caption) {
    contextLayout_->addWidget(sectionLabel(head));
    contextLayout_->addWidget(text(caption, 9.5, QFont::Normal, t.muted));
    context_->show();
  };
  auto byThreads = [&](const QString &name, const QString &kernel) {
    ChartLine l;
    l.name = plainRatio(name);
    for (const auto &x : r->ratios)
      if (x.name == name && (kernel.isEmpty() || x.kernel == kernel) && x.n > 0) l.pts.push_back({double(x.n), x.v.v, x.v.lo, x.v.hi});
    std::sort(l.pts.begin(), l.pts.end(), [](const auto &a, const auto &b) { return a[0] < b[0]; });
    return l;
  };

  const QString kind = m->context.section(':', 0, 0), arg = m->context.section(':', 1);
  if (kind == "wake" && !r->cold.isEmpty()) {
    ChartLine l;
    l.name = tr("speed from rest");
    for (const auto &c : r->cold) l.pts.push_back({c.w, c.r.v, c.r.lo, c.r.hi});
    std::sort(l.pts.begin(), l.pts.end(), [](const auto &a, const auto &b) { return a[0] < b[0]; });
    show(tr("Speed from rest, by task length"),
         tr("For %1: how fast tasks of different lengths run when they arrive while the processor rests, in % of a "
            "busy core. Short tasks suffer most from the wake-up delay; 100 % means no penalty.").arg(esc(who)));
    auto *ch = new ChartView;
    ch->setData({l}, true, tr("task length (ms)"), 0, 100, false);
    contextLayout_->addWidget(ch);
  } else if (kind == "scaling") {
    auto byN = [](const auto &a, const auto &b) { return a[0] < b[0]; };
    ChartLine watts, perf, ghz;
    watts.name = tr("power");
    watts.color = 1;
    perf.name = tr("result");
    perf.color = 0;
    ghz.name = tr("clock");
    ghz.unit = tr("GHz");
    ghz.color = 2;
    /* On hybrid and multi-CCX processors, one clock per core type or CCX ("P" first, then by name). */
    QVector<ChartLine> clocks;
    for (const auto &x : r->load) {
      if (x.kernel != arg) continue;
      perf.unit = x.unit == "Msamples/s" ? tr("M/s") : x.unit;
      if (std::isfinite(x.watts)) watts.pts.push_back({double(x.n), x.watts, NAN, NAN});
      if (std::isfinite(x.perf)) perf.pts.push_back({double(x.n), x.perf, NAN, NAN});
      if (std::isfinite(x.mhz)) ghz.pts.push_back({double(x.n), x.mhz / 1000, NAN, NAN});
      for (const auto &d : x.mhzBy) {
        if (!std::isfinite(d.mhz)) continue;
        auto it = std::find_if(clocks.begin(), clocks.end(), [&](const ChartLine &l) { return l.name == d.domain; });
        if (it == clocks.end()) {
          ChartLine l = ghz;
          l.name = d.domain;
          l.pts.clear();
          clocks.push_back(l);
          it = clocks.end() - 1;
        }
        if (!d.idle) it->pts.push_back({double(x.n), d.mhz / 1000, NAN, NAN}); /* a domain not running the test: left out */
      }
    }
    std::sort(clocks.begin(), clocks.end(), [](const ChartLine &a, const ChartLine &b) {
      return a.name.startsWith('P') != b.name.startsWith('P') ? a.name.startsWith('P') : a.name < b.name;
    });
    for (int i = 0; i < clocks.size(); i++) {
      clocks[i].color = 2 + i; /* named by its domain alone: the strip is the clock */
    }
    for (ChartLine *l : {&watts, &perf, &ghz}) std::sort(l->pts.begin(), l->pts.end(), byN);
    for (ChartLine &l : clocks) std::sort(l.pts.begin(), l.pts.end(), byN);
    QVector<ChartLine> clockLines;
    for (const ChartLine &l : clocks)
      if (l.pts.size() >= 2) clockLines.push_back(l);
    if (clockLines.isEmpty() && ghz.pts.size() >= 2) clockLines.push_back(ghz);

    /* Stacked on the thread counts: the speed-up against perfect scaling (or the result itself, without
       scaling), then the power and the clock(s), each on an axis of its own. */
    ChartLine speedup;
    speedup.name = tr("speed-up");
    speedup.color = 0;
    for (const auto &x : r->scaling)
      if (x.kernel == arg) speedup.pts.push_back({double(x.n), x.s.v, x.s.lo, x.s.hi});
    std::sort(speedup.pts.begin(), speedup.pts.end(), byN);
    bool power = watts.pts.size() >= 2, clock = !clockLines.isEmpty();
    if (speedup.pts.size() < 2 && perf.pts.size() < 2) return;
    show(power ? tr("Speed-up, power and clock with more threads") : tr("Speed-up and clock with more threads"),
         tr("For %1, by number of threads. The speed-up is how many times faster than one thread; the dashed line is "
            "perfect scaling (8 threads = 8× faster), and real programs fall below it as threads wait for each other, "
            "for memory or for power.").arg(esc(who)) +
             (power ? " " + tr("More threads should draw more power until the power limit; from there the clock eases "
                               "off. If the speed-up falls together with the power and the clock, something outside "
                               "the program (a power setting, a charger or dock) is holding the processor back.")
                    : QString()));
    if (speedup.pts.size() >= 2) {
      auto *ch = new ChartView;
      ch->setData({speedup}, false, power || clock ? QString() : tr("threads"), 0, NAN, true);
      ch->setYUnit(QStringLiteral("×"));
      ch->setAspect(0.4, 190);
      contextLayout_->addWidget(ch);
    } else {
      addStrip(contextLayout_, {perf}, perf.unit, power || clock ? QString() : tr("threads"));
    }
    if (power) addStrip(contextLayout_, {watts}, tr("W"), clock ? QString() : tr("threads"));
    if (clock) addStrip(contextLayout_, clockLines, tr("GHz"), tr("threads"));
  } else if (kind == "ratio") {
    ChartLine l = byThreads(arg, QString());
    if (l.pts.isEmpty()) return;
    if (l.pts.size() >= 2) {
      show(tr("Time added by the build tools, by number of threads"),
           tr("For %1: time of the full build divided by the time of compiling the same files in memory. 1.0 means "
              "the tools add nothing; 2.0 means the build takes twice as long as the compiling alone.").arg(esc(who)));
      auto *ch = new ChartView;
      ch->setData({l}, false, tr("threads"), 0, NAN, false);
      contextLayout_->addWidget(ch);
      return;
    }
    /* One thread count (full builds run only on all threads): the two times side by side. */
    const double ratio = l.pts[0][1];
    const int n = int(l.pts[0][0]);
    if (!r->metrics.contains(m->id) || !(ratio > 0)) return;
    const double build = r->metrics[m->id].v, compile = build / ratio;
    show(tr("Time added by the build tools"),
         tr("For %1, on %2 threads: the full build against compiling the same files in memory. The difference (%3) "
            "is the time the build tools take: starting programs, reading and writing files, linking.")
             .arg(esc(who))
             .arg(n)
             .arg(ratio >= 1 ? tr("+%1 %").arg(QString::number((ratio - 1) * 100, 'f', 0))
                             : tr("%1 %").arg(QString::number((ratio - 1) * 100, 'f', 0))));
    contextLayout_->addWidget(new BarsView({{tr("Full build with CMake and Ninja"), build,
                                             tr("%1 s").arg(formatValue(build)), 0},
                                            {tr("Compiling the same files in memory"), compile,
                                             tr("%1 s").arg(formatValue(compile)), 2}}));
  } else if (kind == "stress" && r->stress.contains(arg)) {
    const RunSummary::Stress &x = r->stress[arg];
    /* At most about 60 points: the median of each minute (of several, in a long run). */
    int per = std::max(1, int(std::lround(60 / x.windowS))), n = int(x.perf.size());
    per *= std::max(1, int(std::ceil(n / double(per) / 60)));
    auto median = [&](const QVector<double> &v, int from, int to, double k) {
      QVector<double> y;
      for (int i = from; i < to && i < v.size(); i++)
        if (std::isfinite(v[i]) && v[i] > 0) y.push_back(v[i] * k);
      if (y.isEmpty()) return double(NAN);
      std::sort(y.begin(), y.end());
      return y.size() % 2 ? y[y.size() / 2] : (y[y.size() / 2 - 1] + y[y.size() / 2]) / 2;
    };
    /* One slim chart per quantity, on the same minutes: speed, temperature, clock, and power where measured. */
    ChartLine perf, temp, ghz, watts;
    perf.name = tr("speed"), perf.color = 0;
    temp.name = tr("temperature"), temp.color = 3;
    ghz.name = tr("clock"), ghz.color = 2;
    watts.name = tr("power"), watts.color = 1;
    for (int i = 0; i < n; i += per) {
      double minute = (i + std::min(per, n - i)) * x.windowS / 60;
      double v = median(x.perf, i, i + per, 1), c = median(x.tempT, i, i + per, 1),
             g = median(x.mhzT, i, i + per, 1e-3), w = median(x.powerT, i, i + per, 1);
      if (std::isfinite(v)) perf.pts.push_back({minute, v, NAN, NAN});
      if (std::isfinite(c)) temp.pts.push_back({minute, c, NAN, NAN});
      if (std::isfinite(g)) ghz.pts.push_back({minute, g, NAN, NAN});
      if (std::isfinite(w)) watts.pts.push_back({minute, w, NAN, NAN});
    }
    if (perf.pts.size() < 2 && x.cores.isEmpty()) return;
    /* The verdict first, as a stability tool states it. */
    auto clock = [](double sec) { return QStringLiteral("%1:%2").arg(int(sec) / 60).arg(int(sec) % 60, 2, 10, QChar('0')); };
    QString verdict;
    if (x.verdict == "failed") {
      QStringList cpus;
      for (auto it = x.wrongByCpu.begin(); it != x.wrongByCpu.end(); ++it)
        cpus << tr("CPU %1 (%2)").arg(it.key()).arg(it.value());
      verdict = QStringLiteral("<span style='color:%1'><b>%2</b></span>")
                    .arg(t.bad.name(),
                         tr("Failed: a wrong result %1%2. This computer is not stable under this load.")
                             .arg(std::isfinite(x.firstErrorS) ? tr("after %1").arg(clock(x.firstErrorS)) : QString(),
                                  x.firstErrorCpu >= 0 ? tr(" on CPU %1").arg(x.firstErrorCpu) : QString()));
      if (!cpus.isEmpty()) verdict += " " + tr("Wrong results by CPU: %1.").arg(cpus.join(", "));
    } else if (x.verdict == "too_hot") {
      verdict = QStringLiteral("<span style='color:%1'><b>%2</b></span>")
                    .arg(t.warnText.name(), tr("Stopped at the temperature limit after %1, with no wrong results.")
                                                .arg(clock(x.durationS)));
    } else {
      verdict = QStringLiteral("<span style='color:%1'><b>%2</b></span>")
                    .arg(t.good.name(), tr("Stable: %1 of full load, every result correct.").arg(clock(x.durationS)));
    }
    if (!x.cores.isEmpty()) { /* core cycling: each core's speed, a wrong one in red */
      QVector<BarsView::Bar> bars;
      for (const auto &core : x.cores)
        bars.push_back({tr("CPU %1 (%2)").arg(core.cpu).arg(core.type), core.perf.v,
                        core.wrong ? tr("%1 %2 · %3 wrong").arg(formatValue(core.perf.v), esc(x.unit)).arg(core.wrong)
                                   : tr("%1 %2").arg(formatValue(core.perf.v), esc(x.unit)),
                        core.wrong ? 1 : 0});
      show(tr("Each core on its own"),
           tr("For %1: the speed of every physical core at its own boost clock, in turn. Similar cores should be "
              "within a few percent of each other; a slow one runs hotter or is held back, and a red one computed a "
              "wrong result.").arg(esc(who)) +
               "<br>" + verdict);
      contextLayout_->addWidget(new BarsView(bars));
      return;
    }
    QStringList facts;
    const Value score = r->metrics.value(m->id, Value{});
    facts << (std::isfinite(score.v) ? tr("score (10th minute) <b>%1 %2</b>").arg(formatValue(score.v), esc(x.unit))
                                     : tr("no score: it ran less than 10 minutes"));
    if (std::isfinite(x.last.v)) facts << tr("last minute %1 %2").arg(formatValue(x.last.v), esc(x.unit));
    if (std::isfinite(x.holdUp.v))
      facts << tr("held <b>%1 %</b> of its first-minute speed").arg(QString::number(x.holdUp.v * 100, 'f', 0));
    if (std::isfinite(x.bytes)) facts << tr("%1 GB of memory tested").arg(QString::number(x.bytes / 1e9, 'f', 1));
    if (std::isfinite(x.tempMax)) facts << tr("hottest %1 °C").arg(QString::number(x.tempMax, 'f', 0));
    if (x.clamped) facts << tr("%1 windows throttled by the platform").arg(x.clamped);
    facts << (x.stopped ? tr("ran %1 min until stopped").arg(std::floor(x.durationS / 60))
                        : tr("ran %1 min").arg(std::round(x.durationS / 60)));
    show(tr("Speed, temperature and clock over the run"),
         tr("For %1, minute by minute. A speed that falls while the temperature climbs and the clock drops is the "
            "processor slowing down to stay within its limits.")
             .arg(esc(who)) +
             "<br>" + verdict + "<br>" + facts.join(QStringLiteral(" · ")));
    QVector<std::pair<const ChartLine *, QString>> strips{{&perf, x.unit}};
    if (temp.pts.size() >= 2) strips.push_back({&temp, QStringLiteral("°C")});
    if (ghz.pts.size() >= 2) strips.push_back({&ghz, tr("GHz")});
    if (watts.pts.size() >= 2) strips.push_back({&watts, tr("W")});
    for (int i = 0; i < strips.size(); i++)
      addStrip(contextLayout_, {*strips[i].first}, strips[i].second, i == strips.size() - 1 ? tr("minutes") : QString());
  } else if (kind == "resp") {
    for (const auto &x : r->ratios)
      if (x.name == "R_resp" && x.kernel == arg) {
        show(tr("Compared with a busy core"),
             tr("For %1: arriving while the processor rests, this task ran at <b>%2 %</b> of its speed on a busy core "
                "(likely between %3 and %4 %).")
                 .arg(esc(who), formatValue(100 * x.v.v), formatValue(100 * x.v.lo), formatValue(100 * x.v.hi)));
        break;
      }
  }
}

void MainWindow::selectMetric(const QString &id) {
  const Metric *m = findMetric(id);
  if (!m) return;
  metric_ = m->id;
  group_ = m->group;
  refresh();
}

void MainWindow::refreshDetails() {
  /* rebuild the report of the highlighted run */
  while (QLayoutItem *it = detailsLayout_->takeAt(0)) {
    if (it->widget()) it->widget()->deleteLater();
    delete it;
  }
  const Theme &t = theme();
  const RunSummary *r = selectedRun();
  if (!r) {
    detailsLayout_->addWidget(text(tr("Pick a result in the ranking to see its report."), 10, QFont::Normal, t.muted));
    detailsLayout_->addStretch();
    return;
  }

  /* heading */
  detailsLayout_->addWidget(text(esc(r->name), 14, QFont::Bold));
  QVector<QLabel *> meta;
  if (r->placeholder) meta << pill(tr("example values"), t.warnText, t.warnBg);
  meta << pill(r->reference ? tr("reference") : tr("your run"), r->reference ? t.muted : t.accent,
               r->reference ? t.track : t.accentSoft);
  if (!r->date.isEmpty()) meta << pill(r->date, t.muted, t.track);
  if (r->quick) meta << pill(tr("quick run"), t.muted, t.track);
  if (!r->complete) meta << pill(tr("incomplete"), t.bad, t.track);
  if (!r->stress.isEmpty()) { /* the stress test's verdict, over its parts */
    QString v = "stable";
    for (const auto &x : r->stress)
      if (x.verdict == "failed" || (x.verdict == "too_hot" && v != "failed")) v = x.verdict;
    meta << (v == "failed"    ? pill(tr("stress test failed"), t.bad, t.track)
             : v == "too_hot" ? pill(tr("stopped hot"), t.warnText, t.warnBg)
                              : pill(tr("stress test stable"), t.good, t.track));
  }
  detailsLayout_->addWidget(pillRow(meta));
  if (r->placeholder)
    detailsLayout_->addWidget(text(tr("Made-up example values shown for comparison until real reference machines "
                                      "have been measured."),
                                   9, QFont::Normal, t.muted));
  else if (r->quick)
    detailsLayout_->addWidget(text(tr("A quick run gives a first look. Its numbers are rough; use a full run to "
                                      "compare computers."),
                                   9, QFont::Normal, t.muted));

  /* results, grouped like the tabs; each name opens its test */
  detailsLayout_->addWidget(sectionLabel(tr("Results")));
  auto *tests = new QWidget;
  auto *tg = new QGridLayout(tests);
  tg->setContentsMargins(0, 0, 0, 0);
  tg->setVerticalSpacing(5);
  tg->setHorizontalSpacing(12);
  tg->setColumnStretch(0, 1);
  int row = 0;
  QString lastGroup;
  int hiddenResults = 0;
  for (const Metric &mt : metrics()) {
    if (!r->metrics.contains(mt.id)) continue;
    if (!visible(mt)) {
      hiddenResults++;
      continue;
    }
    if (mt.group != lastGroup) {
      lastGroup = mt.group;
      for (const Group &gr : groups())
        if (gr.id == mt.group) {
          QString label = QString(gr.label).replace("&&", "&");
          QLabel *h = text(esc(label), 8.5, QFont::DemiBold, t.faint);
          h->setContentsMargins(0, row ? 6 : 0, 0, 0);
          tg->addWidget(h, row++, 0, 1, 2);
        }
    }
    const Value &v = r->metrics[mt.id];
    bool current = mt.id == metric_;
    QLabel *name = text(QStringLiteral("<a href='%1' style='color:%2; text-decoration:none'>%3</a>")
                            .arg(mt.id, (current ? t.accent : t.text).name(), esc(mt.name)),
                        9.5, current ? QFont::DemiBold : QFont::Normal);
    name->setCursor(Qt::PointingHandCursor);
    name->setToolTip(esc(mt.title));
    connect(name, &QLabel::linkActivated, this, &MainWindow::selectMetric);
    tg->addWidget(name, row, 0);
    auto *val = text(QStringLiteral("<b>%1</b> <span style='color:%2'>%3</span>").arg(formatValue(v.v), t.muted.name(), esc(mt.unit)), 10);
    val->setAlignment(Qt::AlignRight | Qt::AlignTop);
    val->setWordWrap(false);
    QString tip = v.hasCi() ? tr("Likely between %1 and %2 %3").arg(formatValue(v.lo), formatValue(v.hi), mt.unit) : QString();
    if (v.unsettled && !r->quick) {
      val->setText(QStringLiteral("<span style='color:%1; font-size:8pt'>%2</span> ").arg(t.warnText.name(), tr("not settled")) + val->text());
      tip += tr("\nAn older result: the speed was still changing when its 10-minute limit was reached.");
    }
    if (v.clamped) {
      val->setText(QStringLiteral("<span style='color:%1; font-size:8pt'>%2</span> ").arg(t.warnText.name(), tr("throttled")) +
                   val->text());
      tip += tr("\nThrottled: during %1 of its measured windows the computer's firmware held the CPU at %2 MHz, "
                "below its own minimum clock. The score includes it.")
                 .arg(v.clamped)
                 .arg(QString::number(v.clampedMhz, 'f', 0));
    }
    if (v.ofThreads) {
      val->setText(QStringLiteral("<span style='color:%1; font-size:8pt'>%2%3</span> ")
                       .arg(t.warnText.name(), tr("%1 of %2 threads").arg(v.threads).arg(v.ofThreads),
                            v.clamped ? QStringLiteral(" ·") : QString()) +
                   val->text());
      tip += tr("\nMeasured on %1 of this computer's %2 threads (see Not measured below), so it is not an all-cores "
                "result.")
                 .arg(v.threads)
                 .arg(v.ofThreads);
    }
    val->setToolTip(tip);
    tg->addWidget(val, row++, 1);
  }
  if (!row)
    tg->addWidget(text(r->stress.isEmpty() ? tr("No results in this run.")
                                           : tr("A stress test shorter than 10 minutes has no score; its verdict is "
                                                "above, and its test page shows the rest."),
                       9.5, QFont::Normal, t.muted),
                  0, 0);
  detailsLayout_->addWidget(tests);
  if (hiddenResults)
    detailsLayout_->addWidget(text(hiddenResults == 1 ? tr("1 more result in Advanced.") : tr("%1 more results in Advanced.").arg(hiddenResults),
                                   9, QFont::Normal, t.faint));

  /* how much slower the computer got during the warm-ups: what sustained use costs it */
  if (!r->hot.isEmpty() || r->clampedSeries) {
    QStringList hot;
    for (const auto &[mode, label] : {std::pair{QStringLiteral("st_sustained"), tr("one core")},
                                      std::pair{QStringLiteral("mc_threaded"), tr("all cores")}}) {
      if (!r->hot.contains(mode)) continue;
      double pct = (1 - r->hot[mode].v) * 100;
      hot << (pct >= 1 ? tr("<b>%1 %</b> slower on %2").arg(QString::number(pct, 'f', 0), label)
                       : tr("no slowdown on %1").arg(label));
    }
    if (r->clampedSeries)
      hot << tr("the firmware throttled the CPU to <b>%1 MHz</b> during %2 test%3")
                 .arg(QString::number(r->clampedMhz, 'f', 0))
                 .arg(r->clampedSeries)
                 .arg(r->clampedSeries == 1 ? QString() : QStringLiteral("s"));
    detailsLayout_->addWidget(sectionLabel(tr("When hot")));
    auto *l = text(hot.join(QStringLiteral(" · ")), 9.5);
    l->setToolTip(tr("Speed at the end of the one-minute warm-up against its first seconds. The long tests are "
                     "measured after the warm-up, so they already include this slowdown.\nFirmware throttling is the "
                     "computer's firmware holding the CPU below its own minimum clock, often when it gets very hot; "
                     "the tests it happened in are marked \"throttled\"."));
    detailsLayout_->addWidget(l);
  }

  /* what was not measured, once per test and reason */
  QStringList un, seen;
  for (const QJsonValue &u : r->unavailable) {
    QJsonObject o = u.toObject();
    bool relevant = advanced_;
    for (const Metric &mt : metrics()) relevant |= visible(mt) && mt.kernels == o["kernel"].toString();
    /* only tests this view shows, and only those the run's platform offers (older phone results list compiling) */
    if (!relevant || !offeredOn(o["kernel"].toString(), r->machine["os"].toString())) continue;
    QString line = QStringLiteral("<b>%1</b> <span style='color:%2'>— %3</span>")
                       .arg(esc(plainKernel(o["kernel"].toString(), o["variant"].toString())), t.muted.name(),
                            esc(plainReason(o["reason"].toString())));
    if (!seen.contains(line)) {
      seen << line;
      un << line;
    }
  }
  if (!un.isEmpty()) {
    detailsLayout_->addWidget(sectionLabel(tr("Not measured")));
    detailsLayout_->addWidget(text(un.join("<br>"), 9.5));
  }

  /* the computer, in a few lines */
  detailsLayout_->addWidget(sectionLabel(r->reference ? tr("Computer") : tr("This computer")));
  auto *grid = new QWidget;
  auto *g = new QGridLayout(grid);
  g->setContentsMargins(0, 0, 0, 0);
  g->setHorizontalSpacing(14);
  g->setVerticalSpacing(5);
  g->setColumnStretch(1, 1);
  int krow = 0;
  auto kv = [&](QGridLayout *into, int &n, const QString &k, const QString &v, const QString &tip = QString()) {
    if (v.trimmed().isEmpty()) return;
    QLabel *key = text(k, 9.5, QFont::Normal, t.muted);
    key->setToolTip(tip);
    into->addWidget(key, n, 0, Qt::AlignTop);
    into->addWidget(text(esc(v), 9.5), n++, 1);
  };
  const QJsonObject &m = r->machine;
  QJsonObject cpu = m["cpu"].toObject();
  QString cores;
  if (cpu.contains("cores")) {
    QMap<QString, int> types;
    for (const QJsonValue &c : cpu["cores"].toArray()) types[c.toObject()["type"].toString()]++;
    QStringList tl;
    for (auto it = types.begin(); it != types.end(); ++it)
      tl << (it.key() == "P" && types.size() == 1 ? QString::number(it.value())
                                                  : QStringLiteral("%1 %2").arg(it.value()).arg(it.key() == "P" ? tr("fast") : tr("efficient")));
    cores = tr("%1 threads").arg(tl.join(" + "));
  } else {
    cores = tr("%1 threads").arg(m["cpus"].toInt());
  }
  QString model = cpu["model"].toString();
  kv(g, krow, tr("Processor"), model.isEmpty() ? cores : model + " · " + cores);
  QString os = m["os"].toString();
  if (!os.isEmpty()) os[0] = os[0].toUpper();
  kv(g, krow, tr("System"), os);
  QJsonArray cpufreq = r->state["cpufreq"].toArray();
  if (!cpufreq.isEmpty())
    kv(g, krow, tr("Power policy"), cpufreq[0].toObject()["governor"].toString(),
       tr("How the operating system picks the clock speed. It affects results, so it is recorded with every run."));
  kv(g, krow, tr("Power mode"), r->state["power_mode"].toString());
  if (r->state.contains("ac_online") && !r->state["ac_online"].isNull())
    kv(g, krow, tr("Power source"), r->state["ac_online"].toBool() ? tr("plugged in") : tr("battery"));
  detailsLayout_->addWidget(grid);

  /* technical details, folded away */
  auto *techBtn = new QPushButton(techOpen_ ? tr("Hide technical details ▴") : tr("Technical details ▾"));
  techBtn->setObjectName("ghost");
  techBtn->setCursor(Qt::PointingHandCursor);
  connect(techBtn, &QPushButton::clicked, this, [this] {
    techOpen_ = !techOpen_;
    refreshDetails();
  });
  auto *tbw = new QWidget;
  auto *tbl = new QHBoxLayout(tbw);
  tbl->setContentsMargins(0, 10, 0, 0);
  tbl->addWidget(techBtn);
  tbl->addStretch();
  detailsLayout_->addWidget(tbw);
  if (techOpen_) {
    auto *tw = new QWidget;
    auto *tgl = new QGridLayout(tw);
    tgl->setContentsMargins(0, 4, 0, 0);
    tgl->setHorizontalSpacing(14);
    tgl->setVerticalSpacing(5);
    tgl->setColumnStretch(1, 1);
    int trow = 0;
    kv(tgl, trow, tr("Architecture"), m["isa"].toString());
    kv(tgl, trow, tr("Kernel"), m["kernel"].toString());
    kv(tgl, trow, tr("Board"), m["board"].toString());
    if (cpu["llc_bytes"].toDouble() > 0)
      kv(tgl, trow, tr("Largest cache"), QStringLiteral("%1 MiB").arg(cpu["llc_bytes"].toDouble() / (1 << 20)));
    QString level = r->tiers["baseline"].toObject()["level"].toString();
    if (!level.isEmpty()) kv(tgl, trow, tr("Built for"), QStringLiteral("%1 (%2)").arg(isaName(level), level));
    if (!r->harness.isEmpty())
      kv(tgl, trow, tr("Prismark"), tr("%1, built with %2").arg(r->harness["version"].toString(), r->harness["compiler"].toString()));
    if (!r->frontend.isEmpty()) {
      QString kind = r->frontend["kind"].toString();
      QString fe = kind == "gui" ? tr("desktop app") : kind == "cli" ? tr("command line") : kind;
      if (r->frontend["ui_state"].toString() == "frozen") fe += tr(", window kept still");
      kv(tgl, trow, tr("Started from"), fe);
    }
    if (!r->file.isEmpty()) kv(tgl, trow, tr("File"), r->file);
    detailsLayout_->addWidget(tw);
  }

  bool ownReference = r->reference && QFileInfo(r->file).absolutePath() == QFileInfo(referencesDir()).absoluteFilePath();
  if (!r->reference || ownReference) {
    auto *w = new QWidget;
    auto *btns = new QHBoxLayout(w);
    btns->setContentsMargins(0, 12, 0, 0);
    auto *show = new QPushButton(tr("Show file"));
    show->setObjectName("ghost");
    connect(show, &QPushButton::clicked, this, [this] {
      if (const RunSummary *s = selectedRun()) QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(s->file).absolutePath()));
    });
    btns->addWidget(show);
    if (ownReference) {
      auto *rm = new QPushButton(tr("Remove reference"));
      rm->setObjectName("ghost");
      connect(rm, &QPushButton::clicked, this, &MainWindow::removeReference);
      btns->addWidget(rm);
    } else {
      auto *save = new QPushButton(tr("Save as reference…"));
      save->setObjectName("ghost");
      save->setToolTip(tr("Keep this run in the rankings as a reference system to compare later runs with"));
      connect(save, &QPushButton::clicked, this, &MainWindow::saveAsReference);
      auto *rm = new QPushButton(tr("Move to trash"));
      rm->setObjectName("ghost");
      connect(rm, &QPushButton::clicked, this, &MainWindow::removeSelected);
      btns->addWidget(save);
      btns->addWidget(rm);
    }
    btns->addStretch();
    detailsLayout_->addWidget(w);
  }
  detailsLayout_->addStretch();
}

/* ---------- pre-run checks ---------- */

bool MainWindow::confirmCompileTests(bool needK1, bool needK1x, bool all, QStringList &args) {
  QSettings settings;
  for (;;) {
    CompileStatus st = compileStatus(info_, settings.value("k1_data").toString());
    bool okK1 = !needK1 || st.canRunK1(), okK1x = !needK1x || st.canRunK1x();
    if (okK1 && okK1x) {
      if (!st.snapshot.isEmpty()) args << "--k1-data" << st.snapshot;
      return true;
    }
    QDialog d(this);
    d.setWindowTitle(tr("Compile tests"));
    d.setMinimumWidth(620);
    auto *l = new QVBoxLayout(&d);
    l->setContentsMargins(26, 22, 26, 20);
    l->setSpacing(10);
    l->addWidget(text(tr("“Compiling code” and “Full software build” need a one-time setup"), 14, QFont::Bold));
    auto mark = [&](const QColor &c, const char *sym, const QString &what, QFont::Weight w = QFont::Normal) {
      l->addWidget(text(QStringLiteral("<span style='color:%1'>%2</span>&nbsp; %3").arg(c.name(), sym, esc(what)), 10, w));
    };
    auto row = [&](bool ok, const QString &what) { mark(ok ? theme().good : theme().bad, ok ? "✓" : "✗", what); };
    /* Prerequisites first, then the install commands, then the snapshot: it is the last step and the user starts it. */
    row(st.missingBuild.isEmpty(), st.missingBuild.isEmpty() ? tr("Build tools for Full build (clang 19, cmake, ninja)")
                                                             : tr("Missing tools: %1").arg(st.missingBuild.join(", ")));
    row(st.k1Built, st.k1Built ? tr("“Compiling code” is built into this Prismark")
                               : tr("“Compiling code” is not built into this Prismark (needs the Clang 19 libraries)"));
    QString cmds = installInstructions(st);
    bool needInstall = !st.missingPrepare.isEmpty() || !st.k1Built;
    bool canPrepare = st.snapshot.isEmpty() && st.missingPrepare.isEmpty();
    if (needInstall && !cmds.isEmpty()) {
      l->addWidget(text(tr("To install what is missing, run in a terminal:"), 10, QFont::Medium));
      auto *box = new QPlainTextEdit(cmds);
      box->setReadOnly(true);
      box->setFont(monoFont(9));
      box->setFixedHeight(110);
      box->setLineWrapMode(QPlainTextEdit::NoWrap);
      l->addWidget(box);
    }
    if (!st.snapshot.isEmpty())
      row(true, tr("Snapshot prepared: %1").arg(st.snapshot));
    else if (canPrepare)
      mark(theme().accent, "→", tr("Last step: prepare the snapshot of the LLVM sources. Press “Prepare now” to start it."),
           QFont::Medium);
    else
      row(false, tr("Last step: prepare the snapshot of the LLVM sources. Once the commands above have run, "
                    "open this again and press “Prepare now”."));
    if (st.snapshot.isEmpty())
      l->addWidget(text(tr("Preparing the snapshot downloads pinned sources (about 160 MB), builds generated files "
                           "once and measures every unit: 20–60 minutes, about 4 GB of disk while it runs."),
                        9.5, QFont::Normal, theme().muted));
    auto *bb = new QDialogButtonBox;
    QPushButton *prep = nullptr, *without = nullptr, *copy = nullptr;
    if (canPrepare) {
      prep = bb->addButton(tr("Prepare now"), QDialogButtonBox::AcceptRole);
      prep->setObjectName("primary");
    }
    if (needInstall && !cmds.isEmpty()) copy = bb->addButton(tr("Copy commands"), QDialogButtonBox::ActionRole);
    if (all) without = bb->addButton(tr("Run without them"), QDialogButtonBox::AcceptRole);
    bb->addButton(QDialogButtonBox::Cancel);
    QAbstractButton *clicked = nullptr;
    connect(bb, &QDialogButtonBox::clicked, &d, [&](QAbstractButton *b) {
      if (b == copy) {
        QApplication::clipboard()->setText(cmds);
        return;
      }
      clicked = b;
      d.accept();
    });
    l->addWidget(bb);
    d.exec();
    if (clicked && clicked == without) {
      if (!st.snapshot.isEmpty()) args << "--k1-data" << st.snapshot;
      return true;
    }
    if (!clicked || clicked != prep) return false;
    PrepareDialog pd(this);
    if (pd.exec() != QDialog::Accepted) return false;
    /* loop: check again with the new snapshot */
  }
}

/* ---------- running ---------- */

void MainWindow::runTest(const QString &metricId, bool quick) {
  if (proc_) return;
  QString cli = cliPath();
  if (cli.isEmpty()) {
    QMessageBox::warning(this, tr("Prismark"), tr("The benchmark runner (prismark) was not found next to this app or in PATH."));
    return;
  }
  const Metric *m = metricId.isEmpty() ? nullptr : findMetric(metricId);
  bool all = !m;
  QStringList args = {"--progress-json", "--cancel-on-stdin", "--frontend", "gui"};
  QString stressTime; /* "10m", "30m" or "until-stopped" */
  QStringList stressOpts;
  if (m && m->group == "stress") {
    /* Its length, and when to end it early: at the first wrong result, or at a temperature (remembered). */
    QDialog d(this);
    d.setWindowTitle(tr("Stress test"));
    auto *f = new QFormLayout(&d);
    f->addRow(text(tr("“%1”. The score is the speed in the 10th minute, so every length from 10 minutes on "
                      "compares; longer runs show how the speed holds up.").arg(esc(m->title)), 10));
    auto *length = new QComboBox;
    length->addItem(tr("10 minutes"), "10m");
    length->addItem(tr("30 minutes"), "30m");
    length->addItem(tr("Until I stop it"), "until-stopped");
    QSettings set;
    length->setCurrentIndex(std::max(0, length->findData(set.value("stress_time", "10m"))));
    f->addRow(tr("Length"), length);
    auto *stopErr = new QCheckBox(tr("Stop at the first wrong result"));
    stopErr->setChecked(set.value("stress_stop_on_error", true).toBool());
    f->addRow(QString(), stopErr);
    auto *maxTemp = new QComboBox;
    maxTemp->addItem(tr("No limit"), 0);
    for (int c : {85, 90, 95, 100}) maxTemp->addItem(tr("Stop at %1 °C").arg(c), c);
    maxTemp->setCurrentIndex(std::max(0, maxTemp->findData(set.value("stress_max_temp", 0))));
    f->addRow(tr("Temperature"), maxTemp);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    bb->button(QDialogButtonBox::Ok)->setText(tr("Start"));
    connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    f->addRow(bb);
    if (d.exec() != QDialog::Accepted) return;
    stressTime = length->currentData().toString();
    set.setValue("stress_time", stressTime);
    set.setValue("stress_stop_on_error", stopErr->isChecked());
    set.setValue("stress_max_temp", maxTemp->currentData());
    stressOpts = {"--stress-on-error", stopErr->isChecked() ? "stop" : "continue"};
    if (maxTemp->currentData().toInt() > 0) stressOpts << "--stress-max-temp" << maxTemp->currentData().toString();
  }
  if (m) {
    args << "--mode" << m->modes.join(",") << "--kernels" << m->kernels;
    if (m->group == "stress") {
      args << "--stress" << m->context.section(':', 1).replace('_', '-');
      if (!stressTime.isEmpty()) args << "--stress-time" << stressTime;
      args << stressOpts;
    }
  }
  if (all && !advanced_) { /* Simple: run what the Simple view shows */
    QStringList modes, kernels;
    for (const Metric &x : metrics())
      if (isSimple(x.id)) {
        for (const QString &md : x.modes)
          if (!modes.contains(md)) modes << md;
        if (!kernels.contains(x.kernels)) kernels << x.kernels;
      }
    args << "--mode" << modes.join(",") << "--kernels" << kernels.join(",");
  }
  if (quick) args << "--quick";
  /* Compile tests need a snapshot: ask when this run includes one. */
  auto runsKernel = [&](const QString &k) {
    if (m) return m->kernels == k;
    if (advanced_) return true;
    for (const Metric &x : metrics())
      if (isSimple(x.id) && x.kernels == k) return true;
    return false;
  };
  bool needK1 = runsKernel("K1"), needK1x = runsKernel("K1x");
  /* Power is measured only with administrator rights here: offer them once per run until told not to. */
  if (powerNeedsAdmin(info_) && !QSettings().value("power_admin_no_ask", false).toBool()) {
    QMessageBox box(QMessageBox::Question, tr("Prismark"),
                    tr("This computer reports the processor's power draw only to administrators. Restart Prismark "
                       "as administrator to record it with the results?"),
                    QMessageBox::NoButton, this);
    QPushButton *restart = box.addButton(tr("Restart as administrator"), QMessageBox::AcceptRole);
    QPushButton *without = box.addButton(tr("Run without power"), QMessageBox::RejectRole);
    box.addButton(QMessageBox::Cancel);
    auto *noAsk = new QCheckBox(tr("Do not ask again (the menu can still restart as administrator)"));
    box.setCheckBox(noAsk);
    box.setDefaultButton(restart);
    box.exec();
    if (noAsk->isChecked()) QSettings().setValue("power_admin_no_ask", true);
    if (box.clickedButton() == restart) {
      if (restartAsAdmin()) qApp->quit();
      return;
    }
    if (box.clickedButton() != without) return;
  }
  if ((needK1 || needK1x) && !confirmCompileTests(needK1, needK1x, all, args)) return;

  procOut_ = QDir(resultsDir()).filePath(QStringLiteral("prismark-%1.json").arg(QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss")));
  args << "-o" << procOut_;
  QString watch = input_->start();
  args << "--input-watch" << (watch.isEmpty() ? QStringLiteral("none") : watch);

  proc_ = new QProcess(this);
  proc_->setProcessChannelMode(QProcess::SeparateChannels);
  proc_->setStandardOutputFile(QProcess::nullDevice());
  connect(proc_, &QProcess::readyReadStandardError, this, &MainWindow::onRunOutput);
  connect(proc_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, &MainWindow::onRunFinished);
  connect(proc_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
    if (e == QProcess::FailedToStart) onRunFinished(-1, QProcess::CrashExit);
  });
  procBuf_.clear();
  procErrTail_.clear();
  phase_.clear();
  kernel_.clear();
  warnings_ = 0;
  part_ = parts_ = runStep_ = runSteps_ = 0;
  inputDuringQuiet_ = cancelling_ = false;
  stressOpen_ = stressTime == "until-stopped";
  fzCancel_->setText(stressOpen_ ? tr("Stop stress test") : tr("Cancel run"));
  runStart_ = phaseStart_ = QDateTime::currentDateTime();
  ring_->setProgress(0, 0, QStringLiteral("…"));
  fzState_->setText(tr("STARTING"));
  fzPhase_->setText(tr("Starting the runner"));
  fzMsg_->setText(QStringLiteral("%1 · %2").arg(m ? tr("%1 only").arg(m->title) : tr("All tests"),
                                                quick             ? tr("quick run")
                                                : stressOpen_     ? tr("until you stop it")
                                                : !stressTime.isEmpty() ? tr("%1 minutes").arg(stressTime.chopped(1))
                                                                        : tr("full run")));
  fzTime_->setText(tr("Started at %1").arg(QLocale().toString(runStart_.time(), QLocale::ShortFormat)));
  fzHands_->hide();
  fzStall_->hide();
  fzWarn_->clear();
  killTimer_->stop();
  stallTimer_->start(stallMinutes(QString(), QString()) * 60000);
  /* The machine is measured as it is configured: the app never changes power settings. */
  QStringList status;
  bool quiet = !m || m->modes.contains("cold_burst") || m->modes.contains("periodic");
  if (quiet)
    status << (watch.isEmpty() ? tr("Keyboard and mouse use cannot be detected here: please do not use them during "
                                    "the run.")
                               : tr("Keyboard and mouse use is detected and skips the tests that need a still "
                                    "machine."));
  fzStatus_->setText(status.join("<br>"));
  fzStatus_->setVisible(!status.isEmpty());
  measure_->adjustSize();
  measure_->move(geometry().center() - measure_->rect().center());
  hide(); /* only the small measuring window remains */
  measure_->show();
  measure_->raise();
  proc_->start(cli, args);
}

void MainWindow::onInputActivity() {
  if (!proc_ || cancelling_) return; /* after a cancel the runner's input is closed */
  proc_->write("input\n"); /* the runner skips the current test if it needs an idle machine */
  if (kQuietPhases.contains(phase_) && !inputDuringQuiet_) {
    inputDuringQuiet_ = true;
    fzHands_->setText(tr("Keyboard or mouse input detected: this test will be skipped. You can run it again later "
                         "from its card."));
  }
}

void MainWindow::onRunOutput() {
  procBuf_ += proc_->readAllStandardError();
  int nl;
  while ((nl = procBuf_.indexOf('\n')) >= 0) {
    QByteArray line = procBuf_.left(nl).trimmed();
    procBuf_.remove(0, nl + 1);
    QJsonObject ev = QJsonDocument::fromJson(line).object();
    if (ev.isEmpty()) {
      if (!line.isEmpty()) procErrTail_ = QString::fromUtf8(line);
      continue;
    }
    onRunEvent();
    /* Only a new phase or a warning changes the page; info events never redraw anything. */
    QString kind = ev["event"].toString();
    if (kind == "info" && ev["phase"].toString() == "run") {
      part_ = ev["step"].toInt(); /* shown with the next phase, which follows at once */
      parts_ = ev["steps"].toInt();
    } else if (kind == "info" && ev["phase"].toString() == "progress") {
      runStep_ = ev["step"].toInt(); /* likewise */
      runSteps_ = ev["steps"].toInt();
    } else if (kind == "phase") {
      QString phase = ev["phase"].toString(), k = ev["kernel"].toString();
      if (phase != phase_ || k != kernel_) phaseStart_ = QDateTime::currentDateTime();
      if (phase != phase_) {
        phase_ = phase;
        inputDuringQuiet_ = false;
        fzHands_->setText(tr("✋  Hands off — this test measures waits and wake-ups. Using the keyboard or mouse now "
                             "skips it."));
        fzHands_->setVisible(kQuietPhases.contains(phase) && !cancelling_);
      }
      kernel_ = k;
      fzMsg_->setText(esc(plainText(ev["message"].toString())));
      showPhase(ev["step"].toInt(), ev["steps"].toInt());
    } else if (kind == "warning") {
      fzWarn_->setText(tr("%1 warning(s); last: %2").arg(++warnings_).arg(esc(plainText(ev["message"].toString()))));
    }
  }
}

/* The ring shows the whole run: its centre counts the steps of every test together (the runner plans them before
   it starts), and it fills with the steps finished. Older runners without a plan: by parts, as far as known. */
void MainWindow::showPhase(int step, int steps) {
  bool done = phase_ == "done";
  double within = steps > 0 ? double(std::min(step > 0 ? step - 1 : 0, steps)) / steps : 0;
  if (done) ring_->setProgress(1, 1, QStringLiteral("✓"));
  else if (runSteps_ > 0) ring_->setProgress(runStep_ > 0 ? runStep_ - 1 : 0, runSteps_,
                                             QStringLiteral("%1/%2").arg(std::max(runStep_, 1)).arg(runSteps_));
  else if (parts_ > 0) ring_->setProgress(int(1000 * (part_ - 1 + within) / parts_), 1000,
                                          QStringLiteral("%1/%2").arg(part_).arg(parts_));
  else ring_->setProgress(0, 0, QStringLiteral("…"));

  fzState_->setText(cancelling_ ? (stressOpen_ ? tr("STOPPING") : tr("CANCELLING"))
                    : done      ? tr("COMPUTING RESULTS")
                    : phase_ == "cooldown" ? tr("WAITING")
                    : phase_ == "preflight" ? tr("PREPARING")
                                            : tr("MEASURING"));
  QString title = phaseTitle(phase_);
  if (!kernel_.isEmpty()) title += QStringLiteral(" · %1").arg(plainKernel(kernel_));
  fzPhase_->setText(title);
  QStringList when;
  if (parts_ > 0 && !done) when << tr("Part %1 of %2").arg(part_).arg(parts_);
  if (runSteps_ <= 0 && steps > 0 && !done) when << tr("step %1 of %2").arg(step).arg(steps);
  when << tr("this step since %1").arg(QLocale().toString(phaseStart_.time(), QLocale::ShortFormat));
  when << tr("run started at %1").arg(QLocale().toString(runStart_.time(), QLocale::ShortFormat));
  fzTime_->setText(when.join(QStringLiteral(" · ")) + "<br>" + esc(phaseHint(phase_, kernel_)));
}

/* Every event proves the runner is alive: hide a stall notice and restart the (silent) watchdog. */
void MainWindow::onRunEvent() {
  if (fzStall_->isVisible()) {
    fzStall_->hide();
    measure_->adjustSize();
  }
  if (cancelling_) killTimer_->start(60000); /* still answering: give it time to finish the partial result */
  else stallTimer_->start(stallMinutes(phase_, kernel_) * 60000);
}

void MainWindow::onStall() {
  if (!proc_ || cancelling_) return;
  int mins = stallMinutes(phase_, kernel_);
  fzState_->setText(tr("NO RESPONSE"));
  fzStall_->setText(tr("No progress report for %1 minutes, longer than this step normally takes. The runner "
                       "process is still alive, so it has not crashed, but it may be stuck. If this persists, press "
                       "Cancel run: the part measured so far is kept.")
                        .arg(mins));
  fzStall_->show();
  measure_->adjustSize();
}

void MainWindow::cancelRun() {
  if (!proc_ || cancelling_) return;
  if (stressOpen_) { /* the normal end of a stress test without a set length: a score needs 10 minutes */
    double mins = phase_ == "stress" ? phaseStart_.secsTo(QDateTime::currentDateTime()) / 60.0 : 0;
    if (mins < 10 &&
        QMessageBox::question(measure_, tr("Stop the stress test?"),
                              tr("It has run %1 of the 10 minutes a score needs. Stopping now keeps the curve of "
                                 "speed, clock and temperature, without a score.")
                                  .arg(QString::number(std::floor(mins)))) != QMessageBox::Yes)
      return;
  }
  cancelling_ = true;
  stallTimer_->stop();
  fzStall_->hide();
  fzHands_->hide();
  fzState_->setText(stressOpen_ ? tr("STOPPING") : tr("CANCELLING"));
  fzPhase_->setText(stressOpen_ ? tr("Stopping the stress test") : tr("Cancelling"));
  fzMsg_->setText(stressOpen_ ? tr("Stopping cleanly and saving the result.")
                              : tr("Stopping cleanly and saving the part measured so far."));
  proc_->write("cancel\n"); /* the runner stops cleanly and keeps the partial result */
  proc_->closeWriteChannel();
  /* Ended by force only if the runner stops answering; every event it still sends restarts this. */
  killTimer_->start(60000);
}

void MainWindow::onRunFinished(int code, QProcess::ExitStatus status) {
  if (!proc_) return;
  /* The last lines (an error message among them) may still be unread. */
  onRunOutput();
  if (!procBuf_.trimmed().isEmpty()) procErrTail_ = QString::fromUtf8(procBuf_.trimmed());
  procBuf_.clear();
  stallTimer_->stop(); /* after the drain above, which restarts them */
  killTimer_->stop();
  input_->stop();
  proc_->deleteLater();
  proc_ = nullptr;
  measure_->hide();
  show();
  raise();
  activateWindow();
  QStringList skipped;
  if (QFileInfo::exists(procOut_) && loadFile(procOut_, false)) {
    for (const RunSummary &r : runs_)
      if (r.file == procOut_) selected_ = r.id;
    QFile f(procOut_);
    if (f.open(QIODevice::ReadOnly))
      for (const QJsonValue &v : QJsonDocument::fromJson(f.readAll()).object()["skipped_modes"].toArray())
        skipped << phaseTitle(v.toString());
  }
  refresh();
  bool saved = QFileInfo::exists(procOut_);
  QString reason = procErrTail_;
  if (reason.startsWith(QStringLiteral("prismark: "))) reason.remove(0, 10);
  if (code == -1) {
    QMessageBox::warning(this, tr("Prismark"), tr("Could not start the runner: %1").arg(cliPath()));
  } else if (code == 4 && status == QProcess::NormalExit) { /* the runner's status for a failed stress test */
    QMessageBox::warning(this, tr("Stress test failed"),
                         tr("The stress test found wrong results: this computer is not stable under this load. The "
                            "test page shows when and on which CPU."));
  } else if (cancelling_ && stressOpen_ && code == 0 && status == QProcess::NormalExit) {
    /* stopped as intended: the result is on the page */
  } else if (cancelling_) {
    QMessageBox::information(this, tr("Run cancelled"),
                             saved ? tr("The run was cancelled. The part measured so far was saved and is shown.")
                                   : tr("The run was cancelled. The runner did not stop in time and was ended, so "
                                        "nothing was saved."));
  } else if (status != QProcess::NormalExit) {
    QMessageBox::warning(this, tr("Prismark"),
                         tr("The runner stopped unexpectedly (it crashed)%1. %2")
                             .arg(reason.isEmpty() ? QString() : ": " + reason,
                                  saved ? tr("A partial result was saved.") : tr("Nothing was saved.")));
  } else if (code != 0) {
    QMessageBox::information(this, tr("Prismark"),
                             tr("The run did not complete%1.%2")
                                 .arg(reason.isEmpty() ? QString() : ": " + reason,
                                      saved ? tr(" Its partial result was saved.") : QString()));
  } else if (!skipped.isEmpty()) {
    QMessageBox::information(this, tr("Some tests were skipped"),
                             tr("Keyboard or mouse input during these tests, which need an idle machine: %1.\n\n"
                                "Their results were discarded. Run them again from their cards (Responsiveness, "
                                "Memory & timing) and leave the machine alone meanwhile.")
                                 .arg(skipped.join(", ")));
  }
}

/* ---------- other actions ---------- */

void MainWindow::openResult() {
  QStringList files = QFileDialog::getOpenFileNames(this, tr("Open Prismark results"), QDir::homePath(), tr("Results (*.json)"));
  QString last;
  for (const QString &f : files) {
    QString name = QFileInfo(f).fileName();
    QString dest = QDir(resultsDir()).filePath(name.startsWith("prismark-") ? name : "prismark-" + name);
    if (QFileInfo(f).absoluteFilePath() != QFileInfo(dest).absoluteFilePath()) { /* keep a copy with the saved runs */
      QFile::remove(dest);
      if (QFile::copy(f, dest)) giveToUser(dest);
    }
    if (loadFile(dest, false)) last = runs_.last().id;
    else QMessageBox::warning(this, tr("Prismark"), tr("%1 is not a Prismark result file.").arg(f));
  }
  if (!last.isEmpty()) selected_ = last;
  refresh();
}

void MainWindow::compareRuns() {
  QVector<const RunSummary *> real;
  for (const RunSummary &r : runs_)
    if (!r.placeholder && !r.file.isEmpty()) real.push_back(&r);
  if (real.size() < 2) {
    QMessageBox::information(this, tr("Compare"), tr("Comparing needs two measured results (placeholders cannot be compared)."));
    return;
  }
  QDialog d(this);
  d.setWindowTitle(tr("Compare two results"));
  auto *f = new QFormLayout(&d);
  f->setContentsMargins(24, 20, 24, 18);
  auto *a = new QComboBox, *b = new QComboBox;
  for (const RunSummary *r : real) {
    QString t = r->name + (r->date.isEmpty() ? "" : " · " + r->date) + (r->reference ? tr(" (reference)") : "");
    a->addItem(t, r->file);
    b->addItem(t, r->file);
  }
  a->setCurrentIndex(real.size() - 1);
  b->setCurrentIndex(real.size() - 2);
  f->addRow(tr("A"), a);
  f->addRow(tr("B"), b);
  f->addRow(text(tr("Shows, test by test, how many times faster A is than B (with the likely range), and a few "
                    "workload mixes (“profiles”). Runs measured with different settings are refused rather than "
                    "compared."),
                 9.5, QFont::Normal, theme().muted));
  auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
  connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
  connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
  f->addRow(bb);
  if (d.exec() != QDialog::Accepted) return;
  QProcess p;
  p.start(cliPath(), {"compare", "--detail", a->currentData().toString(), b->currentData().toString()});
  p.waitForFinished(120000);
  QDialog out(this);
  out.setWindowTitle(tr("Comparison"));
  out.resize(940, 620);
  auto *l = new QVBoxLayout(&out);
  auto *view = new QPlainTextEdit(QString::fromUtf8(p.readAllStandardOutput() + p.readAllStandardError()));
  view->setReadOnly(true);
  view->setFont(monoFont(9.5));
  l->addWidget(view);
  auto *close = new QDialogButtonBox(QDialogButtonBox::Close);
  connect(close, &QDialogButtonBox::rejected, &out, &QDialog::reject);
  l->addWidget(close);
  out.exec();
}

void MainWindow::prepareCompileTests() {
  QStringList ignored;
  CompileStatus st = compileStatus(info_, QSettings().value("k1_data").toString());
  if (!st.missingPrepare.isEmpty()) {
    confirmCompileTests(true, true, false, ignored); /* explains what to install */
    return;
  }
  if (!st.snapshot.isEmpty() &&
      QMessageBox::question(this, tr("Compile tests"), tr("A snapshot is already prepared at %1. Prepare it again?").arg(st.snapshot)) !=
          QMessageBox::Yes)
    return;
  PrepareDialog pd(this);
  pd.exec();
  refresh();
}

void MainWindow::chooseSnapshot() {
  QSettings s;
  QString dir = QFileDialog::getExistingDirectory(this, tr("Compile-test snapshot (made by Prepare compile tests)"),
                                                  s.value("k1_data", defaultSnapshotDir()).toString());
  if (dir.isEmpty()) return;
  if (!QFileInfo::exists(dir + "/manifest.json")) {
    QMessageBox::warning(this, tr("Compile-test snapshot"), tr("%1 is not a prepared snapshot (it has no manifest.json).").arg(dir));
    return;
  }
  s.setValue("k1_data", dir);
}

void MainWindow::showGlossary() {
  QDialog d(this);
  d.setWindowTitle(tr("What the terms mean"));
  d.resize(720, 640);
  auto *l = new QVBoxLayout(&d);
  l->setContentsMargins(24, 20, 24, 16);
  l->addWidget(text(tr("What the terms mean"), 14, QFont::Bold));
  auto *body = text(glossaryHtml(), 9.5);
  body->setTextInteractionFlags(Qt::TextSelectableByMouse);
  auto *scroll = new QScrollArea;
  scroll->setWidget(body);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  l->addWidget(scroll, 1);
  auto *bb = new QDialogButtonBox(QDialogButtonBox::Close);
  connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
  l->addWidget(bb);
  d.exec();
}

void MainWindow::showDataFolders() {
  QMessageBox box(this);
  box.setWindowTitle(tr("Data folders"));
  box.setTextFormat(Qt::RichText);
  box.setText(tr("<b>Your saved runs</b><br>%1<br><br><b>Your reference systems</b> — result files, or placeholder "
                 "files as described in references/README.md, added to the built-in ones<br>%2<br><br><b>Compile-test snapshot</b><br>%3<br><br><b>Runner</b><br>%4")
                  .arg(esc(resultsDir()), esc(referencesDir()), esc(defaultSnapshotDir()),
                       esc(cliPath().isEmpty() ? tr("not found") : cliPath())));
  QPushButton *open = box.addButton(tr("Open results folder"), QMessageBox::ActionRole);
  box.addButton(QMessageBox::Close);
  box.exec();
  if (box.clickedButton() == open) QDesktopServices::openUrl(QUrl::fromLocalFile(resultsDir()));
}

void MainWindow::showAbout() {
  QMessageBox::about(this, tr("About Prismark"),
                     tr("<b>Prismark %1</b><br>A CPU benchmark that treats every vendor the same. Each test is "
                        "reported and ranked on its own; there is never one overall score.<br><br>Reference systems "
                        "tagged PLACEHOLDER are example values, not measurements.<br><br>Apache License 2.0. Inter "
                        "typeface: SIL Open Font License 1.1.")
                         .arg(QCoreApplication::applicationVersion()));
}

/* "amd-ryzen-7-8845hs" from "AMD Ryzen 7 8845HS w/ Radeon 780M Graphics": the name offered for a reference (as
   the command line's references add offers it). */
static QString suggestReferenceName(QString model) {
  for (const char *cut : {" w/ ", " with ", " @ ", " CPU"}) {
    int at = model.indexOf(QLatin1String(cut));
    if (at >= 0) model.truncate(at);
  }
  model.remove(QStringLiteral("(R)"), Qt::CaseInsensitive).remove(QStringLiteral("(TM)"), Qt::CaseInsensitive);
  QString name = model.toLower().replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")), QStringLiteral("-"));
  name = name.left(48).remove(QRegularExpression(QStringLiteral("^-+|-+$")));
  return name.isEmpty() ? QStringLiteral("my-computer") : name;
}

void MainWindow::saveAsReference() {
  const RunSummary *r = selectedRun();
  if (!r || r->reference) return;
  QString file = r->file, model = r->machine["cpu"].toObject()["model"].toString(r->name);
  bool quick = r->quick;
  static const QRegularExpression valid(QStringLiteral("^[A-Za-z0-9_-][A-Za-z0-9._-]{0,79}$"));
  QString name = suggestReferenceName(model);
  for (;;) {
    bool ok = false;
    name = QInputDialog::getText(
               this, tr("Save as reference"),
               tr("Keep this run as a reference system: it stays in the rankings next to the bundled ones, and "
                  "later runs can be compared with it (also from the command line: prismark compare latest NAME).%1"
                  "\n\nName (letters, digits, - _ .):")
                   .arg(quick ? tr("\n\nThis is a quick run: comparisons with it are rough.") : QString()),
               QLineEdit::Normal, name, &ok)
               .trimmed();
    if (!ok) return;
    if (!valid.match(name).hasMatch()) {
      QMessageBox::information(this, tr("Save as reference"),
                               tr("“%1” cannot be a reference name. Use letters, digits, “-”, “_” and “.”.").arg(name));
      continue;
    }
    QString dest = QDir(referencesDir()).filePath(name + ".json");
    if (QFileInfo::exists(dest)) {
      if (QMessageBox::question(this, tr("Save as reference"),
                                tr("You already have a reference system “%1”. Replace it?").arg(name)) != QMessageBox::Yes)
        continue;
      QFile::remove(dest);
    }
    if (!QFile::copy(file, dest)) {
      QMessageBox::warning(this, tr("Prismark"), tr("Could not save %1.").arg(dest));
      return;
    }
    giveToUser(dest);
    loadFile(dest, true);
    selected_ = QStringLiteral("reference-") + name; /* show the new reference */
    refresh();
    return;
  }
}

void MainWindow::removeReference() {
  const RunSummary *r = selectedRun();
  if (!r || !r->reference) return;
  QString file = r->file, name = QFileInfo(file).completeBaseName();
  if (QMessageBox::question(this, tr("Remove reference"),
                            tr("Remove your reference system “%1” from the rankings? Its file goes to the trash.\n%2")
                                .arg(name, file)) != QMessageBox::Yes)
    return;
  if (!QFile::moveToTrash(file) && !QFile::remove(file)) {
    QMessageBox::warning(this, tr("Prismark"), tr("Could not remove %1.").arg(file));
    return;
  }
  loadAll(); /* a bundled reference of the same name, which this one replaced, comes back */
  selected_.clear();
  refresh();
}

void MainWindow::removeSelected() {
  const RunSummary *r = selectedRun();
  if (!r || r->reference) return;
  if (QMessageBox::question(this, tr("Move to trash"), tr("Move this result to the trash?\n%1").arg(r->file)) != QMessageBox::Yes)
    return;
  if (!QFile::moveToTrash(r->file) && !QFile::remove(r->file)) {
    QMessageBox::warning(this, tr("Prismark"), tr("Could not remove %1.").arg(r->file));
    return;
  }
  QString id = r->id;
  runs_.erase(std::remove_if(runs_.begin(), runs_.end(), [&](const RunSummary &x) { return x.id == id && !x.reference; }),
              runs_.end());
  selected_.clear();
  refresh();
}
