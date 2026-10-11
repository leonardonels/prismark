/* Copyright 2026 The Prismark Authors. Apache-2.0. */
#include "setup.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QDirIterator>
#include <QVBoxLayout>

#if defined(Q_OS_UNIX) && !defined(Q_OS_ANDROID)
#include <pwd.h>
#include <unistd.h>
#define PMK_ELEVATION 1
#endif

#include "theme.h"
#include "widgets.h"

namespace {
constexpr const char *kClang = "clang-19", *kClangxx = "clang++-19";

bool have(const QString &tool) { return !QStandardPaths::findExecutable(tool).isEmpty(); }

bool haveLld() {
  return have("ld.lld") || have("ld.lld-19") || QFileInfo("/usr/lib/llvm-19/bin/ld.lld").isExecutable();
}
}  // namespace

namespace {
/* The user who started the app as root through sudo or pkexec (the same rule as the command line's). */
struct ElevatedUser {
  bool ok = false;
  unsigned uid = 0, gid = 0;
  QString home;
};

ElevatedUser elevatedUser() {
  ElevatedUser u;
#ifdef PMK_ELEVATION
  if (geteuid() != 0) return u;
  QByteArray pk = qgetenv("PKEXEC_UID"), su = qgetenv("SUDO_UID"), sg = qgetenv("SUDO_GID");
  if (pk.isEmpty() && su.isEmpty()) return u;
  struct passwd *pw = getpwuid((uid_t)(pk.isEmpty() ? su : pk).toUInt());
  if (!pw || pw->pw_uid == 0 || !pw->pw_dir || !*pw->pw_dir) return u;
  u.ok = true;
  u.uid = pw->pw_uid;
  u.gid = pk.isEmpty() && !sg.isEmpty() ? sg.toUInt() : pw->pw_gid;
  u.home = QString::fromLocal8Bit(pw->pw_dir);
#endif
  return u;
}

void chownTo(const ElevatedUser &u, const QString &path) {
#ifdef PMK_ELEVATION
  if (chown(QFile::encodeName(path).constData(), (uid_t)u.uid, (gid_t)u.gid))
    qWarning().noquote() << "could not give" << path << "to the user who started Prismark";
#else
  (void)u;
  (void)path;
#endif
}
}  // namespace

void makeUserDir(const QString &dir) {
  ElevatedUser u = elevatedUser();
  if (!u.ok) {
    QDir().mkpath(dir);
    return;
  }
  QStringList missing; /* from the deepest up */
  for (QString d = QDir::cleanPath(QFileInfo(dir).absoluteFilePath()); !QFileInfo::exists(d); d = QFileInfo(d).path())
    missing.prepend(d);
  for (const QString &d : missing)
    if (QDir().mkdir(d)) chownTo(u, d);
}

void giveToUser(const QString &path) {
  ElevatedUser u = elevatedUser();
  if (!u.ok) return;
  chownTo(u, path);
  if (QFileInfo(path).isDir() && !QFileInfo(path).isSymLink())
    for (QDirIterator it(path, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                         QDirIterator::Subdirectories);
         it.hasNext();)
      chownTo(u, it.next());
}

QString dataDir() {
  ElevatedUser u = elevatedUser();
  QString base = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
#if defined(Q_OS_MACOS)
  if (u.ok) base = u.home + "/Library/Application Support";
#else
  if (u.ok) base = u.home + "/.local/share"; /* XDG_DATA_HOME, if set, is root's */
#endif
  QString d = base + "/prismark";
  makeUserDir(d);
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
  /* the one set in the app, the one shipped beside the program (portable Windows package), the prepared one */
  for (const QString &dir : {configured, QCoreApplication::applicationDirPath() + "/k1x", defaultSnapshotDir()})
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
  /* Preparing links target feature checks; the measured build only compiles, so running it needs no linker. */
  if (!haveLld()) s.missingPrepare << "lld";
  if (!have("python3")) s.missingPrepare << "python3";
  if (prepareScript().isEmpty()) s.missingPrepare << "prepare.py";
  return s;
}

QString installInstructions(const CompileStatus &s) {
  QStringList out;
#if defined(Q_OS_LINUX)
  bool llvm = s.missingPrepare.contains(kClang) || s.missingPrepare.contains("lld");
  QStringList pkgs;
  if (s.missingPrepare.contains("cmake")) pkgs << "cmake";
  if (s.missingPrepare.contains("ninja")) pkgs << "ninja-build";
  if (s.missingPrepare.contains("python3")) pkgs << "python3";
  if (have("dnf")) {
    // Fedora ships LLVM 19 as versioned compat packages (clang-19, ld.lld-19, /usr/lib64/llvm19).
    if (s.missingPrepare.contains(kClang)) pkgs << "clang19";
    if (s.missingPrepare.contains("lld")) pkgs << "lld19";
    if (!s.k1Built) pkgs << "clang19-devel" << "llvm19-devel";
    if (!pkgs.isEmpty()) out << "sudo dnf install " + pkgs.join(' ');
  } else {
    if (llvm) out << "wget https://apt.llvm.org/llvm.sh && sudo bash llvm.sh 19     # clang-19, clang++-19, lld-19";
    if (!s.k1Built) pkgs << "libclang-19-dev" << "llvm-19-dev";
    if (!pkgs.isEmpty()) out << "sudo apt install " + pkgs.join(' ');
  }
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
  giveToUser(defaultSnapshotDir());
  ring_->setProgress(done_ ? 7 : 0, 7, done_ ? QStringLiteral("✓") : QStringLiteral("✗"));
  step_->setText(done_ ? tr("Ready. Compile and Full build can now run.") : tr("Preparation failed; see the log."));
  button_->setText(done_ ? tr("Done") : tr("Close"));
}
