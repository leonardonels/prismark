/* Copyright 2026 The Prismark Authors. Apache-2.0. */
#include "widgets.h"

#include <QDate>
#include <QKeyEvent>
#include <QLocale>
#include <QMap>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSvgRenderer>
#include <algorithm>
#include <cmath>

#include "theme.h"

namespace {
constexpr int kRowH = 56, kPad = 0, kRankW = 26; /* row tall enough for the name line, the bar and its range */

QPointF mousePos(QMouseEvent *e) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
  return e->position();
#else
  return e->localPos();
#endif
}
}  // namespace

QColor seriesColor(int i) {
  static const QColor fixed[] = {QColor(242, 120, 12), QColor(0, 163, 137), QColor(138, 99, 255)};
  if (i == 0) return theme().link;
  return i % 4 == 0 ? theme().text : fixed[(i - 1) % 4];
}

void paintLogo(QPainter &p, const QRectF &r) {
  static QSvgRenderer logo(QStringLiteral(":/prismark.svg"));
  double s = std::min(r.width(), r.height());
  logo.render(&p, QRectF(r.center() - QPointF(s / 2, s / 2), QSizeF(s, s)));
}

/* ---------- RankingView ---------- */

RankingView::RankingView(QWidget *parent) : QWidget(parent) {
  setFocusPolicy(Qt::StrongFocus);
  setMouseTracking(true);
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
}

void RankingView::setRows(QVector<RankRow> rows, bool higherBetter, const QString &unit, const QString &selected) {
  higher_ = higherBetter;
  unit_ = unit;
  selected_ = selected;
  /* Bars are proportional to speed: the value, or its inverse when lower is better. */
  std::sort(rows.begin(), rows.end(), [&](const RankRow &a, const RankRow &b) { return speed(a.value.v) > speed(b.value.v); });
  rows_ = std::move(rows);
  updateGeometry();
  update();
}

double RankingView::speed(double v) const { return higher_ ? v : 1.0 / v; }
QSize RankingView::sizeHint() const { return {520, (rows_.isEmpty() ? 2 : int(rows_.size())) * kRowH + 2 * kPad}; }
QSize RankingView::minimumSizeHint() const { return {280, sizeHint().height()}; }

int RankingView::rowAt(int y) const {
  int i = (y - kPad) / kRowH;
  return y >= kPad && i < rows_.size() ? i : -1;
}

void RankingView::mousePressEvent(QMouseEvent *e) {
  int i = rowAt(int(mousePos(e).y()));
  if (i < 0) return;
  selected_ = rows_[i].runId;
  update();
  emit runSelected(selected_);
}

