/* Copyright 2026 The Prismark Authors. Apache-2.0. */
#include "mainwindow.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QDate>
#include <QDateTime>
#include <QDebug>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLocale>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QScreen>
#include <QScrollArea>
#include <QSettings>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>
#include <array>
#include <cmath>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

#include "inputwatch.h"
#include "setup.h"
#include "stub.h"
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
  auto *l = text(esc(s), 8, QFont::Normal, theme().muted);
  l->setFont(labelFont(8));
  l->setContentsMargins(0, 14, 0, 4);
  return l;
}

/* A square tag in mono capitals, e.g. "quick" or "✓ pinning". */
QLabel *pill(const QString &s, const QColor &fg, const QColor &bg) {
  auto *l = new QLabel(s);
  l->setFont(labelFont(7.5));
  l->setStyleSheet(QStringLiteral("color:%1; background:%2; padding:4px 9px;").arg(fg.name(), bg.name()));
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

/* The logo and the name on a volt tile, in both themes: the head of every page. */
class Brand : public QWidget {
 public:
  Brand() {
    setFixedHeight(68);
    setFixedWidth(16 + 36 + 10 + QFontMetrics(displayFont(12.5, QFont::ExtraBold, 125)).horizontalAdvance(kName) + 18);
  }

 protected:
  void paintEvent(QPaintEvent *) override {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), theme().accent);
    paintLogo(p, QRectF(16, 16, 36, 36));
    p.setPen(theme().onAccent);
    p.setFont(displayFont(12.5, QFont::ExtraBold, 125));
    p.drawText(QRectF(62, 0, width() - 62, height()), Qt::AlignVCenter | Qt::AlignLeft, kName);
  }

 private:
  const QString kName = QStringLiteral("PRISMARK");
};

/* A panel that can carry the pass's printed dots (dynamic property "dots"), as on the phones. */
class PrintedFrame : public QFrame {
 protected:
  void paintEvent(QPaintEvent *e) override {
    QFrame::paintEvent(e);
    if (!property("dots").toBool()) return;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(92, 128, 255));
    for (int x = 12; x < width(); x += 24)
      for (int y = 12; y < height(); y += 24) p.drawEllipse(QPointF(x, y), 1.2, 1.2);
  }
};

/* Puts widgets just added under root in place at once. Qt would show them at the next turn of the event loop, and
   for that one turn the column stands empty. */
void settle(QWidget *root) {
  for (QWidget *c : root->findChildren<QWidget *>())
    if (!c->testAttribute(Qt::WA_WState_ExplicitShowHide)) c->show();
  if (root->layout()) root->layout()->activate();
}

QFrame *hairline() {
  auto *f = new QFrame;
  f->setFixedHeight(1);
  f->setStyleSheet(QStringLiteral("background:%1; border:none;").arg(theme().border.name()));
  return f;
}

QPushButton *loudButton(const QString &label, const QString &object = QStringLiteral("primary")) {
  auto *b = new QPushButton(label);
  b->setObjectName(object);
  b->setCursor(Qt::PointingHandCursor);
  b->setFont(displayFont(10.5, QFont::ExtraBold, 125));
  b->setFixedHeight(68);
  return b;
}

/* The processor's name without the marks and words every processor has, as on the phones. */
QString shortCpu(QString name) {
  for (const char *junk : {"(R)", "(TM)", "AMD ", "Intel ", " Processor", "Genuine ", "Apple "}) name.remove(QString::fromLatin1(junk));
  name.replace(QStringLiteral("Ryzen Threadripper PRO"), QStringLiteral("Threadripper"));
  name.remove(QRegularExpression(QStringLiteral(" w/ .*$| \\d+-Cores?.*$| CPU.*$")));
  return name.simplified();
}

QString prettyOs(const QString &os) {
  static const QMap<QString, QString> names = {{"macos", "macOS"}, {"windows", "Windows"}, {"linux", "Linux"},
                                               {"ios", "iOS"},     {"ipados", "iPadOS"},   {"android", "Android"}};
  return names.value(os, os);
}

QString prettyIsa(const QString &isa) {
  return isa == "aarch64" ? QStringLiteral("ARM64") : isa == "x86_64" ? QStringLiteral("x86-64") : isa;
}

/* Empties a layout rebuilt in place; the widgets go at the next turn of the event loop. */
void clearLayout(QLayout *l) {
  while (QLayoutItem *it = l->takeAt(0)) {
    if (it->widget()) {
      it->widget()->hide();
      it->widget()->deleteLater();
    }
    if (it->layout()) clearLayout(it->layout());
    delete it;
  }
}

/* The small window shown alone during a run; closing it asks to cancel. */
class MeasureWindow : public QWidget {
 public:
  std::function<bool()> onClose; /* returns true when the window may close */

 protected:
  /* Its texts change with each step and wrap at its fixed width: the window takes the height they need, so none is
     cut off. Only when the layout changes, which is when a new step begins. */
  bool event(QEvent *e) override {
    bool done = QWidget::event(e);
    if (e->type() == QEvent::LayoutRequest && layout()) {
      int h = layout()->totalHeightForWidth(width());
      if (h > 0 && h != height()) setFixedHeight(h);
    }
    return done;
  }
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
  ch->setData(lines, xLabel, y0, y0 + 4 * step, false);
  ch->setYUnit(unit);
  ch->setAspect(0.3, xLabel.isEmpty() ? 150 : 170);
  to->addWidget(ch);
}

/* No event for this long: something is probably wrong. Above the longest silent step of each phase. */
int stallMinutes(const QString &phase, const QString &kernel) {
  if (kernel == "K1x") return 20;
  return 8;
}

/* The runner found an energy meter (package power) that only administrators may read: Windows' EMI, or on Linux
   the RAPL counter, root-only since Linux 5.10 (it can leak secrets to unprivileged code: PLATYPUS). */
bool powerNeedsAdmin(const QJsonObject &info) {
  const QString note = info["machine"].toObject()["capabilities"].toObject()["power_note"].toString();
  return note == "emi-needs-admin" || note == "rapl-needs-root";
}

/* Linux: the only file the runner reads power from (pal/linux/pal_linux.c). */
const QString kRaplCounter = QStringLiteral("/sys/class/powercap/intel-rapl:0/energy_uj");

/* Linux: makes the energy counter readable by every user until the next reboot, as root through the desktop's own
   password prompt (pkexec); true when it is readable now. Nothing else changes, and nothing is kept: a reboot
   restores the kernel's protection. */
#ifdef Q_OS_LINUX
bool allowPowerReadings() {
  const QString pkexec = QStandardPaths::findExecutable("pkexec"), chmod = QStandardPaths::findExecutable("chmod");
  if (pkexec.isEmpty() || chmod.isEmpty()) return false;
  QProcess p;
  p.start(pkexec, {chmod, "o+r", kRaplCounter});
  if (!p.waitForStarted()) return false;
  while (!p.waitForFinished(100)) QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
  return QFileInfo(kRaplCounter).isReadable();
}
#endif

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
  /* PRISMARK_GUI_DARK and PRISMARK_GUI_PAGE, with PRISMARK_GUI_SCREENSHOT: pictures of a page in a theme, without
     touching the settings. */
  const QByteArray darkEnv = qgetenv("PRISMARK_GUI_DARK");
  applyTheme(darkEnv.isEmpty() ? s.value("dark", true).toBool() : darkEnv != "0");
  page_ = qBound(0, qEnvironmentVariableIntValue("PRISMARK_GUI_PAGE"), 2);
  if (const Metric *m = findMetric(s.value("metric").toString())) {
    metric_ = m->id;
    group_ = m->group;
  }
  advanced_ = s.value("advanced", false).toBool();
  input_ = new InputWatch(this);
  connect(input_, &InputWatch::activity, this, &MainWindow::onInputActivity);
  buildUi();
  measure_ = buildMeasuringWindow();
  auto shortcut = [this](const QKeySequence &key, auto slot) {
    auto *a = new QAction(this);
    a->setShortcut(key);
    connect(a, &QAction::triggered, this, slot);
    addAction(a);
  };
  shortcut(QKeySequence::Open, [this] { openResult(); });
  shortcut(QKeySequence::HelpContents, [this] { showGlossary(); });
  shortcut(QKeySequence::Quit, [] { QApplication::quit(); });
  loadInfo();
  refreshDevice();
  moveOldDataFolders();
  loadAll();
  for (auto it = runs_.rbegin(); it != runs_.rend(); ++it)
    if (!it->reference) {
      selected_ = it->id;
      break;
    }
  refresh();
  resize(1440, 900);
  setMinimumSize(1280, 720);
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

void MainWindow::buildUi() {
  auto *page = new QWidget;
  setCentralWidget(page); /* a rebuild, for a new theme: the old page goes with it */
  auto *outer = new QHBoxLayout(page);
  outer->setContentsMargins(8, 8, 8, 8);
  outer->setSpacing(8);
  pages_ = new QStackedWidget;
  pages_->addWidget(buildTestsPage());
  pages_->addWidget(buildResultsPage());
  pages_->addWidget(buildDevicePage());
  pages_->setCurrentIndex(page_);
  outer->addWidget(pages_, 1);
  auto *side = new SideColumn({tr("Tests"), tr("Results"), tr("Device")});
  side->setCurrent(page_);
  connect(side, &SideColumn::changed, this, [this](int i) {
    page_ = i;
    pages_->setCurrentIndex(i);
    if (i == 0) refreshRanking();
  });
  side_ = side;
  outer->addWidget(side);
}

void MainWindow::showPage(int i) {
  page_ = i;
  pages_->setCurrentIndex(i);
  side_->setCurrent(i);
  if (i == 0) refreshRanking();
}

/* The chosen theme is the whole window: every label is built with its colours, so it is built again. */
void MainWindow::setDark(bool on) {
  QSettings().setValue("dark", on);
  applyTheme(on);
  measure_->setStyleSheet(QStringLiteral("#measureWindow { background:%1; }").arg(theme().card.name()));
  QTimer::singleShot(0, this, [this] { /* not under the switch that was just pressed */
    buildUi();
    refreshDevice();
    refresh();
  });
}

/* Tests: the header with the groups and Run all, the cards, the ranking, and the highlighted result with what the
   test means. */
