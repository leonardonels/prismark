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
constexpr int kRowH = 64, kPad = 4, kRankW = 34; /* row tall enough for name, bar and detail line */

QPointF mousePos(QMouseEvent *e) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
  return e->position();
#else
  return e->localPos();
#endif
}
}  // namespace

QColor seriesColor(int i) {
  static const QColor fixed[] = {QColor(240, 118, 108), QColor(74, 196, 140), QColor(139, 108, 255), QColor(78, 163, 255)};
  return i == 0 ? theme().accent : fixed[(i - 1) % 4];
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
QSize RankingView::sizeHint() const { return {520, std::max(3, int(rows_.size())) * kRowH + 2 * kPad}; }
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

  QFont fName = uiFont(10.5, QFont::Medium), fNameBold = uiFont(10.5, QFont::DemiBold), fVal = uiFont(11.5, QFont::DemiBold);
  QFont fSmall = uiFont(8.5, QFont::Medium), fRank = uiFont(10, QFont::DemiBold);
  QFontMetrics mName(fName), mVal(fVal), mSmall(fSmall);
  int w = width();
  for (int i = 0; i < rows_.size(); i++) {
    const RankRow &r = rows_[i];
    QRectF row(kPad, kPad + i * kRowH, w - 2 * kPad, kRowH - 6);
    bool sel = r.runId == selected_;
    if (sel || i == hover_) {
      p.setPen(sel ? QPen(t.accent, 1.2) : Qt::NoPen);
      QColor bg = sel ? t.accentSoft : t.raised;
      if (!sel) bg.setAlpha(170);
      p.setBrush(bg);
      p.drawRoundedRect(row.adjusted(0.5, 0.5, -0.5, -0.5), 12, 12);
    }
    /* rank badge */
    QRectF badge(row.left() + 8, row.center().y() - 13, 26, 26);
    p.setPen(Qt::NoPen);
    p.setBrush(r.yours ? t.accent : t.track);
    p.drawEllipse(badge);
    p.setPen(r.yours ? t.onAccent : t.muted);
    p.setFont(fRank);
    p.drawText(badge, Qt::AlignCenter, QString::number(i + 1));

    double x0 = row.left() + kRankW + 14, x1 = row.right() - 12;
    double ty = row.top() + 8 + mName.ascent();
    /* value and relative difference */
    QString val = formatValue(r.value.v), unit = unit_.isEmpty() ? QString() : " " + unit_;
    QString rel;
    if (refRow && refRow != &r) {
      double d = 100.0 * (speed(r.value.v) / speed(refRow->value.v) - 1);
      rel = QString("%1%2%").arg(d >= 0 ? "+" : "").arg(formatValue(d));
    }
    double relW = rel.isEmpty() ? 0 : mSmall.horizontalAdvance(rel) + 10;
    double unitW = mSmall.horizontalAdvance(unit);
    double valW = mVal.horizontalAdvance(val);
    if (!rel.isEmpty()) {
      p.setFont(fSmall);
      p.setPen(rel.startsWith('+') ? t.good : t.bad);
      p.drawText(QPointF(x1 - relW + 10, ty), rel);
    }
    p.setFont(fSmall);
    p.setPen(t.muted);
    p.drawText(QPointF(x1 - relW - unitW, ty), unit);
    p.setFont(fVal);
    p.setPen(t.text);
    p.drawText(QPointF(x1 - relW - unitW - valW, ty), val);

    /* name, then tags */
    double nameMax = x1 - relW - unitW - valW - 18 - x0;
    QString tagsText = r.tags.join(" · ").toUpper();
    double tagsW = tagsText.isEmpty() ? 0 : mSmall.horizontalAdvance(tagsText) + 16;
    p.setFont(r.yours ? fNameBold : fName);
    QFontMetrics fm(p.font());
    QString name = fm.elidedText(r.name, Qt::ElideRight, int(std::max(50.0, nameMax - tagsW - 8))); /* 8: gap before the tag */
    p.setPen(t.text);
    p.drawText(QPointF(x0, ty), name);
    double nx = x0 + fm.horizontalAdvance(name) + 8;
    if (tagsW && nx + tagsW <= x0 + nameMax + 4) {
      QRectF tr(nx, ty - mSmall.ascent() - 3, tagsW - 6, mSmall.height() + 4);
      p.setPen(Qt::NoPen);
      bool warn = r.tags.contains("placeholder") || r.tags.contains("did not settle");
      p.setBrush(warn ? t.warnBg : t.track);
      p.drawRoundedRect(tr, 6, 6);
      p.setFont(fSmall);
      p.setPen(warn ? t.warnText : t.muted);
      p.drawText(tr, Qt::AlignCenter, tagsText);
    }

    /* bar */
    QRectF bar(x0, row.top() + 30, x1 - x0, 9);
    p.setPen(Qt::NoPen);
    p.setBrush(t.track);
    p.drawRoundedRect(bar, 4.5, 4.5);
    QRectF fill = bar;
    fill.setWidth(std::max(9.0, bar.width() * speed(r.value.v) / top));
    p.setBrush(r.yours ? t.accent : t.ref);
    p.drawRoundedRect(fill, 4.5, 4.5);
    if (r.value.hasCi()) {
      double a = bar.left() + bar.width() * speed(r.value.lo) / top, b = bar.left() + bar.width() * speed(r.value.hi) / top;
      QColor wc = t.text;
      wc.setAlphaF(0.6);
      p.setPen(QPen(wc, 1.4, Qt::SolidLine, Qt::RoundCap));
      double cy = bar.center().y();
      p.drawLine(QPointF(std::min(a, b), cy), QPointF(std::max(a, b), cy));
      p.drawLine(QPointF(a, cy - 4), QPointF(a, cy + 4));
      p.drawLine(QPointF(b, cy - 4), QPointF(b, cy + 4));
    }
    if (!r.detail.isEmpty()) {
      p.setFont(fSmall);
      p.setPen(t.faint);
      p.drawText(QPointF(x0, bar.bottom() + mSmall.ascent() + 3), r.detail);
    }
  }
}

