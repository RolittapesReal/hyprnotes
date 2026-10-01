#include "command_palette.h"
#include "ui_common.h"
#include <QApplication>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <algorithm>

namespace hn::app {

int CommandPalette::fuzzyScore(const QString &query, const QString &text) {
    const QString q = query.trimmed().toLower(), t = text.toLower();
    if (q.isEmpty()) return 0;
    int score = 0, ti = 0, run = 0;
    for (const QChar qc : q) {
        if (qc.isSpace()) { run = 0; continue; }
        const int found = t.indexOf(qc, ti);
        if (found < 0) return -1;
        const bool wordStart = found == 0 || !t[found - 1].isLetterOrNumber();
        run = found == ti && run > 0 ? run + 1 : 1;
        score += 10 + (wordStart ? 8 : 0) + run * 3 - qMin(found - ti, 6);
        ti = found + 1;
    }
    score -= int(t.size() - q.size()) / 4;   // prefer tighter candidates
    // A contiguous match beats a scattered one ("sel" in "shout selection" must not be matched as s..e..l from "shout").
    if (const int at = t.indexOf(q); at >= 0) {
        const bool wordStart = at == 0 || !t[at - 1].isLetterOrNumber();
        score = qMax(score, 100 + int(q.size()) * 5 + (wordStart ? 20 : 0) - at / 2 - int(t.size() - q.size()) / 4);
    }
    return score;
}

QStringList CommandPalette::visibleIds() const {
    QStringList ids;
    for (const auto &i : m_shown) ids << i.id;
    return ids;
}
QString CommandPalette::currentId() const { return m_sel >= 0 && m_sel < m_shown.size() ? m_shown[m_sel].id : QString(); }

CommandPalette *CommandPalette::openOn(QWidget *host) { return host ? host->findChild<CommandPalette *>("hnPalette", Qt::FindDirectChildrenOnly) : nullptr; }

CommandPalette *CommandPalette::toggle(QWidget *host, const QList<Item> &items, std::function<void(const QString &)> run) {
    if (auto *open = openOn(host)) { open->dismiss(); return nullptr; }
    return new CommandPalette(host, items, std::move(run));
}

CommandPalette::CommandPalette(QWidget *host, const QList<Item> &items, std::function<void(const QString &)> run)
    : QWidget(host), m_all(items), m_run(std::move(run)), m_host(host) {
    setObjectName("hnPalette");
    setAttribute(Qt::WA_OpaquePaintEvent);
    setFocusPolicy(Qt::StrongFocus);
    setAccessibleName(tr("Command palette"));
    m_prevFocus = QApplication::focusWidget();
    m_input = new QLineEdit(this);
    m_input->setPlaceholderText(tr("Type a plugin command"));
    m_input->setAccessibleName(tr("Search plugin commands"));
    m_input->setFrame(false);
    m_input->setFont(ui::uiFont(15));
    m_input->setStyleSheet(QString("QLineEdit { background: transparent; border: none; padding: 0 16px; color: %1; }").arg(ui::theme().text.name()));
    m_input->installEventFilter(this);
    host->installEventFilter(this);
    connect(m_input, &QLineEdit::textChanged, this, [this] { refilter(); });
    refilter();
    place();
    show();
    raise();
    m_input->setFocus();
}

void CommandPalette::place() {
    const int w = qMin(560, m_host->width() - 32);
    const int rows = qMax(1, qMin(int(m_shown.size()), kMaxRows));
    setGeometry((m_host->width() - w) / 2, qMin(48, qMax(8, m_host->height() / 8)), w, kInput + 1 + rows * kRow + 1);
    m_input->setGeometry(0, 0, width(), kInput);
}

void CommandPalette::setFilter(const QString &q) { m_input->setText(q); }

void CommandPalette::refilter() {
    const QString q = m_input->text();
    QList<QPair<int, Item>> scored;
    for (const auto &i : std::as_const(m_all)) {
        const int s = qMax(fuzzyScore(q, i.title + QLatin1Char(' ') + i.plugin), fuzzyScore(q, i.title));
        if (s >= 0) scored.append({s, i});
    }
    std::stable_sort(scored.begin(), scored.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
    m_shown.clear();
    for (const auto &p : scored) m_shown << p.second;
    m_sel = 0;
    m_top = 0;
    if (m_host) place();
    update();
}

void CommandPalette::activateCurrent() {
    const QString id = currentId();
    if (id.isEmpty()) return;
    dismiss();
    if (m_run) m_run(id);
}

void CommandPalette::dismiss() {
    if (m_host) m_host->removeEventFilter(this);
    QPointer<QWidget> prev = m_prevFocus;
    hide();
    deleteLater();
    if (prev) prev->setFocus();
}

bool CommandPalette::eventFilter(QObject *o, QEvent *e) {
    if (o == m_host && e->type() == QEvent::Resize) place();
    if (o == m_host && e->type() == QEvent::MouseButtonPress && !geometry().contains(static_cast<QMouseEvent *>(e)->position().toPoint())) dismiss();
    if (o == m_input && e->type() == QEvent::ShortcutOverride) { e->accept(); return true; }   // window shortcuts must not act on the note behind
    if (o == m_input && e->type() == QEvent::KeyPress) {
        auto *k = static_cast<QKeyEvent *>(e);
        const int n = int(m_shown.size());
        switch (k->key()) {
        case Qt::Key_Escape: dismiss(); return true;
        case Qt::Key_Return: case Qt::Key_Enter: activateCurrent(); return true;
        case Qt::Key_Down: if (n) m_sel = (m_sel + 1) % n; break;
        case Qt::Key_Up: if (n) m_sel = (m_sel + n - 1) % n; break;
        case Qt::Key_PageDown: m_sel = qMin(n - 1, m_sel + kMaxRows); break;
        case Qt::Key_PageUp: m_sel = qMax(0, m_sel - kMaxRows); break;
        default: return false;
        }
        if (m_sel < m_top) m_top = m_sel;
        if (m_sel >= m_top + kMaxRows) m_top = m_sel - kMaxRows + 1;
        update();
        return true;
    }
    return QWidget::eventFilter(o, e);
}

void CommandPalette::mousePressEvent(QMouseEvent *e) {
    const int row = (e->position().toPoint().y() - kInput - 1) / kRow;
    if (row >= 0 && m_top + row < m_shown.size()) { m_sel = m_top + row; activateCurrent(); }
}

void CommandPalette::paintEvent(QPaintEvent *) {
    QPainter p(this);
    const auto &t = ui::theme();
    p.fillRect(rect(), t.surface);
    p.fillRect(QRect(0, kInput, width(), 1), t.border);
    p.setPen(QPen(t.text, 1));
    p.drawRect(rect().adjusted(0, 0, -1, -1));
    p.fillRect(QRect(0, 0, 4, kInput), t.accent);
    if (m_shown.isEmpty()) {
        p.setFont(ui::uiFont(13));
        p.setPen(t.muted);
        p.drawText(QRect(16, kInput + 1, width() - 32, kRow), Qt::AlignVCenter | Qt::AlignLeft,
                   m_all.isEmpty() ? tr("No plugin commands. Enable plugins in Settings > Plugins.") : tr("No matching command"));
        return;
    }
    for (int r = 0; r < kMaxRows && m_top + r < m_shown.size(); ++r) {
        const Item &it = m_shown[m_top + r];
        const QRect row(1, kInput + 1 + r * kRow, width() - 2, kRow);
        const bool sel = m_top + r == m_sel;
        if (sel) { p.fillRect(row, t.selection); p.fillRect(QRect(row.left(), row.top(), 3, row.height()), t.accent); }
        p.setFont(ui::uiFont(13, sel ? QFont::DemiBold : QFont::Normal));
        p.setPen(t.text);
        const QFont small = ui::uiFont(11, QFont::DemiBold);
        const QFontMetrics sm(small);
        const QString right = it.keyText.isEmpty() ? it.plugin : it.plugin + QStringLiteral("   ") + it.keyText;
        const int rw = qMin(width() / 2, sm.horizontalAdvance(right) + 8);
        p.drawText(QRect(16, row.top(), row.width() - rw - 32, kRow), Qt::AlignVCenter | Qt::AlignLeft,
                   QFontMetrics(p.font()).elidedText(it.title, Qt::ElideRight, row.width() - rw - 32));
        p.setFont(small);
        p.setPen(t.muted);
        p.drawText(QRect(row.right() - rw - 12, row.top(), rw, kRow), Qt::AlignVCenter | Qt::AlignRight, sm.elidedText(right, Qt::ElideLeft, rw));
    }
}

} // namespace hn::app