QWidget *MainWindow::buildTestsPage() {
  auto *pageW = new QWidget;
  auto *outer = new QVBoxLayout(pageW);
  outer->setContentsMargins(0, 0, 0, 0);
  outer->setSpacing(8);

  /* header */
  auto *head = new QHBoxLayout;
  head->setSpacing(8);
  head->addWidget(new Brand);
  groups_ = new Segmented; /* items set in refresh(): Simple shows fewer tabs */
  groups_->setNumbered(1);
  groups_->setFilling(68);
  connect(groups_, &Segmented::changed, this, [this](int i) {
    QVector<QString> shown = visibleGroups();
    if (i < 0 || i >= shown.size()) return;
    group_ = shown[i];
    metric_ = firstVisible(group_);
    refresh();
  });
  head->addWidget(groups_, 1);
  QFrame *quickPanel = card();
  quickPanel->setFixedSize(92, 68);
  auto *qp = new QVBoxLayout(quickPanel);
  qp->setContentsMargins(12, 9, 12, 7);
  qp->setSpacing(0);
  auto *quickLabel = new QLabel(tr("Quick"));
  quickLabel->setFont(labelFont(8));
  qp->addWidget(quickLabel);
  quick_ = new Toggle(QString());
  quick_->setToolTip(tr("On: quick runs, a few minutes, for a first look. Their numbers are rough, so they are not "
                        "ranked: each test page shows your latest one under its ranking.\n"
                        "Off: full runs, accurate and ranked; all tests take about 30 minutes on a laptop."));
  quick_->setChecked(QSettings().value("quick", false).toBool());
  connect(quick_, &Toggle::toggled, this, [this](bool on) {
    QSettings().setValue("quick", on);
    refreshRunLabels();
  });
  qp->addWidget(quick_);
  quickPanel_ = quickPanel;
  head->addWidget(quickPanel);
  runAll_ = loudButton(QString());
  connect(runAll_, &QPushButton::clicked, this, [this] {
    if (group_ == "stress") runTest(metric_, false); /* the selected test, as set on its tab */
    else runTest(QString(), quick_->isChecked());
  });
  head->addWidget(runAll_);
  outer->addLayout(head);

  banner_ = text(tr("Reference systems show <b>placeholder values</b>, not measurements, until real reference results "
                    "are added."),
                 9.5, QFont::Normal, theme().warnText);
  banner_->setStyleSheet(QStringLiteral("color:%1; background:%2; padding:10px 14px;")
                             .arg(theme().warnText.name(), theme().warnBg.name()));
  outer->addWidget(banner_);

  auto *body = new QHBoxLayout;
  body->setSpacing(8);
  outer->addLayout(body, 1);

  /* tests column: what the group is, then its cards; a volt card fills what they leave */
  auto *left = new QWidget;
  auto *ll = new QVBoxLayout(left);
  ll->setContentsMargins(0, 0, 0, 0);
  ll->setSpacing(8);
  QFrame *info = card();
  auto *il = new QVBoxLayout(info);
  il->setContentsMargins(14, 12, 14, 12);
  il->setSpacing(6);
  testsTitle_ = new QLabel;
  testsTitle_->setFont(labelFont(8));
  il->addWidget(testsTitle_);
  groupDesc_ = text(QString(), 9.5, QFont::Normal, theme().muted);
  il->addWidget(groupDesc_);
  moreTests_ = text(QString(), 9, QFont::Normal, theme().muted);
  moreTests_->setTextInteractionFlags(Qt::LinksAccessibleByMouse);
  connect(moreTests_, &QLabel::linkActivated, this, [this] { level_->setCurrent(1); emit level_->changed(1); });
  il->addWidget(moreTests_);
  ll->addWidget(info);
  ll->addWidget(buildStressSettings());
  cardsLayout_ = new QVBoxLayout;
  cardsLayout_->setSpacing(8);
  ll->addLayout(cardsLayout_);
  progress_ = new GroupProgress;
  ll->addWidget(progress_, 1);
  testsColumn_ = left;
  auto *ls = new QScrollArea;
  ls->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  ls->setWidget(left);
  ls->setWidgetResizable(true);
  ls->setFrameShape(QFrame::NoFrame);
  /* the column, with the Simple/Advanced switch in its own panel at the foot */
  auto *leftCol = new QVBoxLayout;
  leftCol->setSpacing(8);
  leftCol->addWidget(ls, 1);
  QFrame *levelPanel = card();
  auto *lv = new QVBoxLayout(levelPanel);
  lv->setContentsMargins(8, 8, 8, 8);
  level_ = new Segmented;
  level_->setFilling(44);
  level_->setItems({tr("Simple"), tr("Advanced")});
  level_->setCurrent(advanced_ ? 1 : 0);
  level_->setToolTip(tr("Simple: a few tests that run everywhere and are easy to relate to.\nAdvanced: every test."));
  connect(level_, &Segmented::changed, this, [this](int i) {
    advanced_ = i == 1;
    QSettings().setValue("advanced", advanced_);
    refresh();
  });
  lv->addWidget(level_);
  leftCol->addWidget(levelPanel);
  auto *leftW = new QWidget;
  leftW->setLayout(leftCol);
  leftCol->setContentsMargins(0, 0, 0, 0);
  leftW->setFixedWidth(340);
  body->addWidget(leftW);
  cards_.setExclusive(true);

  /* test page: what the test is, the ranking, and the chart that explains the highlighted result */
  QFrame *center = card();
  auto *cl = new QVBoxLayout(center);
  cl->setContentsMargins(0, 0, 0, 0);
  cl->setSpacing(0);
  auto *testPage = new QWidget;
  auto *pl = new QVBoxLayout(testPage);
  pl->setContentsMargins(0, 0, 0, 0);
  pl->setSpacing(0);
  auto *top = new QVBoxLayout;
  top->setContentsMargins(20, 18, 20, 8);
  top->setSpacing(6);
  title_ = text(QString(), 17, QFont::Bold);
  title_->setFont(displayFont(21, QFont::ExtraBold, 118));
  desc_ = text(QString(), 10.5, QFont::Normal, theme().muted);
  top->addWidget(title_);
  top->addWidget(desc_);
  auto *rankHead = new QHBoxLayout;
  rankHead->setContentsMargins(0, 6, 0, 0);
  rankTitle_ = new QLabel;
  rankTitle_->setFont(labelFont(8));
  rankHead->addWidget(rankTitle_, 1);
  auto *legend = new QLabel(QStringLiteral("<span style='color:%1'>■</span> %2 &nbsp; <span style='color:%3'>■</span> %4")
                                .arg(theme().mine.name(), tr("YOUR RUN"), theme().ref.name(), tr("REFERENCE")));
  legend->setFont(monoFont(8));
  legend->setToolTip(tr("The thin line under a bar is the measurement's uncertainty: the true value lies within it with "
                        "95 % confidence. Overlapping ranges: treat as equal."));
  rankHead->addWidget(legend);
  top->addLayout(rankHead);
  pl->addLayout(top);
  ranking_ = new RankingView;
  connect(ranking_, &RankingView::runSelected, this, [this](const QString &id) {
    selected_ = id;
    refreshRanking();
    refreshDetails();
  });
  pl->addWidget(ranking_);
  quickNote_ = text(QString(), 9, QFont::Normal, theme().muted);
  quickNote_->setContentsMargins(20, 8, 20, 0);
  quickNote_->setTextInteractionFlags(Qt::LinksAccessibleByMouse);
  connect(quickNote_, &QLabel::linkActivated, this, [this](const QString &id) { openRun(id); });
  quickNote_->hide();
  pl->addWidget(quickNote_);
  context_ = new QWidget;
  contextLayout_ = new QVBoxLayout(context_);
  contextLayout_->setContentsMargins(20, 8, 20, 12);
  contextLayout_->setSpacing(4);
  pl->addWidget(context_);
  pl->addStretch();
  auto *ps = new QScrollArea;
  ps->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  ps->setWidget(testPage);
  ps->setWidgetResizable(true);
  ps->setFrameShape(QFrame::NoFrame);
  cl->addWidget(ps, 1);
  cl->addWidget(hairline());
  auto *foot = new QHBoxLayout;
  foot->setContentsMargins(20, 6, 20, 6);
  allRuns_ = new Toggle(tr("All my runs"));
  allRuns_->setToolTip(tr("Show every saved run, not only the latest one per test"));
  connect(allRuns_, &Toggle::toggled, this, [this] { refreshRanking(); });
  foot->addWidget(allRuns_);
  QLabel *rel = text(tr("+/− % against the highlighted result"), 8.5, QFont::Normal, theme().muted);
  rel->setWordWrap(false);
  rel->setMinimumWidth(0);
  rel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  rel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
  foot->addSpacing(12);
  foot->addWidget(rel, 1);
  cl->addLayout(foot);
  body->addWidget(center, 1);

  /* the highlighted result as a pass, and what the test means */
  auto *aside = new QVBoxLayout;
  aside->setSpacing(8);
  pass_ = new PrintedFrame;
  pass_->setObjectName("pass");
  /* the same height with or without its hot line, and its foot well clear of the side column's first seam */
  pass_->setMinimumHeight(310);
  pass_->setAttribute(Qt::WA_StyledBackground);
  auto *pa = new QVBoxLayout(pass_);
  pa->setContentsMargins(16, 14, 16, 16);
  pa->setSpacing(0);
  auto *passHead = new QHBoxLayout;
  passKind_ = new QLabel;
  passKind_->setFont(monoFont(8));
  passDate_ = new QLabel;
  passDate_->setFont(monoFont(8));
  passHead->addWidget(passKind_, 1);
  passHead->addWidget(passDate_);
  pa->addLayout(passHead);
  passScore_ = new QLabel;
  pa->addWidget(passScore_);
  passUnit_ = new QLabel;
  passUnit_->setFont(monoFont(9));
  passUnit_->setWordWrap(true);
  pa->addWidget(passUnit_);
  pa->addStretch();
  passRule_ = new QFrame;
  passRule_->setFixedHeight(2);
  pa->addSpacing(14);
  pa->addWidget(passRule_);
  pa->addSpacing(12);
  passHotHead_ = new QLabel(tr("WHEN HOT"));
  passHotHead_->setFont(labelFont(8));
  pa->addWidget(passHotHead_);
  passHot_ = new QLabel;
  passHot_->setFont(uiFont(11, QFont::DemiBold));
  passHot_->setWordWrap(true);
  passHot_->setContentsMargins(0, 2, 0, 12);
  pa->addWidget(passHot_);
  passOpen_ = new QPushButton(tr("OPEN THIS RUN"));
  passOpen_->setCursor(Qt::PointingHandCursor);
  passOpen_->setFixedHeight(44);
  passOpen_->setFont(displayFont(9, QFont::ExtraBold, 125));
  connect(passOpen_, &QPushButton::clicked, this, [this] { openRun(selected_); });
  pa->addWidget(passOpen_);
  aside->addWidget(pass_);
  QFrame *explain = card();
  auto *el = new QVBoxLayout(explain);
  el->setContentsMargins(0, 0, 0, 0);
  el->setSpacing(0);
  auto *et = new QWidget;
  auto *etl = new QVBoxLayout(et);
  etl->setContentsMargins(16, 16, 16, 12);
  etl->setSpacing(4);
  auto section = [&](const QString &head, QLabel *&bodyLabel, int topMargin) {
    QLabel *h = new QLabel(head);
    h->setFont(labelFont(8));
    h->setContentsMargins(0, topMargin, 0, 0);
    etl->addWidget(h);
    bodyLabel = text(QString(), 10);
    etl->addWidget(bodyLabel);
  };
  section(tr("How it is measured"), how_, 0);
  section(tr("Reading the result"), read_, 12);
  etl->addStretch();
  auto *es = new QScrollArea;
  es->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  es->setWidget(et);
  es->setWidgetResizable(true);
  es->setFrameShape(QFrame::NoFrame);
  el->addWidget(es, 1);
  QLabel *help = text(QStringLiteral("<a href='#' style='color:%1; text-decoration:none'>%2</a>")
                          .arg(theme().link.name(), tr("What do these terms mean?")),
                      10, QFont::DemiBold);
  help->setContentsMargins(16, 6, 16, 14);
  help->setCursor(Qt::PointingHandCursor);
  connect(help, &QLabel::linkActivated, this, &MainWindow::showGlossary);
  el->addWidget(help);
  aside->addWidget(explain, 1);
  auto *asideW = new QWidget;
  asideW->setLayout(aside);
  aside->setContentsMargins(0, 0, 0, 0);
  asideW->setFixedWidth(300);
  body->addWidget(asideW);
  return pageW;
}

