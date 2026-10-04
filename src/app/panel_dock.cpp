#include "panel_dock.h"
#include "controller.h"
#include "ui_common.h"
#include <QAbstractTextDocumentLayout>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QMenu>
#include <QToolButton>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextLayout>
#include <functional>

using hn::plugins::PanelBlock;

namespace hn::app {

// ================================================================ hub
const PanelHub::Panel *PanelHub::find(const QString &qid) const {
    for (const auto &p : m_panels) if (p.qid == qid) return &p;
    return nullptr;
}

void PanelHub::showPanel(const QString &qid, const QString &title, const QString &icon) {
    for (auto &p : m_panels)
        if (p.qid == qid) { p.title = title; p.icon = icon; emit panelsChanged(); return; }
    Panel p;
    p.qid = qid; p.title = title; p.icon = icon;
    const int c = int(qid.indexOf(QLatin1Char(':')));
    p.pluginId = c > 0 ? qid.left(c) : qid;
    p.id = c > 0 ? qid.mid(c + 1) : QString();
    m_panels.append(p);
    emit panelsChanged();
}

void PanelHub::updatePanel(const QString &qid, const QList<PanelBlock> &blocks, const QString &error) {
    for (auto &p : m_panels)
        if (p.qid == qid) { p.blocks = blocks; p.error = error; p.rendered = true; emit panelUpdated(qid); return; }
}

void PanelHub::removePanel(const QString &qid) {
    for (int i = 0; i < m_panels.size(); ++i)
        if (m_panels[i].qid == qid) { m_panels.removeAt(i); emit panelsChanged(); return; }
}

// ================================================================ view
namespace {
constexpr int kPad = 16;
QFont headingFont(int level) {
    const int base = ui::theme().baseSize;
    return level <= 1 ? ui::uiFont(base + 4, QFont::Bold) : level == 2 ? ui::uiFont(base + 1, QFont::Bold) : ui::labelFont(qMax(10, base - 4));
}
QFont detailFont(const QFont &base) { QFont f = base; f.setPixelSize(qMax(11, base.pixelSize() - 2)); return f; }
}  // namespace

PanelView::PanelView(QWidget *parent) : QAbstractScrollArea(parent) {
    setFrameShape(QFrame::NoFrame);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    viewport()->setMouseTracking(true);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    verticalScrollBar()->setSingleStep(24);
    setAccessibleName(tr("Panel"));
    connect(ui::themeNotifier(), &ui::ThemeNotifier::changed, this, &PanelView::restyle);
    restyle();
}
PanelView::~PanelView() = default;

void PanelView::restyle() {
    setFont(ui::uiFont(qMax(13, ui::theme().baseSize - 1)));
    m_laidOutW = -1;
    relayout();
    viewport()->update();
}
void PanelView::changeEvent(QEvent *event) {
    QAbstractScrollArea::changeEvent(event);
    if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange) {
        m_laidOutW = -1; relayout(); viewport()->update();
    }
}

bool PanelView::actionable(const Row &r) const { return (r.kind == Row::Item && (r.click || !r.path.isEmpty())) || (r.kind == Row::Button && r.click); }

void PanelView::setContent(const QList<PanelBlock> &blocks, const QString &error) {
    const QString keep = currentTitle();
    m_blocks = blocks;
    m_error = error;
    m_laidOutW = -1;
    relayout();
    m_cur = -1;
    if (!keep.isEmpty())
        for (int i = 0; i < m_act.size(); ++i) if (m_rows[m_act[i]].text == keep) { m_cur = i; break; }
    if (m_cur < 0 && !m_act.isEmpty() && hasFocus()) m_cur = 0;
    m_hover = -1;
    viewport()->update();
}

