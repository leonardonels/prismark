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
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>

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
      {"preflight", "Checking the machine is idle"}, {"st_burst", "Single core, burst"},
      {"st_sustained", "Single core, sustained"},    {"mc_threaded", "Multi core, threaded"},
      {"mc_instances", "Multi core, instances"},     {"cold_burst", "Cold start"},
      {"periodic", "Wake-up lateness"},              {"cooldown", "Cooling down"},
      {"done", "Computing statistics"}};
  return names.value(phase, phase);
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
  m->addAction(tr("Open result…"), this, &MainWindow::openResult, QKeySequence::Open);
  m->addAction(tr("Compare two results…"), this, &MainWindow::compareRuns);
  m->addSeparator();
  m->addAction(tr("Prepare compile tests…"), this, &MainWindow::prepareCompileTests);
  m->addAction(tr("Use another compile-test snapshot…"), this, &MainWindow::chooseSnapshot);
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
  m->addAction(tr("What do these terms mean?"), this, &MainWindow::showGlossary, QKeySequence::HelpContents);
  m->addAction(tr("Data folders…"), this, &MainWindow::showDataFolders);
  m->addAction(tr("About Prismark"), this, &MainWindow::showAbout);
  m->addSeparator();
  m->addAction(tr("Quit"), qApp, &QApplication::quit, QKeySequence::Quit);
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
    if (i < 0 || i >= shown.size()) return;
    group_ = shown[i];
    metric_ = firstVisible(group_);
    refresh();
  });
  head->addWidget(groups_);
  head->addStretch();
  quick_ = new Toggle(tr("Quick"));
  quick_->setToolTip(tr("On: quick runs, a few minutes, for a first look (not comparable).\n"
                        "Off: full runs, accurate and comparable; they can take hours."));
  quick_->setChecked(QSettings().value("quick", true).toBool());
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
    if (QMessageBox::question(measure_, tr("Cancel the run?"),
                              tr("Closing this window cancels the run. The partial result is kept.")) != QMessageBox::Yes)
      return false;
    cancelRun();
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
  tv->addWidget(text(tr("MEASURING"), 9, QFont::DemiBold, theme().accent));
  fzPhase_ = text(QString(), 19, QFont::Bold);
  tv->addWidget(fzPhase_);
  fzMsg_ = text(QString(), 10, QFont::Normal, theme().muted);
  tv->addWidget(fzMsg_);
  tv->addStretch();
  top->addLayout(tv, 1);
  l->addLayout(top);
  fzHands_ = text(QString(), 10, QFont::Medium, theme().warnText);
  fzHands_->setStyleSheet(QStringLiteral("color:%1; background:%2; border-radius:10px; padding:10px 14px;")
                              .arg(theme().warnText.name(), theme().warnBg.name()));
  l->addWidget(fzHands_);
  l->addWidget(text(tr("The benchmark runs as a separate process and this window stays still, so it does not wake "
                       "the CPU. Leave the machine alone until the run ends."),
                    9.5, QFont::Normal, theme().muted));
  fzStatus_ = text(QString(), 8.5, QFont::Normal, theme().faint);
  l->addWidget(fzStatus_);
  fzWarn_ = text(QString(), 9, QFont::Normal, theme().bad);
  l->addWidget(fzWarn_);
  auto *cancel = new QPushButton(tr("Cancel run"));
  cancel->setCursor(Qt::PointingHandCursor);
  connect(cancel, &QPushButton::clicked, this, &MainWindow::cancelRun);
  l->addWidget(cancel, 0, Qt::AlignLeft);
  return win;
}


/* ---------- data ---------- */

QString MainWindow::resultsDir() const {
  QString d = dataDir() + "/results/runs"; /* shared with the command line, which saves here too */
  QDir().mkpath(d);
  return d;
}