/* A page's head: the logo and its title, with an action on the right. */
QHBoxLayout *MainWindow::pageHead(const QString &title, QLabel **right) {
  auto *head = new QHBoxLayout;
  head->setSpacing(8);
  head->addWidget(new Brand);
  QFrame *panel = card();
  panel->setFixedHeight(68);
  auto *h = new QHBoxLayout(panel);
  h->setContentsMargins(20, 0, 20, 0);
  QLabel *t = new QLabel(title);
  t->setFont(displayFont(19, QFont::ExtraBold, 118));
  h->addWidget(t);
  h->addStretch();
  if (right) {
    *right = new QLabel;
    h->addWidget(*right);
  }
  head->addWidget(panel, 1);
  return head;
}

/* Results: your runs as tickets, the report of the one highlighted, and what it ran on with what can be done to it. */
QWidget *MainWindow::buildResultsPage() {
  auto *pageW = new QWidget;
  auto *outer = new QVBoxLayout(pageW);
  outer->setContentsMargins(0, 0, 0, 0);
  outer->setSpacing(8);
  auto *head = pageHead(tr("Your runs"), &runsCount_);
  runsCount_->setFont(labelFont(8));
  auto *open = loudButton(tr("OPEN RESULT…"), QStringLiteral("raisedBlock"));
  connect(open, &QPushButton::clicked, this, &MainWindow::openResult);
  head->addWidget(open);
  outer->addLayout(head);
  auto *body = new QHBoxLayout;
  body->setSpacing(8);
  outer->addLayout(body, 1);

  auto *left = new QWidget;
  auto *ll = new QVBoxLayout(left);
  ll->setContentsMargins(0, 0, 0, 0);
  ll->setSpacing(8);
  tickets_.clear();
  runsLayout_ = new QVBoxLayout;
  runsLayout_->setSpacing(8);
  ll->addLayout(runsLayout_);
  QFrame *cmp = card();
  auto *cp = new QVBoxLayout(cmp);
  cp->setContentsMargins(16, 16, 16, 16);
  cp->setSpacing(8);
  cp->addStretch();
  QLabel *cmpHead = new QLabel(tr("COMPARE"));
  cmpHead->setFont(labelFont(8));
  cp->addWidget(cmpHead);
  cp->addWidget(text(tr("Two of your runs side by side: <span style='font-family:\"%1\"; color:%2'>prismark compare</span> "
                        "on the command line.")
                         .arg(monoFont(9).family(), theme().text.name()),
                     10, QFont::Normal, theme().muted));
  cp->addWidget(webLink());
  ll->addWidget(cmp, 1);
  auto *ls = new QScrollArea;
  ls->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  ls->setWidget(left);
  ls->setWidgetResizable(true);
  ls->setFrameShape(QFrame::NoFrame);
  ls->setFixedWidth(340);
  body->addWidget(ls);

  QFrame *center = card();
  auto *cl = new QVBoxLayout(center);
  cl->setContentsMargins(0, 0, 0, 0);
  details_ = new QWidget;
  detailsLayout_ = new QVBoxLayout(details_);
  detailsLayout_->setContentsMargins(20, 20, 20, 20);
  detailsLayout_->setSpacing(8);
  auto *ds = new QScrollArea;
  ds->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  ds->setWidget(details_);
  ds->setWidgetResizable(true);
  ds->setFrameShape(QFrame::NoFrame);
  cl->addWidget(ds);
  body->addWidget(center, 1);

  QFrame *right = card();
  auto *rl = new QVBoxLayout(right);
  rl->setContentsMargins(0, 0, 0, 0);
  auto *rw = new QWidget;
  aboutLayout_ = new QVBoxLayout(rw);
  aboutLayout_->setContentsMargins(16, 16, 16, 16);
  aboutLayout_->setSpacing(4);
  auto *rs = new QScrollArea;
  rs->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  rs->setWidget(rw);
  rs->setWidgetResizable(true);
  rs->setFrameShape(QFrame::NoFrame);
  rl->addWidget(rs);
  right->setFixedWidth(300);
  body->addWidget(right);
  return pageW;
}

/* The site's public comparison, which replaced the app's own. */
QLabel *MainWindow::webLink() {
  QLabel *web = text(QStringLiteral("<a href='https://prismark.leonardonels.com/compare.html' style='color:%1; "
                                    "text-decoration:none'>%2 ↗</a>")
                         .arg(theme().link.name(), tr("Compare processors on the web")),
                     10, QFont::DemiBold);
  web->setWordWrap(false);
  web->setCursor(Qt::PointingHandCursor);
  web->setOpenExternalLinks(true);
  return web;
}

/* Device: this computer as `prismark info` sees it, and the app's own settings and tools. */
QWidget *MainWindow::buildDevicePage() {
  auto *pageW = new QWidget;
  deviceLayout_ = new QVBoxLayout(pageW);
  deviceLayout_->setContentsMargins(0, 0, 0, 0);
  deviceLayout_->setSpacing(8);
  return pageW;
}