void RankingView::mouseMoveEvent(QMouseEvent *e) {
  int i = rowAt(int(mousePos(e).y()));
  if (i != hover_) {
    hover_ = i;
    setCursor(i >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
    update();
  }
}

void RankingView::leaveEvent(QEvent *) {
  hover_ = -1;
  update();
}

void RankingView::keyPressEvent(QKeyEvent *e) {
  int cur = -1;
  for (int i = 0; i < rows_.size(); i++)
    if (rows_[i].runId == selected_) cur = i;
  int next = e->key() == Qt::Key_Down ? cur + 1 : e->key() == Qt::Key_Up ? cur - 1 : -2;
  if (next == -2 || rows_.isEmpty()) return QWidget::keyPressEvent(e);
  next = std::clamp(next, 0, int(rows_.size()) - 1);
  selected_ = rows_[next].runId;
  update();
  emit runSelected(selected_);
}

void RankingView::paintEvent(QPaintEvent *) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  const Theme &t = theme();
  if (rows_.isEmpty()) {
    p.setPen(t.muted);
    p.setFont(uiFont(10.5));
    p.drawText(rect(), Qt::AlignCenter, tr("No result for this test yet.\nPress Run on its card to measure it."));
    return;
  }
  double top = 0;
  for (const RankRow &r : rows_) {
    top = std::max(top, speed(r.value.v));
    if (r.value.hasCi()) top = std::max({top, speed(r.value.lo), speed(r.value.hi)});
  }
  top *= 1.02;
  const RankRow *refRow = nullptr;
  for (const RankRow &r : rows_)
    if (r.runId == selected_) refRow = &r;

  QFont fName = uiFont(10.5, QFont::Medium), fNameBold = uiFont(10.5, QFont::Bold),
        fVal = displayFont(16, QFont::DemiBold, 75);
  QFont fSmall = monoFont(8), fRel = monoFont(8.5), fTag = labelFont(7), fRank = monoFont(9, QFont::Medium);
  QFontMetrics mName(fName), mVal(fVal), mSmall(fSmall), mRel(fRel), mTag(fTag);
  /* the columns on the right line up from row to row */
  double valCol = 0, relCol = mRel.horizontalAdvance(QStringLiteral("+100.0%"));
  for (const RankRow &r : rows_) valCol = std::max<double>(valCol, mVal.horizontalAdvance(formatValue(r.value.v)));
  int w = width();
  for (int i = 0; i < rows_.size(); i++) {
    const RankRow &r = rows_[i];
    QRectF row(0, i * kRowH, w, kRowH);
    bool sel = r.runId == selected_;
    /* the highlighted result: a raised row with a volt edge. Its bar keeps its colour: yours or a reference. */
    p.setPen(Qt::NoPen);
    if (sel || i == hover_) {
      p.setBrush(t.raised);
      p.drawRect(row);
    }
    if (sel) {
      p.setBrush(t.accent);
      p.drawRect(QRectF(row.left(), row.top(), 6, row.height()));
    }
    p.setPen(QPen(t.border, 1));
    p.drawLine(QPointF(row.left(), row.top() + 0.5), QPointF(row.right(), row.top() + 0.5));
    /* rank */
    p.setFont(fRank);
    p.setPen(t.text);
    p.drawText(QRectF(row.left() + 20, row.top(), kRankW, row.height()), Qt::AlignVCenter | Qt::AlignLeft,
               QStringLiteral("%1").arg(i + 1, 2, 10, QLatin1Char('0')));

    double x0 = row.left() + 20 + kRankW + 8, x1 = row.right() - 20;
    /* value and relative difference */
    double vy = row.center().y() + mVal.ascent() / 2.0 - 2;
    QString rel = QStringLiteral("—");
    if (refRow && refRow != &r) {
      double d = 100.0 * (speed(r.value.v) / speed(refRow->value.v) - 1);
      rel = QString("%1%2%").arg(d >= 0 ? "+" : "−").arg(formatValue(std::abs(d)));
    }
    p.setFont(fRel);
    p.setPen(rel.startsWith('+') ? t.good : rel.startsWith(QChar(0x2212)) ? t.bad : t.muted);
    p.drawText(QRectF(x1 - relCol, row.top(), relCol, row.height()), Qt::AlignVCenter | Qt::AlignRight, rel);
    double valRight = x1 - relCol - 12;
    p.setFont(fVal);
    p.setPen(t.text);
    QString val = formatValue(r.value.v);
    p.drawText(QPointF(valRight - mVal.horizontalAdvance(val), vy), val);

    /* the name line: name, where it ran, then tags where they fit */
    double right = valRight - valCol - 14, ty = row.top() + 8 + mName.ascent();
    p.setFont(r.yours ? fNameBold : fName);
    QFontMetrics fm(p.font());
    QString name = fm.elidedText(r.name, Qt::ElideRight, int(right - x0));
    p.setPen(t.text);
    p.drawText(QPointF(x0, ty), name);
    double nx = x0 + fm.horizontalAdvance(name) + 8;
    if (!r.detail.isEmpty() && nx + 30 < right) {
      p.setFont(fSmall);
      p.setPen(t.muted);
      QString detail = mSmall.elidedText(r.detail, Qt::ElideRight, int(right - nx));
      p.drawText(QPointF(nx, ty), detail);
      nx += mSmall.horizontalAdvance(detail) + 8;
    }
    QString tagsText = r.tags.join(" · ");
    double tagsW = tagsText.isEmpty() ? 0 : mTag.horizontalAdvance(tagsText.toUpper()) + 12;
    if (tagsW && nx + tagsW <= right) {
      QRectF tr(nx, ty - mTag.ascent() - 2, tagsW, mTag.height() + 3);
      bool warn = r.tags.contains("placeholder") || r.tags.contains("did not settle");
      p.setPen(Qt::NoPen);
      p.setBrush(warn ? t.warnBg : t.recess);
      p.drawRect(tr);
      p.setFont(fTag);
      p.setPen(warn ? t.warnText : t.text);
      p.drawText(tr, Qt::AlignCenter, tagsText);
    }

    /* bar: square, on no track; yours in ink or volt, references in signal blue */
    QRectF bar(x0, row.top() + 30, right - x0, 8);
    QRectF fill = bar;
    fill.setWidth(std::max(3.0, bar.width() * speed(r.value.v) / top));
    p.setPen(Qt::NoPen);
    p.setBrush(r.yours ? t.mine : t.ref);
    p.drawRect(fill);
    if (r.value.hasCi()) {
      double a = bar.left() + bar.width() * speed(r.value.lo) / top, b = bar.left() + bar.width() * speed(r.value.hi) / top;
      QColor wc = t.text;
      wc.setAlphaF(0.7);
      p.setPen(QPen(wc, 1.4, Qt::SolidLine, Qt::FlatCap));
      double cy = bar.bottom() + 6;
      p.drawLine(QPointF(std::min(a, b), cy), QPointF(std::max(a, b), cy));
      p.drawLine(QPointF(a, cy - 3), QPointF(a, cy + 3));
      p.drawLine(QPointF(b, cy - 3), QPointF(b, cy + 3));
    }
  }
}

/* ---------- ChartView ---------- */

ChartView::ChartView(QWidget *parent) : QWidget(parent) {
  QSizePolicy sp(QSizePolicy::Expanding, QSizePolicy::Preferred);
  sp.setHeightForWidth(true);
  setSizePolicy(sp);
}

void ChartView::setData(QVector<ChartLine> lines, const QString &xLabel, double yMin, double yMax, bool ideal) {
  lines_ = std::move(lines);
  xLabel_ = xLabel;
  yMin_ = yMin;
  yMax_ = yMax;
  ideal_ = ideal;
  updateGeometry();
  update();
}

