// NoteEditor, Plugin API v2: wiki-link overlay, link activation, completion popup. Everything here is inert (m_l == nullptr)
// until a plugin-facing setter is called.
#include <QApplication>
#include <QAbstractScrollArea>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextLayout>

#include "completion_popup.h"
#include "hn/editor/note_editor.h"
#include "md_io.h"
#include "visual_edit.h"

namespace hn::editor {

namespace {
// Per-block cache of scanned ranges, stored as block user data (freed with the block). Invalidated from contentsChange.
struct BlockCache : QTextBlockUserData {
    int len = -1, fence = -2;
    bool vis = false;
    QList<LinkRange> ranges;
};

bool isFenceLine(const QString &t)
{
    const QString s = t.trimmed();
    return s.startsWith(QLatin1String("```")) || s.startsWith(QLatin1String("~~~"));
}
} // namespace

struct NoteEditor::LinksImpl {
    bool linksOn = false, clickActivates = false, filterOn = false, hand = false;
    std::function<LinkState(const QString &)> resolver;
    QHash<QString, LinkState> states;
    QStringList triggers;
    QMetaObject::Connection cv, cs;
    // press tracking for click activation
    std::optional<LinkRefInfo> pressed;
    // completion session
    bool active = false, atLineStart = false;
    int start = 0, gen = 0;
    QString id, query;
    CompletionPopup *popup = nullptr;
    int pendingGen = 0;
};

NoteEditor::~NoteEditor() { delete m_l; }

static hn::theme::Theme effective(const hn::theme::Theme &t, const QPalette &pal)
{
    hn::theme::Theme e = t;
    if (!e.bg.isValid()) e.bg = pal.color(QPalette::Base);
    if (!e.surface.isValid()) e.surface = pal.color(QPalette::Window);
    if (!e.text.isValid()) e.text = pal.color(QPalette::Text);
    if (!e.muted.isValid()) e.muted = pal.color(QPalette::PlaceholderText);
    if (!e.accent.isValid()) e.accent = pal.color(QPalette::Highlight);
    if (!e.border.isValid()) e.border = pal.color(QPalette::Mid);
    if (!e.danger.isValid()) e.danger = QColor(0xc0, 0x39, 0x2b);
    return e;
}

// ---- fence tracking (source mode): QTextBlock::userState 0 = outside a ``` fence after this block, 1 = inside, -1 = unknown ----

static void fenceUpdate(QTextDocument *d, int pos, int added)
{
    QTextBlock b = d->findBlock(qBound(0, pos, qMax(0, d->characterCount() - 1)));
    int in = 0;
    if (b.previous().isValid()) {
        in = b.previous().userState();
        if (in < 0) { b = d->begin(); in = 0; }   // never computed: start over from the top
    }
    const int endPos = pos + added;
    for (; b.isValid(); b = b.next()) {
        const int out = in ^ (isFenceLine(b.text()) ? 1 : 0);
        const int old = b.userState();
        b.setUserState(out);
        in = out;
        if (b.position() > endPos && old == out) break;
    }
}

// ---- setup / hooks -------------------------------------------------------------------------------------------------

void NoteEditor::linksRefreshHooks()
{
    auto &l = *m_l;
    const bool needFilter = l.linksOn || !l.triggers.isEmpty();
    if (needFilter && !l.filterOn) {
        for (QAbstractScrollArea *w : {static_cast<QAbstractScrollArea *>(m_vis), static_cast<QAbstractScrollArea *>(m_src)}) {
            w->installEventFilter(this);
            w->viewport()->installEventFilter(this);
            w->viewport()->setMouseTracking(true);
        }
        l.filterOn = true;
    } else if (!needFilter && l.filterOn) {
        for (QAbstractScrollArea *w : {static_cast<QAbstractScrollArea *>(m_vis), static_cast<QAbstractScrollArea *>(m_src)}) {
            w->removeEventFilter(this);
            w->viewport()->removeEventFilter(this);
        }
        l.filterOn = false;
    }
    if (l.linksOn && !l.cv) {
        l.cv = connect(m_vis->document(), &QTextDocument::contentsChange, this,
                       [this](int pos, int, int added) { linksDocChanged(m_vis->document(), pos, added); });
        l.cs = connect(m_src->document(), &QTextDocument::contentsChange, this,
                       [this](int pos, int, int added) { linksDocChanged(m_src->document(), pos, added); });
        fenceUpdate(m_src->document(), 0, m_src->document()->characterCount());
        m_vis->overlayPaint = [this](QPainter &p) { paintLinks(p); };
        m_src->overlayPaint = [this](QPainter &p) { paintLinks(p); };
    } else if (!l.linksOn && l.cv) {
        disconnect(l.cv);
        disconnect(l.cs);
        l.cv = l.cs = {};
        m_vis->overlayPaint = nullptr;
        m_src->overlayPaint = nullptr;
        for (QTextDocument *d : {m_vis->document(), m_src->document()})
            for (QTextBlock b = d->begin(); b.isValid(); b = b.next())
                if (dynamic_cast<BlockCache *>(b.userData())) b.setUserData(nullptr);
    }
    m_vis->viewport()->update();
    m_src->viewport()->update();
}

#define ENSURE_L() do { if (!m_l) m_l = new LinksImpl; } while (0)

void NoteEditor::setWikiLinksEnabled(bool on)
{
    if (!m_l && !on) return;
    ENSURE_L();
    if (m_l->linksOn == on) return;
    m_l->linksOn = on;
    linksRefreshHooks();
}
bool NoteEditor::wikiLinksEnabled() const { return m_l && m_l->linksOn; }
void NoteEditor::setLinkResolver(std::function<LinkState(const QString &)> r)
{
    if (!m_l && !r) return;
    ENSURE_L();
    m_l->resolver = std::move(r);
    invalidateLinkStates();
}
void NoteEditor::invalidateLinkStates()
{
    if (!m_l) return;
    m_l->states.clear();
    m_vis->viewport()->update();
    m_src->viewport()->update();
}
void NoteEditor::setLinkClickActivates(bool on)
{
    if (!m_l && !on) return;
    ENSURE_L();
    m_l->clickActivates = on;
}
bool NoteEditor::linkClickActivates() const { return m_l && m_l->clickActivates; }

void NoteEditor::setCompletionTriggers(const QStringList &triggers)
{
    if (!m_l && triggers.isEmpty()) return;
    ENSURE_L();
    m_l->triggers = triggers;
    if (triggers.isEmpty()) dismissCompletions();
    linksRefreshHooks();
}

void NoteEditor::linksDocChanged(QTextDocument *d, int pos, int added)
{
    // Edits only invalidate the blocks they touched; other blocks keep their cached ranges.
    if (d == m_src->document()) fenceUpdate(d, pos, added);
    QTextBlock b = d->findBlock(pos);
    const int end = pos + added;
    for (; b.isValid() && b.position() <= end; b = b.next())
        if (b.userData()) b.setUserData(nullptr);
}

// ---- scanning (visible blocks only, cached per block) ---------------------------------------------------------------------

static const QList<LinkRange> &blockRanges(QTextBlock b, bool vis)
{
    static const QList<LinkRange> none;
    const QString text = b.text();
    if (!text.contains(QLatin1String("[["))) return none;   // no allocation for ordinary blocks
    int fence = 0;
    if (!vis) fence = b.previous().isValid() ? qMax(0, b.previous().userState()) : 0;
    if (auto *c = dynamic_cast<BlockCache *>(b.userData()); c && c->len == b.length() && c->fence == fence && c->vis == vis)
        return c->ranges;
    auto *c = new BlockCache;
    c->len = b.length();
    c->fence = fence;
    c->vis = vis;
    if (vis) {
        if (!b.blockFormat().hasProperty(QTextFormat::BlockCodeFence)) {
            QList<QPair<int, int>> skip;
            for (auto it = b.begin(); !it.atEnd(); ++it) {
                const QTextFragment f = it.fragment();
                if (f.charFormat().fontFixedPitch() || f.charFormat().isAnchor()) skip.append({f.position() - b.position(), f.position() - b.position() + f.length()});
            }
            c->ranges = scanWikiLinks(text, skip, false, b.blockNumber() + 1);
        }
    } else if (!fence && !isFenceLine(text)) {
        c->ranges = scanWikiLinks(text, {}, true, b.blockNumber() + 1);
    }
    b.setUserData(c);   // takes ownership, frees the previous cache
    return c->ranges;
}

// Viewport geometry covered by [a,b) of block `blk`, one entry per wrapped line: the line box, and the glyph box
// (baseline - ascent .. baseline + descent) the overlay paints on.
struct LineRect { QRect line; int base = 0, asc = 0, desc = 0; };
static QList<LineRect> rangeRects(const QTextBlock &blk, int a, int e, QPointF origin)
{
    QList<LineRect> out;
    const QTextLayout *lay = blk.layout();
    if (!lay) return out;
    for (int i = 0; i < lay->lineCount(); ++i) {
        const QTextLine ln = lay->lineAt(i);
        const int ls = ln.textStart(), le = ls + ln.textLength();
        const int s = qMax(a, ls), t = qMin(e, le);
        if (s >= t) continue;
        const qreal x1 = ln.cursorToX(s), x2 = ln.cursorToX(t - 1, QTextLine::Trailing);
        const QPointF o = origin + QPointF(0, ln.y());
        LineRect lr;
        lr.line = QRectF(o.x() + qMin(x1, x2), o.y(), qAbs(x2 - x1), ln.height()).toAlignedRect();
        lr.asc = qRound(ln.ascent());
        lr.desc = qRound(ln.descent());
        lr.base = qRound(o.y() + ln.ascent());
        out.append(lr);
    }
    return out;
}

static QPointF blockOrigin(const QTextBlock &b, QTextEdit *v, SourceEdit *s, bool vis)
{
    if (vis) return b.layout()->position() - QPointF(v->horizontalScrollBar()->value(), v->verticalScrollBar()->value());
    return s->blockOrigin(b);
}


LinkState NoteEditor::stateOf(const QString &target) const
{
    if (!m_l->resolver) return LinkState::Resolved;
    auto it = m_l->states.constFind(target);
    if (it != m_l->states.cend()) return *it;
    const LinkState s = m_l->resolver(target);
    m_l->states.insert(target, s);
    return s;
}

QList<LinkRange> NoteEditor::linkRangesInBlock(int n) const
{
    if (!m_l || !m_l->linksOn) return {};
    const bool vis = m_mode == Mode::Visual;
    const QTextBlock b = (vis ? m_vis->document() : m_src->document())->findBlockByNumber(n);
    if (!b.isValid()) return {};
    QList<LinkRange> r = blockRanges(b, vis);
    for (auto &x : r) x.state = stateOf(x.ref.target);
    return r;
}

std::optional<LinkRefInfo> NoteEditor::linkAt(const QPoint &p) const
{
    if (!m_l || !m_l->linksOn) return std::nullopt;
    const bool vis = m_mode == Mode::Visual;
    const QTextBlock b = vis ? m_vis->cursorForPosition(p).block() : m_src->cursorForPosition(p).block();
    if (!b.isValid()) return std::nullopt;
    const auto &rs = blockRanges(b, vis);
    if (rs.isEmpty()) return std::nullopt;
    const QPointF o = blockOrigin(b, m_vis, m_src, vis);
    for (const auto &r : rs)
        for (const LineRect &q : rangeRects(b, r.start, r.end, o))
            if (q.line.contains(p)) return r.ref;
    return std::nullopt;
}

// ---- painting --------------------------------------------------------------------------------------------------------

void NoteEditor::paintLinks(QPainter &p)
{
    if (!m_l || !m_l->linksOn) return;
    const bool vis = m_mode == Mode::Visual;
    QAbstractScrollArea *area = vis ? static_cast<QAbstractScrollArea *>(m_vis) : m_src;
    const auto t = effective(m_theme, palette());
    const int vh = area->viewport()->height();
    QTextBlock b = m_src->firstVisible();
    if (vis) {   // binary search on block geometry: QTextEdit::cursorForPosition() hit-tests linearly in the block count
        QTextDocument *doc = m_vis->document();
        auto *lay = doc->documentLayout();
        const int dy = m_vis->verticalScrollBar()->value();
        int lo = 0, hi = doc->blockCount() - 1;
        while (lo < hi) {
            const int mid = (lo + hi) / 2;
            if (lay->blockBoundingRect(doc->findBlockByNumber(mid)).bottom() < dy) lo = mid + 1; else hi = mid;
        }
        b = doc->findBlockByNumber(lo);
    }
    p.save();
    p.setRenderHint(QPainter::Antialiasing, false);
    QColor tint = t.accent;
    tint.setAlpha(t.dark ? 46 : 30);
    QColor warn = t.danger;
    warn = QColor::fromRgbF(warn.redF() * 0.5 + t.muted.redF() * 0.5, warn.greenF() * 0.5 + t.muted.greenF() * 0.5,
                            warn.blueF() * 0.5 + t.muted.blueF() * 0.5);
    for (; b.isValid(); b = b.next()) {
        if (!b.isVisible()) continue;
        const QPointF o = blockOrigin(b, m_vis, m_src, vis);
        if (o.y() > vh) break;
        const auto &rs = blockRanges(b, vis);
        if (rs.isEmpty()) continue;
        for (const auto &r : rs) {
            const LinkState st = stateOf(r.ref.target);
            for (const LineRect &q : rangeRects(b, r.start, r.end, o)) {
                if (q.line.bottom() < 0 || q.line.top() > vh) continue;
                const int x = q.line.left(), w = q.line.width(), uy = q.base + qMax(2, q.desc / 2 + 1);
                if (st == LinkState::Resolved) {
                    p.fillRect(QRect(x, q.base - q.asc, w, q.asc + q.desc), tint);
                    p.fillRect(QRect(x, uy, w, 1), t.accent);
                } else if (st == LinkState::Unresolved) {
                    QPen pen(warn, 1, Qt::CustomDashLine);
                    pen.setDashPattern({3, 2});
                    pen.setCapStyle(Qt::FlatCap);
                    p.setPen(pen);
                    p.drawLine(x, uy, x + w - 1, uy);
                } else {   // ambiguous: double underline
                    p.fillRect(QRect(x, uy - 1, w, 1), t.accent);
                    p.fillRect(QRect(x, uy + 2, w, 1), t.accent);
                }
            }
        }
    }
    p.restore();
}

// ---- activation + events ----------------------------------------------------------------------------------------------------

bool NoteEditor::activateAtCaret()
{
    if (!m_l || !m_l->linksOn) return false;
    const bool vis = m_mode == Mode::Visual;
    const QTextCursor c = vis ? m_vis->textCursor() : m_src->textCursor();
    const int pos = c.positionInBlock();
    for (const auto &r : blockRanges(c.block(), vis))
        if (pos >= r.start && pos <= r.end) {
            emit wikiLinkActivated(r.ref);
            return true;
        }
    return false;
}

static bool plainNav(const QKeyEvent *k) { return !(k->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)); }