void MainWindow::refreshDevice() {
  clearLayout(deviceLayout_);
  const Theme &t = theme();
  QLabel *linkHolder = nullptr;
  auto *head = pageHead(tr("This device"), &linkHolder);
  linkHolder->setFont(uiFont(10, QFont::DemiBold));
  linkHolder->setTextFormat(Qt::RichText);
  linkHolder->setText(webLink()->text());
  linkHolder->setCursor(Qt::PointingHandCursor);
  linkHolder->setOpenExternalLinks(true);
  deviceLayout_->addLayout(head);
  auto *body = new QHBoxLayout;
  body->setSpacing(8);
  deviceLayout_->addLayout(body, 1);

  const QJsonObject m = info_["machine"].toObject();
  const QJsonObject cpu = m["cpu"].toObject();
  const QJsonArray coreList = cpu["cores"].toArray();
  QMap<QString, int> types;
  for (const QJsonValue &c : coreList) types[c.toObject()["type"].toString()]++;
  const bool hybrid = types.size() > 1;

  /* the processor, as large as its name allows: the model at full size, the family above the rule */
  QString model = cpu["model"].toString();
  for (const char *junk : {"(R)", "(TM)", " Processor", "Genuine "}) model.remove(QString::fromLatin1(junk));
  model.remove(QRegularExpression(QStringLiteral(" w/ .*$| \\d+-Core.*$| CPU(?= @|$)| @ .*$")));
  QStringList words = model.simplified().split(' ', Qt::SkipEmptyParts);
  const QString big = words.isEmpty() ? tr("CPU") : words.takeLast();
  QFrame *hero = new PrintedFrame;
  hero->setProperty("dots", true);
  hero->setObjectName("hero");
  hero->setAttribute(Qt::WA_StyledBackground);
  hero->setStyleSheet(QStringLiteral("#hero { background:#2D5BFF; } #hero QLabel { color:#F7F8F9; }"));
  hero->setFixedWidth(460);
  auto *hl = new QVBoxLayout(hero);
  hl->setContentsMargins(20, 16, 20, 20);
  hl->setSpacing(0);
  auto *top = new QHBoxLayout;
  auto *procLabel = new QLabel(tr("Processor:"));
  procLabel->setFont(monoFont(9));
  auto *rev = new QLabel(tr("REV. %1").arg(info_["version"].toString()));
  rev->setFont(monoFont(9));
  top->addWidget(procLabel, 1);
  top->addWidget(rev);
  hl->addLayout(top);
  hl->addStretch();
  double pt = 300;
  QFont bigFont = displayFont(pt, QFont::DemiBold, 50);
  while (pt > 40 && QFontMetrics(bigFont).horizontalAdvance(big) > 420) bigFont = displayFont(pt -= 10, QFont::DemiBold, 50);
  auto *bigLabel = new QLabel(big);
  bigLabel->setFont(bigFont);
  bigLabel->setStyleSheet(QStringLiteral("color:#0C1430"));
  hl->addWidget(bigLabel);
  auto *family = new QLabel(words.join(' ').toUpper());
  family->setFont(displayFont(19, QFont::ExtraBold, 125));
  family->setWordWrap(true);
  hl->addWidget(family);
  auto *rule = new QFrame;
  rule->setFixedHeight(1);
  rule->setStyleSheet(QStringLiteral("background:#9DB3FF; border:none;"));
  hl->addSpacing(12);
  hl->addWidget(rule);
  hl->addSpacing(8);
  QStringList counts;
  if (hybrid) {
    const QStringList kinds = types.keys();
    for (auto k = kinds.rbegin(); k != kinds.rend(); ++k) /* P before E: fast first */
      counts << QStringLiteral("%1 %2").arg(types[*k]).arg(*k == "P" ? tr("fast") : tr("efficient"));
  } else {
    counts << tr("%1 threads").arg(coreList.size());
  }
  auto *line = new QLabel(QStringLiteral("%1 · %2").arg(counts.join(QStringLiteral(" + ")), prettyOs(m["os"].toString())));
  line->setFont(uiFont(11, QFont::DemiBold));
  hl->addWidget(line);
  body->addWidget(hero);

  /* facts, then one tile per thread */
  auto *mid = new QVBoxLayout;
  mid->setSpacing(8);
  auto *facts = new QGridLayout;
  facts->setSpacing(8);
  const QString powerSource = m["capabilities"].toObject()["power"].toString();
  QVector<std::array<QString, 3>> stats = {
      {tr("ARCHITECTURE"), prettyIsa(m["isa"].toString()), tr("The tests are built for this")},
      {tr("LARGEST CACHE"),
       cpu["llc_bytes"].toDouble() > 0 ? QStringLiteral("%1 MiB").arg(cpu["llc_bytes"].toDouble() / (1 << 20)) : QStringLiteral("—"),
       tr("Shared between threads")},
      {tr("POWER READINGS"), powerNeedsAdmin(info_) ? tr("Locked") : powerSource.isEmpty() ? tr("None") : tr("Readable"),
       powerNeedsAdmin(info_) ? tr("Needs administrator rights") : tr("What runs can record")},
      {tr("MODEL"), m["board"].toString().isEmpty() ? QStringLiteral("—") : m["board"].toString(), tr("As the system names it")},
  };
  for (int i = 0; i < stats.size(); i++) {
    QFrame *f = card();
    f->setFixedHeight(120);
    auto *fl = new QVBoxLayout(f);
    fl->setContentsMargins(16, 12, 16, 12);
    fl->setSpacing(0);
    auto *k = new QLabel(stats[i][0]);
    k->setFont(labelFont(8));
    fl->addWidget(k);
    fl->addStretch();
    auto *v = new QLabel(stats[i][1]);
    double vp = 28;
    QFont vf = displayFont(vp, QFont::DemiBold, 75);
    while (vp > 14 && QFontMetrics(vf).horizontalAdvance(stats[i][1]) > 210) vf = displayFont(vp -= 2, QFont::DemiBold, 75);
    v->setFont(vf);
    fl->addWidget(v);
    fl->addStretch();
    fl->addWidget(text(stats[i][2], 9.5, QFont::Normal, t.muted));
    facts->addWidget(f, i / 2, i % 2);
  }
  mid->addLayout(facts);
  QFrame *threads = card();
  auto *th = new QVBoxLayout(threads);
  th->setContentsMargins(16, 14, 16, 16);
  th->setSpacing(10);
  auto *thHead = new QLabel(tr("THREADS"));
  thHead->setFont(labelFont(8));
  th->addWidget(thHead);
  auto *tiles = new QWidget;
  auto *tg = new QGridLayout(tiles);
  tg->setContentsMargins(0, 0, 0, 0);
  tg->setSpacing(8);
  const int n = int(coreList.size()), cols = std::max(4, int(std::ceil(std::sqrt(n * 1.4))));
  QMap<QString, int> seen;
  for (int i = 0; i < n; i++) {
    const QString type = coreList[i].toObject()["type"].toString();
    const bool fast = hybrid && type == "P";
    const QString kind = !hybrid ? tr("THREAD") : fast ? tr("FAST") : tr("EFFICIENT");
    auto *f = new QFrame;
    f->setObjectName("threadTile");
    f->setAttribute(Qt::WA_StyledBackground);
    const QColor bg = fast ? t.accent : t.raised, fg = fast ? t.onAccent : t.text;
    f->setStyleSheet(QStringLiteral("#threadTile { background:%1; } #threadTile QLabel { color:%2; }").arg(bg.name(), fg.name()));
    f->setMinimumHeight(n > 16 ? 44 : 84);
    auto *fl = new QVBoxLayout(f);
    fl->setContentsMargins(cols > 6 ? 6 : 10, 8, cols > 6 ? 6 : 10, 8);
    fl->setSpacing(0);
    if (cols <= 6) {
      auto *kl = new QLabel(kind);
      kl->setFont(monoFont(8));
      fl->addWidget(kl);
      fl->addStretch();
    }
    auto *num = new QLabel(QString::number(++seen[type]));
    num->setFont(displayFont(cols > 6 ? 16 : 46, QFont::DemiBold, 50));
    fl->addWidget(num);
    tg->addWidget(f, i / cols, i % cols);
  }
  auto *ts = new QScrollArea;
  ts->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  ts->setWidget(tiles);
  ts->setWidgetResizable(true);
  ts->setFrameShape(QFrame::NoFrame);
  th->addWidget(ts, 1);
  mid->addWidget(threads, 1);
  body->addLayout(mid, 1);

  /* the app: what used to hide behind a menu */
  QFrame *app = card();
  app->setFixedWidth(300);
  auto *al = new QVBoxLayout(app);
  al->setContentsMargins(16, 16, 16, 16);
  al->setSpacing(0);
  auto *appHead = new QLabel(tr("THIS APP"));
  appHead->setFont(labelFont(8));
  appHead->setContentsMargins(0, 0, 0, 8);
  al->addWidget(appHead);
  al->addWidget(hairline());
  auto *darkRow = new QWidget;
  darkRow->setFixedHeight(52);
  auto *dr = new QHBoxLayout(darkRow);
  dr->setContentsMargins(0, 0, 0, 0);
  auto *darkLabel = new QLabel(tr("Dark theme"));
  darkLabel->setFont(uiFont(10.5, QFont::DemiBold));
  dr->addWidget(darkLabel, 1);
  auto *dark = new Toggle(QString());
  dark->setChecked(t.dark);
  connect(dark, &Toggle::toggled, this, &MainWindow::setDark);
  dr->addWidget(dark);
  al->addWidget(darkRow);
  al->addWidget(hairline());
  auto row = [&](const QString &label, auto slot) {
    auto *b = new QPushButton(label);
    b->setObjectName("row");
    b->setCursor(Qt::PointingHandCursor);
    b->setStyleSheet(QStringLiteral("min-height:52px; max-height:52px;"));
    connect(b, &QPushButton::clicked, this, slot);
    al->addWidget(b);
    al->addWidget(hairline());
  };
  row(tr("Prepare compile tests…"), [this] { prepareCompileTests(); });
  row(tr("Use another compile-test snapshot…"), [this] { chooseSnapshot(); });
  if (powerNeedsAdmin(info_)) {
#ifdef Q_OS_LINUX
    row(tr("Allow power readings until reboot…"), [this] {
      if (proc_) return;
      if (allowPowerReadings()) {
        loadInfo();
        QTimer::singleShot(0, this, [this] { refreshDevice(); });
        Stub::tell(this, Stub::System, tr("SYS / POWER"), tr("Power readings allowed"),
                   tr("Runs record the processor's power until the next reboot."));
      } else {
        powerNotAllowed(false);
      }
    });
#else
    row(tr("Restart as administrator to measure power"), [this] {
      if (!proc_ && restartAsAdmin()) qApp->quit();
    });
#endif
  }
  row(tr("Data folders…"), [this] { showDataFolders(); });
  row(tr("What do these terms mean?"), [this] { showGlossary(); });
  row(tr("About Prismark"), [this] { showAbout(); });
  al->addStretch();
  body->addWidget(app);
}

/* The stress test's settings, above its cards and shown only on its tab: Run starts it at once with these. Each
   change is remembered. */
QWidget *MainWindow::buildStressSettings() {
  stressSettings_ = new QFrame; /* signal blue, in both themes */
  stressSettings_->setObjectName("stressPanel");
  stressSettings_->setAttribute(Qt::WA_StyledBackground);
  stressSettings_->setStyleSheet(QStringLiteral("#stressPanel { background:#2D5BFF; } #stressPanel QLabel { color:#F7F8F9; }"));
  auto *l = new QVBoxLayout(stressSettings_);
  l->setContentsMargins(14, 12, 14, 12);
  l->setSpacing(6);
  auto *settingsHead = new QLabel(tr("Run settings"));
  settingsHead->setFont(labelFont(8));
  l->addWidget(settingsHead);
  QSettings set;
  auto choice = [&](const QString &label, const QString &key, const QVariant &dflt,
                    const QVector<QPair<QString, QVariant>> &items) {
    l->addWidget(text(label, 9.5, QFont::DemiBold));
    auto *seg = new Segmented;
    seg->setOnSignal();
    seg->setFilling(44); /* every row of choices as wide as the panel */
    QStringList names;
    int cur = 0;
    for (int i = 0; i < items.size(); i++) {
      names << items[i].first;
      if (items[i].second.toString() == set.value(key, dflt).toString()) cur = i; /* stored as text on some systems */
    }
    seg->setItems(names);
    seg->setCurrent(cur);
    connect(seg, &Segmented::changed, this, [key, items](int i) { QSettings().setValue(key, items[i].second); });
    l->addWidget(seg);
  };
  choice(tr("Length"), "stress_time", "10m",
         {{tr("10 min"), "10m"}, {tr("30 min"), "30m"}, {tr("Until stopped"), "until-stopped"}});
  choice(tr("Stop at a temperature"), "stress_max_temp", 0,
         {{tr("Never"), 0}, {QStringLiteral("85°"), 85}, {QStringLiteral("90°"), 90}, {QStringLiteral("95°"), 95},
          {QStringLiteral("100°"), 100}});
  auto *stopErr = new Toggle(tr("Stop at the first wrong result"));
  stopErr->setOnSignal();
  stopErr->setChecked(set.value("stress_stop_on_error", true).toBool());
  connect(stopErr, &Toggle::toggled, this, [](bool on) { QSettings().setValue("stress_stop_on_error", on); });
  l->addWidget(stopErr);
  return stressSettings_;
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
    cancelRun(); /* asks first */
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
  fzState_ = text(tr("MEASURING"), 9, QFont::DemiBold, theme().link);
  fzState_->setFont(labelFont(9));
  tv->addWidget(fzState_);
  fzPhase_ = text(QString(), 19, QFont::Bold);
  fzPhase_->setFont(displayFont(22, QFont::Bold, 112));
  tv->addWidget(fzPhase_);
  fzMsg_ = text(QString(), 10, QFont::Normal, theme().muted);
  tv->addWidget(fzMsg_);
  fzTime_ = text(QString(), 9, QFont::Normal, theme().muted);
  tv->addWidget(fzTime_);
  tv->addStretch();
  top->addLayout(tv, 1);
  l->addLayout(top);
  fzStall_ = text(QString(), 10, QFont::Medium, theme().warnText);
  fzStall_->setStyleSheet(QStringLiteral("color:%1; background:%2; padding:10px 14px;")
                              .arg(theme().warnText.name(), theme().warnBg.name()));
  fzStall_->hide();
  l->addWidget(fzStall_);
  fzHands_ = text(QString(), 10, QFont::Medium, theme().warnText);
  fzHands_->setStyleSheet(QStringLiteral("color:%1; background:%2; padding:10px 14px;")
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
  fzCancel_->setObjectName("outline");
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
    b->hide();
    b->deleteLater();
  }
  for (const Group &g : groups())
    if (g.id == group_) {
      testsTitle_->setText(QStringLiteral("%1 · %2").arg(tr("TESTS"), QString(g.label).replace("&&", "&").toUpper()));
      groupDesc_->setText(esc(g.desc));
    }
  int hidden = 0, shownCards = 0, measured = 0;
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
    shownCards++;
    if (latest) measured++;
    c->setContent(m.name, m.sub, latest ? formatValue(latest->metrics[m.id].v) : QString(), m.unit, note);
    c->setToolTip(QStringLiteral("<p>%1</p>").arg(esc(m.desc)));
    c->setChecked(m.id == metric_);
    connect(c, &TestCard::clicked, this, [this, id = m.id] {
      metric_ = id;
      refreshRanking();
      refreshDetails();
    });
    /* a stress test has no quick run: it runs as set above its cards */
    connect(c, &TestCard::runRequested, this,
            [this](const QString &id) { runTest(id, quick_->isChecked() && findMetric(id)->group != "stress"); });
    cards_.addButton(c);
    cardsLayout_->addWidget(c);
  }
  const QString link =
      QStringLiteral("<a href='#' style='color:%1; text-decoration:none'>%2</a>").arg(theme().link.name(), tr("Advanced"));
  moreTests_->setText(hidden == 1 ? tr("1 more test on this tab in %1").arg(link)
                      : hidden    ? tr("%1 more tests on this tab in %2").arg(hidden).arg(link)
                                  : QString());
  moreTests_->setVisible(hidden > 0);
  stressSettings_->setVisible(group_ == "stress");
  const QVector<QString> shownGroups = visibleGroups();
  for (const Group &g : groups())
    if (g.id == group_)
      progress_->setContent(QString(g.label).replace("&&", "&").toUpper(),
                            tr("%1 OF %2").arg(shownGroups.indexOf(group_) + 1, 2, 10, QChar('0')).arg(shownGroups.size(), 2, 10, QChar('0')),
                            measured, shownCards);
  refreshRunLabels();
  settle(testsColumn_);
}