/* The smallest tidy number (1, 1.5, 2, 2.5, 3, 4, 5, 6, 8 × a power of ten) at or above v, for axis tops. */
static double niceCeil(double v) {
  if (!(v > 0)) return 1;
  double step = std::pow(10.0, std::floor(std::log10(v)));
  for (double m : {1.0, 1.5, 2.0, 2.5, 3.0, 4.0, 5.0, 6.0, 8.0, 10.0})
    if (m * step >= v * (1 - 1e-9)) return m * step;
  return 10 * step;
}

void ChartView::paintEvent(QPaintEvent *) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  const Theme &t = theme();
  QFont small = monoFont(8), val = displayFont(11, QFont::DemiBold, 75);
  QFontMetrics fm(small), fv(val);

  /* the steps, in order, and the value range */
  QVector<double> xs;
  double y0 = INFINITY, y1 = -INFINITY, e0 = INFINITY, e1 = -INFINITY;
  for (const ChartLine &l : lines_)
    for (const auto &pt : l.pts) {
      if (!xs.contains(pt[0])) xs.push_back(pt[0]);
      if (std::isfinite(pt[1])) y0 = std::min(y0, pt[1]), y1 = std::max(y1, pt[1]);
      for (int k = 2; k < 4; k++)
        if (std::isfinite(pt[k])) e0 = std::min(e0, pt[k]), e1 = std::max(e1, pt[k]);
    }
  if (xs.isEmpty() || !std::isfinite(y1)) return;
  std::sort(xs.begin(), xs.end());
  /* Intervals widen the axis by at most half the values' span: one wild interval (a run disturbed by throttling)
     would otherwise squash every column. Whiskers beyond it are clipped. */
  double room = std::max(y1 - y0, std::abs(y1)) * 0.5;
  if (std::isfinite(e1)) y1 = std::max(y1, std::min(e1, y1 + room));
  y0 = std::isfinite(yMin_) ? yMin_ : std::min(0.0, y0);
  if (ideal_) y1 = std::max(y1, xs.last());
  /* four grid steps of a tidy size */
  y1 = std::isfinite(yMax_) ? yMax_ : y0 + 4 * niceCeil((y1 * 1.05 - y0) / 4);
  if (y1 <= y0) y1 = y0 + 1;

  /* a whole number without decimals (128, not 128.0) */
  auto tick = [](double v) { return v == std::round(v) && std::abs(v) < 1e9 ? QString::number(qint64(v)) : formatValue(v); };
  QString unit = yUnit_.isEmpty() ? QString() : " " + yUnit_;
  bool legend = lines_.size() > 1;
  double legendH = legend || ideal_ ? fm.height() + 10 : 4, labelH = fv.height() + 2;
  double left = 0; /* the unit only on the top line */
  for (int k = 0; k <= 4; k++) left = std::max(left, double(fm.horizontalAdvance(tick(y0 + (y1 - y0) * k / 4) + (k == 4 ? unit : QString()))));
  left += 12;
  QRectF plot(left, legendH + labelH, width() - left - 4, height() - legendH - labelH - fm.height() * (xLabel_.isEmpty() ? 1 : 2) - 12);
  auto fy = [&](double y) { return plot.bottom() - (std::clamp(y, y0, y1) - y0) / (y1 - y0) * plot.height(); };

  /* grid: four steps, thin; the axis does not start at 0 when the strip is fitted to its values */
  p.setFont(small);
  for (int k = 0; k <= 4; k++) {
    double yv = y0 + (y1 - y0) * k / 4;
    p.setPen(QPen(k ? t.border : t.muted, k ? 1 : 1.5));
    p.drawLine(QPointF(plot.left(), fy(yv)), QPointF(plot.right(), fy(yv)));
    QString s = tick(yv) + (k == 4 ? unit : QString());
    p.setPen(t.faint);
    p.drawText(QPointF(plot.left() - fm.horizontalAdvance(s) - 8, fy(yv) + fm.ascent() / 2.6), s);
  }

  /* one group per step: columns side by side, a gap between groups */
  int n = int(xs.size()), m = int(lines_.size());
  double slot = plot.width() / n, gap = std::max(2.0, std::min(slot * 0.28, 24.0));
  double colW = std::max(1.0, (slot - gap) / m - (m > 1 ? 2 : 0));
  bool labelAll = colW >= fv.horizontalAdvance(QStringLiteral("00.0")) + 4;
  /* with narrow columns, values only on the first, the last, the lowest and the highest step of each line */
  auto keyPoints = [&](const ChartLine &l) {
    QSet<double> keep;
    if (l.pts.isEmpty()) return keep;
    keep << l.pts.first()[0] << l.pts.last()[0];
    auto lo = std::min_element(l.pts.begin(), l.pts.end(), [](const auto &a, const auto &b) { return a[1] < b[1]; });
    auto hi = std::max_element(l.pts.begin(), l.pts.end(), [](const auto &a, const auto &b) { return a[1] < b[1]; });
    keep << (*lo)[0] << (*hi)[0];
    return keep;
  };
  for (int li = 0; li < m; li++) {
    const ChartLine &l = lines_[li];
    QColor c = seriesColor(l.color >= 0 ? l.color : li);
    QSet<double> keep = labelAll ? QSet<double>() : keyPoints(l);
    for (const auto &pt : l.pts) {
      if (!std::isfinite(pt[1])) continue;
      int xi = int(xs.indexOf(pt[0]));
      double x = plot.left() + xi * slot + gap / 2 + li * (colW + (m > 1 ? 2 : 0));
      QRectF col(x, fy(pt[1]), colW, plot.bottom() - fy(pt[1]));
      if (ideal_) { /* the perfect speed-up for this many threads: an outline above the column */
        p.setPen(QPen(t.muted, 1.2, Qt::DashLine));
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(x, fy(pt[0]), colW, plot.bottom() - fy(pt[0])));
      }
      bool muted = l.muted.contains(pt[0]);
      p.setPen(muted ? QPen(t.faint, 1.2) : Qt::NoPen);
      p.setBrush(muted ? Qt::transparent : c);
      p.drawRect(muted ? col.adjusted(0.6, 0.6, -0.6, 0) : col);
      if (std::isfinite(pt[2]) && std::isfinite(pt[3]) && colW >= 6) { /* uncertainty: a whisker through the top */
        QColor wc = t.text;
        wc.setAlphaF(0.75);
        p.setPen(QPen(wc, 1.3));
        p.setClipRect(plot.adjusted(0, -labelH, 0, 0));
        double cx = col.center().x(), a = fy(pt[2]), b = fy(pt[3]);
        p.drawLine(QPointF(cx, a), QPointF(cx, b));
        p.drawLine(QPointF(cx - 3, a), QPointF(cx + 3, a));
        p.drawLine(QPointF(cx - 3, b), QPointF(cx + 3, b));
        p.setClipping(false);
      }
      if (labelAll || keep.contains(pt[0])) {
        QString s = tick(pt[1]);
        double tw = fv.horizontalAdvance(s);
        double top = std::min(col.top(), std::isfinite(pt[3]) ? fy(pt[3]) : col.top());
        if (ideal_) top = std::min(top, fy(pt[0]));
        double tx = std::clamp(col.center().x() - tw / 2, plot.left(), plot.right() - tw);
        p.setFont(val);
        p.setPen(muted ? t.faint : t.text);
        p.drawText(QPointF(tx, top - 4), s);
      }
    }
  }

  /* step names under the groups, skipping those that would collide */
  p.setFont(small);
  p.setPen(t.muted);
  double lastRight = -1e9;
  for (int i = 0; i < n; i++) {
    QString s = tick(xs[i]);
    double tw = fm.horizontalAdvance(s), x = plot.left() + (i + 0.5) * slot - tw / 2;
    if (x < lastRight + 6 && i != n - 1) continue;
    p.drawText(QPointF(x, plot.bottom() + fm.ascent() + 5), s);
    lastRight = x + tw;
  }
  if (!xLabel_.isEmpty())
    p.drawText(QRectF(plot.left(), height() - fm.height() - 2, plot.width(), fm.height()), Qt::AlignLeft, xLabel_.toUpper());

  /* legend: only for several lines */
  if (legend) {
    double lx = plot.left();
    for (int i = 0; i < m; i++) {
      p.setPen(Qt::NoPen);
      p.setBrush(seriesColor(lines_[i].color >= 0 ? lines_[i].color : i));
      p.drawRect(QRectF(lx, 2 + fm.height() / 2.0 - 4, 10, 8));
      p.setPen(t.muted);
      p.drawText(QPointF(lx + 14, 2 + fm.ascent()), lines_[i].name);
      lx += 14 + fm.horizontalAdvance(lines_[i].name) + 18;
    }
  }
  if (ideal_) {
    QString s = QObject::tr("dashed: perfect scaling");
    p.setPen(t.faint);
    p.drawText(QPointF(plot.right() - fm.horizontalAdvance(s), legend ? 2 + fm.ascent() : fm.ascent()), s);
  }
}