/* ---------- ChartView ---------- */

ChartView::ChartView(QWidget *parent) : QWidget(parent) {
  QSizePolicy sp(QSizePolicy::Expanding, QSizePolicy::Preferred);
  sp.setHeightForWidth(true);
  setSizePolicy(sp);
}

void ChartView::setData(QVector<ChartLine> lines, bool logX, const QString &xLabel, double yMin, double yMax, bool ideal) {
  lines_ = std::move(lines);
  logX_ = logX;
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
  QFont small = uiFont(8.5);
  p.setFont(small);
  QFontMetrics fm(small);

  /* The y axis belongs to the ordinary lines; overlay lines are scaled to the chart (or, with only overlays, each
     to 100 % of its own peak). */
  QVector<ChartLine> lines = lines_;
  bool base = std::any_of(lines.begin(), lines.end(), [](const ChartLine &l) { return !l.overlay; });
  double x0 = INFINITY, x1 = -INFINITY, y0 = INFINITY, y1 = -INFINITY;
  QVector<double> xs;
  for (const ChartLine &l : lines)
    for (const auto &pt : l.pts) {
      x0 = std::min(x0, pt[0]);
      x1 = std::max(x1, pt[0]);
      if (!xs.contains(pt[0])) xs.push_back(pt[0]);
      if (l.overlay) continue;
      for (int k = 1; k < 4; k++)
        if (std::isfinite(pt[k])) {
          y0 = std::min(y0, pt[k]);
          y1 = std::max(y1, pt[k]);
        }
    }
  if (!std::isfinite(x0)) return;
  if (base) {
    y0 = std::isfinite(yMin_) ? yMin_ : std::min(0.0, y0);
    y1 = std::isfinite(yMax_) ? yMax_ : niceCeil(y1 * 1.05);
    if (ideal_) y1 = std::max(y1, x1);
  } else {
    y0 = 0, y1 = 125; /* room above 100 % for the labels */
  }
  if (x1 == x0) x1 = x0 + 1;
  if (y1 == y0) y1 = y0 + 1;
  double overlayTop = base ? y0 + (y1 - y0) * 0.85 : 100;
  for (ChartLine &l : lines) {
    if (!l.overlay) continue;
    double peak = 0;
    for (const auto &pt : l.pts) peak = std::max(peak, pt[1]);
    if (peak > 0)
      for (auto &pt : l.pts)
        for (int k = 1; k < 4; k++) pt[k] = y0 + pt[k] / peak * (overlayTop - y0);
  }

  double legendH = fm.height() + 10,
         left = fm.horizontalAdvance("00.00" + (yUnit_.isEmpty() ? QString() : " " + yUnit_)) + 10;
  QRectF plot(left, legendH, width() - left - 10, height() - legendH - 2 * fm.height() - 10);
  auto fx = [&](double x) {
    double u = logX_ ? std::log(x / x0) / std::log(x1 / x0) : (x - x0) / (x1 - x0);
    return plot.left() + u * plot.width();
  };
  auto fy = [&](double y) { return plot.bottom() - (y - y0) / (y1 - y0) * plot.height(); };

  /* grid */
  QColor grid = t.border;
  for (int k = 0; k <= 4; k++) {
    double yv = base ? y0 + (y1 - y0) * k / 4 : 25.0 * k;
    p.setPen(QPen(grid, 1, k ? Qt::DotLine : Qt::SolidLine));
    p.drawLine(QPointF(plot.left(), fy(yv)), QPointF(plot.right(), fy(yv)));
    {
      QString s = base ? formatValue(yv) + (yUnit_.isEmpty() ? QString() : " " + yUnit_)
                       : QString::number(yv, 'f', 0) + " %";
      p.setPen(t.faint);
      p.drawText(QPointF(plot.left() - fm.horizontalAdvance(s) - 6, fy(yv) + fm.ascent() / 2.6), s);
    }
  }
  std::sort(xs.begin(), xs.end());
  p.setPen(t.faint);
  double lastRight = -1e9;
  for (int i = 0; i < xs.size(); i++) {
    QString s = formatValue(xs[i]);
    double tw = fm.horizontalAdvance(s);
    double x = fx(xs[i]) - (i == xs.size() - 1 ? tw : i == 0 ? 0 : tw / 2.0);
    if (x < lastRight + 4) continue; /* skip labels that would overlap */
    p.drawText(QPointF(x, plot.bottom() + fm.ascent() + 4), s);
    lastRight = x + tw;
  }
  p.drawText(QRectF(plot.left(), height() - fm.height() - 2, plot.width(), fm.height()), Qt::AlignCenter, xLabel_);
  if (ideal_) {
    p.setPen(QPen(t.muted, 1, Qt::DashLine));
    p.drawLine(QPointF(fx(x0), fy(x0)), QPointF(fx(x1), fy(std::min(x1, y1))));
  }
  double lx = plot.left();
  for (int i = 0; i < lines.size(); i++) {
    const ChartLine &l = lines[i];
    QColor c = seriesColor(l.color >= 0 ? l.color : i);
    /* soft area under the first series */
    QPainterPath path;
    for (int k = 0; k < l.pts.size(); k++) {
      QPointF q(fx(l.pts[k][0]), fy(l.pts[k][1]));
      k ? path.lineTo(q) : path.moveTo(q);
    }
    if (i == 0 && l.pts.size() > 1 && !l.overlay) {
      QPainterPath area = path;
      area.lineTo(fx(l.pts.last()[0]), plot.bottom());
      area.lineTo(fx(l.pts.first()[0]), plot.bottom());
      area.closeSubpath();
      QColor a = c;
      a.setAlpha(28);
      p.setPen(Qt::NoPen);
      p.setBrush(a);
      p.drawPath(area);
    }
    p.setPen(QPen(c, 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);
    for (const auto &pt : l.pts) {
      if (std::isfinite(pt[2]) && std::isfinite(pt[3])) {
        p.setPen(QPen(c, 1.2));
        p.drawLine(QPointF(fx(pt[0]), fy(pt[2])), QPointF(fx(pt[0]), fy(pt[3])));
      }
      p.setPen(QPen(t.card, 1.5));
      p.setBrush(c);
      p.drawEllipse(QPointF(fx(pt[0]), fy(pt[1])), 3.2, 3.2);
    }
    p.setPen(Qt::NoPen);
    p.setBrush(c);
    p.drawRoundedRect(QRectF(lx, 3 + fm.height() / 2.0 - 4, 10, 8), 3, 3);
    p.setPen(t.muted);
    p.drawText(QPointF(lx + 14, 3 + fm.ascent()), l.name);
    lx += 14 + fm.horizontalAdvance(l.name) + 16;
  }

  /* Overlays: each point labelled with its value, above it, nudged apart where lines meet at the same x. */
  {
    struct Tag { double x, y; QString s; QColor c; int line; };
    QMap<double, QVector<Tag>> byX;
    for (int i = 0; i < lines.size(); i++)
      for (int k = 0; lines[i].overlay && k < lines[i].pts.size(); k++) {
        double v = lines_[i].pts[k][1];
        if (!std::isfinite(v)) continue;
        QString s = formatValue(v) + (lines_[i].unit.isEmpty() ? QString() : " " + lines_[i].unit);
        byX[lines[i].pts[k][0]].push_back({fx(lines[i].pts[k][0]), fy(lines[i].pts[k][1]) - 7, s,
                                           seriesColor(lines[i].color >= 0 ? lines[i].color : i), i});
      }
    QMap<int, double> lastRight; /* per line: where its previous label ended; closer points go unlabelled */
    for (auto it = byX.begin(); it != byX.end(); ++it) {
      QVector<Tag> &tags = it.value();
      /* stacked upwards from the lowest, so no label sits on a line below its own point */
      std::sort(tags.begin(), tags.end(), [](const Tag &a, const Tag &b) { return a.y > b.y; });
      for (int k = 1; k < tags.size(); k++) tags[k].y = std::min(tags[k].y, tags[k - 1].y - fm.height());
      for (const Tag &g : tags) {
        double tw = fm.horizontalAdvance(g.s);
        double x = std::clamp(g.x - tw / 2, plot.left(), plot.right() - tw);
        if (lastRight.contains(g.line) && x < lastRight[g.line] + 4) continue;
        lastRight[g.line] = x + tw;
        QColor bg = t.card;
        bg.setAlpha(220);
        p.setPen(Qt::NoPen);
        p.setBrush(bg);
        p.drawRoundedRect(QRectF(x - 3, g.y - fm.ascent(), tw + 6, fm.height()), 3, 3);
        p.setPen(g.c);
        p.drawText(QPointF(x, g.y), g.s);
      }
    }
  }
}

/* ---------- BarsView ---------- */

BarsView::BarsView(QVector<Bar> bars, QWidget *parent) : QWidget(parent), bars_(std::move(bars)) {
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  setFixedHeight(sizeHint().height());
}

QSize BarsView::sizeHint() const {
  QFontMetrics fm(uiFont(9));
  return {320, int(bars_.size() * (fm.height() * 2 + 14))};
}

void BarsView::paintEvent(QPaintEvent *) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  const Theme &t = theme();
  QFont f = uiFont(9);
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
    p.drawRoundedRect(bar, 3, 3);
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
  setFixedHeight(96);
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
  double w = QFontMetrics(uiFont(9.5, QFont::DemiBold)).horizontalAdvance(runLabel_) + 44;
  return QRectF(width() - 12 - w, height() / 2.0 - 17, w, 34);
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
  QRectF r = QRectF(rect()).adjusted(1, 1, -1, -3);
  bool on = isChecked();
  p.setPen(QPen(on ? t.accent : t.border, on ? 1.5 : 1));
  p.setBrush(on ? t.accentSoft : hover_ ? t.raised : t.card);
  p.drawRoundedRect(r, 14, 14);
  if (on) { /* accent edge */
    p.setPen(Qt::NoPen);
    p.setBrush(t.accent);
    p.drawRoundedRect(QRectF(r.left() + 6, r.top() + 14, 4, r.height() - 28), 2, 2);
  }
  double x = r.left() + 20, right = runRect().left() - 10;
  p.setFont(uiFont(11, QFont::DemiBold));
  p.setPen(t.text);
  QFontMetrics fn(p.font());
  p.drawText(QPointF(x, r.top() + 12 + fn.ascent()), fn.elidedText(name_, Qt::ElideRight, int(right - x)));
  p.setFont(uiFont(8.5));
  QFontMetrics fs(p.font());
  p.setPen(t.muted);
  p.drawText(QPointF(x, r.top() + 34 + fs.ascent()), fs.elidedText(sub_, Qt::ElideRight, int(right - x)));
  if (!score_.isEmpty()) {
    p.setFont(uiFont(15, QFont::Bold));
    QFontMetrics fv(p.font());
    p.setPen(t.text);
    double by = r.bottom() - 12;
    p.drawText(QPointF(x, by), score_);
    p.setFont(uiFont(8.5, QFont::Medium));
    p.setPen(t.muted);
    p.drawText(QPointF(x + fv.horizontalAdvance(score_) + 5, by), unit_);
  } else {
    p.setFont(uiFont(8.5, QFont::Medium));
    p.setPen(t.faint);
    p.drawText(QPointF(x, r.bottom() - 14), note_);
  }
  /* run button */
  QRectF b = runRect();
  p.setPen(QPen(hoverRun_ ? t.accent : t.border, 1));
  p.setBrush(hoverRun_ ? t.accent : t.raised);
  p.drawRoundedRect(b, 10, 10);
  QColor fg = hoverRun_ ? t.onAccent : t.text;
  QPainterPath tri;
  double cx = b.left() + 18, cy = b.center().y();
  tri.moveTo(cx - 4, cy - 6);
  tri.lineTo(cx + 6, cy);
  tri.lineTo(cx - 4, cy + 6);
  tri.closeSubpath();
  p.setPen(Qt::NoPen);
  p.setBrush(hoverRun_ ? fg : t.accent);
  p.drawPath(tri);
  p.setPen(fg);
  p.setFont(uiFont(9.5, QFont::DemiBold));
  p.drawText(b.adjusted(28, 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft, runLabel_);
}

/* ---------- Segmented ---------- */

Segmented::Segmented(QWidget *parent) : QWidget(parent) {
  setCursor(Qt::PointingHandCursor);
  setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
  setFixedHeight(40);
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

QVector<QRectF> Segmented::rects() const {
  QFontMetrics fm(uiFont(10, QFont::DemiBold));
  QVector<QRectF> out;
  double x = 4;
  for (const QString &s : items_) {
    double w = fm.horizontalAdvance(s) + 28;
    out.push_back(QRectF(x, 4, w, height() - 8));
    x += w + 2;
  }
  return out;
}

QSize Segmented::sizeHint() const {
  auto r = rects();
  return {r.isEmpty() ? 100 : int(r.last().right()) + 4, 40};
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

void Segmented::paintEvent(QPaintEvent *) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  const Theme &t = theme();
  p.setPen(QPen(t.border, 1));
  p.setBrush(t.card);
  p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), height() / 2.0, height() / 2.0);
  auto r = rects();
  for (int i = 0; i < r.size(); i++) {
    if (i == cur_) {
      p.setPen(Qt::NoPen);
      p.setBrush(t.accent);
      p.drawRoundedRect(r[i], r[i].height() / 2, r[i].height() / 2);
    }
    p.setFont(uiFont(10, QFont::DemiBold));
    p.setPen(i == cur_ ? t.onAccent : t.muted);
    p.drawText(r[i], Qt::AlignCenter, items_[i]);
  }
}