void MainWindow::refreshRunLabels() {
  bool quick = quick_->isChecked(), stress = group_ == "stress";
  runAll_->setText(quick ? tr("RUN ALL QUICK") : tr("RUN ALL FULL"));
  /* a stress test has no quick run: the button runs the selected one and takes the switch's place, and the tabs stay
     where they are */
  const int width = runAll_->sizeHint().width();
  quickPanel_->setVisible(!stress);
  runAll_->setFixedWidth(width + (stress ? quickPanel_->width() + 8 : 0));
  if (stress) runAll_->setText(tr("RUN STRESS TEST"));
  runAll_->setToolTip(stress ? tr("Runs the selected stress test, with the settings on this tab.")
                      : advanced_ ? tr("Runs every test but the stress tests, which run from their own tab.")
                                  : tr("Runs the tests of the Simple view. Switch to Advanced to run every test."));
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
  rankTitle_->setText(tr("RANKING · %1 · %2").arg(m->unit.toUpper(), m->higherBetter ? tr("HIGHER IS BETTER") : tr("LOWER IS BETTER")));
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
    row.name = r.reference ? shortCpu(r.name) : tr("This device · %1").arg(shortCpu(r.name));
    row.detail = r.reference ? QString("%1 · %2").arg(r.machine["os"].toString(), r.machine["isa"].toString()) : r.date;
    if (m->kernels == "K12" && !r.vectorIsa.isEmpty()) row.detail += QStringLiteral(" · ") + r.vectorIsa;
    if (r.placeholder) row.tags << "placeholder";
    if (&r == latest && allRuns_->isChecked()) row.tags << "latest";
    if (r.state["ac_online"] == QJsonValue(false)) row.tags << tr("on battery");
    if (!r.complete) row.tags << "partial";
    if (r.busy) row.tags << tr("busy machine");
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
  /* on the Tests page, follow the test: select your result that it shows. Results may open any run. */
  if (!shown && page_ == 0 && (latest || quick)) {
    selected_ = (latest ? latest : quick)->id;
    QTimer::singleShot(0, this, [this] { refreshDetails(); });
  }
  if (quick) {
    const QString link = QStringLiteral("<a href='%1' style='color:%2; text-decoration:none'>%3</a>")
                             .arg(quick->id, theme().link.name(), tr("Show its report"));
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
  refreshPass();
}

/* The highlighted result on the test page: volt when it is yours, signal blue when it is a reference. */
void MainWindow::refreshPass() {
  const Theme &t = theme();
  const Metric *m = findMetric(metric_);
  const RunSummary *r = selectedRun();
  const bool has = m && r && r->metrics.contains(m->id), reference = r && r->reference;
  const QColor bg = reference ? QColor(45, 91, 255) : t.accent, fg = reference ? t.card : t.onAccent;
  const QColor ink = t.onAccent, onInk = reference ? QColor(247, 248, 249) : t.accent;
  pass_->setStyleSheet(QStringLiteral("#pass { background:%1; } #pass QLabel { color:%2; } "
                                      "#pass QPushButton { background:%3; color:%4; border:none; }")
                           .arg(bg.name(), reference ? QStringLiteral("#F7F8F9") : fg.name(), ink.name(), onInk.name()));
  pass_->setProperty("dots", reference);
  pass_->update();
  passKind_->setText(!r ? tr("NO RUN YET") : reference ? tr("REFERENCE") : r->quick ? tr("QUICK RUN") : tr("YOUR RUN"));
  passDate_->setText(r && r->date.size() >= 16
                         ? QStringLiteral("%1 %2").arg(QDate::fromString(r->date.left(10), QStringLiteral("yyyy-MM-dd")).toString(QStringLiteral("d")),
                                                       QLocale::c().monthName(r->date.mid(5, 2).toInt(), QLocale::ShortFormat).toUpper()) +
                               " " + r->date.mid(11)
                         : QString());
  /* the number takes the size that fits the pass */
  const QString score = has ? formatValue(r->metrics[m->id].v) : QStringLiteral("—");
  double pt = 96;
  QFont f = displayFont(pt, QFont::DemiBold, 50);
  while (pt > 40 && QFontMetrics(f).horizontalAdvance(score) > 268) f = displayFont(pt -= 6, QFont::DemiBold, 50);
  passScore_->setFont(f);
  passScore_->setFixedHeight(QFontMetrics(f).capHeight() + 22); /* the figures, without the font's tall line */
  passScore_->setText(score);
  passUnit_->setText(!m ? QString() : r ? QStringLiteral("%1 · %2").arg(m->unit, r->name) : m->unit);
  /* a dotted rule, the pass's tear line */
  passRule_->setStyleSheet(QStringLiteral("border:none; border-top:2px dotted %1; background:transparent;")
                               .arg(reference ? QStringLiteral("#F7F8F9") : t.onAccent.name()));
  QString hot;
  if (r && m) {
    const QString mode = m->group == "st" ? QStringLiteral("st_sustained") : m->group == "mc" ? QStringLiteral("mc_threaded") : QString();
    if (!mode.isEmpty() && r->hot.contains(mode)) {
      const QString label = m->group == "st" ? tr("one core") : tr("all cores");
      const double pct = (1 - r->hot[mode].v) * 100;
      hot = pct >= 1 ? tr("%1 % slower on %2").arg(QString::number(pct, 'f', 0), label) : tr("no slowdown on %1").arg(label);
    }
  }
  passHotHead_->setVisible(!hot.isEmpty());
  passHot_->setVisible(!hot.isEmpty());
  passHot_->setText(hot);
  passRule_->setVisible(r != nullptr);
  passOpen_->setVisible(r != nullptr);
}

void MainWindow::openRun(const QString &id) {
  selected_ = id;
  showPage(1);
  refreshRanking();
  refreshDetails();
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
    ch->setData({l}, tr("task length (ms)"), 0, 100, false);
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
    /* A core type that ran at only one thread count is still shown, as one point: efficiency cores join last, so
       with thread counts 1, 2, 4, 8 on 4 P + 4 E cores they run only at 8. The chart needs one line with two. */
    QVector<ChartLine> clockLines;
    bool spans = false;
    for (const ChartLine &l : clocks) spans |= l.pts.size() >= 2;
    for (const ChartLine &l : clocks)
      if (spans && !l.pts.isEmpty()) clockLines.push_back(l);
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
      ch->setData({speedup}, power || clock ? QString() : tr("threads"), 0, NAN, true);
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
      ch->setData({l}, tr("threads"), 0, NAN, false);
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
  refreshRuns();
  /* rebuild the report of the highlighted run */
  clearLayout(detailsLayout_);
  clearLayout(aboutLayout_);
  const Theme &t = theme();
  const RunSummary *r = selectedRun();
  if (!r) {
    detailsLayout_->addWidget(text(tr("No runs yet. Press Run on a test to measure this computer."), 10.5, QFont::Normal, t.muted));
    detailsLayout_->addStretch();
    aboutLayout_->addStretch();
    return;
  }

  /* heading */
  QVector<QLabel *> meta;
  if (r->placeholder) meta << pill(tr("example values"), t.warnText, t.warnBg);
  meta << pill(r->reference ? tr("reference") : tr("your run"), r->reference ? t.text : t.onAccent,
               r->reference ? t.recess : t.accentSoft);
  if (!r->date.isEmpty()) meta << pill(r->date, t.text, t.raised);
  if (r->quick) meta << pill(tr("quick run"), t.text, t.raised);
  if (!r->complete) meta << pill(tr("incomplete"), t.bad, t.raised);
  if (r->busy) meta << pill(tr("busy machine"), t.warnText, t.warnBg);
  if (!r->stress.isEmpty()) { /* the stress test's verdict, over its parts */
    QString v = "stable";
    for (const auto &x : r->stress)
      if (x.verdict == "failed" || (x.verdict == "too_hot" && v != "failed")) v = x.verdict;
    meta << (v == "failed"    ? pill(tr("stress test failed"), t.bad, t.raised)
             : v == "too_hot" ? pill(tr("stopped hot"), t.warnText, t.warnBg)
                              : pill(tr("stress test stable"), t.good, t.raised));
  }
  detailsLayout_->addWidget(pillRow(meta));
  QLabel *heading = text(esc(r->name), 22, QFont::Bold);
  heading->setFont(displayFont(25, QFont::ExtraBold, 118));
  detailsLayout_->addWidget(heading);
  if (r->busy)
    detailsLayout_->addWidget(text(tr("Other programs were working when this run started, so its results may be lower "
                                      "than this computer can do."),
                                   9.5, QFont::Normal, t.muted));
  if (r->placeholder)
    detailsLayout_->addWidget(text(tr("Made-up example values shown for comparison until real reference machines "
                                      "have been measured."),
                                   9.5, QFont::Normal, t.muted));
  else if (r->quick)
    detailsLayout_->addWidget(text(tr("A quick run gives a first look. Its numbers are rough; use a full run to "
                                      "compare computers."),
                                   9.5, QFont::Normal, t.muted));

  /* results, one tab per group, as on the phones; each tile opens its test */
  QVector<QString> tabs;
  int hiddenResults = 0;
  for (const Group &g : groups())
    for (const Metric &mt : metrics())
      if (mt.group == g.id && r->metrics.contains(mt.id)) {
        if (!visible(mt)) {
          hiddenResults++;
          continue;
        }
        if (!tabs.contains(g.id)) tabs << g.id;
      }
  QString shownGroup = reportGroup_.isEmpty() ? group_ : reportGroup_;
  if (!tabs.contains(shownGroup)) shownGroup = tabs.value(0);
  if (!tabs.isEmpty()) {
    auto *seg = new Segmented;
    seg->setFilling(40);
    QStringList names;
    for (const QString &id : tabs)
      for (const Group &g : groups())
        if (g.id == id) names << QString(g.label).replace("&&", "&");
    seg->setItems(names);
    seg->setCurrent(int(tabs.indexOf(shownGroup)));
    connect(seg, &Segmented::changed, this, [this, tabs](int i) {
      reportGroup_ = tabs.value(i);
      QTimer::singleShot(0, this, [this] { refreshDetails(); });
    });
    detailsLayout_->addSpacing(4);
    detailsLayout_->addWidget(seg);
  }
  auto *grid = new QWidget;
  grid->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
  auto *tg = new QGridLayout(grid);
  tg->setContentsMargins(0, 4, 0, 0);
  tg->setSpacing(8);
  for (int c = 0; c < 3; c++) tg->setColumnStretch(c, 1);
  int cell = 0;
  for (const Metric &mt : metrics()) {
    if (mt.group != shownGroup || !visible(mt) || !r->metrics.contains(mt.id)) continue;
    const Value &v = r->metrics[mt.id];
    QStringList flags;
    QString tip = v.hasCi() ? tr("Likely between %1 and %2 %3").arg(formatValue(v.lo), formatValue(v.hi), mt.unit) : QString();
    if (v.unsettled && !r->quick) {
      flags << tr("not settled");
      tip += tr("\nAn older result: the speed was still changing when its 10-minute limit was reached.");
    }
    if (v.clamped) {
      flags << tr("throttled");
      tip += tr("\nThrottled: during %1 of its measured windows the computer's firmware held the CPU at %2 MHz, "
                "below its own minimum clock. The score includes it.")
                 .arg(v.clamped)
                 .arg(QString::number(v.clampedMhz, 'f', 0));
    }
    if (v.ofThreads) {
      flags << tr("%1 of %2 threads").arg(v.threads).arg(v.ofThreads);
      tip += tr("\nMeasured on %1 of this computer's %2 threads (see Not measured below), so it is not an all-cores "
                "result.")
                 .arg(v.threads)
                 .arg(v.ofThreads);
    }
    auto *tile = new ScoreTile(mt.name, mt.unit, mt.higherBetter, formatValue(v.v), flags.join(QStringLiteral(" · ")));
    tile->setToolTip(tip.isEmpty() ? esc(mt.title) : tip.trimmed());
    connect(tile, &ScoreTile::clicked, this, [this, id = mt.id] {
      QTimer::singleShot(0, this, [this, id] {
        page_ = 0; /* first: the Tests page may then select your result for this test */
        selectMetric(id);
        showPage(0);
      });
    });
    tg->addWidget(tile, cell / 3, cell % 3);
    cell++;
  }
  /* how much slower the computer got during the warm-ups: what sustained use costs it */
  const QString hotMode = shownGroup == "st" ? QStringLiteral("st_sustained") : shownGroup == "mc" ? QStringLiteral("mc_threaded") : QString();
  if (!hotMode.isEmpty() && r->hot.contains(hotMode)) {
    const double pct = std::max(0.0, (1 - r->hot[hotMode].v) * 100);
    const QString label = shownGroup == "st" ? tr("one core") : tr("all cores");
    auto *f = new QFrame;
    f->setObjectName("hotTile");
    f->setAttribute(Qt::WA_StyledBackground);
    f->setStyleSheet(QStringLiteral("#hotTile { background:%1; } #hotTile QLabel { color:%2; }").arg(t.accent.name(), t.onAccent.name()));
    f->setFixedHeight(150);
    f->setToolTip(tr("Speed at the end of the one-minute warm-up against its first seconds. The long tests are measured "
                     "after the warm-up, so they already include this slowdown."));
    auto *fl = new QVBoxLayout(f);
    fl->setContentsMargins(12, 10, 12, 10);
    fl->setSpacing(0);
    auto *k = new QLabel(tr("WHEN HOT"));
    k->setFont(labelFont(8));
    fl->addWidget(k);
    fl->addStretch();
    auto *n = new QLabel(tr("%1 %").arg(QString::number(pct, 'f', 0)));
    n->setFont(displayFont(56, QFont::DemiBold, 50));
    fl->addWidget(n);
    fl->addStretch();
    auto *s = new QLabel(pct >= 1 ? tr("slower on %1").arg(label) : tr("no slowdown on %1").arg(label));
    s->setFont(uiFont(9.5, QFont::Bold));
    fl->addWidget(s);
    tg->addWidget(f, cell / 3, cell % 3, 1, cell % 3 == 2 ? 1 : 3 - cell % 3);
  }
  if (tabs.isEmpty())
    tg->addWidget(text(r->stress.isEmpty() ? tr("No results in this run.")
                                           : tr("A stress test shorter than 10 minutes has no score; its verdict is "
                                                "above, and its test page shows the rest."),
                       9.5, QFont::Normal, t.muted),
                  0, 0, 1, 3);
  detailsLayout_->addWidget(grid);
  if (hiddenResults)
    detailsLayout_->addWidget(text(hiddenResults == 1 ? tr("1 more result in Advanced.") : tr("%1 more results in Advanced.").arg(hiddenResults),
                                   9, QFont::Normal, t.muted));
  if (r->clampedSeries) {
    auto *l = text(tr("The firmware throttled the CPU to <b>%1 MHz</b> during %2 test%3.")
                       .arg(QString::number(r->clampedMhz, 'f', 0))
                       .arg(r->clampedSeries)
                       .arg(r->clampedSeries == 1 ? QString() : QStringLiteral("s")),
                   9.5);
    l->setToolTip(tr("Firmware throttling is the computer's firmware holding the CPU below its own minimum clock, often "
                     "when it gets very hot; the tests it happened in are marked \"throttled\"."));
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
  detailsLayout_->addStretch();

  /* the computer, in a few lines */
  QLabel *deviceHead = new QLabel(r->reference ? tr("DEVICE") : tr("THIS DEVICE"));
  deviceHead->setFont(labelFont(8));
  aboutLayout_->addWidget(deviceHead);
  auto kv = [&](QVBoxLayout *into, const QString &k, const QString &v, const QString &tip = QString()) {
    if (v.trimmed().isEmpty()) return;
    QLabel *key = text(esc(k), 9.5, QFont::Normal, t.muted);
    key->setContentsMargins(0, 8, 0, 0);
    key->setToolTip(tip);
    into->addWidget(key);
    into->addWidget(text(esc(v), 10, QFont::DemiBold));
  };
  const QJsonObject &m = r->machine;
  QJsonObject cpu = m["cpu"].toObject();
  QString cores;
  if (cpu.contains("cores")) {
    QMap<QString, int> types;
    for (const QJsonValue &c : cpu["cores"].toArray()) types[c.toObject()["type"].toString()]++;
    QStringList tl;
    const QStringList kinds = types.keys();
    for (auto k = kinds.rbegin(); k != kinds.rend(); ++k) /* P before E: fast first */
      tl << (*k == "P" && types.size() == 1 ? QString::number(types[*k])
                                            : QStringLiteral("%1 %2").arg(types[*k]).arg(*k == "P" ? tr("fast") : tr("efficient")));
    cores = tr("%1 threads").arg(tl.join(" + "));
  } else {
    cores = tr("%1 threads").arg(m["cpus"].toInt());
  }
  QString model = cpu["model"].toString();
  kv(aboutLayout_, tr("Processor"), model.isEmpty() ? cores : model + " · " + cores);
  kv(aboutLayout_, tr("System"), prettyOs(m["os"].toString()));
  QJsonArray cpufreq = r->state["cpufreq"].toArray();
  if (!cpufreq.isEmpty())
    kv(aboutLayout_, tr("Power policy"), cpufreq[0].toObject()["governor"].toString(),
       tr("How the operating system picks the clock speed. It affects results, so it is recorded with every run."));
  kv(aboutLayout_, tr("Power mode"), r->state["power_mode"].toString());
  if (r->state.contains("ac_online") && !r->state["ac_online"].isNull())
    kv(aboutLayout_, tr("Power source"), r->state["ac_online"].toBool() ? tr("plugged in") : tr("battery"));

  /* technical details, folded away */
  auto *techBtn = new QPushButton(techOpen_ ? tr("Hide technical details ▴") : tr("Technical details ▾"));
  techBtn->setObjectName("row");
  techBtn->setCursor(Qt::PointingHandCursor);
  connect(techBtn, &QPushButton::clicked, this, [this] {
    techOpen_ = !techOpen_;
    QTimer::singleShot(0, this, [this] { refreshDetails(); });
  });
  aboutLayout_->addSpacing(8);
  aboutLayout_->addWidget(techBtn);
  if (techOpen_) {
    kv(aboutLayout_, tr("Architecture"), m["isa"].toString());
    kv(aboutLayout_, tr("Kernel"), m["kernel"].toString());
    kv(aboutLayout_, tr("Board"), m["board"].toString());
    if (cpu["llc_bytes"].toDouble() > 0)
      kv(aboutLayout_, tr("Largest cache"), QStringLiteral("%1 MiB").arg(cpu["llc_bytes"].toDouble() / (1 << 20)));
    QString level = r->tiers["baseline"].toObject()["level"].toString();
    if (!level.isEmpty()) kv(aboutLayout_, tr("Built for"), QStringLiteral("%1 (%2)").arg(isaName(level), level));
    if (!r->harness.isEmpty())
      kv(aboutLayout_, tr("Prismark"), tr("%1, built with %2").arg(r->harness["version"].toString(), r->harness["compiler"].toString()));
    if (!r->frontend.isEmpty()) {
      QString kind = r->frontend["kind"].toString();
      QString fe = kind == "gui" ? tr("desktop app") : kind == "cli" ? tr("command line") : kind;
      if (r->frontend["ui_state"].toString() == "frozen") fe += tr(", window kept still");
      kv(aboutLayout_, tr("Started from"), fe);
    }
    if (!r->file.isEmpty()) kv(aboutLayout_, tr("File"), r->file);
  }
  aboutLayout_->addStretch();

  bool ownReference = r->reference && QFileInfo(r->file).absolutePath() == QFileInfo(referencesDir()).absoluteFilePath();
  if (!r->reference || ownReference) {
    auto action = [&](const QString &label, const char *object) {
      auto *b = new QPushButton(label);
      b->setObjectName(object);
      b->setCursor(Qt::PointingHandCursor);
      b->setStyleSheet(QStringLiteral("min-height:26px;"));
      aboutLayout_->addSpacing(4);
      aboutLayout_->addWidget(b);
      return b;
    };
    connect(action(tr("Show file"), "plain"), &QPushButton::clicked, this, [this] {
      if (const RunSummary *s = selectedRun()) QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(s->file).absolutePath()));
    });
    if (ownReference) {
      connect(action(tr("Remove reference"), "outline"), &QPushButton::clicked, this, &MainWindow::removeReference);
    } else {
      QPushButton *save = action(tr("Save as reference…"), "plain");
      save->setToolTip(tr("Keep this run in the rankings as a reference system to compare later runs with"));
      connect(save, &QPushButton::clicked, this, &MainWindow::saveAsReference);
      connect(action(tr("Move to trash"), "outline"), &QPushButton::clicked, this, &MainWindow::removeSelected);
    }
  }
  settle(details_);
  settle(aboutLayout_->parentWidget());
}