QString MainWindow::referencesDir() const {
  QString d = dataDir() + "/results/references";
  QDir().mkpath(d);
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
  QStringList names;
  for (const QString &id : shown)
    for (const Group &g : groups())
      if (g.id == id) names << QString(g.label).replace("&&", "&");
  groups_->setItems(names);
  groups_->setCurrent(int(shown.indexOf(group_)));
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
    const RunSummary *latest = nullptr;
    for (const RunSummary &r : runs_)
      if (!r.reference && r.metrics.contains(m.id)) latest = &r;
    auto *c = new TestCard(m.id);
    QString note = tr("No result yet");
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
    connect(c, &TestCard::runRequested, this, [this](const QString &id) { runTest(id, quick_->isChecked()); });
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
  runAll_->setToolTip(advanced_ ? tr("Runs every test.")
                                : tr("Runs the tests of the Simple view. Switch to Advanced to run every test."));
  for (QAbstractButton *b : cards_.buttons())
    static_cast<TestCard *>(b)->setRunLabel(quick ? tr("Run quick") : tr("Run full"));
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
  const RunSummary *latest = nullptr;
  for (const RunSummary &r : runs_)
    if (!r.reference && r.metrics.contains(m->id)) latest = &r;
  for (const RunSummary &r : runs_) {
    if (!r.metrics.contains(m->id)) continue;
    if (!r.reference && !allRuns_->isChecked() && &r != latest) continue;
    RankRow row;
    row.runId = r.id;
    row.yours = !r.reference;
    row.name = r.reference ? r.name : tr("This PC · %1").arg(r.name);
    row.detail = r.reference ? QString("%1 · %2").arg(r.machine["os"].toString(), r.machine["isa"].toString()) : r.date;
    if (m->group == "isa" && !isaPair(r.tiers).isEmpty()) row.detail = isaPair(r.tiers) + " · " + row.detail;
    if (r.placeholder) row.tags << "placeholder";
    if (r.reference && !r.placeholder) row.tags << "reference";
    if (&r == latest && allRuns_->isChecked()) row.tags << "latest";
    if (r.quick) row.tags << "quick";
    if (!r.complete) row.tags << "partial";
    row.value = r.metrics[m->id];
    if (row.value.unsettled && !r.quick) row.tags << "did not settle";
    rows.push_back(row);
  }
  bool shown = std::any_of(rows.begin(), rows.end(), [&](const RankRow &x) { return x.runId == selected_; });
  if (!shown && latest) { /* follow the test: select your result that this ranking shows */
    selected_ = latest->id;
    QTimer::singleShot(0, this, [this] { refreshDetails(); });
  }
  ranking_->setRows(rows, m->higherBetter, m->unit, selected_);
  refreshContext();
}