/* ---------- BarsView ---------- */

BarsView::BarsView(QVector<Bar> bars, QWidget *parent) : QWidget(parent), bars_(std::move(bars)) {
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  setFixedHeight(sizeHint().height());
}

QSize BarsView::sizeHint() const {
  QFontMetrics fm(monoFont(9));
  return {320, int(bars_.size() * (fm.height() * 2 + 14))};
}

void BarsView::paintEvent(QPaintEvent *) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  const Theme &t = theme();
  QFont f = monoFont(9);
  p.setFont(f);
  QFontMetrics fm(f);
  double top = 0;
  for (const Bar &b : bars_) top = std::max(top, b.value);
  double y = 0, rowH = fm.height() * 2 + 14;
  for (const Bar &b : bars_) {
    p.setPen(t.muted);
    p.drawText(QPointF(0, y + fm.ascent()), b.label);
    double tw = fm.horizontalAdvance(b.text) + 10, w = (width() - tw) * (top > 0 ? b.value / top : 0);
    QRectF bar(0, y + fm.height() + 4, std::max(w, 2.0), fm.height());
    p.setPen(Qt::NoPen);
    p.setBrush(seriesColor(b.color));
    p.drawRect(bar);
    p.setPen(t.text);
    p.drawText(QPointF(bar.right() + 8, bar.top() + fm.ascent()), b.text);
    y += rowH;
  }
}

/* ---------- TestCard ---------- */