/* The Results page's list: your runs, newest first. Opening a run only moves the highlight: the list is built again
   only when the runs themselves change, so it never stirs. */
void MainWindow::refreshRuns() {
  QStringList ids;
  for (auto it = runs_.rbegin(); it != runs_.rend(); ++it)
    if (!it->reference) ids << it->id;
  bool same = ids.size() == tickets_.size();
  for (int i = 0; same && i < ids.size(); i++) same = tickets_[i].first == ids[i];
  if (same) {
    for (const auto &[id, ticket] : tickets_) ticket->setChecked(id == selected_);
    return;
  }
  clearLayout(runsLayout_);
  tickets_.clear();
  for (auto it = runs_.rbegin(); it != runs_.rend(); ++it) {
    if (it->reference) continue;
    QString kind = tr("Full"), note = it->complete ? tr("Every test it ran is ranked") : tr("Stopped early: a partial result");
    if (it->quick) {
      kind = tr("Quick");
      note = tr("A first look, not ranked");
    } else if (!it->stress.isEmpty()) {
      kind = tr("Stress");
      QString v = "stable";
      for (const auto &x : it->stress)
        if (x.verdict == "failed" || (x.verdict == "too_hot" && v != "failed")) v = x.verdict;
      note = v == "failed" ? tr("Failed: a wrong result") : v == "too_hot" ? tr("Stopped at the temperature limit") : tr("Stable");
    }
    if (it->busy) note += tr(" · busy machine");
    auto *ticket = new RunTicket(it->date, kind, note);
    ticket->setChecked(it->id == selected_);
    connect(ticket, &RunTicket::clicked, this, [this, id = it->id] {
      QTimer::singleShot(0, this, [this, id] {
        selected_ = id;
        refreshRanking();
        refreshDetails();
      });
    });
    runsLayout_->addWidget(ticket);
    tickets_.push_back({it->id, ticket});
  }
  runsCount_->setText(tr("%1 SAVED").arg(ids.size()));
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
    Stub d(this, Stub::System, tr("SYS / COMPILE"), tr("The compile tests need a one-time setup"),
           tr("“Compiling code” and “Full software build” compile a pinned snapshot of real sources, which is "
              "prepared once per machine."));
    d.setWide(620);
    QVBoxLayout *l = d.body();
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
      auto *head = new QHBoxLayout;
      head->addWidget(text(tr("To install what is missing, run in a terminal:"), 10, QFont::Medium), 1);
      auto *copy = new QPushButton(tr("Copy"));
      copy->setObjectName("ghost");
      copy->setCursor(Qt::PointingHandCursor);
      connect(copy, &QPushButton::clicked, &d, [cmds, copy] {
        QApplication::clipboard()->setText(cmds);
        copy->setText(tr("Copied"));
      });
      head->addWidget(copy);
      l->addLayout(head);
      auto *box = new QPlainTextEdit(cmds);
      box->setReadOnly(true);
      box->setFont(monoFont(9));
      box->setFixedHeight(110);
      box->setLineWrapMode(QPlainTextEdit::NoWrap);
      box->setFrameShape(QFrame::NoFrame);
      box->setStyleSheet(QStringLiteral("background:%1; color:%2;").arg(theme().raised.name(), theme().text.name()));
      l->addWidget(box);
    }
    if (!st.snapshot.isEmpty())
      row(true, tr("Snapshot prepared: %1").arg(st.snapshot));
    else if (canPrepare)
      mark(theme().link, "→", tr("Last step: prepare the snapshot of the LLVM sources. Press “Prepare now” to start it."),
           QFont::Medium);
    else
      row(false, tr("Last step: prepare the snapshot of the LLVM sources. Once the commands above have run, "
                    "open this again and press “Prepare now”."));
    if (st.snapshot.isEmpty())
      l->addWidget(text(tr("Preparing the snapshot downloads pinned sources (about 160 MB), builds generated files "
                           "once and measures every unit: 20–60 minutes, about 4 GB of disk while it runs."),
                        9.5, QFont::Normal, theme().muted));
    d.addButton(tr("Cancel"), Stub::Secondary);
    QPushButton *without = all ? d.addButton(tr("Run without them"), canPrepare ? Stub::Secondary : Stub::Primary) : nullptr;
    QPushButton *prep = canPrepare ? d.addButton(tr("Prepare now"), Stub::Primary) : nullptr;
    d.exec();
    QAbstractButton *clicked = d.clicked();
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