bool NoteEditor::eventFilter(QObject *o, QEvent *e)
{
    if (!m_l) return QWidget::eventFilter(o, e);
    auto &l = *m_l;
    QWidget *edit = activeEdit();
    QWidget *vp = m_mode == Mode::Visual ? m_vis->viewport() : m_src->viewport();
    switch (e->type()) {
    case QEvent::KeyPress:
        if (o != edit) break;
        {
            auto *k = static_cast<QKeyEvent *>(e);
            if (l.popup && l.popup->isVisible() && l.popup->count() && plainNav(k)) {
                switch (k->key()) {
                case Qt::Key_Up: l.popup->step(-1); return true;
                case Qt::Key_Down: l.popup->step(+1); return true;
                case Qt::Key_PageUp: l.popup->page(-1); return true;
                case Qt::Key_PageDown: l.popup->page(+1); return true;
                case Qt::Key_Return: case Qt::Key_Enter: case Qt::Key_Tab: acceptCompletion(); return true;
                case Qt::Key_Escape: dismissCompletions(); return true;
                default: break;
                }
            } else if (l.active && k->key() == Qt::Key_Escape && plainNav(k)) {
                dismissCompletions();   // waiting for items: Esc still ends the session
                return true;
            }
            if (l.linksOn && (k->key() == Qt::Key_Return || k->key() == Qt::Key_Enter) && (k->modifiers() & Qt::ControlModifier)
                && activateAtCaret()) {
                return true;
            }
            if (l.linksOn && k->key() == Qt::Key_Control) {
                const QPoint p = vp->mapFromGlobal(QCursor::pos());
                const bool h = vp->rect().contains(p) && linkAt(p).has_value();
                if (h != l.hand) { l.hand = h; vp->setCursor(h ? Qt::PointingHandCursor : Qt::IBeamCursor); }
            }
        }
        break;
    case QEvent::KeyRelease:
        if (o == edit && l.linksOn && static_cast<QKeyEvent *>(e)->key() == Qt::Key_Control && l.hand && !l.clickActivates) {
            l.hand = false;
            vp->setCursor(Qt::IBeamCursor);
        }
        break;
    case QEvent::FocusOut:
        if (o == edit && l.active && (!l.popup || !l.popup->underMouse())) dismissCompletions();
        break;
    case QEvent::Wheel:
        if (o == vp && l.active) dismissCompletions();
        break;
    case QEvent::MouseButtonPress:
        if (o != vp) break;
        if (l.active) dismissCompletions();
        if (l.linksOn && static_cast<QMouseEvent *>(e)->button() == Qt::LeftButton)
            l.pressed = linkAt(static_cast<QMouseEvent *>(e)->position().toPoint());
        break;
    case QEvent::MouseButtonRelease:
        if (o != vp || !l.linksOn) break;
        {
            auto *m = static_cast<QMouseEvent *>(e);
            if (m->button() != Qt::LeftButton || !l.pressed) break;
            const auto pressed = *l.pressed;
            l.pressed.reset();
            const bool ctrl = m->modifiers() & Qt::ControlModifier;
            const QTextCursor c = m_mode == Mode::Visual ? m_vis->textCursor() : m_src->textCursor();
            if ((ctrl || (l.clickActivates && !c.hasSelection())) && linkAt(m->position().toPoint()) == pressed)
                emit wikiLinkActivated(pressed);   // the press already placed the caret; never consumed
        }
        break;
    case QEvent::MouseMove:
        if (o != vp || !l.linksOn) break;
        {
            auto *m = static_cast<QMouseEvent *>(e);
            const bool want = (l.clickActivates || (m->modifiers() & Qt::ControlModifier)) && !(m->buttons() & Qt::LeftButton)
                              && linkAt(m->position().toPoint()).has_value();
            if (want != l.hand) { l.hand = want; vp->setCursor(want ? Qt::PointingHandCursor : Qt::IBeamCursor); }
        }
        break;
    case QEvent::Leave:
        if (o == vp && l.hand) { l.hand = false; vp->setCursor(Qt::IBeamCursor); }
        break;
    default: break;
    }
    return QWidget::eventFilter(o, e);
}