TestCard::TestCard(const QString &id, QWidget *parent) : QAbstractButton(parent), id_(id) {
  setCheckable(true);
  setAutoExclusive(true);
  setMouseTracking(true);
  setCursor(Qt::PointingHandCursor);
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  setFixedHeight(86);
}

void TestCard::setContent(const QString &name, const QString &sub, const QString &score, const QString &unit,
                          const QString &note) {
  name_ = name;
  sub_ = sub;
  score_ = score;
  unit_ = unit;
  note_ = note;
  setToolTip(sub);
  update();
}

void TestCard::setRunLabel(const QString &label) {
  runLabel_ = label;
  update();
}

QRectF TestCard::runRect() const { return QRectF(width() - 82, height() / 2.0 - 28, 72, 56); }

void TestCard::mousePressEvent(QMouseEvent *e) {
  if (runRect().contains(mousePos(e))) {
    emit runRequested(id_);
    return;
  }
  QAbstractButton::mousePressEvent(e);
}

void TestCard::mouseMoveEvent(QMouseEvent *e) {
  bool h = runRect().contains(mousePos(e));
  if (h != hoverRun_) {
    hoverRun_ = h;
    update();
  }
  QAbstractButton::mouseMoveEvent(e);
}

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
void TestCard::enterEvent(QEnterEvent *) {
#else
void TestCard::enterEvent(QEvent *) {
#endif
  hover_ = true;
  update();
}

void TestCard::leaveEvent(QEvent *) {
  hover_ = hoverRun_ = false;
  update();
}

void TestCard::paintEvent(QPaintEvent *) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  const Theme &t = theme();
  QRectF r = rect();
  bool on = isChecked();
  /* the open test is a volt block, the others panels */
  QColor fg = on ? t.onAccent : t.text;
  p.setPen(Qt::NoPen);
  p.setBrush(on ? t.accent : hover_ ? t.raised : t.card);
  p.drawRect(r);
  double x = r.left() + 14, right = runRect().left() - 8;
  p.setFont(uiFont(10.5, QFont::Bold));
  p.setPen(fg);
  QFontMetrics fn(p.font());
  p.drawText(QPointF(x, r.top() + 9 + fn.ascent()), fn.elidedText(name_, Qt::ElideRight, int(right - x)));
  p.setFont(uiFont(8.5));
  QFontMetrics fs(p.font());
  p.drawText(QPointF(x, r.top() + 27 + fs.ascent()), fs.elidedText(sub_, Qt::ElideRight, int(right - x)));
  double by = r.bottom() - 9;
  if (!score_.isEmpty()) {
    p.setFont(displayFont(22, QFont::DemiBold, 50));
    QFontMetrics fv(p.font());
    p.drawText(QPointF(x, by), score_);
    p.setFont(monoFont(7.5));
    p.drawText(QPointF(x + fv.horizontalAdvance(score_) + 5, by), unit_);
  } else {
    p.setFont(monoFont(8.5));
    QFontMetrics fo(p.font());
    p.drawText(QPointF(x, by), fo.elidedText(note_, Qt::ElideRight, int(right - x)));
  }
  /* run button: on the open test an ink block with volt type; elsewhere recessed, so only one action is loud */
  QRectF b = runRect();
  QColor bbg = on ? t.onAccent : hoverRun_ ? t.accent : hover_ ? t.card : t.raised, bfg = on ? t.accent : hoverRun_ ? t.onAccent : t.text;
  if (on && hoverRun_) bbg = bbg.lighter(160);
  p.setPen(Qt::NoPen);
  p.setBrush(bbg);
  p.drawRect(b);
  p.setPen(bfg);
  p.setFont(displayFont(9, QFont::ExtraBold, 125));
  QString label = runLabel_.toUpper();
  label.replace(' ', '\n');
  p.drawText(b, Qt::AlignCenter, label);
}

/* ---------- Segmented ---------- */

Segmented::Segmented(QWidget *parent) : QWidget(parent) {
  setCursor(Qt::PointingHandCursor);
  setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
  setFixedHeight(44);
}

void Segmented::setItems(const QStringList &items) {
  items_ = items;
  updateGeometry();
  update();
}

void Segmented::setCurrent(int i) {
  cur_ = i;
  update();
}

void Segmented::setNumbered(int first) {
  numbered_ = first;
  if (!filling_) setFixedHeight(first ? 54 : 44);
  updateGeometry();
  update();
}

void Segmented::setFilling(int h) {
  filling_ = true;
  setFixedHeight(h);
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  updateGeometry();
}

void Segmented::setCompact() {
  compact_ = true;
  setFixedHeight(28);
  updateGeometry();
}

QVector<QRectF> Segmented::rects() const {
  QFontMetrics fm(uiFont(compact_ ? 8.5 : 10, QFont::Bold));
  QVector<QRectF> out;
  double x = 0;
  if (filling_ && !items_.isEmpty()) { /* equal widths; the last takes what rounding leaves */
    const double gap = 8, w = (width() - gap * (items_.size() - 1)) / items_.size();
    for (int i = 0; i < items_.size(); i++) out.push_back(QRectF(i * (w + gap), 0, w, height()));
    return out;
  }
  for (const QString &s : items_) {
    double w = std::max(fm.horizontalAdvance(s) + (compact_ ? 22 : 32), numbered_ ? 96 : 0);
    out.push_back(QRectF(x, 0, w, height()));
    x += w + (compact_ ? 2 : 4);
  }
  return out;
}

