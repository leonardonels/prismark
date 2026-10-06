/*
 * Reference systems for the rankings, read from files: the repository's
 * references/ folder is built into the app (see references/README.md), and
 * users can add their own in the references folder next to their results.
 *
 * A file is either a real result document (prismark/1) or a placeholder
 * (prismark-reference/1) with hand-typed example values, which the app labels
 * as such.
 *
 * Copyright 2026 The Prismark Authors. Apache-2.0.
 */
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QJsonDocument>

#include "metrics.h"

namespace {

bool fromPlaceholder(const QJsonObject &doc, const QString &file, RunSummary &r) {
  r = RunSummary();
  r.id = QStringLiteral("reference-") + QFileInfo(file).completeBaseName();
  r.file = file;
  r.name = doc["name"].toString(QFileInfo(file).completeBaseName());
  r.reference = true;
  r.placeholder = doc["placeholder"].toBool(true);
  r.machine = doc["machine"].toObject();
  QString isa = r.machine["isa"].toString();
  QJsonObject tiers{{"baseline", QJsonObject{{"level", isa == "aarch64" ? "armv8.2-a" : "x86-64-v2"}}}};
  if (doc.contains("newest_instructions"))
    tiers["max"] = QJsonObject{{"level", doc["newest_instructions"].toString()}};
  r.tiers = tiers;
  QJsonObject results = doc["results"].toObject();
  for (auto it = results.begin(); it != results.end(); ++it) {
    if (!findMetric(it.key())) {
      qWarning().noquote() << QStringLiteral("%1: unknown test \"%2\" (see references/README.md)").arg(file, it.key());
      continue;
    }
    QJsonObject o = it.value().toObject();
    double v = o["value"].toDouble(NAN), u = o["uncertainty"].toDouble(NAN);
    if (!std::isfinite(v)) continue;
    Value val{v};
    if (std::isfinite(u)) {
      val.lo = v * (1 - u);
      val.hi = v * (1 + u);
    }
    r.metrics[it.key()] = val;
  }
  return true;
}

}  // namespace

bool readReference(const QString &file, RunSummary &out) {
  QFile f(file);
  if (!f.open(QIODevice::ReadOnly)) return false;
  QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
  if (!doc.isObject()) return false;
  QJsonObject o = doc.object();
  if (o["schema"].toString() == "prismark-reference/1") return fromPlaceholder(o, file, out);
  if (!summarize(o, file, out)) return false;
  out.reference = true;
  /* Its own id, as placeholders have: a reference is often a copy of a run that is also in the user's runs, and
     with the run's id both rows would be selected (and open) together. */
  out.id = QStringLiteral("reference-") + QFileInfo(file).completeBaseName();
  return true;
}

QVector<RunSummary> bundledReferences() {
  QVector<RunSummary> out;
  for (const QFileInfo &fi : QDir(QStringLiteral(":/references")).entryInfoList({"*.json"}, QDir::Files, QDir::Name)) {
    RunSummary r;
    if (readReference(fi.filePath(), r)) out.push_back(r);
    else qWarning().noquote() << QStringLiteral("%1: not a Prismark result or reference file").arg(fi.fileName());
  }
  return out;
}