// ---- completion ---------------------------------------------------------------------------------------------------------------

namespace {
struct TriggerMatch {
    int start = -1;       // absolute document position of the trigger
    QString id, query;
    bool atLineStart = false;
};
} // namespace

static std::optional<TriggerMatch> matchTrigger(const QStringList &triggers, const QTextCursor &c)
{
    const QString blockText = c.block().text();
    const int caret = c.positionInBlock();
    int ls = qMax(blockText.lastIndexOf(QChar(0x2028), caret - 1), blockText.lastIndexOf(QLatin1Char('\n'), caret - 1)) + 1;
    if (caret == 0) ls = 0;
    const QString pre = blockText.mid(ls, caret - ls);
    std::optional<TriggerMatch> best;
    int bestIdx = -1;
    for (const QString &t : triggers) {
        if (t.isEmpty()) continue;
        int idx = -1;
        QString query;
        if (t.startsWith(QLatin1Char('['))) {
            idx = pre.lastIndexOf(t);
            if (idx < 0) continue;
            if (idx > 0 && pre[idx - 1] == u'\\') continue;
            query = pre.mid(idx + t.size());
            QString close = t;
            close.replace(QLatin1Char('['), QLatin1Char(']'));
            if (query.contains(close) || query.contains(t) || query.size() > 256) continue;
        } else {
            int s = pre.size();
            while (s > 0 && !pre[s - 1].isSpace()) --s;
            if (!pre.mid(s).startsWith(t)) continue;
            // the token must end at the caret: a following non-space char means the caret is mid-word
            if (caret < blockText.size() && !blockText[caret].isSpace()) continue;
            idx = s;
            query = pre.mid(s + t.size());
            if (query.size() > 128) continue;
        }
        if (idx > bestIdx || (idx == bestIdx && best && t.size() > best->id.size())) {
            bestIdx = idx;
            TriggerMatch m;
            m.start = c.block().position() + ls + idx;
            m.id = t;
            m.query = query;
            m.atLineStart = pre.left(idx).trimmed().isEmpty();
            best = m;
        }
    }
    return best;
}

