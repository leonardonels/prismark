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
  bool unsettled = false; /* a long test that hit its time limit before its speed settled */
  /* An all-cores test measured on fewer threads than the machine has (memory, or --max-threads): threads used
     and threads the machine has; 0 when it used them all. */
  int threads = 0, ofThreads = 0;
  /* Windows of the measurement in which the platform clamped the CPU far below its own minimum clock (firmware,
     e.g. BD PROCHOT), and the lowest clock seen: the score includes the clamp. */
  int clamped = 0;
  double clampedMhz = NAN;
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
 *            "ratio:R_build" (build-tool overhead by threads), "resp:<kernel>" (speed from rest),
 *            "isa" (which instruction sets were compared), or ""
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
  /* Median clock (MHz) and package power (W) over the measured windows of an all-cores series; NaN if unknown. */
  struct LoadPt { QString kernel; int n; double mhz, watts; };
  QVector<ColdPt> cold;
  QVector<ScalePt> scaling;
  QVector<LoadPt> load;
  QVector<Ratio> ratios;
  bool checksumsOk = true;
  /* How much slower each warm-up ended than it started (speed hot / speed cold, below 1 when it throttles), by
     mode: "st_sustained" (one core) and "mc_threaded" (all cores). */
  QMap<QString, Value> hot;
  int clampedSeries = 0;        /* series of the run with platform-clamped windows */
  double clampedMhz = NAN;      /* the lowest clamped clock among them */
};

const QVector<Group> &groups();
const QVector<Metric> &metrics();
const Metric *findMetric(const QString &id);

/* Reduces a prismark/1 result document; returns false if it is not one. */
bool summarize(const QJsonObject &doc, const QString &file, RunSummary &out);

/* The reference systems built into the app from the repository's references/ folder (references/README.md). */
QVector<RunSummary> bundledReferences();
/* Reads one reference file: a result document, or a placeholder with example values. */
bool readReference(const QString &file, RunSummary &out);

QString formatValue(double v);

/* Plain-language names for the identifiers in result files. Unknown ids are returned unchanged. */
QString plainKernel(const QString &kernel, const QString &variant = QString()); /* "K2" -> "3D rendering" */
QString plainMode(const QString &mode);                                       /* "mc_threaded" -> "all cores, working together" */
QString plainRatio(const QString &name);                                      /* "R_build" -> "Full build vs. in-memory compile" */
QString plainReason(const QString &reason);                                    /* why a test was not measured */
QString plainText(QString text);                                              /* replaces kernel ids in a runner message */

/*
 * Simple view: at most two tests per tab, chosen to run on every platform without setup and to be about
 * something people do. Advanced shows every test.
 */
bool isSimple(const QString &metricId);

/* Whether a platform (a result's machine.os) offers a test at all; mirrors kernel_offered() in the engine. */
bool offeredOn(const QString &kernel, const QString &os);

/* Instruction-set level of a result ("x86-64-v4") as the name people know ("AVX-512"); unknown levels unchanged. */
QString isaName(const QString &level);
/* The two instruction sets a run compared, e.g. "SSE4.2 → AVX-512", or "SSE4.2 only" without a newer one. */
QString isaPair(const QJsonObject &tiers);

/* The glossary shown by "What do these terms mean?", as rich text. */
QString glossaryHtml();
