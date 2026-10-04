/*
 * The tests the app ranks, and the reduction of a result document to them.
 * Each test is one kernel in one mode; values are read from the statistics
 * the C core computed (the document's "analysis" section), never recomputed.
 *
 * Copyright 2026 The Prismark Authors. Apache-2.0.
 */
#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVector>
#include <cmath>
#include <functional>
#include <optional>

struct Value {
  double v = NAN, lo = NAN, hi = NAN;
  bool hasCi() const { return std::isfinite(lo) && std::isfinite(hi); }
};

struct Group {
  QString id, label;
  QString desc; /* one plain sentence: what this group of tests is about */
};

/*
 * Everything the app shows about a test is plain language: the kernel ids
 * (K1-K10) and mode names of the spec stay in the result files.
 *
 *   name     short, for the test card
 *   title    complete, for the test page
 *   sub      one line under the name on the card
 *   desc     what the test measures and why it matters
 *   how      how it is measured
 *   read     how to read the number (unit, which direction is better)
 *   context  the chart or comparison that explains the result, shown under the ranking:
 *            "wake" (speed from rest by task length), "scaling:<kernel>" (speed-up by threads),
 *            "ratio:<name>" (a same-workload ratio by threads), "resp:<kernel>" (speed from rest), or ""
 */
struct Metric {
  QString id, group, name, title, sub, unit, desc, how, read, context;
  bool higherBetter;
  QStringList modes; /* what to run for this test alone */
  QString kernels;
  std::function<std::optional<Value>(const QJsonObject &doc)> get;
};

struct RunSummary {
  QString id, name, file, date; /* date: local "YYYY-MM-DD HH:MM", empty for references */
  bool reference = false, placeholder = false, complete = true, verified = false, quick = false;
  QJsonObject machine, tiers, harness, frontend, state;
  QJsonArray unavailable;
  QMap<QString, Value> metrics;
  struct ColdPt { double w; Value r; }; /* r: R_resp in percent */
  struct ScalePt { QString kernel; int n; Value s; };
  struct Ratio { QString name, kernel, variant; int n; Value v; };
  QVector<ColdPt> cold;
  QVector<ScalePt> scaling;
  QVector<Ratio> ratios;
  bool checksumsOk = true;
};

const QVector<Group> &groups();
const QVector<Metric> &metrics();
const Metric *findMetric(const QString &id);

/* Reduces a prismark/1 result document; returns false if it is not one. */
bool summarize(const QJsonObject &doc, const QString &file, RunSummary &out);

/* Reference systems with PLACEHOLDER values (not measurements), until real references exist. */
QVector<RunSummary> placeholderReferences();

QString formatValue(double v);

/* Plain-language names for the identifiers in result files. Unknown ids are returned unchanged. */
QString plainKernel(const QString &kernel, const QString &variant = QString()); /* "K2" -> "3D rendering" */
QString plainMode(const QString &mode);                                       /* "mc_threaded" -> "all cores, working together" */
QString plainRatio(const QString &name);                                      /* "R_serial" -> "Teamwork vs. separate copies" */
QString plainReason(const QString &reason);                                    /* why a test was not measured */
QString plainText(QString text);                                              /* replaces kernel ids in a runner message */

/* The glossary shown by "What do these terms mean?", as rich text. */
QString glossaryHtml();
