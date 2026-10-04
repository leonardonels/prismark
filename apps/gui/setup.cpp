/* Copyright 2026 The Prismark Authors. Apache-2.0. */
#include "setup.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QVBoxLayout>

#include "theme.h"
#include "widgets.h"

namespace {
constexpr const char *kClang = "clang-19", *kClangxx = "clang++-19";

bool have(const QString &tool) { return !QStandardPaths::findExecutable(tool).isEmpty(); }

bool haveLld() {
  return have("ld.lld") || have("ld.lld-19") || QFileInfo("/usr/lib/llvm-19/bin/ld.lld").isExecutable();
}
}  // namespace

QString dataDir() {
  QString d = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/prismark";
  QDir().mkpath(d);
  return d;
}

QString defaultSnapshotDir() { return dataDir() + "/k1x"; }

QString prepareScript() {
  const QString app = QCoreApplication::applicationDirPath();
  for (const QString &p : {app + "/../share/prismark/prepare.py", app + "/prepare.py",
                           QStringLiteral(PMK_SOURCE_DIR "/tools/k1x/prepare.py")})
    if (QFileInfo(p).isFile()) return QFileInfo(p).absoluteFilePath();
  return QString();
}

CompileStatus compileStatus(const QJsonObject &info, const QString &configured) {
  CompileStatus s;
  s.k1Built = info["k1_built"].toBool();
  for (const QString &dir : {configured, defaultSnapshotDir()})
    if (!dir.isEmpty() && QFileInfo(dir + "/manifest.json").isFile()) {
      s.snapshot = dir;
      break;
    }
  for (const char *t : {kClang, kClangxx, "cmake", "ninja"}) {
    if (!have(t)) {
      s.missingPrepare << t;
      s.missingBuild << t;
    }
  }
  if (!haveLld()) {
    s.missingPrepare << "lld";
    s.missingBuild << "lld";
  }
  if (!have("python3")) s.missingPrepare << "python3";
  if (prepareScript().isEmpty()) s.missingPrepare << "prepare.py";
  return s;
}

QString installInstructions(const CompileStatus &s) {
  QStringList out;
#if defined(Q_OS_LINUX)
  bool llvm = s.missingPrepare.contains(kClang) || s.missingPrepare.contains("lld");
  if (llvm) out << "wget https://apt.llvm.org/llvm.sh && sudo bash llvm.sh 19     # clang-19, clang++-19, lld-19";
  QStringList apt;
  if (s.missingPrepare.contains("cmake")) apt << "cmake";
  if (s.missingPrepare.contains("ninja")) apt << "ninja-build";
  if (s.missingPrepare.contains("python3")) apt << "python3";
  if (!s.k1Built) apt << "libclang-19-dev" << "llvm-19-dev";
  if (!apt.isEmpty()) out << "sudo apt install " + apt.join(' ');
  if (!s.k1Built)
    out << "# then rebuild Prismark so the “Compiling code” tests are built in:"
        << "cmake --preset linux-clang && cmake --build --preset linux-clang";
#elif defined(Q_OS_MACOS)
  out << "brew install llvm@19 lld cmake ninja python";
  if (!s.k1Built) out << "# then rebuild Prismark with the macos-clang preset so the “Compiling code” tests are built in";
#else
  out << "Install LLVM 19 (clang, lld) from https://github.com/llvm/llvm-project/releases, CMake, Ninja and Python 3.";
  if (!s.k1Built) out << "Then rebuild Prismark with the windows-clang preset so the “Compiling code” tests are built in.";
#endif
  return out.join('\n');
}

/* ---------- PrepareDialog ---------- */

PrepareDialog::PrepareDialog(QWidget *parent) : QDialog(parent) {
  setWindowTitle(tr("Prepare the compile tests"));
  resize(720, 520);
  auto *l = new QVBoxLayout(this);
  l->setContentsMargins(24, 22, 24, 20);
  l->setSpacing(12);
  auto *title = new QLabel(tr("Preparing the compile tests"));
  title->setFont(uiFont(15, QFont::Bold));
  l->addWidget(title);
  auto *intro = new QLabel(tr("A pinned LLVM subset and a pinned aarch64 sysroot are downloaded and checked, generated "
                              "sources are built once, and every translation unit is measured. This runs once per "
                              "machine and takes 20–60 minutes; about 4 GB of disk are used while it runs."));
  intro->setWordWrap(true);
  intro->setStyleSheet(QStringLiteral("color:%1").arg(theme().muted.name()));
  l->addWidget(intro);
  auto *row = new QHBoxLayout;
  ring_ = new StepRing;
  ring_->setProgress(0, 7, QStringLiteral("0/7"));
  row->addWidget(ring_);
  step_ = new QLabel(tr("starting"));
  step_->setFont(uiFont(12, QFont::DemiBold));
  step_->setWordWrap(true);
  row->addWidget(step_, 1);
  l->addLayout(row);
  log_ = new QPlainTextEdit;
  log_->setReadOnly(true);
  log_->setMaximumBlockCount(400);
  log_->setFont(monoFont(9));
  l->addWidget(log_, 1);
  button_ = new QPushButton(tr("Cancel"));
  connect(button_, &QPushButton::clicked, this, [this] {
    if (done_) accept();
    else reject();
  });
  l->addWidget(button_, 0, Qt::AlignRight);

  proc_.setProcessChannelMode(QProcess::MergedChannels);
  connect(&proc_, &QProcess::readyRead, this, &PrepareDialog::onOutput);
  connect(&proc_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
          [this](int code, QProcess::ExitStatus) { onFinished(code); });
}

PrepareDialog::~PrepareDialog() {
  if (proc_.state() != QProcess::NotRunning) {
    proc_.terminate(); /* prepare.py stops its child processes on SIGTERM */
    if (!proc_.waitForFinished(10000)) proc_.kill();
  }
}

int PrepareDialog::exec() {
  proc_.start("python3", {prepareScript(), "--out", defaultSnapshotDir(), "--force", "--clang", "clang-19", "--clangxx",
                          "clang++-19"});
  return QDialog::exec();
}

void PrepareDialog::onOutput() {
  buf_ += proc_.readAll();
  int nl;
  static const QRegularExpression re(R"(step (\d+)/(\d+): (.*))");
  while ((nl = buf_.indexOf('\n')) >= 0) {
    QString line = QString::fromUtf8(buf_.left(nl)).trimmed();
    buf_.remove(0, nl + 1);
    log_->appendPlainText(line);
    auto m = re.match(line);
    if (m.hasMatch()) {
      int s = m.captured(1).toInt(), n = m.captured(2).toInt();
      ring_->setProgress(s - 1, n, QStringLiteral("%1/%2").arg(s).arg(n));
      step_->setText(m.captured(3));
    }
  }
}

void PrepareDialog::onFinished(int code) {
  done_ = code == 0;
  ring_->setProgress(done_ ? 7 : 0, 7, done_ ? QStringLiteral("✓") : QStringLiteral("✗"));
  step_->setText(done_ ? tr("Ready. Compile and Full build can now run.") : tr("Preparation failed; see the log."));
  button_->setText(done_ ? tr("Done") : tr("Close"));
}