/* ---------- Toggle ---------- */

Toggle::Toggle(const QString &text, QWidget *parent) : QAbstractButton(parent) {
  setText(text);
  setCheckable(true);
  setCursor(Qt::PointingHandCursor);
}

QSize Toggle::sizeHint() const {
  return {QFontMetrics(uiFont(10)).horizontalAdvance(text()) + 56, 30};
}

void Toggle::paintEvent(QPaintEvent *) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  const Theme &t = theme();
  QRectF track(0, height() / 2.0 - 10, 38, 20);
  if (isChecked()) {
    p.setBrush(t.accent);
    p.setPen(Qt::NoPen);
  } else {
    p.setBrush(t.track);
    p.setPen(QPen(t.border, 1));
  }
  p.drawRoundedRect(track, 10, 10);
  p.setPen(Qt::NoPen);
  p.setBrush(isChecked() ? t.onAccent : t.muted);
  p.drawEllipse(QPointF(isChecked() ? track.right() - 10 : track.left() + 10, track.center().y()), 7.5, 7.5);
  p.setPen(t.text);
  p.setFont(uiFont(10));
  p.drawText(QRectF(48, 0, width() - 48, height()), Qt::AlignVCenter | Qt::AlignLeft, text());
}

/* ---------- StepRing ---------- */

StepRing::StepRing(QWidget *parent) : QWidget(parent) { setFixedSize(170, 170); }

void StepRing::setProgress(int step, int steps, const QString &centre) {
  step_ = step;
  steps_ = steps;
  centre_ = centre;
  update();
}

void StepRing::paintEvent(QPaintEvent *) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  const Theme &t = theme();
  QRectF r = QRectF(rect()).adjusted(10, 10, -10, -10);
  p.setPen(QPen(t.track, 10, Qt::SolidLine, Qt::RoundCap));
  p.drawArc(r, 0, 360 * 16);
  if (steps_ > 0 && step_ > 0) {
    p.setPen(QPen(t.accent, 10, Qt::SolidLine, Qt::RoundCap));
    p.drawArc(r, 90 * 16, -int(360.0 * 16 * std::min(step_, steps_) / steps_));
  }
  p.setPen(t.text);
  p.setFont(uiFont(18, QFont::Bold));
  p.drawText(r, Qt::AlignCenter, centre_);
}