/* Power is measured only with administrator rights here: offer them before each run until told not to. False
   when the run should not start. */
bool MainWindow::askPower() {
  if (!powerNeedsAdmin(info_) || QSettings().value("power_admin_no_ask", false).toBool()) return true;
#ifdef Q_OS_LINUX
  Stub box(this, Stub::System, tr("SYS / POWER"), tr("Allow power readings"),
           tr("Linux lets only root read the processor's energy counter. Allowing it asks for your password and "
              "lasts until the next reboot. Scores are the same either way; without it, results have no watts."));
  const QString yes = tr("Allow power readings"), menu = tr("the Device page can still allow them");
#else
  Stub box(this, Stub::System, tr("SYS / POWER"), tr("Restart as administrator to measure power"),
           tr("This computer reports the processor's power draw only to administrators. Scores are the same "
              "either way; without it, results have no watts."));
  const QString yes = tr("Restart as administrator"), menu = tr("the Device page can still restart as administrator");
#endif
  box.setTag(tr("optional"));
  QAbstractButton *noAsk = box.addCheck(tr("Do not ask again (%1)").arg(menu));
  QPushButton *without = box.addButton(tr("Not now"), Stub::Secondary);
  QPushButton *allow = box.addButton(yes, Stub::Primary);
  box.exec(); /* Escape: no run */
  if (noAsk->isChecked()) QSettings().setValue("power_admin_no_ask", true);
  if (box.clicked() == without) return true;
  if (box.clicked() != allow) return false;
#ifdef Q_OS_LINUX
  if (allowPowerReadings()) {
    loadInfo();
    return true;
  }
  return powerNotAllowed(true);
#else
  if (restartAsAdmin()) qApp->quit();
  return false;
#endif
}

/* Linux: the password prompt was dismissed, or there is none (no pkexec): the command that does the same. With a
   run waiting, it can go on without power. */
bool MainWindow::powerNotAllowed(bool runWaiting) {
  Stub d(this, Stub::Error, tr("ERR / POWER"), tr("Power readings were not allowed"),
         tr("The password prompt was closed, or this system has none. This command in a terminal allows them until "
            "the next reboot:"));
  d.setCode(QStringLiteral("sudo chmod o+r %1").arg(kRaplCounter));
  if (!runWaiting) {
    d.addButton(tr("OK"), Stub::Primary);
    d.exec();
    return false;
  }
  d.addButton(tr("Cancel"), Stub::Secondary);
  QPushButton *go = d.addButton(tr("Run without power"), Stub::Primary);
  d.exec();
  return d.clicked() == go;
}