void NoteEditor::evaluateCompletion(bool fromTyping)
{
    auto &l = *m_l;
    if (l.triggers.isEmpty() || m_invalid) return;
    const bool vis = m_mode == Mode::Visual;
    if (vis ? m_vis->imePreedit() : m_src->imePreedit()) return;
    const QTextCursor c = vis ? m_vis->textCursor() : m_src->textCursor();
    std::optional<TriggerMatch> m;
    if (!c.hasSelection()) m = matchTrigger(l.triggers, c);   // cheap string rules first, code check only on a hit
    if (m && caretInCode()) m.reset();
    if (!m) {
        if (l.active) dismissCompletions();
        return;
    }
    if (!fromTyping && !l.active) return;   // sessions start from typing only
    if (l.active && l.start == m->start && l.id == m->id && l.query == m->query) return;
    l.active = true;
    l.start = m->start;
    l.id = m->id;
    l.query = m->query;
    l.atLineStart = m->atLineStart;
    CompletionRequest r;
    r.triggerId = m->id;
    r.query = m->query;
    r.atLineStart = m->atLineStart;
    r.generation = ++l.gen;
    QWidget *vp = vis ? m_vis->viewport() : m_src->viewport();
    r.caretRect = QRect(vp->mapToGlobal((vis ? m_vis->cursorRect() : m_src->cursorRect()).topLeft()),
                        (vis ? m_vis->cursorRect() : m_src->cursorRect()).size());
    emit completionRequested(r);
}

