#include "status_strip.h"
#include "controller.h"
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
    auto *col = new QVBoxLayout(this);
    col->setContentsMargins(0, 1, 0, 0);   // 1 px hairline above (painted by the strip)
    col->setSpacing(0);
    auto *row = new QWidget(this);
    row->setFixedHeight(kRow);
    auto *h = new QHBoxLayout(row);
    h->setContentsMargins(16, 0, 16, 0);
    h->setSpacing(8);
    m_dot = new StateDot(row);
    h->addWidget(m_dot);
    m_label = new QLabel(row);
    m_detail = new ui::ElidedLabel(row);
    m_chip = new QLabel(tr("SOURCE"), row);
    h->addWidget(m_label);
    h->addWidget(m_detail, 1);
    h->addWidget(m_chip);
    m_hint = new QLabel(tr("F1 KEYS"), row);
    m_hint->setToolTip(tr("Keyboard shortcuts (F1)"));
    h->addWidget(m_hint);
    col->addWidget(row);

    m_actions = new QWidget(this);
    auto *ah = new QHBoxLayout(m_actions);
    ah->setContentsMargins(16, 0, 16, 8);
    ah->setSpacing(8);
    auto add = [&](const QString &name, const QString &text, std::function<void()> fn) {
        auto *b = new QPushButton(text, m_actions);
        b->setFixedHeight(24);
        b->setCursor(Qt::PointingHandCursor);
        b->setProperty("hnName", name);
        connect(b, &QPushButton::clicked, this, [fn] { fn(); });
        m_buttons.insert(name, b);
        ah->addWidget(b);
    };
    add("retry", tr("Retry"), [this] { if (m_s) m_s->retry(); });
    add("reload", tr("Reload from disk"), [this] { if (m_s) m_s->reload(); });
    add("copy", tr("Save mine as copy"), [this] { if (m_s) m_s->saveLocalCopy(); });
    add("replace", tr("Replace disk"), [this] { if (m_s) m_s->replaceDisk(); });
    add("recreate", tr("Recreate file"), [this] { if (m_s) m_s->recreate(); });
    add("discard", tr("Discard edits"), [this] { if (m_s) m_c->discardNote(m_s->rel()); });
    ah->addStretch(1);
    m_actions->hide();

    col->addWidget(m_actions);
    m_flashTimer.setSingleShot(true);
    m_flashTimer.setInterval(6000);
    connect(&m_flashTimer, &QTimer::timeout, this, [this] { m_flash.clear(); refresh(); });
    connect(c, &AppController::themeChanged, this, &StatusStrip::refresh);
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
    m_label->setFont(ui::labelFont(10));
    m_detail->setTextFont(ui::uiFont(12));
    m_chip->setFont(ui::labelFont(9));
    m_hint->setFont(ui::labelFont(9));
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
    setFixedHeight(kRow + 1 + (problem ? 32 : 0));
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

void StatusStrip::paintEvent(QPaintEvent *) {
    QPainter p(this);
    const auto &t = ui::theme();
    p.fillRect(rect(), t.bg);
    p.fillRect(QRect(0, 0, width(), 1), t.border);
}

} // namespace hn::app