void MainWindow::runTest(const QString &metricId, bool quick) {
  if (proc_) return;
  QString cli = cliPath();
  if (cli.isEmpty()) {
    Stub::tell(this, Stub::Error, tr("ERR / RUNNER"), tr("The runner is missing"),
               tr("The benchmark runner, prismark, is not next to this app or in PATH, so nothing can be measured. "
                  "Install Prismark again."));
    return;
  }
  const Metric *m = metricId.isEmpty() ? nullptr : findMetric(metricId);
  bool all = !m;
  QStringList args = {"--progress-json", "--cancel-on-stdin", "--frontend", "gui"};
  QString stressTime; /* "10m", "30m" or "until-stopped" */
  QStringList stressOpts;
  if (m && m->group == "stress") { /* as set beside its cards (buildStressSettings) */
    QSettings set;
    stressTime = set.value("stress_time", "10m").toString();
    stressOpts = {"--stress-on-error", set.value("stress_stop_on_error", true).toBool() ? "stop" : "continue"};
    if (int c = set.value("stress_max_temp", 0).toInt(); c > 0) stressOpts << "--stress-max-temp" << QString::number(c);
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
  if (!askPower()) return;
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
    if (e == QProcess::FailedToStart) {
      procErrTail_ = proc_->errorString();
      onRunFinished(-1, QProcess::CrashExit);
    }
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

/* Cancel, with the run's progress so far: what is lost is plain before it is. */
bool MainWindow::askCancel() {
  Stub d(measure_, Stub::Run, tr("RUN / CANCEL"), tr("Cancel the run?"),
         tr("The tests finished so far are kept as a partial result. It is not ranked."));
  if (parts_ > 0) d.setTag(tr("Part %1 of %2").arg(part_, 2, 10, QChar('0')).arg(parts_));
  if (runSteps_ > 0) d.setStrip(std::max(runStep_ - 1, 0), 1, std::max(runSteps_ - runStep_, 0));
  else if (parts_ > 0) d.setStrip(std::max(part_ - 1, 0), 1, std::max(parts_ - part_, 0));
  d.addButton(tr("Keep measuring"), Stub::Primary);
  QPushButton *cancel = d.addButton(tr("Cancel run"), Stub::Outline);
  d.exec();
  return d.clicked() == cancel;
}

void MainWindow::cancelRun() {
  if (!proc_ || cancelling_) return;
  if (!stressOpen_ && !askCancel()) return;
  if (stressOpen_) { /* the normal end of a stress test without a set length: a score needs 10 minutes */
    double mins = phase_ == "stress" ? phaseStart_.secsTo(QDateTime::currentDateTime()) / 60.0 : 0;
    if (mins < 10) {
      Stub d(measure_, Stub::Run, tr("RUN / STOP"), tr("Stop the stress test?"),
             tr("It has run %1 of the 10 minutes a score needs. Stopping now keeps the curve of speed, clock and "
                "temperature, without a score.")
                 .arg(QString::number(std::floor(mins))));
      d.setTag(tr("%1 of 10 min").arg(QString::number(std::floor(mins))));
      d.setStrip(std::floor(mins), 1, std::max(0.0, 9 - std::floor(mins)));
      d.addButton(tr("Keep running"), Stub::Primary);
      QPushButton *stop = d.addButton(tr("Stop now"), Stub::Outline);
      d.exec();
      if (d.clicked() != stop) return;
    }
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
  auto sentence = [](QString s) { /* the runner's reason as a sentence of its own */
    if (!s.isEmpty()) s[0] = s[0].toUpper();
    return s.isEmpty() || s.endsWith('.') ? s : s + '.';
  };
  if (code == -1) {
    Stub d(this, Stub::Error, tr("ERR / RUNNER"), tr("Could not start the runner"),
           tr("%1 Nothing was saved.").arg(reason.isEmpty() ? tr("The system did not say why.") : sentence(reason)));
    d.setDetails(QDir::toNativeSeparators(cliPath()));
    d.addButton(tr("OK"), Stub::Primary);
    d.exec();
  } else if (code == 4 && status == QProcess::NormalExit) { /* the runner's status for a failed stress test */
    Stub::tell(this, Stub::Error, tr("ERR / STRESS"), tr("The stress test failed"),
               tr("It found wrong results: this computer is not stable under this load. The test page shows when "
                  "and on which CPU."));
  } else if (cancelling_ && stressOpen_ && code == 0 && status == QProcess::NormalExit) {
    /* stopped as intended: the result is on the page */
  } else if (cancelling_) {
    Stub::tell(this, Stub::Run, tr("RUN / CANCELLED"), tr("The run was cancelled"),
               saved ? tr("The part measured so far was saved and is shown. It is not ranked.")
                     : tr("The runner did not stop in time and was ended, so nothing was saved."));
  } else if (status != QProcess::NormalExit) {
    Stub d(this, Stub::Error, tr("ERR / RUNNER"), tr("The runner stopped unexpectedly"),
           saved ? tr("It crashed. The part measured so far was saved.") : tr("It crashed. Nothing was saved."));
    if (!reason.isEmpty()) d.setDetails(reason);
    d.addButton(tr("OK"), Stub::Primary);
    d.exec();
  } else if (code != 0) {
    Stub::tell(this, Stub::Run, tr("RUN / INCOMPLETE"), tr("The run did not complete"),
               (reason.isEmpty() ? QString() : sentence(reason) + " ") +
                   (saved ? tr("Its partial result was saved; it is not ranked.") : tr("Nothing was saved.")));
  } else if (!skipped.isEmpty()) {
    Stub::tell(this, Stub::Run, tr("RUN / SKIPPED"), tr("Some tests were skipped"),
               tr("Keyboard or mouse input during tests that need an idle machine (%1): their results were "
                  "discarded. Run them again from their cards and leave the machine alone meanwhile.")
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
    else
      Stub::tell(this, Stub::Error, tr("ERR / FILE"), tr("Not a Prismark result"),
                 tr("This file is not one Prismark saved, so it was not opened."), QDir::toNativeSeparators(f));
  }
  if (!last.isEmpty()) selected_ = last;
  refresh();
}

void MainWindow::prepareCompileTests() {
  QStringList ignored;
  CompileStatus st = compileStatus(info_, QSettings().value("k1_data").toString());
  if (!st.missingPrepare.isEmpty()) {
    confirmCompileTests(true, true, false, ignored); /* explains what to install */
    return;
  }
  if (!st.snapshot.isEmpty()) {
    Stub d(this, Stub::System, tr("SYS / COMPILE"), tr("Prepare the snapshot again?"),
           tr("The compile tests already have one. Preparing it again takes 20–60 minutes."));
    d.setCode(QDir::toNativeSeparators(st.snapshot));
    d.addButton(tr("Keep it"), Stub::Secondary);
    QPushButton *again = d.addButton(tr("Prepare again"), Stub::Primary);
    d.exec();
    if (d.clicked() != again) return;
  }
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
    Stub::tell(this, Stub::Error, tr("ERR / SNAPSHOT"), tr("Not a prepared snapshot"),
               tr("This folder has no manifest.json, so the compile tests cannot use it. Choose the folder that "
                  "Prepare compile tests made."),
               QDir::toNativeSeparators(dir));
    return;
  }
  s.setValue("k1_data", dir);
}

void MainWindow::showGlossary() {
  Stub d(this, Stub::System, tr("APP / TERMS"), tr("What the terms mean"));
  d.setWide(720);
  auto *body = text(glossaryHtml(), 9.5);
  body->setTextInteractionFlags(Qt::TextSelectableByMouse);
  body->setContentsMargins(0, 0, 12, 0);
  auto *scroll = new QScrollArea;
  scroll->setWidget(body);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  scroll->setFixedHeight(std::min(480, int(screen()->availableGeometry().height() * 0.55)));
  d.body()->addWidget(scroll);
  d.addButton(tr("Close"), Stub::Primary);
  d.exec();
}

void MainWindow::showDataFolders() {
  Stub d(this, Stub::System, tr("SYS / FOLDERS"), tr("Where Prismark keeps its data"),
         tr("Your runs and reference systems are shared with the command line. Reference systems are result files, "
            "or placeholder files as described in references/README.md."));
  d.setCode(tr("Saved runs\n%1\n\nReference systems\n%2\n\nCompile-test snapshot\n%3\n\nRunner\n%4")
                .arg(QDir::toNativeSeparators(resultsDir()), QDir::toNativeSeparators(referencesDir()),
                     QDir::toNativeSeparators(defaultSnapshotDir()),
                     cliPath().isEmpty() ? tr("not found") : QDir::toNativeSeparators(cliPath())));
  d.setWide(560);
  d.addButton(tr("Close"), Stub::Secondary);
  QPushButton *open = d.addButton(tr("Open results folder"), Stub::Primary);
  d.exec();
  if (d.clicked() == open) QDesktopServices::openUrl(QUrl::fromLocalFile(resultsDir()));
}

void MainWindow::showAbout() {
  Stub d(this, Stub::System, tr("APP / ABOUT"), tr("Prismark %1").arg(QCoreApplication::applicationVersion()),
         tr("A CPU benchmark that treats every vendor the same. Each test is reported and ranked on its own; there "
            "is never one overall score. Reference systems tagged PLACEHOLDER are example values, not "
            "measurements."));
  d.setCode(tr("Apache License 2.0\nArchivo, Saira, IBM Plex Mono: SIL Open Font License 1.1"));
  d.addButton(tr("OK"), Stub::Primary);
  d.exec();
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
  if (r->busy) {
    Stub::tell(this, Stub::System, tr("RES / REFERENCE"), tr("This run cannot be a reference"),
               tr("Other programs were working when it started. Close them and run it again."));
    return;
  }
  QString file = r->file, model = r->machine["cpu"].toObject()["model"].toString(r->name);
  bool quick = r->quick;
  static const QRegularExpression valid(QStringLiteral("^[A-Za-z0-9_-][A-Za-z0-9._-]{0,79}$"));
  QString name = suggestReferenceName(model);
  for (;;) {
    Stub d(this, Stub::System, tr("RES / REFERENCE"), tr("Save as reference"),
           tr("It stays in the rankings next to the bundled systems, and later runs compare with it (also from the "
              "command line: prismark compare latest NAME).%1")
               .arg(quick ? tr(" This is a quick run: comparisons with it are rough.") : QString()));
    auto *field = new QLineEdit(name);
    field->setFont(monoFont(10));
    field->setMinimumHeight(44);
    field->setStyleSheet(QStringLiteral("QLineEdit { background:%1; color:%2; border:1.5px solid %1; padding:0 10px; } "
                                        "QLineEdit:focus { border-color:%3; }")
                             .arg(theme().raised.name(), theme().text.name(), theme().link.name()));
    d.body()->addSpacing(4);
    d.body()->addWidget(text(tr("Name: letters, digits, - _ ."), 9, QFont::DemiBold));
    d.body()->addWidget(field);
    QLabel *bad = text(QString(), 9, QFont::Normal, theme().bad);
    bad->hide();
    d.body()->addWidget(bad);
    d.addButton(tr("Cancel"), Stub::Secondary);
    QPushButton *save = d.addButton(tr("Save"), Stub::Primary);
    connect(field, &QLineEdit::textChanged, &d, [&](const QString &s) {
      bool ok = valid.match(s.trimmed()).hasMatch();
      save->setEnabled(ok);
      bad->setText(s.trimmed().isEmpty() ? QString() : tr("“%1” cannot be a name.").arg(esc(s.trimmed())));
      bad->setVisible(!ok && !s.trimmed().isEmpty());
    });
    field->selectAll();
    field->setFocus();
    d.exec();
    if (d.clicked() != save) return;
    name = field->text().trimmed();
    QString dest = QDir(referencesDir()).filePath(name + ".json");
    if (QFileInfo::exists(dest)) {
      if (!Stub::ask(this, Stub::System, tr("RES / REFERENCE"), tr("Replace “%1”?").arg(name),
                     tr("You already have a reference system with this name. The new run takes its place."),
                     tr("Replace"), tr("Choose another name"), true))
        continue;
      QFile::remove(dest);
    }
    if (!QFile::copy(file, dest)) {
      Stub::tell(this, Stub::Error, tr("ERR / FILE"), tr("Could not save the reference"),
                 tr("The file could not be written. Check that the folder exists and is yours."),
                 QDir::toNativeSeparators(dest));
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
  {
    Stub d(this, Stub::System, tr("RES / REMOVE"), tr("Remove “%1”?").arg(name),
           tr("It leaves the rankings, and its file goes to the trash."));
    d.setCode(QDir::toNativeSeparators(file));
    d.addButton(tr("Keep it"), Stub::Primary);
    QPushButton *remove = d.addButton(tr("Remove"), Stub::Outline);
    d.exec();
    if (d.clicked() != remove) return;
  }
  if (!QFile::moveToTrash(file) && !QFile::remove(file)) {
    Stub::tell(this, Stub::Error, tr("ERR / FILE"), tr("Could not remove the reference"),
               tr("The file could not be moved to the trash or deleted."), QDir::toNativeSeparators(file));
    return;
  }
  loadAll(); /* a bundled reference of the same name, which this one replaced, comes back */
  selected_.clear();
  refresh();
}

void MainWindow::removeSelected() {
  const RunSummary *r = selectedRun();
  if (!r || r->reference) return;
  {
    Stub d(this, Stub::System, tr("RES / TRASH"), tr("Move this result to the trash?"),
           tr("It leaves the rankings. You can restore the file from the trash."));
    d.setCode(QDir::toNativeSeparators(r->file));
    d.addButton(tr("Keep it"), Stub::Primary);
    QPushButton *trash = d.addButton(tr("Move to trash"), Stub::Outline);
    d.exec();
    if (d.clicked() != trash) return;
  }
  if (!QFile::moveToTrash(r->file) && !QFile::remove(r->file)) {
    Stub::tell(this, Stub::Error, tr("ERR / FILE"), tr("Could not remove the result"),
               tr("The file could not be moved to the trash or deleted."), QDir::toNativeSeparators(r->file));
    return;
  }
  QString id = r->id;
  runs_.erase(std::remove_if(runs_.begin(), runs_.end(), [&](const RunSummary &x) { return x.id == id && !x.reference; }),
              runs_.end());
  selected_.clear();
  refresh();
}