QSize Segmented::sizeHint() const {
  if (filling_) return {int(items_.size()) * 80, height()};
  auto r = rects();
  return {r.isEmpty() ? 100 : int(r.last().right()), height()};
}

void Segmented::mousePressEvent(QMouseEvent *e) {
  auto r = rects();
  for (int i = 0; i < r.size(); i++)
    if (r[i].contains(mousePos(e)) && i != cur_) {
      cur_ = i;
      update();
      emit changed(i);
    }
}

/* Square tabs: the current one a panel with a volt foot, the others recessed. Inside a panel, the current one is
   raised so it still stands out. */
void Segmented::paintEvent(QPaintEvent *) {
  QPainter p(this);
  const Theme &t = theme();
  auto r = rects();
  for (int i = 0; i < r.size(); i++) {
    bool on = i == cur_;
    p.setPen(Qt::NoPen);
    const QColor fg = onSignal_ ? t.onAccent : t.text;
    p.setBrush(onSignal_ ? (on ? QColor(247, 248, 249) : QColor(201, 208, 218)) : on ? (numbered_ ? t.card : t.raised) : t.recess);
    p.drawRect(r[i]);
    int foot = compact_ ? 3 : 5;
    if (on) {
      p.setBrush(t.accent);
      p.drawRect(QRectF(r[i].left(), r[i].bottom() - foot + 1, r[i].width(), foot));
    }
    QRectF tr = compact_ ? r[i].adjusted(8, 0, -4, -foot + 1)
                : onSignal_ ? r[i].adjusted(2, 0, -2, -foot + 1) : r[i].adjusted(12, 0, -8, -foot + 1);
    if (numbered_) {
      p.setFont(monoFont(8));
      p.setPen(t.text);
      p.drawText(tr.adjusted(0, 9, 0, 0), Qt::AlignTop | Qt::AlignLeft, QStringLiteral("%1").arg(numbered_ + i, 2, 10, QLatin1Char('0')));
    }
    QFont f = uiFont(compact_ ? 8.5 : 10, QFont::Bold);
    p.setFont(f);
    p.setPen(fg);
    p.drawText(tr.adjusted(0, 0, 0, numbered_ ? -8 : 0), (numbered_ ? Qt::AlignBottom | Qt::AlignLeft : onSignal_ ? int(Qt::AlignCenter) : Qt::AlignVCenter | Qt::AlignLeft),
               QFontMetrics(f).elidedText(items_[i], Qt::ElideRight, int(tr.width())));
  }
}

/* ---------- Toggle ---------- */

Toggle::Toggle(const QString &text, QWidget *parent) : QAbstractButton(parent) {
  setText(text);
  setCheckable(true);
  setCursor(Qt::PointingHandCursor);
}

QSize Toggle::sizeHint() const {
  return {text().isEmpty() ? 58 : QFontMetrics(uiFont(10, QFont::Medium)).horizontalAdvance(text()) + 70, 32};
}

/* A square switch that states its position: OFF on a recessed track with the knob on the left, ON on volt with an
   ink knob on the right. The knob is solid in both, so off reads as a setting, not as disabled. */
void Toggle::paintEvent(QPaintEvent *) {
  QPainter p(this);
  const Theme &t = theme();
  bool on = isChecked();
  QRectF track(0, height() / 2.0 - 13, 58, 26);
  p.setPen(Qt::NoPen);
  p.setBrush(on ? t.accent : t.recess);
  p.drawRect(track);
  QRectF knob(0, 0, 20, 20);
  knob.moveCenter(QPointF(on ? track.right() - 13 : track.left() + 13, track.center().y()));
  p.setBrush(on ? t.onAccent : t.text);
  p.drawRect(knob);
  p.setFont(monoFont(7.5, QFont::DemiBold));
  p.setPen(on ? t.onAccent : t.text);
  QRectF word = on ? QRectF(track.left() + 4, track.top(), knob.left() - track.left() - 4, track.height())
                   : QRectF(knob.right(), track.top(), track.right() - knob.right() - 2, track.height());
  p.drawText(word, Qt::AlignCenter, on ? tr("ON") : tr("OFF"));
  p.setPen(onSignal_ ? QColor(247, 248, 249) : t.text);
  p.setFont(uiFont(10, QFont::Medium));
  p.drawText(QRectF(68, 0, width() - 68, height()), Qt::AlignVCenter | Qt::AlignLeft, text());
}

/* ---------- StepRing ---------- */

StepRing::StepRing(QWidget *parent) : QWidget(parent) { setFixedSize(170, 170); }

void StepRing::setProgress(int step, int steps, const QString &centre) {
  step_ = step;
  steps_ = steps;
  centre_ = centre;
  update();
}

