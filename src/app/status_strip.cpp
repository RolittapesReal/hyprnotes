#include "status_strip.h"
#include "controller.h"
#include "action_row.h"
#include <QApplication>
#include <QScrollArea>
#include <QScrollBar>
#include <QHBoxLayout>
#include <QPainter>
#include <QPushButton>
#include <QVBoxLayout>

using namespace hn::editor;

namespace hn::app {

namespace {
class StateDot : public QWidget {
public:
    QColor color;
    explicit StateDot(QWidget *p) : QWidget(p) { setFixedSize(8, 8); }
protected:
    void paintEvent(QPaintEvent *) override { QPainter(this).fillRect(rect(), color); }
};
}

StatusStrip::StatusStrip(AppController *c, QWidget *parent) : QWidget(parent), m_c(c) {
    m_scroll = new QScrollArea(this);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scroll->setWidgetResizable(false);
    m_scroll->setFocusPolicy(Qt::NoFocus);
    m_content = new QWidget;
    m_content->installEventFilter(this);
    m_scroll->setWidget(m_content);
    auto *row = m_content;
    m_dot = new StateDot(row);
    m_label = new QLabel(row);
    m_label->setObjectName("hnSaveState");
    m_label->setWordWrap(true);
    m_detail = new ui::ElidedLabel(row);
    m_chip = new QLabel(tr("SOURCE"), row);
    m_chip->setObjectName("hnSourceBadge");
    m_hint = new QLabel(tr("F1 KEYS"), row);
    m_hint->setObjectName("hnShortcutHint");
    m_hint->setToolTip(tr("Keyboard shortcuts (F1)"));
    auto *actions = new ui::ActionRow(m_content);
    m_actions = actions;
    m_actions->installEventFilter(this);
    auto add = [&](const QString &name, const QString &text, std::function<void()> fn) {
        auto *b = new ui::WrappingButton(text, m_actions);
        b->setCursor(Qt::PointingHandCursor);
        b->setProperty("hnName", name);
        connect(b, &QPushButton::clicked, this, [fn] { fn(); });
        m_buttons.insert(name, b);
        actions->addButton(b);
    };
    add("retry", tr("Retry"), [this] { if (m_s) m_s->retry(); });
    add("reload", tr("Reload from disk"), [this] { if (m_s) m_s->reload(); });
    add("copy", tr("Save mine as copy"), [this] { if (m_s) m_s->saveLocalCopy(); });
    add("replace", tr("Replace disk"), [this] { if (m_s) m_s->replaceDisk(); });
    add("recreate", tr("Recreate file"), [this] { if (m_s) m_s->recreate(); });
    add("discard", tr("Discard edits"), [this] { if (m_s) m_c->discardNote(m_s->rel()); });
    m_actions->hide();
    if (parent) parent->installEventFilter(this);
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget *, QWidget *now) {
        if (now && m_content->isAncestorOf(now)) m_scroll->ensureWidgetVisible(now, 0, 0);
    });
    m_flashTimer.setSingleShot(true);
    m_flashTimer.setInterval(6000);
    connect(&m_flashTimer, &QTimer::timeout, this, [this] { m_flash.clear(); refresh(); });
    connect(ui::themeNotifier(), &ui::ThemeNotifier::changed, this, &StatusStrip::refresh);
    refresh();
}

QPushButton *StatusStrip::button(const QString &name) const { return m_buttons.value(name); }

void StatusStrip::setSession(NoteSession *s) {
    for (const auto &c : std::as_const(m_conns)) disconnect(c);
    m_conns.clear();
    m_s = s;
    if (s) {
        m_conns << connect(s, &NoteSession::statusChanged, this, &StatusStrip::refresh);
        m_conns << connect(s->editor(), &NoteEditor::modeChanged, this, &StatusStrip::refresh);
    }
    refresh();
}

void StatusStrip::flash(const QString &message) {
    m_flash = message;
    m_flashTimer.start();
    refresh();
}