void NoteEditor::linksTyped() { evaluateCompletion(true); }
void NoteEditor::linksCaretMoved() { if (m_l->active) evaluateCompletion(false); }

bool NoteEditor::completionActive() const { return m_l && m_l->active; }
QWidget *NoteEditor::completionPopup() const { return m_l ? m_l->popup : nullptr; }
QRect NoteEditor::placeCompletionPopup(const QRect &c, const QSize &s, const QRect &a) { return CompletionPopup::place(c, s, a); }

void NoteEditor::restyleCompletionPopup()
{
    if (!m_l || !m_l->active || !m_l->popup || !m_l->popup->isVisible()) return;
    const bool vis = m_mode == Mode::Visual;
    m_l->popup->setTheme(effective(m_theme, palette()), activeEdit()->font());
    const QRect cr = vis ? m_vis->cursorRect() : m_src->cursorRect();
    QWidget *vp = vis ? m_vis->viewport() : m_src->viewport();
    m_l->popup->showAt(QRect(vp->mapToGlobal(cr.topLeft()), cr.size()));
}

void NoteEditor::showCompletions(const QList<CompletionItem> &items, int generation)
{
    if (!m_l || !m_l->active) return;
    auto &l = *m_l;
    if (generation >= 0 && generation != l.gen) return;   // stale reply
    if (items.isEmpty()) {
        if (l.popup) l.popup->hide();
        return;
    }
    const bool vis = m_mode == Mode::Visual;
    if (!l.popup) {
        l.popup = new CompletionPopup(this);
        l.popup->picked = [this](int row) { acceptCompletion(row); };
    }
    QFont f = (vis ? static_cast<QWidget *>(m_vis) : m_src)->font();
    l.popup->setTheme(effective(m_theme, palette()), f);
    l.popup->setItems(items, l.query);
    const QRect cr = vis ? m_vis->cursorRect() : m_src->cursorRect();
    QWidget *vp = vis ? m_vis->viewport() : m_src->viewport();
    l.popup->showAt(QRect(vp->mapToGlobal(cr.topLeft()), cr.size()));
}