/* A volt block with the step in large condensed figures, and the run as a strip of parts along its foot. */
void StepRing::paintEvent(QPaintEvent *) {
  QPainter p(this);
  const Theme &t = theme();
  QRectF r = rect();
  p.setPen(Qt::NoPen);
  p.setBrush(t.accent);
  p.drawRect(r);
  p.setBrush(t.onAccent);
  p.drawRect(QRectF(r.left(), r.top(), r.width(), 8));
  QRectF strip(r.left() + 12, r.bottom() - 24, r.width() - 24, 10);
  if (steps_ > 0 && steps_ <= 40) {
    double gap = 3, w = (strip.width() - gap * (steps_ - 1)) / steps_;
    for (int i = 0; i < steps_; i++) {
      QRectF seg(strip.left() + i * (w + gap), strip.top(), w, strip.height());
      if (i < step_) {
        p.setPen(Qt::NoPen);
        p.setBrush(t.onAccent);
      } else {
        p.setPen(QPen(t.onAccent, 1.2));
        p.setBrush(Qt::NoBrush);
      }
      p.drawRect(i < step_ ? seg : seg.adjusted(0.6, 0.6, -0.6, -0.6));
    }
  } else if (steps_ > 0) {
    p.setPen(QPen(t.onAccent, 1.2));
    p.setBrush(Qt::NoBrush);
    p.drawRect(strip.adjusted(0.6, 0.6, -0.6, -0.6));
    p.setPen(Qt::NoPen);
    p.setBrush(t.onAccent);
    p.drawRect(QRectF(strip.left(), strip.top(), strip.width() * std::min(step_, steps_) / steps_, strip.height()));
  }
  p.setPen(t.onAccent);
  double pt = centre_.size() <= 3 ? 50 : centre_.size() <= 5 ? 38 : 28;
  p.setFont(displayFont(pt, QFont::DemiBold, 50));
  p.drawText(r.adjusted(12, 12, -12, -34), Qt::AlignLeft | Qt::AlignBottom, centre_);
}

/* ---------- SideColumn ---------- */

SideColumn::SideColumn(const QStringList &items, QWidget *parent) : QWidget(parent), items_(items) {
  setCursor(Qt::PointingHandCursor);
  setFixedWidth(72);
}

void SideColumn::setCurrent(int i) {
  cur_ = i;
  update();
}

/* The current place is a little taller than the others. */
QVector<QRectF> SideColumn::rects() const {
  const double gap = 8, unit = (height() - gap * (items_.size() - 1)) / (items_.size() + 0.15);
  QVector<QRectF> out;
  double y = 0;
  for (int i = 0; i < items_.size(); i++) {
    double h = i == cur_ ? unit * 1.15 : unit;
    out.push_back(QRectF(0, y, width(), h));
    y += h + gap;
  }
  return out;
}

void SideColumn::mousePressEvent(QMouseEvent *e) {
  auto r = rects();
  for (int i = 0; i < r.size(); i++)
    if (r[i].contains(mousePos(e)) && i != cur_) {
      cur_ = i;
      update();
      emit changed(i);
    }
}

void SideColumn::paintEvent(QPaintEvent *) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  const Theme &t = theme();
  auto r = rects();
  for (int i = 0; i < r.size(); i++) {
    bool on = i == cur_;
    p.setPen(Qt::NoPen);
    p.setBrush(on ? t.card : t.recess);
    p.drawRect(r[i]);
    if (on) {
      p.setBrush(t.accent);
      p.drawRect(QRectF(r[i].left(), r[i].top(), 6, r[i].height()));
    }
    p.setPen(t.text);
    p.setFont(monoFont(8));
    p.drawText(r[i].adjusted(0, 14, 0, 0), Qt::AlignTop | Qt::AlignHCenter, QStringLiteral("%1").arg(i + 1, 2, 10, QLatin1Char('0')));
    /* the label stands at the foot of its block and reads bottom to top */
    p.save();
    p.translate(r[i].center().x(), r[i].bottom() - 16);
    p.rotate(-90);
    p.setFont(displayFont(15, QFont::ExtraBold, 125));
    QFontMetrics fm(p.font());
    p.drawText(QPointF(0, (fm.ascent() - fm.descent()) / 2.0), items_[i].toUpper());
    p.restore();
  }
}

/* ---------- GroupProgress ---------- */

GroupProgress::GroupProgress(QWidget *parent) : QWidget(parent) {
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void GroupProgress::setContent(const QString &group, const QString &position, int done, int total) {
  group_ = group;
  position_ = position;
  done_ = done;
  total_ = total;
  update();
}

void GroupProgress::paintEvent(QPaintEvent *) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  const Theme &t = theme();
  p.fillRect(rect(), t.accent);
  if (height() < 36) return; /* too little room to say anything: a volt band */
  p.setPen(t.onAccent);
  QRectF in = QRectF(rect()).adjusted(14, 10, -14, -10);
  const QString count = QStringLiteral("%1/%2").arg(done_).arg(total_);
  p.setFont(labelFont(8));
  if (height() < 96) { /* one line */
    p.drawText(in, Qt::AlignVCenter | Qt::AlignLeft, group_);
    p.setFont(displayFont(16, QFont::DemiBold, 50));
    p.drawText(in, Qt::AlignVCenter | Qt::AlignRight, count);
    return;
  }
  p.drawText(in, Qt::AlignTop | Qt::AlignLeft, group_);
  p.drawText(in, Qt::AlignTop | Qt::AlignRight, position_);
  /* the count as large as the free height allows */
  const QRectF middle = in.adjusted(0, 18, 0, -20);
  double pt = 120;
  QFont f = displayFont(pt, QFont::DemiBold, 50);
  while (pt > 16 && (QFontMetrics(f).capHeight() > middle.height() - 4 || QFontMetrics(f).horizontalAdvance(count) > in.width()))
    f = displayFont(pt -= 4, QFont::DemiBold, 50);
  p.setFont(f);
  p.drawText(middle, Qt::AlignVCenter | Qt::AlignLeft, count);
  p.setFont(uiFont(9.5, QFont::DemiBold));
  p.drawText(in, Qt::AlignBottom | Qt::AlignLeft, tr("Measured in your runs"));
}