void StatusStrip::refresh() {
    const auto &t = ui::theme();
    using St = NoteSession::State;
    m_label->setFont(ui::labelFont(qMax(10, t.baseSize - 4)));
    m_detail->setTextFont(ui::uiFont(qMax(12, t.baseSize - 2)));
    m_chip->setFont(ui::labelFont(qMax(9, t.baseSize - 5)));
    m_hint->setFont(ui::labelFont(qMax(9, t.baseSize - 5)));
    for (auto *button : m_buttons) button->setFont(ui::uiFont(t.baseSize, QFont::DemiBold));
    m_hint->setStyleSheet(QString("color: %1; background: transparent;").arg(t.muted.name()));
    QString label, detail;
    QColor labelColor = t.text;
    bool problem = false;
    QStringList show;
    if (m_s) {
        label = m_s->stateLabel();
        detail = m_s->detail();
        switch (m_s->state()) {
        case St::Failed: problem = true; show = {"retry", "discard"}; labelColor = t.danger; break;
        case St::Conflict: problem = true; show = {"reload", "copy", "replace"}; labelColor = t.danger; break;
        case St::Removed: problem = true; show = {"recreate", "discard"}; labelColor = t.danger; break;
        default: break;
        }
        const bool source = m_s->editor()->mode() == Mode::Source;
        if (detail.isEmpty() && source) detail = m_s->editor()->modeReason();
        m_chip->setVisible(source);
        m_chip->setStyleSheet(QString("background: %1; color: %2; padding: 1px 6px;").arg(t.text.name(), t.bg.name()));
    } else m_chip->hide();
    if (!m_flash.isEmpty()) detail = m_flash;
    m_label->setStyleSheet(QString("color: %1; background: transparent;").arg(labelColor.name()));
    m_label->setText(label);
    m_detail->setText(detail);
    m_detail->setColor(problem ? t.text : t.muted);
    for (auto it = m_buttons.begin(); it != m_buttons.end(); ++it) it.value()->setVisible(show.contains(it.key()));
    m_actions->setVisible(problem);
    setAccessibleName(tr("Save status: %1 %2").arg(label, detail));
    relayout();
    QColor dc = t.muted;
    if (m_s) {
        switch (m_s->state()) {
        case St::Clean: dc = t.success; break;
        case St::Dirty: dc = t.muted; break;
        case St::Saving: dc = t.text; break;
        default: dc = t.danger; break;
        }
    }
    static_cast<StateDot *>(m_dot)->color = dc;
    m_dot->update();
    update();
}

int StatusStrip::layoutContent(int width) {
    const int pad = 8, gap = 8, available = qMax(1, width - 2 * pad);
    const int labelW = qMin(available - 16, m_label->sizeHint().width());
    const int labelH = qMax(kRow - 8, m_label->heightForWidth(qMax(1, labelW)));
    m_dot->move(pad, 4 + (labelH - 8) / 2);
    m_label->setGeometry(pad + 16, 4, qMax(1, labelW), labelH);
    int x = pad + 16 + labelW + gap, y = 4, rowH = labelH;
    for (auto *item : {m_chip, m_hint}) {
        if (item->isHidden()) continue;
        const QSize size = item->sizeHint().boundedTo(QSize(available, 10000));
        if (x + size.width() > width - pad) { x = pad; y += rowH + 4; rowH = 0; }
        item->setGeometry(x, y, size.width(), size.height());
        x += size.width() + gap; rowH = qMax(rowH, size.height());
    }
    int bottom = y + rowH + 4;
    // Details are supplementary, unlike the state and destructive action labels.
    const bool detail = !m_detail->text().isEmpty();
    m_detail->setVisible(detail);
    if (detail) { m_detail->setGeometry(pad, bottom, available, m_detail->sizeHint().height()); bottom += m_detail->height(); }
    if (!m_actions->isHidden()) {
        const int h = m_actions->heightForWidth(available);
        m_actions->setGeometry(pad, bottom + 4, available, h);
        bottom += h + 12;
    }
    return qMax(kRow, bottom);
}

void StatusStrip::relayout() {
    if (m_layoutBusy) return;
    m_layoutBusy = true;
    // Keep room for the sticky header and an editor line, even at the 260x180 minimum.
    const int budget = qMax(40, (parentWidget() ? parentWidget()->height() : 300) - 72);
    int contentW = width();
    int natural = layoutContent(contentW);
    if (natural + 1 > budget) {
        contentW = qMax(1, width() - m_scroll->verticalScrollBar()->sizeHint().width());
        natural = layoutContent(contentW);
    }
    setFixedHeight(qMin(natural + 1, budget));
    m_scroll->setGeometry(0, 1, width(), height() - 1);
    m_content->resize(contentW, natural);
    m_layoutBusy = false;
}

void StatusStrip::resizeEvent(QResizeEvent *) { relayout(); }
bool StatusStrip::eventFilter(QObject *, QEvent *event) {
    switch (event->type()) {
    case QEvent::Resize: case QEvent::LayoutRequest: case QEvent::FontChange: case QEvent::StyleChange: relayout(); break;
    default: break;
    }
    return false;
}

void StatusStrip::paintEvent(QPaintEvent *) {
    QPainter p(this);
    const auto &t = ui::theme();
    p.fillRect(rect(), t.bg);
    p.fillRect(QRect(0, 0, width(), 1), t.border);
}

} // namespace hn::app