void PanelView::relayout() {
    const int w = qMax(1, viewport()->width());
    if (w == m_laidOutW) return;
    m_laidOutW = w;
    m_rows.clear();
    m_docs.clear();
    m_act.clear();
    auto add = [&](Row r) { m_rows.append(std::move(r)); };
    std::function<void(const PanelBlock &)> addBlock = [&](const PanelBlock &b) {
        Row r;
        r.text = b.text;
        if (b.type == QLatin1String("heading")) { r.kind = Row::Heading; r.level = b.level; add(r); }
        else if (b.type == QLatin1String("text")) { r.kind = Row::Text; add(r); }
        else if (b.type == QLatin1String("empty")) { r.kind = Row::Empty; add(r); }
        else if (b.type == QLatin1String("button")) { r.kind = Row::Button; r.click = b.click; add(r); }
        else if (b.type == QLatin1String("item")) { r.kind = Row::Item; r.text = b.title; r.sub = b.subtitle; r.path = b.path; r.line = b.line; r.click = b.click; add(r); }
        else if (b.type == QLatin1String("list")) { for (const auto &it : b.items) addBlock(it); }
        else if (b.type == QLatin1String("markdown")) {
            auto d = std::make_unique<QTextDocument>();
            d->setDefaultFont(font());
            d->setDefaultStyleSheet(QString("a { color: %1; } code { font-family: monospace; }").arg(ui::theme().accent.name()));
            d->setDocumentMargin(0);
            d->setMarkdown(b.text, QTextDocument::MarkdownFeatures(QTextDocument::MarkdownDialectGitHub | QTextDocument::MarkdownNoHTML));   // same dialect family as the editor; no raw HTML
            // Markdown imports anchor formats directly; HTML's defaultStyleSheet and
            // the paint palette do not override those explicit foreground brushes.
            for (auto block = d->begin(); block.isValid(); block = block.next()) {
                for (auto it = block.begin(); !it.atEnd(); ++it) {
                    const auto fragment = it.fragment();
                    if (!fragment.isValid() || !fragment.charFormat().isAnchor()) continue;
                    QTextCursor cursor(d.get());
                    cursor.setPosition(fragment.position());
                    cursor.setPosition(fragment.position() + fragment.length(), QTextCursor::KeepAnchor);
                    QTextCharFormat format; format.setForeground(ui::theme().accent); cursor.mergeCharFormat(format);
                }
            }
            d->setTextWidth(qMax(1, w - 2 * kPad));
            r.kind = Row::Markdown;
            r.doc = int(m_docs.size());
            m_docs.push_back(std::move(d));
            add(r);
        }
    };
    if (!m_error.isEmpty()) { Row r; r.kind = Row::Error; r.text = tr("This panel failed to render: %1").arg(m_error); add(r); }
    else for (const auto &b : std::as_const(m_blocks)) addBlock(b);
    if (m_rows.isEmpty()) { Row r; r.kind = Row::Empty; r.text = tr("Nothing to show."); add(r); }
    int y = 0;
    for (int i = 0; i < m_rows.size(); ++i) {
        Row &r = m_rows[i];
        r.y = y;
        const int tw = qMax(1, w - 2 * kPad);
        switch (r.kind) {
        case Row::Heading: r.h = QFontMetrics(headingFont(r.level)).height() + (r.level <= 1 ? 28 : 24); break;
        case Row::Text: r.h = fontMetrics().boundingRect(QRect(0, 0, tw, 100000), Qt::TextWordWrap, r.text).height() + 14; break;
        case Row::Item: r.h = qMax(40, fontMetrics().height() + 16) + (r.sub.isEmpty() ? 0 : QFontMetrics(detailFont(font())).height() + 4); break;
        case Row::Button: r.h = qMax(32, fontMetrics().height() + 8) + 20; break;
        case Row::Empty: r.h = qMax(120, fontMetrics().boundingRect(QRect(0, 0, qMax(1, tw - 8), 100000), Qt::TextWordWrap, r.text).height() + 64); break;
        case Row::Error: r.h = fontMetrics().boundingRect(QRect(0, 0, qMax(1, tw - 12), 100000), Qt::TextWordWrap, r.text).height() + 24; break;
        case Row::Markdown: r.h = int(m_docs[size_t(r.doc)]->size().height()) + 16; break;
        }
        y += r.h;
        if (actionable(r)) m_act << i;
    }
    m_contentH = y;
    verticalScrollBar()->setRange(0, qMax(0, m_contentH - viewport()->height()));
    verticalScrollBar()->setPageStep(viewport()->height());
}