/* ---------- RunTicket ---------- */

RunTicket::RunTicket(const QString &date, const QString &kind, const QString &note, QWidget *parent)
    : QAbstractButton(parent), date_(date), kind_(kind), note_(note) {
  setCheckable(true);
  setCursor(Qt::PointingHandCursor);
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  setFixedHeight(112);
}

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
void RunTicket::enterEvent(QEnterEvent *) {
#else
void RunTicket::enterEvent(QEvent *) {
#endif
  hover_ = true;
  update();
}

void RunTicket::leaveEvent(QEvent *) {
  hover_ = false;
  update();
}

void RunTicket::paintEvent(QPaintEvent *) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  const Theme &t = theme();
  QRectF r = rect();
  bool on = isChecked();
  QColor fg = on ? t.onAccent : t.text;
  p.setPen(Qt::NoPen);
  p.setBrush(on ? t.accent : hover_ ? t.raised : t.card);
  p.drawRect(r);
  /* date_ is "YYYY-MM-DD HH:MM" */
  const QDate d = QDate::fromString(date_.left(10), QStringLiteral("yyyy-MM-dd"));
  const double stub = 108;
  p.setPen(fg);
  p.setFont(displayFont(38, QFont::DemiBold, 50));
  QFontMetrics fd(p.font());
  p.drawText(QPointF(14, 12 + fd.ascent() * 0.86), d.isValid() ? d.toString(QStringLiteral("dd")) : QStringLiteral("—"));
  p.setFont(monoFont(8.5));
  p.drawText(QPointF(14, r.bottom() - 12),
             QStringLiteral("%1 %2").arg(QLocale::c().monthName(d.month(), QLocale::ShortFormat).toUpper(), date_.mid(11)));
  p.setPen(QPen(fg, 2, Qt::DotLine));
  p.drawLine(QPointF(stub, 0), QPointF(stub, r.bottom()));
  p.setPen(fg);
  p.setFont(displayFont(11, QFont::ExtraBold, 125));
  QFontMetrics fk(p.font());
  p.drawText(QPointF(stub + 14, 12 + fk.ascent()), kind_.toUpper());
  p.setFont(uiFont(9.5));
  p.drawText(QRectF(stub + 14, 40, r.width() - stub - 26, r.height() - 52), Qt::AlignBottom | Qt::AlignLeft | Qt::TextWordWrap, note_);
}

/* ---------- ScoreTile ---------- */

ScoreTile::ScoreTile(const QString &name, const QString &unit, bool higherBetter, const QString &value, const QString &flag,
                     QWidget *parent)
    : QAbstractButton(parent), name_(name), unit_(unit), value_(value), flag_(flag), higher_(higherBetter) {
  setCursor(Qt::PointingHandCursor);
  setFixedHeight(150);
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
void ScoreTile::enterEvent(QEnterEvent *) {
#else
void ScoreTile::enterEvent(QEvent *) {
#endif
  hover_ = true;
  update();
}

void ScoreTile::leaveEvent(QEvent *) {
  hover_ = false;
  update();
}

void ScoreTile::paintEvent(QPaintEvent *) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  const Theme &t = theme();
  QRectF r = rect();
  p.setPen(Qt::NoPen);
  p.setBrush(hover_ ? t.recess : t.raised);
  p.drawRect(r);
  QRectF in = r.adjusted(12, 10, -12, -10);
  p.setPen(t.text);
  p.setFont(monoFont(8));
  p.drawText(in, Qt::AlignTop | Qt::AlignLeft, unit_);
  p.drawText(in, Qt::AlignTop | Qt::AlignRight, higher_ ? QStringLiteral("↑") : QStringLiteral("↓"));
  /* the number takes the size that fits: scores of any length stay on one line */
  double pt = 56;
  QFont f = displayFont(pt, QFont::DemiBold, 50);
  while (pt > 24 && QFontMetrics(f).horizontalAdvance(value_) > in.width()) f = displayFont(pt -= 4, QFont::DemiBold, 50);
  p.setFont(f);
  p.drawText(in.adjusted(0, 12, 0, -20), Qt::AlignVCenter | Qt::AlignLeft, value_);
  QFont fn = uiFont(9.5, QFont::Bold);
  p.setFont(fn);
  QFontMetrics mn(fn);
  p.drawText(in, Qt::AlignBottom | Qt::AlignLeft, mn.elidedText(name_, Qt::ElideRight, int(in.width())));
  if (!flag_.isEmpty()) {
    p.setFont(labelFont(6.5));
    p.setPen(t.warnText);
    p.drawText(in.adjusted(0, 14, 0, 0), Qt::AlignTop | Qt::AlignLeft, flag_);
  }
}
