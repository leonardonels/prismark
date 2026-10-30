/*
 * Copyright 2026 The Prismark Authors. Apache-2.0.
 */
#include "stub.h"

#include <QAbstractButton>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QVBoxLayout>

#include "theme.h"

namespace {

const QColor kRed(255, 79, 58), kBlue(45, 91, 255), kVolt(214, 245, 59), kInk(12, 20, 48), kPaper(247, 248, 249);

/* The dashed rule and outlines: between the panel and the text, as the design has them. */
QColor rule() { return theme().dark ? QColor(58, 72, 116) : QColor(154, 165, 180); }

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
QPoint globalPos(QMouseEvent *e) { return e->globalPosition().toPoint(); }
#else
QPoint globalPos(QMouseEvent *e) { return e->globalPos(); }
#endif

/* The strip: the code on the left, the tag on the right, a warning sign on errors. */
class Head : public QWidget {
 public:
  Head(Stub::Kind kind, const QString &code) : kind_(kind), code_(code) { setFixedHeight(44); }
  void setTag(const QString &tag) {
    tag_ = tag;
    update();
  }

 protected:
  void paintEvent(QPaintEvent *) override {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QColor bg = kind_ == Stub::Error ? kRed : kind_ == Stub::Run ? kBlue : kVolt;
    const QColor fg = kind_ == Stub::Run ? kPaper : kInk;
    p.fillRect(rect(), bg);
    QFont f = monoFont(9, QFont::DemiBold);
    f.setCapitalization(QFont::AllUppercase);
    f.setLetterSpacing(QFont::PercentageSpacing, 106);
    p.setFont(f);
    p.setPen(fg);
    QRectF r = rect().adjusted(20, 0, -20, 0);
    p.drawText(r, Qt::AlignVCenter | Qt::AlignLeft, code_);
    if (kind_ == Stub::Error) { /* a triangle with an exclamation mark, stroked */
      QPointF c(r.right() - 10, height() / 2.0);
      p.setPen(QPen(fg, 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
      p.setBrush(Qt::NoBrush);
      p.drawPolygon(QPolygonF({c + QPointF(0, -8.5), c + QPointF(9, 7.5), c + QPointF(-9, 7.5)}));
      p.drawLine(c + QPointF(0, -2.5), c + QPointF(0, 1.5));
      p.drawPoint(c + QPointF(0, 4.6));
      r.setRight(r.right() - 30);
    }
    p.drawText(r, Qt::AlignVCenter | Qt::AlignRight, tag_);
  }

 private:
  Stub::Kind kind_;
  QString code_, tag_;
};

/* The run so far, as three blocks: done (ink), the current part (volt), still to come (an outline). */
class Strip : public QWidget {
 public:
  Strip(double done, double now, double left) : w_{done, now, left} { setFixedHeight(18); }

 protected:
  void paintEvent(QPaintEvent *) override {
    QPainter p(this);
    double total = w_[0] + w_[1] + w_[2];
    if (total <= 0) return;
    int parts = (w_[0] > 0) + (w_[1] > 0) + (w_[2] > 0);
    double x = 0, room = width() - 4.0 * (parts - 1), y = 8, h = 10;
    for (int i = 0; i < 3; i++) {
      if (w_[i] <= 0) continue;
      QRectF r(x, y, room * w_[i] / total, h);
      if (i == 0) p.fillRect(r, theme().text);
      else if (i == 1) p.fillRect(r, kVolt);
      else {
        p.setPen(QPen(rule(), 1.5));
        p.drawRect(r.adjusted(0.75, 0.75, -0.75, -0.75));
      }
      x = r.right() + 4;
    }
  }

 private:
  double w_[3];
};

/* A square checkbox with its label, in the dialog's own colours. */
class Check : public QAbstractButton {
 public:
  explicit Check(const QString &text) {
    setText(text);
    setCheckable(true);
    setCursor(Qt::PointingHandCursor);
    setFont(uiFont(10));
  }
  QSize sizeHint() const override {
    return {fontMetrics().horizontalAdvance(text()) + 30, std::max(44, heightForWidth(400))};
  }
  bool hasHeightForWidth() const override { return true; }
  int heightForWidth(int w) const override {
    QRect r = fontMetrics().boundingRect(QRect(0, 0, std::max(40, w - 30), 1000), Qt::TextWordWrap, text());
    return std::max(44, r.height() + 12);
  }

 protected:
  void paintEvent(QPaintEvent *) override {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const Theme &t = theme();
    QRectF box(0.75, height() / 2.0 - 9, 18, 18);
    if (isChecked()) {
      p.fillRect(box, kBlue);
      p.setPen(QPen(kPaper, 2.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
      p.drawPolyline(QPolygonF({box.topLeft() + QPointF(4, 9.5), box.topLeft() + QPointF(7.5, 13), box.topLeft() + QPointF(14, 5.5)}));
    } else {
      p.setPen(QPen(t.text, 1.5));
      p.drawRect(box);
    }
    p.setPen(t.text);
    p.drawText(QRectF(30, 0, width() - 30, height()), Qt::AlignVCenter | Qt::AlignLeft | Qt::TextWordWrap, text());
  }
};

/* The dashed rule over the buttons. */
class Rule : public QWidget {
 public:
  Rule() { setFixedHeight(2); }

 protected:
  void paintEvent(QPaintEvent *) override {
    QPainter p(this);
    QPen pen(rule(), 2, Qt::CustomDashLine);
    pen.setDashPattern({3, 2});
    p.setPen(pen);
    p.drawLine(QPointF(0, 1), QPointF(width(), 1));
  }
};

QLabel *monoBlock(const QString &s) {
  auto *l = new QLabel(s);
  l->setFont(monoFont(9));
  l->setWordWrap(true);
  l->setTextFormat(Qt::PlainText);
  l->setTextInteractionFlags(Qt::TextSelectableByMouse);
  l->setStyleSheet(QStringLiteral("background:%1; color:%2; padding:10px 12px;").arg(theme().raised.name(), theme().text.name()));
  return l;
}

}  // namespace

Stub::Stub(QWidget *parent, Kind kind, const QString &code, const QString &title, const QString &sentence)
    : QDialog(parent, Qt::Dialog | Qt::FramelessWindowHint) {
  const Theme &t = theme();
  setObjectName("stub");
  setWindowTitle(title);
  setAttribute(Qt::WA_StyledBackground);
  setStyleSheet(QStringLiteral("#stub { background:%1; border:1px solid %2; }").arg(t.card.name(), rule().name()));
  setFixedWidth(460);
  auto *l = new QVBoxLayout(this);
  l->setContentsMargins(1, 1, 1, 1); /* inside the border */
  l->setSpacing(0);
  head_ = new Head(kind, code);
  l->addWidget(head_);
  body_ = new QVBoxLayout;
  body_->setContentsMargins(20, 20, 20, 18);
  body_->setSpacing(10);
  auto *h = new QLabel(title);
  h->setFont(displayFont(19.5, QFont::Bold, 112));
  h->setWordWrap(true);
  h->setStyleSheet(QStringLiteral("color:%1").arg(t.text.name()));
  body_->addWidget(h);
  if (!sentence.isEmpty()) {
    auto *s = new QLabel(sentence);
    s->setFont(uiFont(10.5));
    s->setWordWrap(true);
    s->setTextFormat(Qt::PlainText);
    s->setStyleSheet(QStringLiteral("color:%1").arg(t.muted.name()));
    body_->addWidget(s);
  }
  l->addLayout(body_);
  l->addWidget(new Rule);
  buttons_ = new QHBoxLayout;
  buttons_->setContentsMargins(20, 14, 20, 18);
  buttons_->setSpacing(6);
  l->addLayout(buttons_);
}

void Stub::setTag(const QString &tag) { static_cast<Head *>(head_)->setTag(tag); }

void Stub::setCode(const QString &mono) {
  QLabel *b = monoBlock(mono);
  body_->addSpacing(4);
  body_->addWidget(b);
}

void Stub::setDetails(const QString &mono) {
  QLabel *b = monoBlock(mono);
  b->hide();
  body_->addWidget(b);
  QPushButton *show = addButton(tr("Show details"), Secondary);
  buttons_->removeWidget(show);
  buttons_->insertWidget(0, show, 10); /* first, as in the design */
  disconnect(show, &QPushButton::clicked, nullptr, nullptr); /* it shows them, it does not close */
  connect(show, &QPushButton::clicked, this, [this, b, show] {
    b->setVisible(!b->isVisible());
    show->setText(b->isVisible() ? tr("Hide details") : tr("Show details"));
    fit();
  });
}

void Stub::setStrip(double done, double now, double left) {
  body_->addWidget(new Strip(done, now, left));
}

QAbstractButton *Stub::addCheck(const QString &text) {
  auto *c = new Check(text);
  body_->addWidget(c);
  return c;
}

void Stub::setWide(int width) { setFixedWidth(width); }

QPushButton *Stub::addButton(const QString &text, Role role) {
  const Theme &t = theme();
  auto *b = new QPushButton(text);
  b->setCursor(Qt::PointingHandCursor);
  b->setFont(uiFont(10.5, role == Primary ? QFont::Bold : QFont::DemiBold));
  b->setMinimumHeight(46);
  const QString base = QStringLiteral("QPushButton { min-height:46px; padding:0 16px; border-radius:0; font-weight:%1; ")
                           .arg(role == Primary ? 700 : 600);
  if (role == Primary)
    b->setStyleSheet(base + QStringLiteral("background:%1; color:%2; border:none; } QPushButton:hover { background:%3; }")
                                .arg(kVolt.name(), kInk.name(), kVolt.darker(108).name()));
  else if (role == Secondary)
    b->setStyleSheet(base + QStringLiteral("background:%1; color:%2; border:1.5px solid transparent; } "
                                           "QPushButton:hover { border-color:%2; }")
                                .arg(t.raised.name(), t.text.name()));
  else
    b->setStyleSheet(base + QStringLiteral("background:transparent; color:%1; border:1.5px solid %1; } "
                                           "QPushButton:hover { background:%2; }")
                                .arg(t.text.name(), t.raised.name()));
  if (role == Primary) {
    b->setDefault(true);
    b->setAutoDefault(true);
  } else {
    b->setAutoDefault(false);
  }
  /* a long label gets more of the row, as Restart as administrator does in the design */
  buttons_->addWidget(b, text.size() > 18 ? 14 : 10);
  connect(b, &QPushButton::clicked, this, [this, b] {
    clicked_ = b;
    accept();
  });
  return b;
}

void Stub::fit() {
  if (QLayout *l = layout()) setFixedHeight(l->totalHeightForWidth(width()) > 0 ? l->totalHeightForWidth(width())
                                                                             : l->sizeHint().height());
}

void Stub::showEvent(QShowEvent *e) {
  fit();
  QDialog::showEvent(e);
}

/* Without a title bar, it moves by a drag anywhere on it that is not a control. */
void Stub::mousePressEvent(QMouseEvent *e) {
  if (e->button() == Qt::LeftButton) drag_ = globalPos(e) - frameGeometry().topLeft();
  QDialog::mousePressEvent(e);
}

void Stub::mouseMoveEvent(QMouseEvent *e) {
  if (e->buttons() & Qt::LeftButton && !drag_.isNull()) move(globalPos(e) - drag_);
  QDialog::mouseMoveEvent(e);
}

void Stub::tell(QWidget *parent, Kind kind, const QString &code, const QString &title, const QString &sentence,
                const QString &mono) {
  Stub d(parent, kind, code, title, sentence);
  if (!mono.isEmpty()) d.setCode(mono);
  d.addButton(tr("OK"), Primary);
  d.exec();
}

bool Stub::ask(QWidget *parent, Kind kind, const QString &code, const QString &title, const QString &sentence,
               const QString &yes, const QString &no, bool destructive) {
  Stub d(parent, kind, code, title, sentence);
  QPushButton *y;
  if (destructive) { /* keeping what is going on is the safe choice: volt, first, and Enter's */
    d.addButton(no, Primary);
    y = d.addButton(yes, Outline);
  } else {
    d.addButton(no, Secondary);
    y = d.addButton(yes, Primary);
  }
  d.exec();
  return d.clicked() == y;
}