int PanelView::actionableCount() const { return int(m_act.size()); }
QString PanelView::currentTitle() const { return m_cur >= 0 && m_cur < m_act.size() ? m_rows[m_act[m_cur]].text : QString(); }
QString PanelView::rowText(int row) const { return row >= 0 && row < m_rows.size() ? m_rows[row].text : QString(); }
QRect PanelView::rowRect(int row) const {
    if (row < 0 || row >= m_rows.size()) return {};
    return QRect(0, m_rows[row].y - verticalScrollBar()->value(), viewport()->width(), m_rows[row].h);
}

int PanelView::rowAt(int contentY) const {
    for (int i = 0; i < m_rows.size(); ++i) if (contentY >= m_rows[i].y && contentY < m_rows[i].y + m_rows[i].h) return i;
    return -1;
}

void PanelView::setCurrentIndex(int i) {
    m_cur = m_act.isEmpty() ? -1 : qBound(0, i, int(m_act.size()) - 1);
    if (m_cur >= 0) ensureVisible(m_act[m_cur]);
    viewport()->update();
}

void PanelView::ensureVisible(int row) {
    const Row &r = m_rows[row];
    auto *sb = verticalScrollBar();
    if (r.y < sb->value()) sb->setValue(r.y);
    else if (r.y + r.h > sb->value() + viewport()->height()) sb->setValue(r.y + r.h - viewport()->height());
}

void PanelView::activateCurrent() {
    if (m_cur < 0 || m_cur >= m_act.size()) return;
    const Row row = m_rows[m_act[m_cur]];
    emit activated(row.click, row.path, row.line);
}

void PanelView::resizeEvent(QResizeEvent *e) {
    QAbstractScrollArea::resizeEvent(e);
    relayout();
    verticalScrollBar()->setRange(0, qMax(0, m_contentH - viewport()->height()));
    verticalScrollBar()->setPageStep(viewport()->height());
}