void NoteEditor::dismissCompletions()
{
    if (!m_l) return;
    const bool was = m_l->active;
    m_l->active = false;
    if (m_l->popup) m_l->popup->hide();
    if (was) emit completionDismissed();
}

bool NoteEditor::acceptCompletion(int row)
{
    if (!m_l || !m_l->active || !m_l->popup || !m_l->popup->isVisible()) return false;
    auto &l = *m_l;
    if (row < 0) row = l.popup->current();
    if (row < 0 || row >= l.popup->count()) return false;
    const CompletionItem it = l.popup->itemAt(row);
    const bool vis = m_mode == Mode::Visual;
    QTextCursor c = vis ? m_vis->textCursor() : m_src->textCursor();
    const int start = l.start, end = c.position();
    if (start > end || c.hasSelection() || c.document()->findBlock(start) != c.block()) { dismissCompletions(); return false; }
    dismissCompletions();   // before editing: the edit's own caret moves must not re-enter the session
    {
        auto scope = m_rec.begin(TxKind::Format, it.insert.contains(QLatin1Char('\n')) ? WindowMode::Wide : WindowMode::Selection);
        QTextCursor e(c.document());
        e.setPosition(start);
        e.setPosition(end, QTextCursor::KeepAnchor);
        e.insertText(it.insert);
        e.setPosition(start + (it.cursorOffset < 0 ? it.insert.size() : qMin(it.cursorOffset, int(it.insert.size()))));
        if (vis) m_vis->setTextCursor(e); else m_src->setTextCursor(e);
    }
    emit completionAccepted(it);
    return true;
}

} // namespace hn::editor