void MainWindow::refreshContext() {
  while (QLayoutItem *it = contextLayout_->takeAt(0)) {
    if (it->widget()) it->widget()->deleteLater();
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
    ChartLine l;
    l.name = tr("speed-up");
    for (const auto &s : r->scaling)
      if (s.kernel == arg) l.pts.push_back({double(s.n), s.s.v, s.s.lo, s.s.hi});
    std::sort(l.pts.begin(), l.pts.end(), [](const auto &a, const auto &b) { return a[0] < b[0]; });
    if (l.pts.size() < 2) return;
    show(tr("Speed-up with more threads"),
         tr("For %1: how many times faster than a single thread. The dashed line is perfect scaling (8 threads = 8× "
            "faster); real programs fall below it as threads wait for each other, for memory, or for power.").arg(esc(who)));
    auto *ch = new ChartView;
    ch->setData({l}, false, tr("threads"), 0, NAN, true);
    contextLayout_->addWidget(ch);
  } else if (kind == "ratio") {
    ChartLine l = byThreads(arg, QString());
    if (l.pts.isEmpty()) return;
    show(tr("Time added by the build tools, by number of threads"),
         tr("For %1: time of the full build divided by the time of compiling the same files in memory. 1.0 means "
            "the tools add nothing; 2.0 means the build takes twice as long as the compiling alone.").arg(esc(who)));
    auto *ch = new ChartView;
    ch->setData({l}, false, tr("threads"), 0, NAN, false);
    contextLayout_->addWidget(ch);
  } else if (kind == "isa") {
    QString pair = isaPair(r->tiers);
    if (pair.isEmpty()) return;
    QString base = r->tiers["baseline"].toObject()["level"].toString(),
            max = r->tiers["max"].toObject()["level"].toString();
    show(tr("Instruction sets compared"),
         max.isEmpty() ? tr("For %1: this processor has nothing newer than %2 (%3), so there was no gain to measure.")
                             .arg(esc(who), esc(isaName(base)), esc(base))
                       : tr("For %1: <b>%2</b> (%3), the common baseline, against <b>%4</b> (%5), the newest this "
                            "processor supports. Every row of the ranking shows the pair its computer compared.")
                             .arg(esc(who), esc(isaName(base)), esc(base), esc(isaName(max)), esc(max)));
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
          if (gr.id == "isa" && !isaPair(r->tiers).isEmpty()) label += " · " + isaPair(r->tiers);
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
      tip += tr("\nThe speed was still changing when the 10-minute limit was reached.");
    }
    val->setToolTip(tip);
    tg->addWidget(val, row++, 1);
  }
  if (!row) tg->addWidget(text(tr("No results in this run."), 9.5, QFont::Normal, t.muted), 0, 0);
  detailsLayout_->addWidget(tests);
  if (hiddenResults)
    detailsLayout_->addWidget(text(hiddenResults == 1 ? tr("1 more result in Advanced.") : tr("%1 more results in Advanced.").arg(hiddenResults),
                                   9, QFont::Normal, t.faint));

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
    QJsonObject max = r->tiers["max"].toObject();
    kv(tgl, trow, tr("Instruction sets"),
       tr("%1 (%2) → %3").arg(isaName(r->tiers["baseline"].toObject()["level"].toString()),
                              r->tiers["baseline"].toObject()["level"].toString(),
                              max["level"].toString().isEmpty() ? tr("none newer")
                                                                : QStringLiteral("%1 (%2)").arg(isaName(max["level"].toString()), max["level"].toString())));
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

  if (!r->reference) {
    auto *w = new QWidget;
    auto *btns = new QHBoxLayout(w);
    btns->setContentsMargins(0, 12, 0, 0);
    auto *show = new QPushButton(tr("Show file"));
    show->setObjectName("ghost");
    connect(show, &QPushButton::clicked, this, [this] {
      if (const RunSummary *s = selectedRun()) QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(s->file).absolutePath()));
    });
    auto *rm = new QPushButton(tr("Move to trash"));
    rm->setObjectName("ghost");
    connect(rm, &QPushButton::clicked, this, &MainWindow::removeSelected);
    btns->addWidget(show);
    btns->addWidget(rm);
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
    auto row = [&](bool ok, const QString &what) {
      l->addWidget(text(QStringLiteral("<span style='color:%1'>%2</span>&nbsp; %3")
                            .arg((ok ? theme().good : theme().bad).name(), ok ? "✓" : "✗", esc(what)),
                        10));
    };
    row(!st.snapshot.isEmpty(), st.snapshot.isEmpty() ? tr("Snapshot of the LLVM sources to compile — not prepared yet")
                                                      : tr("Snapshot prepared: %1").arg(st.snapshot));
    row(st.missingBuild.isEmpty(), st.missingBuild.isEmpty() ? tr("Build tools for Full build (clang 19, lld, cmake, ninja)")
                                                             : tr("Missing tools: %1").arg(st.missingBuild.join(", ")));
    row(st.k1Built, st.k1Built ? tr("“Compiling code” is built into this Prismark")
                               : tr("“Compiling code” is not built into this Prismark (needs the Clang 19 libraries)"));
    QString cmds = installInstructions(st);
    bool needInstall = !st.missingPrepare.isEmpty() || !st.k1Built;
    if (needInstall && !cmds.isEmpty()) {
      l->addWidget(text(tr("To install what is missing, run in a terminal:"), 10, QFont::Medium));
      auto *box = new QPlainTextEdit(cmds);
      box->setReadOnly(true);
      box->setFont(monoFont(9));
      box->setFixedHeight(110);
      box->setLineWrapMode(QPlainTextEdit::NoWrap);
      l->addWidget(box);
    }
    if (st.snapshot.isEmpty())
      l->addWidget(text(tr("Preparing the snapshot downloads pinned sources (about 160 MB), builds generated files "
                           "once and measures every unit: 20–60 minutes, about 4 GB of disk while it runs."),
                        9.5, QFont::Normal, theme().muted));
    auto *bb = new QDialogButtonBox;
    QPushButton *prep = nullptr, *without = nullptr, *copy = nullptr;
    if (st.snapshot.isEmpty() && st.missingPrepare.isEmpty()) {
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
  if (m) {
    if (m->group == "isa") args << "--mode" << "st_sustained" << "--kernels" << "K2,K3,K4,K8";
    else {
      args << "--mode" << m->modes.join(",") << "--kernels" << m->kernels;
      if (m->modes.contains("st_sustained")) args << "--no-isa-uplift";
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
    if (!visibleGroups().contains("isa")) args << "--no-isa-uplift"; /* no New instructions test to fill */
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
  warnings_ = 0;
  inputDuringQuiet_ = false;
  ring_->setProgress(0, 0, QStringLiteral("…"));
  fzPhase_->setText(tr("Starting"));
  fzMsg_->setText(QStringLiteral("%1 · %2").arg(m ? tr("%1 only").arg(m->name) : tr("All tests"),
                                                quick ? tr("quick run") : tr("full run")));
  fzHands_->hide();
  fzWarn_->clear();
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
  if (!proc_) return;
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
    /* Only a new phase or a warning changes the page; info events never redraw anything. */
    QString kind = ev["event"].toString();
    if (kind == "phase") {
      QString phase = ev["phase"].toString(), k = ev["kernel"].toString();
      if (phase != phase_) {
        phase_ = phase;
        inputDuringQuiet_ = false;
        fzHands_->setText(tr("✋  Hands off — this test measures waits and wake-ups. Using the keyboard or mouse now "
                             "skips it."));
        fzHands_->setVisible(kQuietPhases.contains(phase));
      }
      int steps = ev["steps"].toInt(), step = ev["step"].toInt();
      ring_->setProgress(step, steps, steps ? QStringLiteral("%1/%2").arg(step).arg(steps) : QStringLiteral("…"));
      fzPhase_->setText(phaseTitle(phase) + (k.isEmpty() ? QString() : QStringLiteral(" · %1").arg(plainKernel(k))));
      fzMsg_->setText(esc(plainText(ev["message"].toString())));
    } else if (kind == "warning") {
      fzWarn_->setText(tr("%1 warning(s); last: %2").arg(++warnings_).arg(esc(plainText(ev["message"].toString()))));
    }
  }
}

void MainWindow::cancelRun() {
  if (!proc_) return;
  fzPhase_->setText(tr("Cancelling"));
  fzMsg_->setText(tr("Stopping cleanly; the partial result is kept and power settings are restored."));
  proc_->write("cancel\n"); /* the runner stops cleanly and keeps the partial result */
  proc_->closeWriteChannel();
  QTimer::singleShot(20000, this, [this] {
    if (proc_) proc_->kill();
  });
}

void MainWindow::onRunFinished(int code, QProcess::ExitStatus status) {
  if (!proc_) return;
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
  if (code == -1) {
    QMessageBox::warning(this, tr("Prismark"), tr("Could not start the runner: %1").arg(cliPath()));
  } else if (status != QProcess::NormalExit || code != 0) {
    QMessageBox::information(this, tr("Prismark"),
                             tr("The run did not complete%1.%2")
                                 .arg(procErrTail_.isEmpty() ? QString() : ": " + procErrTail_,
                                      QFileInfo::exists(procOut_) ? tr(" Its partial result was saved.") : QString()));
  } else if (!skipped.isEmpty()) {
    QMessageBox::information(this, tr("Some tests were skipped"),
                             tr("Keyboard or mouse input during these tests, which need an idle machine: %1.\n\n"
                                "Their results were discarded. Run them again from their cards (Responsiveness, "
                                "Memory & jitter) and leave the machine alone meanwhile.")
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
      QFile::copy(f, dest);
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
  p.start(cliPath(), {"compare", a->currentData().toString(), b->currentData().toString()});
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
