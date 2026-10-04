#include "action_row.h"
#include <QEvent>
#include <QPainter>
#include <QStyleOptionButton>
#include <QTextLayout>

namespace hn::app::ui {
namespace {
// Shared by measurement and painting, including long unbroken translated words.
QSize textSize(const QFont &font, const QString &text, int width) {
    QTextLayout layout(text, font);
    QTextOption option; option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    layout.setTextOption(option);
    qreal h = 0, w = 0;
    layout.beginLayout();
    while (true) {
        auto line = layout.createLine();
        if (!line.isValid()) break;
        line.setLineWidth(qMax(1, width));
        h += line.height(); w = qMax(w, line.naturalTextWidth());
    }
    layout.endLayout();
    return {qCeil(w), qCeil(h)};
}
}

WrappingButton::WrappingButton(const QString &text, QWidget *parent) : QPushButton(text, parent) {
    QSizePolicy policy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    policy.setHeightForWidth(true);
    setSizePolicy(policy);
}
bool WrappingButton::hasHeightForWidth() const { return true; }
// 24px of padding, plus room for the menu indicator when the button opens a menu.
static int labelPad(const QPushButton *b) { return b->menu() ? 44 : 24; }
int WrappingButton::heightForWidth(int width) const { return qMax(32, textSize(font(), text(), width - labelPad(this)).height() + 8); }
QSize WrappingButton::sizeHint() const {
    const int w = textSize(font(), text(), 100000).width() + labelPad(this);
    return {w, heightForWidth(w)};
}
QSize WrappingButton::minimumSizeHint() const { return {labelPad(this) + fontMetrics().maxWidth(), heightForWidth(sizeHint().width())}; }
void WrappingButton::paintEvent(QPaintEvent *) {
    QStyleOptionButton option; initStyleOption(&option);
    QPainter painter(this);
    style()->drawControl(QStyle::CE_PushButtonBevel, &option, &painter, this);
    QTextLayout layout(text(), font());
    QTextOption wrap; wrap.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere); wrap.setAlignment(Qt::AlignHCenter);
    layout.setTextOption(wrap);
    qreal h = 0;
    layout.beginLayout();
    while (true) {
        auto line = layout.createLine(); if (!line.isValid()) break;
        line.setLineWidth(qMax(1, width() - labelPad(this))); line.setPosition({0, h}); h += line.height();
    }
    layout.endLayout();
    // Let the style resolve hover/pressed/disabled foregrounds just as for QPushButton.
    for (int i = 0; i < layout.lineCount(); ++i) {
        const auto line = layout.lineAt(i);
        QStyleOptionButton label = option;
        label.text = text().mid(line.textStart(), line.textLength());
        label.rect = QRect(12, qRound((height() - h) / 2 + line.y()), qMax(1, width() - 24), qCeil(line.height()));  // the style subtracts the menu indicator itself
        style()->drawControl(QStyle::CE_PushButtonLabel, &label, &painter, this);
    }
    if (hasFocus()) {
        QStyleOptionFocusRect focus;
        focus.QStyleOption::operator=(option);
        focus.rect = rect().adjusted(3, 3, -3, -3);
        style()->drawPrimitive(QStyle::PE_FrameFocusRect, &focus, &painter, this);
    }
}

ActionRow::ActionRow(QWidget *parent) : QWidget(parent) {
    QSizePolicy policy(QSizePolicy::Preferred, QSizePolicy::Preferred); policy.setHeightForWidth(true); setSizePolicy(policy);
    installEventFilter(this);
}
void ActionRow::addButton(QPushButton *button) {
    button->setParent(this);
    if (!m_buttons.isEmpty() && m_buttons.last()) QWidget::setTabOrder(m_buttons.last(), button);
    m_buttons << button;
    button->installEventFilter(this);
    button->show();
    arrange(width(), true); updateGeometry();
}
bool ActionRow::hasHeightForWidth() const { return true; }
int ActionRow::arrange(int width, bool apply) const {
    int x = 0, y = 0, rowH = 0;
    for (auto button : m_buttons) {
        if (!button || button->isHidden()) continue;
        const int w = qMin(qMax(0, width), button->sizeHint().width());
        const int h = button->hasHeightForWidth() ? button->heightForWidth(w) : button->sizeHint().height();
        if (x && x + w > width) { x = 0; y += rowH + 8; rowH = 0; }
        if (apply) button->setGeometry(x, y, w, h);
        x += w + 8; rowH = qMax(rowH, h);
    }
    return y + rowH;
}
int ActionRow::heightForWidth(int width) const { return arrange(width, false); }
QSize ActionRow::sizeHint() const {
    int w = 0;
    for (auto button : m_buttons) if (button && !button->isHidden()) w += button->sizeHint().width() + 8;
    w = qMax(0, w - 8); return {w, heightForWidth(w)};
}
QSize ActionRow::minimumSizeHint() const { return {0, 0}; }
void ActionRow::resizeEvent(QResizeEvent *) { arrange(width(), true); }
bool ActionRow::eventFilter(QObject *, QEvent *event) {
    switch (event->type()) {
    case QEvent::Show: case QEvent::Hide: case QEvent::FontChange: case QEvent::StyleChange: case QEvent::LayoutRequest:
        arrange(width(), true); updateGeometry(); break;
    default: break;
    }
    return false;
}
} // namespace hn::app::ui