void PanelView::mouseMoveEvent(QMouseEvent *e) {
    const int row = rowAt(e->position().toPoint().y() + verticalScrollBar()->value());
    const int h = row >= 0 && actionable(m_rows[row]) ? row : -1;
    viewport()->setCursor(h >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
    if (h != m_hover) { m_hover = h; viewport()->update(); }
}

void PanelView::leaveEvent(QEvent *) { if (m_hover >= 0) { m_hover = -1; viewport()->update(); } }

void PanelView::mousePressEvent(QMouseEvent *e) {
    setFocus(Qt::MouseFocusReason);
    if (e->button() != Qt::LeftButton) return;
    const int row = rowAt(e->position().toPoint().y() + verticalScrollBar()->value());
    if (row < 0 || !actionable(m_rows[row])) return;
    const Row &r = m_rows[row];
    if (r.kind == Row::Button && !QRect(kPad, r.y + 10 - verticalScrollBar()->value(), viewport()->width() - 2 * kPad, r.h - 20).contains(e->position().toPoint())) return;
    m_cur = int(m_act.indexOf(row));
    viewport()->update();
    activateCurrent();
}

void PanelView::keyPressEvent(QKeyEvent *e) {
    const int n = int(m_act.size());
    switch (e->key()) {
    case Qt::Key_Down: setCurrentIndex(m_cur < 0 ? 0 : qMin(n - 1, m_cur + 1)); return;
    case Qt::Key_Up: setCurrentIndex(m_cur < 0 ? 0 : qMax(0, m_cur - 1)); return;
    case Qt::Key_Home: setCurrentIndex(0); return;
    case Qt::Key_End: setCurrentIndex(n - 1); return;
    case Qt::Key_PageDown: case Qt::Key_PageUp: {
        const int dir = e->key() == Qt::Key_PageDown ? 1 : -1;
        const int target = verticalScrollBar()->value() + dir * viewport()->height();
        verticalScrollBar()->setValue(target);
        int best = m_cur < 0 ? 0 : m_cur;
        for (int i = 0; i < n; ++i) if (dir > 0 ? m_rows[m_act[i]].y <= target : m_rows[m_act[i]].y >= target - viewport()->height()) { best = i; if (dir < 0) break; }
        setCurrentIndex(best);
        return;
    }
    case Qt::Key_Return: case Qt::Key_Enter: case Qt::Key_Space: activateCurrent(); return;
    case Qt::Key_Left: emit switchPanel(-1); return;
    case Qt::Key_Right: emit switchPanel(1); return;
    default: break;
    }
    QAbstractScrollArea::keyPressEvent(e);
}

void PanelView::paintEvent(QPaintEvent *) {
    QPainter p(viewport());
    const auto &t = ui::theme();
    p.fillRect(viewport()->rect(), t.bg);
    const int off = verticalScrollBar()->value(), w = viewport()->width(), vh = viewport()->height();
    const int curRow = m_cur >= 0 && m_cur < m_act.size() ? m_act[m_cur] : -1;
    for (int i = 0; i < m_rows.size(); ++i) {
        const Row &r = m_rows[i];
        const int y = r.y - off;
        if (y + r.h < 0 || y > vh) continue;
        const QRect rr(0, y, w, r.h);
        switch (r.kind) {
        case Row::Heading:
            p.setFont(headingFont(r.level));
            p.setPen(r.level >= 3 ? t.muted : t.text);
            p.drawText(QRect(kPad, y + (r.level <= 1 ? 12 : 8), w - 2 * kPad, r.h - 12), Qt::AlignVCenter | Qt::AlignLeft,
                       QFontMetrics(p.font()).elidedText(r.level >= 3 ? r.text.toUpper() : r.text, Qt::ElideRight, w - 2 * kPad));
            break;
        case Row::Text:
            p.setFont(font());
            p.setPen(t.text);
            p.drawText(QRect(kPad, y + 4, w - 2 * kPad, r.h - 8), Qt::TextWordWrap | Qt::AlignTop, r.text);
            break;
        case Row::Item: {
            const bool cur = i == curRow, hov = i == m_hover;
            if (cur || hov) p.fillRect(rr, t.selection);
            if (cur) p.fillRect(QRect(0, y, 3, r.h), t.accent);
            p.fillRect(QRect(0, y + r.h - 1, w, 1), t.border);
            const int tw = w - 2 * kPad;
            QFont title = font(); title.setWeight(QFont::DemiBold); p.setFont(title);
            p.setPen(t.text);
            p.drawText(QRect(kPad, y + (r.sub.isEmpty() ? 0 : 8), tw, r.sub.isEmpty() ? r.h : fontMetrics().height()), Qt::AlignVCenter | Qt::AlignLeft, QFontMetrics(p.font()).elidedText(r.text, Qt::ElideRight, tw));
            if (!r.sub.isEmpty()) {
                p.setFont(detailFont(font()));
                p.setPen(t.muted);
                p.drawText(QRect(kPad, y + 12 + fontMetrics().height(), tw, p.fontMetrics().height()), Qt::AlignVCenter | Qt::AlignLeft, p.fontMetrics().elidedText(r.sub, Qt::ElideRight, tw));
            }
            if (cur && hasFocus()) ui::paintFocusRing(&p, rr);
            break;
        }
        case Row::Button: {
            const bool cur = i == curRow, hov = i == m_hover;
            const QRect b(kPad, y + 10, w - 2 * kPad, r.h - 20);
            p.setBrush(hov || cur ? t.accent : t.bg);
            p.setPen(QPen(t.accent, 1));
            p.drawRoundedRect(b.adjusted(0, 0, -1, -1), t.radius, t.radius);
            QFont button = font(); button.setBold(true); p.setFont(button);
            p.setPen(hov || cur ? t.accentText : t.accent);
            p.drawText(b, Qt::AlignCenter, QFontMetrics(p.font()).elidedText(r.text, Qt::ElideRight, b.width() - 16));
            if (cur && hasFocus()) ui::paintFocusRing(&p, b.adjusted(-2, -2, 2, 2));
            break;
        }
        case Row::Empty:
            p.fillRect(QRect(w / 2 - 12, y + 28, 24, 4), t.accent);
            p.setFont(font());
            p.setPen(t.muted);
            p.drawText(QRect(kPad + 4, y + 44, w - 2 * kPad - 8, r.h - 52), Qt::TextWordWrap | Qt::AlignHCenter | Qt::AlignTop, r.text);
            break;
        case Row::Error:
            p.fillRect(QRect(kPad, y + 6, 3, r.h - 12), t.danger);
            p.setFont(font());
            p.setPen(t.danger);
            p.drawText(QRect(kPad + 12, y + 6, w - 2 * kPad - 12, r.h - 12), Qt::TextWordWrap | Qt::AlignTop, r.text);
            break;
        case Row::Markdown: {
            p.save();
            p.translate(kPad, y + 8);
            QAbstractTextDocumentLayout::PaintContext ctx;
            ctx.palette.setColor(QPalette::Text, t.text);
            ctx.palette.setColor(QPalette::Link, t.accent);
            ctx.clip = QRectF(0, 0, w - 2 * kPad, r.h);
            m_docs[size_t(r.doc)]->documentLayout()->draw(&p, ctx);
            p.restore();
            break;
        }
        }
    }
}

// ================================================================ dock
PanelDock::PanelDock(AppController *c, QWidget *parent, bool popup) : QWidget(parent, popup ? Qt::Popup | Qt::FramelessWindowHint : Qt::Widget), m_c(c), m_popup(popup) {
    setObjectName(popup ? "hnPanelPopup" : "hnPanelDock");
    setMouseTracking(true);
    setAutoFillBackground(false);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setFocusPolicy(Qt::StrongFocus);
    m_view = new PanelView(this);
    m_view->setObjectName("hnPanelView");
    m_close = new ui::IconButton("close", tr("Hide panel"), this, 28);
    m_close->setAccessibleName(tr("Hide panel"));
    m_overflow = new QToolButton(this);
    m_overflow->setObjectName("hnPanelOverflow");
    m_overflow->setText(QStringLiteral("…"));
    m_overflow->setAccessibleName(tr("More panels"));
    m_overflow->setToolTip(tr("Choose panel (Left/Right or Ctrl+PageUp/PageDown)"));
    m_overflow->setFocusPolicy(Qt::TabFocus);
    m_overflow->setFixedSize(28, 28);
    m_overflow->setPopupMode(QToolButton::InstantPopup);
    m_overflow->setMenu(new QMenu(m_overflow));
    connect(m_close, &QAbstractButton::clicked, this, &PanelDock::closeRequested);
    connect(m_view, &PanelView::activated, this, [this](int click, const QString &path, int line) { activated(click, path, line); });
    connect(m_view, &PanelView::switchPanel, this, [this](int d) {
        const auto ts = tabs();
        for (int i = 0; i < ts.size(); ++i) if (ts[i].first == m_cur) { setCurrentPanel(ts[qBound(0, i + d, int(ts.size()) - 1)].first); return; }
    });
    if (auto *hub = c->plugins().hub()) {
        connect(hub, &PanelHub::panelsChanged, this, [this] { rebuild(); });
        connect(hub, &PanelHub::panelUpdated, this, [this](const QString &qid) { if (qid == m_cur) showCurrent(); });
    }
    connect(ui::themeNotifier(), &ui::ThemeNotifier::changed, this, [this] { layoutTabs(); update(); });
    setMinimumWidth(kMinW);
    rebuild();
}

PanelDock::~PanelDock() { m_c->plugins().setPanelsShown(this, {}); }

QList<QPair<QString, QString>> PanelDock::tabs() const {
    QList<QPair<QString, QString>> out;
    if (auto *hub = m_c->plugins().hub()) for (const auto &p : hub->panels()) out.append({p.qid, p.title});
    return out;
}
QStringList PanelDock::panelIds() const { QStringList l; for (const auto &t : tabs()) l << t.first; return l; }

void PanelDock::rebuild() {
    const auto ts = tabs();
    bool has = false;
    for (const auto &t : ts) has |= t.first == m_cur;
    if (!has) m_cur = ts.isEmpty() ? QString() : ts.first().first;
    m_overflow->menu()->clear();
    for (const auto &tab : ts) {
        auto *action = m_overflow->menu()->addAction(tab.second);
        action->setCheckable(true); action->setData(tab.first);
        connect(action, &QAction::triggered, this, [this, id = tab.first] { setCurrentPanel(id); });
    }
    layoutTabs();
    update();
    markShown();
    showCurrent();
    if (isVisible() && !m_cur.isEmpty()) refresh();
}

void PanelDock::setSession(NoteSession *s) {
    if (m_session == s) return;
    m_session = s;
    if (isVisible()) refresh();
}

void PanelDock::setCurrentPanel(const QString &qid) {
    if (qid == m_cur) return;
    bool known = false;
    for (const auto &t : tabs()) known |= t.first == qid;
    if (!known) return;
    m_cur = qid;
    layoutTabs();
    update();
    markShown();
    showCurrent();
    if (isVisible()) refresh();
}

void PanelDock::markShown() { m_c->plugins().setPanelsShown(this, isVisible() && !m_cur.isEmpty() ? QStringList{m_cur} : QStringList{}); }

void PanelDock::showCurrent() {
    const auto *hub = m_c->plugins().hub();
    const auto *p = hub ? hub->find(m_cur) : nullptr;
    if (p && p->rendered) m_view->setContent(p->blocks, p->error);
    else m_view->setContent({}, QString());
}

void PanelDock::refresh() {
    if (m_cur.isEmpty()) return;
    m_c->plugins().requestPanelRefresh(m_cur, m_session.data());   // the result arrives through PanelHub::panelUpdated
    showCurrent();
}

void PanelDock::activated(int click, const QString &path, int) {
    if (click > 0) { m_c->plugins().panelClick(m_cur, click, m_session.data()); return; }
    if (path.isEmpty()) return;
    if (m_popup) { close(); m_c->openSticky(path, true); }
    else { m_c->showOrganizer(true); m_c->openInOrganizer(path); }
}

void PanelDock::resizeEvent(QResizeEvent *e) {
    QWidget::resizeEvent(e);
    layoutTabs();
}

void PanelDock::changeEvent(QEvent *event) {
    QWidget::changeEvent(event);
    if (m_overflow && (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange)) { layoutTabs(); update(); }
}

QFont PanelDock::tabFont() const { auto f = ui::labelFont(qMax(10, ui::theme().baseSize - 4)); f.setBold(true); return f; }
int PanelDock::headerHeight() const { return qMax(kHeader, QFontMetrics(tabFont()).height() + 12); }
void PanelDock::layoutTabs() {
    const auto ts = tabs();
    const int header = headerHeight();
    m_close->move(qMax(0, width() - 34), (header - 28) / 2);
    m_view->setGeometry(kGrip, header, qMax(0, width() - kGrip - (m_popup ? 1 : 0)), qMax(0, height() - header - (m_popup ? 1 : 0)));
    const int left = kGrip + 8, right = qMax(left, m_close->x() - 8);
    int needed = 0;
    QList<int> widths;
    for (const auto &tab : ts) {
        QTextLayout layout(tab.second.toUpper(), tabFont());
        layout.beginLayout(); auto line = layout.createLine();
        if (line.isValid()) line.setLineWidth(100000);
        const int w = (line.isValid() ? qCeil(line.naturalTextWidth()) : 0) + 24;
        layout.endLayout(); widths << w; needed += w;
    }
    const bool overflow = needed > right - left;
    m_overflow->setVisible(overflow);
    m_overflow->move(qMax(left, right - 28), (header - 28) / 2);
    m_tabRects = QList<QRect>(ts.size());
    int x = left;
    for (int i = 0; i < ts.size(); ++i) {
        if (overflow && ts[i].first != m_cur) continue;
        const int available = qMax(0, (overflow ? m_overflow->x() - 8 : right) - x);
        const int w = qMin(available, widths[i]);
        if (w > 0) m_tabRects[i] = QRect(x, 0, w, header);
        x += w;
    }
    for (auto *action : m_overflow->menu()->actions()) action->setChecked(action->data().toString() == m_cur);
}

void PanelDock::showEvent(QShowEvent *e) {
    QWidget::showEvent(e);
    markShown();
    refresh();
}
void PanelDock::hideEvent(QHideEvent *e) {
    QWidget::hideEvent(e);
    markShown();
}

QRect PanelDock::tabRect(int i) const {
    return m_tabRects.value(i);
}
int PanelDock::tabAt(const QPoint &p) const {
    const int n = int(tabs().size());
    for (int i = 0; i < n; ++i) if (tabRect(i).contains(p)) return i;
    return -1;
}

void PanelDock::paintEvent(QPaintEvent *) {
    QPainter p(this);
    const auto &t = ui::theme();
    const int header = headerHeight();
    p.fillRect(rect(), t.bg);
    p.fillRect(QRect(0, 0, width(), header), t.surface);
    p.fillRect(QRect(0, header - 1, width(), 1), t.border);
    p.setPen(t.border);
    if (m_popup) {
        p.setBrush(Qt::NoBrush);
        p.setRenderHint(QPainter::Antialiasing, hn::theme::popupRadius(t) > 0);
        p.drawRoundedRect(rect().adjusted(0, 0, -1, -1), hn::theme::popupRadius(t), hn::theme::popupRadius(t));
    }
    const auto ts = tabs();
    p.setFont(tabFont());
    for (int i = 0; i < ts.size(); ++i) {
        const QRect r = tabRect(i);
        if (!r.isValid()) continue;
        const bool cur = ts[i].first == m_cur;
        if (i == m_hoverTab && !cur) p.fillRect(r, t.selection);
        p.setPen(cur ? t.text : t.muted);
        const QFont f = tabFont();
        p.drawText(r.adjusted(12, 0, -12, 0), Qt::AlignVCenter | Qt::AlignLeft, QFontMetrics(f).elidedText(ts[i].second.toUpper(), Qt::ElideRight, r.width() - 24));
        if (cur) p.fillRect(QRect(r.left(), header - 3, r.width(), 3), t.accent);
    }
    if (!m_popup && m_dragX >= 0) p.fillRect(QRect(0, 0, 3, height()), t.accent);
}

void PanelDock::mousePressEvent(QMouseEvent *e) {
    const QPoint pt = e->position().toPoint();
    if (e->button() != Qt::LeftButton) return;
    if (!m_popup && pt.x() < kGrip) { m_dragX = int(e->globalPosition().x()); m_dragW = m_dragDesired = width(); update(); return; }
    if (const int i = tabAt(pt); i >= 0) setCurrentPanel(tabs()[i].first);
}
void PanelDock::mouseMoveEvent(QMouseEvent *e) {
    const QPoint pt = e->position().toPoint();
    if (m_dragX >= 0) {   // dragging the left edge: moving left widens the dock
        m_dragDesired = qBound(int(kMinW), m_dragW + (m_dragX - int(e->globalPosition().x())), int(kMaxW));
        setFixedWidth(m_dragDesired);
        emit widthPreviewed(m_dragDesired);
        return;
    }
    setCursor(!m_popup && pt.x() < kGrip ? Qt::SizeHorCursor : tabAt(pt) >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
    const int h = tabAt(pt);
    if (h != m_hoverTab) { m_hoverTab = h; update(); }
}
void PanelDock::mouseReleaseEvent(QMouseEvent *) {
    if (m_dragX < 0) return;
    m_dragX = -1;
    update();
    emit widthChosen(m_dragDesired);
}

void PanelDock::keyPressEvent(QKeyEvent *e) {
    if (e->key() == Qt::Key_Escape) { emit closeRequested(); return; }
    if (e->modifiers() & Qt::ControlModifier && (e->key() == Qt::Key_PageDown || e->key() == Qt::Key_PageUp)) { emit m_view->switchPanel(e->key() == Qt::Key_PageDown ? 1 : -1); return; }
    QWidget::keyPressEvent(e);
}

}  // namespace hn::app
