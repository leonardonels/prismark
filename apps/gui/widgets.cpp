/* Copyright 2026 The Prismark Authors. Apache-2.0. */
#include "widgets.h"

#include <QKeyEvent>
#include <QMap>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSvgRenderer>
#include <algorithm>
#include <cmath>

#include "theme.h"

namespace {
constexpr int kRowH = 70, kPad = 4, kRankW = 26; /* row tall enough for name, bar and detail line */

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
    p.drawText(rect(), Qt::AlignCenter, tr("No result for this test yet.\nPress ▶ Run on its card to measure it."));
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
        fVal = displayFont(17, QFont::DemiBold, 75);
  QFont fSmall = monoFont(8.5), fTag = labelFont(7.5), fRank = monoFont(9.5, QFont::Medium);
  QFontMetrics mName(fName), mVal(fVal), mSmall(fSmall), mTag(fTag);
  int w = width();
  for (int i = 0; i < rows_.size(); i++) {
    const RankRow &r = rows_[i];
    QRectF row(kPad, kPad + i * kRowH, w - 2 * kPad, kRowH - 2);
    bool sel = r.runId == selected_;
    /* the highlighted result is a volt block; everything on it is ink */
    QColor fg = sel ? t.onAccent : t.text, fgMuted = sel ? t.onAccent : t.muted;
    if (sel || i == hover_) {
      p.setPen(Qt::NoPen);
      p.setBrush(sel ? t.accentSoft : t.raised);
      p.drawRect(row);
    }
    if (!sel && i > 0 && i - 1 != hover_ && i != hover_ && rows_[i - 1].runId != selected_) {
      p.setPen(QPen(t.border, 1));
      p.drawLine(QPointF(row.left(), row.top() - 1), QPointF(row.right(), row.top() - 1));
    }
    /* rank */
    p.setFont(fRank);
    p.setPen(fgMuted);
    p.drawText(QRectF(row.left() + 8, row.top(), kRankW, row.height()), Qt::AlignVCenter | Qt::AlignLeft,
               QStringLiteral("%1").arg(i + 1, 2, 10, QLatin1Char('0')));

    double x0 = row.left() + kRankW + 14, x1 = row.right() - 12;
    double ty = row.top() + 9 + mName.ascent();
    /* value and relative difference */
    QString val = formatValue(r.value.v), unit = unit_.isEmpty() ? QString() : " " + unit_;
    QString rel;
    if (refRow && refRow != &r) {
      double d = 100.0 * (speed(r.value.v) / speed(refRow->value.v) - 1);
      rel = QString("%1%2%").arg(d >= 0 ? "+" : "−").arg(formatValue(std::abs(d)));
    }
    double relW = rel.isEmpty() ? 0 : mSmall.horizontalAdvance(rel) + 12;
    double unitW = mSmall.horizontalAdvance(unit);
    double valW = mVal.horizontalAdvance(val);
    double vy = row.center().y() + mVal.ascent() / 2.0 - 2;
    if (!rel.isEmpty()) {
      p.setFont(fSmall);
      p.setPen(sel ? t.onAccent : rel.startsWith('+') ? t.good : t.bad);
      p.drawText(QPointF(x1 - relW + 12, vy), rel);
    }
    p.setFont(fSmall);
    p.setPen(fgMuted);
    p.drawText(QPointF(x1 - relW - unitW, vy), unit);
    p.setFont(fVal);
    p.setPen(fg);
    p.drawText(QPointF(x1 - relW - unitW - valW, vy), val);

    /* name, then tags */
    double nameMax = x1 - relW - unitW - valW - 18 - x0;
    QString tagsText = r.tags.join(" · ");
    double tagsW = tagsText.isEmpty() ? 0 : mTag.horizontalAdvance(tagsText.toUpper()) + 16;
    p.setFont(r.yours ? fNameBold : fName);
    QFontMetrics fm(p.font());
    QString name = fm.elidedText(r.name, Qt::ElideRight, int(std::max(50.0, nameMax - tagsW - 8))); /* 8: gap before the tag */
    p.setPen(fg);
    p.drawText(QPointF(x0, ty), name);
    double nx = x0 + fm.horizontalAdvance(name) + 8;
    if (tagsW && nx + tagsW <= x0 + nameMax + 4) {
      QRectF tr(nx, ty - mTag.ascent() - 3, tagsW - 6, mTag.height() + 4);
      bool warn = r.tags.contains("placeholder") || r.tags.contains("did not settle");
      p.setPen(sel ? QPen(t.onAccent, 1) : Qt::NoPen);
      p.setBrush(sel ? Qt::transparent : warn ? t.warnBg : t.track);
      p.drawRect(tr);
      p.setFont(fTag);
      p.setPen(sel ? t.onAccent : warn ? t.warnText : t.muted);
      p.drawText(tr, Qt::AlignCenter, tagsText);
    }

    /* bar: square, on no track; yours in ink or volt, references in signal blue */
    QRectF bar(x0, row.top() + 31, x1 - x0 - relW - unitW - valW - 18, 10);
    QRectF fill = bar;
    fill.setWidth(std::max(3.0, bar.width() * speed(r.value.v) / top));
    p.setPen(Qt::NoPen);
    p.setBrush(sel ? t.onAccent : r.yours ? t.mine : t.ref);
    p.drawRect(fill);
    if (r.value.hasCi()) {
      double a = bar.left() + bar.width() * speed(r.value.lo) / top, b = bar.left() + bar.width() * speed(r.value.hi) / top;
      QColor wc = sel ? t.onAccent : t.text;
      wc.setAlphaF(0.7);
      p.setPen(QPen(wc, 1.4, Qt::SolidLine, Qt::FlatCap));
      double cy = bar.center().y() + 9;
      p.drawLine(QPointF(std::min(a, b), cy), QPointF(std::max(a, b), cy));
      p.drawLine(QPointF(a, cy - 3), QPointF(a, cy + 3));
      p.drawLine(QPointF(b, cy - 3), QPointF(b, cy + 3));
    }
    if (!r.detail.isEmpty()) {
      p.setFont(fSmall);
      p.setPen(sel ? t.onAccent : t.faint);
      p.drawText(QPointF(x0, bar.bottom() + mSmall.ascent() + 6), r.detail);
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
  setFixedHeight(100);
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

QRectF TestCard::runRect() const {
  double w = QFontMetrics(displayFont(9.5, QFont::ExtraBold, 125)).horizontalAdvance(runLabel_.toUpper()) + 46;
  return QRectF(width() - 14 - w, height() / 2.0 - 22, w, 44);
}

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
  QRectF r = QRectF(rect()).adjusted(0, 0, 0, -2);
  bool on = isChecked();
  /* the open test is a volt block, the others panels */
  QColor fg = on ? t.onAccent : t.text, fgMuted = on ? t.onAccent : t.muted;
  p.setPen(Qt::NoPen);
  p.setBrush(on ? t.accent : hover_ ? t.raised : t.card);
  p.drawRect(r);
  double x = r.left() + 18, right = runRect().left() - 10;
  p.setFont(uiFont(10.5, QFont::Bold));
  p.setPen(fg);
  QFontMetrics fn(p.font());
  p.drawText(QPointF(x, r.top() + 12 + fn.ascent()), fn.elidedText(name_, Qt::ElideRight, int(right - x)));
  p.setFont(uiFont(8.5));
  QFontMetrics fs(p.font());
  p.setPen(fgMuted);
  p.drawText(QPointF(x, r.top() + 33 + fs.ascent()), fs.elidedText(sub_, Qt::ElideRight, int(right - x)));
  if (!score_.isEmpty()) {
    p.setFont(displayFont(24, QFont::DemiBold, 60));
    QFontMetrics fv(p.font());
    p.setPen(fg);
    double by = r.bottom() - 11;
    p.drawText(QPointF(x, by), score_);
    p.setFont(monoFont(8.5, QFont::Medium));
    p.setPen(fgMuted);
    p.drawText(QPointF(x + fv.horizontalAdvance(score_) + 6, by), unit_);
  } else {
    p.setFont(monoFont(8.5));
    p.setPen(on ? t.onAccent : t.faint);
    p.drawText(QPointF(x, r.bottom() - 14), note_);
  }
  /* run button: on the open test an ink block with volt type; elsewhere recessed, so only one action is loud */
  QRectF b = runRect();
  QColor bbg = on ? t.onAccent : hoverRun_ ? t.accent : t.raised, bfg = on ? t.accent : hoverRun_ ? t.onAccent : t.text;
  if (on && hoverRun_) bbg = bbg.lighter(160);
  p.setPen(Qt::NoPen);
  p.setBrush(bbg);
  p.drawRect(b);
  QPainterPath tri;
  double cx = b.left() + 18, cy = b.center().y();
  tri.moveTo(cx - 4, cy - 6);
  tri.lineTo(cx + 6, cy);
  tri.lineTo(cx - 4, cy + 6);
  tri.closeSubpath();
  p.setBrush(on || hoverRun_ ? bfg : t.link);
  p.drawPath(tri);
  p.setPen(bfg);
  p.setFont(displayFont(9.5, QFont::ExtraBold, 125));
  p.drawText(b.adjusted(30, 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft, runLabel_.toUpper());
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
  setFixedHeight(first ? 54 : 44);
  updateGeometry();
  update();
}

QVector<QRectF> Segmented::rects() const {
  QFontMetrics fm(uiFont(10, QFont::Bold));
  QVector<QRectF> out;
  double x = 0;
  for (const QString &s : items_) {
    double w = std::max(fm.horizontalAdvance(s) + 32, numbered_ ? 96 : 0);
    out.push_back(QRectF(x, 0, w, height()));
    x += w + 4;
  }
  return out;
}

QSize Segmented::sizeHint() const {
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

/* Square tabs: the current one a panel with a volt foot, the others recessed. */
void Segmented::paintEvent(QPaintEvent *) {
  QPainter p(this);
  const Theme &t = theme();
  auto r = rects();
  for (int i = 0; i < r.size(); i++) {
    bool on = i == cur_;
    p.setPen(Qt::NoPen);
    p.setBrush(on ? t.card : t.raised);
    p.drawRect(r[i]);
    if (on) {
      p.setBrush(t.accent);
      p.drawRect(QRectF(r[i].left(), r[i].bottom() - 4, r[i].width(), 5));
    }
    QRectF tr = r[i].adjusted(14, 0, -10, -4);
    if (numbered_) {
      p.setFont(monoFont(8));
      p.setPen(on ? t.text : t.muted);
      p.drawText(tr.adjusted(0, 8, 0, 0), Qt::AlignTop | Qt::AlignLeft, QStringLiteral("%1").arg(numbered_ + i, 2, 10, QLatin1Char('0')));
    }
    p.setFont(uiFont(10, on ? QFont::Bold : QFont::DemiBold));
    p.setPen(on ? t.text : t.muted);
    p.drawText(tr.adjusted(0, 0, 0, numbered_ ? -6 : 0), (numbered_ ? Qt::AlignBottom : Qt::AlignVCenter) | Qt::AlignLeft,
               items_[i]);
  }
}

/* ---------- Toggle ---------- */

Toggle::Toggle(const QString &text, QWidget *parent) : QAbstractButton(parent) {
  setText(text);
  setCheckable(true);
  setCursor(Qt::PointingHandCursor);
}

QSize Toggle::sizeHint() const {
  return {QFontMetrics(uiFont(10, QFont::Medium)).horizontalAdvance(text()) + 70, 32};
}

/* A square switch that states its position: OFF on a recessed track with the knob on the left, ON on volt with an
   ink knob on the right. The knob is solid in both, so off reads as a setting, not as disabled. */
void Toggle::paintEvent(QPaintEvent *) {
  QPainter p(this);
  const Theme &t = theme();
  bool on = isChecked();
  QRectF track(0, height() / 2.0 - 13, 58, 26);
  p.setPen(Qt::NoPen);
  p.setBrush(on ? t.accent : t.raised);
  p.drawRect(track);
  QRectF knob(0, 0, 20, 20);
  knob.moveCenter(QPointF(on ? track.right() - 13 : track.left() + 13, track.center().y()));
  p.setBrush(on ? t.onAccent : t.text);
  p.drawRect(knob);
  p.setFont(monoFont(7.5, QFont::DemiBold));
  p.setPen(on ? t.onAccent : t.muted);
  QRectF word = on ? QRectF(track.left() + 4, track.top(), knob.left() - track.left() - 4, track.height())
                   : QRectF(knob.right(), track.top(), track.right() - knob.right() - 2, track.height());
  p.drawText(word, Qt::AlignCenter, on ? tr("ON") : tr("OFF"));
  p.setPen(t.text);
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
