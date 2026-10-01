#include "visual_edit.h"

#include "md_io.h"

#include <QApplication>
#include <QClipboard>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMimeData>
#include <QAbstractTextDocumentLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QRegularExpression>
#include <QSet>
#include <QTextBlock>
#include <QTextDocumentFragment>
#include <QTextList>
#include <algorithm>

namespace hn::editor {

using MT = QTextBlockFormat::MarkerType;

namespace {

bool isCode(const QTextBlock &b) { return b.blockFormat().hasProperty(QTextFormat::BlockCodeFence); }
bool isQuote(const QTextBlock &b) { return b.blockFormat().hasProperty(QTextFormat::BlockQuoteLevel); }
bool isCheck(const QTextBlock &b) { return b.textList() && b.blockFormat().marker() != MT::NoMarker; }
bool isRule(const QTextBlock &b) { return b.blockFormat().boolProperty(kHrProp); }
int contDepth(const QTextBlockFormat &bf) { return bf.intProperty(kContProp); }
int quoteLevelOf(const QTextBlockFormat &bf) { return bf.hasProperty(QTextFormat::BlockQuoteLevel) ? qMax(1, bf.intProperty(QTextFormat::BlockQuoteLevel)) : 0; }
// Left margin implied by the block's container context (list-item depth + quote depth).
void fixMargin(QTextBlockFormat &bf, bool inList)
{
    const int m = (inList ? 0 : kIndentPx * contDepth(bf)) + 16 * quoteLevelOf(bf);
    if (m) bf.setLeftMargin(m); else bf.clearProperty(QTextFormat::BlockLeftMargin);
}
int listIndent(const QTextBlock &b) { return b.textList() ? b.textList()->format().indent() : 0; }
bool orderedStyle(QTextListFormat::Style s)
{
    return s == QTextListFormat::ListDecimal || s == QTextListFormat::ListLowerAlpha || s == QTextListFormat::ListUpperAlpha
        || s == QTextListFormat::ListLowerRoman || s == QTextListFormat::ListUpperRoman;
}

BlockStyle blockStyleOf(const QTextBlock &b)
{
    const int h = b.blockFormat().headingLevel();
    if (h >= 1 && h <= 6) return BlockStyle(int(BlockStyle::H1) + h - 1);
    if (isCode(b)) return BlockStyle::Code;
    if (isQuote(b)) return BlockStyle::Quote;
    return BlockStyle::Paragraph;
}

ListKind listKindOf(const QTextBlock &b)
{
    if (!b.textList()) return ListKind::None;
    if (b.blockFormat().marker() != MT::NoMarker) return ListKind::Check;
    return orderedStyle(b.textList()->format().style()) ? ListKind::Ordered : ListKind::Bullet;
}

bool hasInline(const QTextCharFormat &f, InlineStyle k)
{
    switch (k) {
    case InlineStyle::Bold: return f.fontWeight() >= QFont::Bold;
    case InlineStyle::Italic: return f.fontItalic();
    case InlineStyle::Strike: return f.fontStrikeOut();
    case InlineStyle::Code: return f.fontFixedPitch();
    }
    return false;
}

void applyInline(QTextCharFormat &f, InlineStyle k, bool on, const QString &mono)
{
    switch (k) {
    case InlineStyle::Bold: f.setFontWeight(on ? QFont::Bold : QFont::Normal); break;
    case InlineStyle::Italic: f.setFontItalic(on); break;
    case InlineStyle::Strike: f.setFontStrikeOut(on); break;
    case InlineStyle::Code:
        if (on) { f.setFontFixedPitch(true); f.setFontFamilies({mono}); f.setFontWeight(QFont::Normal); }
        else { f.setFontFixedPitch(false); f.clearProperty(QTextFormat::FontFamilies); f.clearProperty(QTextFormat::FontFamily); }
        break;
    }
}

void modifyRange(QTextDocument *doc, int from, int to, const std::function<void(QTextCharFormat &)> &fn)
{
    struct R { int a, b; QTextCharFormat f; };
    QList<R> rs;
    for (QTextBlock b = doc->findBlock(from); b.isValid() && b.position() < to; b = b.next())
        for (auto it = b.begin(); !it.atEnd(); ++it) {
            const QTextFragment fr = it.fragment();
            const int a = qMax(from, fr.position()), e = qMin(to, fr.position() + fr.length());
            if (a < e) rs.append({a, e, fr.charFormat()});
        }
    for (auto &r : rs) {
        QTextCursor c(doc);
        c.setPosition(r.a);
        c.setPosition(r.b, QTextCursor::KeepAnchor);
        fn(r.f);
        c.setCharFormat(r.f);
    }
}

bool allHave(QTextDocument *doc, int from, int to, InlineStyle k)
{
    bool any = false;
    for (QTextBlock b = doc->findBlock(from); b.isValid() && b.position() < to; b = b.next())
        for (auto it = b.begin(); !it.atEnd(); ++it) {
            const QTextFragment fr = it.fragment();
            if (qMax(from, fr.position()) < qMin(to, fr.position() + fr.length())) {
                any = true;
                if (!hasInline(fr.charFormat(), k)) return false;
            }
        }
    return any;
}

bool isPrintableText(const QString &t) { return !t.isEmpty() && t.at(0).unicode() >= 0x20 && t.at(0).unicode() != 0x7f; }

// Finds `open` such that t[open..open+dlen) .. content .. t[end..) is a complete span ending at t's end.
bool findSpan(const QString &t, QChar d, int dlen, int &open)
{
    const int n = t.size();
    if (n < 2 * dlen + 1) return false;
    for (int i = 0; i < dlen; ++i) if (t[n - 1 - i] != d) return false;
    if (n > dlen && t[n - dlen - 1] == d) return false;   // longer delimiter run still being typed
    const int end = n - dlen;
    for (int i = end - dlen - 1; i >= 0; --i) {
        bool run = true;
        for (int k = 0; k < dlen; ++k) if (t[i + k] != d) { run = false; break; }
        if (!run) continue;
        if ((i > 0 && t[i - 1] == d) || t[i + dlen] == d) continue;
        if (i > 0 && t[i - 1] == QLatin1Char('\\')) continue;
        if (d == QLatin1Char('_') && i > 0 && t[i - 1].isLetterOrNumber()) continue;
        const QString content = t.mid(i + dlen, end - i - dlen);
        if (content.isEmpty() || content.front().isSpace() || content.back().isSpace()) continue;
        if (dlen == 1 && content.contains(d)) continue;
        open = i;
        return true;
    }
    return false;
}

} // namespace

// ---------------------------------------------------------------------------------------------

VisualEdit::VisualEdit(EditRecorder *rec, QWidget *parent) : QTextEdit(parent), m_rec(rec)
{
    setUndoRedoEnabled(false);
    document()->setUndoRedoEnabled(false);
    document()->setIndentWidth(kIndentPx);
    setAcceptRichText(false);
    setTabChangesFocus(true);
    setFrameShape(QFrame::NoFrame);
    setContextMenuPolicy(Qt::CustomContextMenu);
    setAutoFormatting(QTextEdit::AutoNone);
}

void VisualEdit::setStyleContext(const QString &body, const QString &mono, qreal lineHeight, const QColor &link)
{
    m_body = body;
    m_mono = mono;
    m_lh = lineHeight;
    m_link = link;
}

bool VisualEdit::event(QEvent *ev)
{
    if (ev->type() == QEvent::KeyPress) {
        auto *k = static_cast<QKeyEvent *>(ev);
        if ((k->key() == Qt::Key_Tab || k->key() == Qt::Key_Backtab)
            && !(k->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) && selectionTouchesList()) {
            indentList((k->key() == Qt::Key_Backtab || (k->modifiers() & Qt::ShiftModifier)) ? -1 : +1);
            ev->accept();
            return true;
        }
    }
    return QTextEdit::event(ev);
}

QList<QTextBlock> VisualEdit::selectedBlocks() const
{
    const QTextCursor c = textCursor();
    QTextBlock b1 = document()->findBlock(c.selectionStart()), b2 = document()->findBlock(c.selectionEnd());
    if (c.hasSelection() && b2 != b1 && c.selectionEnd() == b2.position()) b2 = b2.previous();
    QList<QTextBlock> out;
    for (QTextBlock b = b1; b.isValid() && b.blockNumber() <= b2.blockNumber(); b = b.next()) out.append(b);
    return out;
}

bool VisualEdit::selectionTouchesList() const
{
    for (const auto &b : selectedBlocks()) if (b.textList()) return true;
    return false;
}

// ---- inline / block / list commands -----------------------------------------------------------

bool VisualEdit::inlineActive(InlineStyle s) const
{
    const QTextCursor c = textCursor();
    if (c.hasSelection()) return allHave(document(), c.selectionStart(), c.selectionEnd(), s);
    return hasInline(currentCharFormat(), s);
}

void VisualEdit::toggleInline(InlineStyle s)
{
    const QTextCursor c = textCursor();
    if (isCode(c.block()) && s != InlineStyle::Code) return;
    if (!c.hasSelection()) {
        QTextCharFormat f = currentCharFormat();
        applyInline(f, s, !hasInline(f, s), m_mono);
        m_rec->history().breakMerge();
        auto sc = m_rec->begin(TxKind::Format, WindowMode::Selection);
        setCurrentCharFormat(f);
        return;
    }
    const int a = c.selectionStart(), b = c.selectionEnd();
    const bool all = allHave(document(), a, b, s);
    auto sc = m_rec->begin(TxKind::Format, WindowMode::Selection);
    modifyRange(document(), a, b, [&](QTextCharFormat &f) { applyInline(f, s, !all, m_mono); });
}

BlockStyle VisualEdit::blockStyle() const { return blockStyleOf(textCursor().block()); }
ListKind VisualEdit::listKind() const { return listKindOf(textCursor().block()); }

void VisualEdit::styleBlock(const QTextBlock &blk, BlockStyle s)
{
    const BlockStyle old = blockStyleOf(blk);
    QTextBlock b = blk;
    if (s != BlockStyle::Paragraph && b.textList()) {   // headings, quotes and code are top-level blocks: leave the list
        b.textList()->remove(b);
        QTextBlockFormat lf = b.blockFormat();
        lf.setMarker(MT::NoMarker);
        lf.setIndent(0);
        QTextCursor(b).setBlockFormat(lf);
    }
    const int from = b.position(), to = from + b.length() - 1;
    QTextBlockFormat bf = b.blockFormat();
    QTextCharFormat cf = b.charFormat();
    bf.setHeadingLevel(0);
    for (auto p : {QTextFormat::BlockQuoteLevel, QTextFormat::BlockCodeFence, QTextFormat::BlockCodeLanguage,
                   QTextFormat::BlockNonBreakableLines, QTextFormat::BlockLeftMargin})
        bf.clearProperty(p);
    if (isRule(b)) return;
    bf.setTopMargin(8);
    bf.setBottomMargin(8);
    const bool wasHeading = old >= BlockStyle::H1 && old <= BlockStyle::H6;
    const int level = (s >= BlockStyle::H1 && s <= BlockStyle::H6) ? int(s) - int(BlockStyle::H1) + 1 : 0;
    if (wasHeading || level) {
        auto clearH = [&](QTextCharFormat &f) { if (wasHeading) { f.setFontWeight(QFont::Normal); f.clearProperty(QTextFormat::FontSizeAdjustment); } };
        clearH(cf);
        modifyRange(document(), from, to, [&](QTextCharFormat &f) { clearH(f); });
    }
    if (old == BlockStyle::Code && s != BlockStyle::Code) {
        applyInline(cf, InlineStyle::Code, false, m_mono);
        modifyRange(document(), from, to, [&](QTextCharFormat &f) { applyInline(f, InlineStyle::Code, false, m_mono); });
    }
    if (level) {
        bf.setHeadingLevel(level);
        const int adj = qMax(0, 4 - level);
        auto setH = [&](QTextCharFormat &f) { f.setFontWeight(QFont::Bold); f.setProperty(QTextFormat::FontSizeAdjustment, adj); };
        setH(cf);
        modifyRange(document(), from, to, setH);
    } else if (s == BlockStyle::Quote) {
        bf.setProperty(QTextFormat::BlockQuoteLevel, 1);
    } else if (s == BlockStyle::Code) {
        bf.setProperty(QTextFormat::BlockCodeFence, QStringLiteral("`"));
        bf.setNonBreakableLines(true);
        bf.setTopMargin(0);
        bf.setBottomMargin(0);
        bf.setMarker(MT::NoMarker);
        applyInline(cf, InlineStyle::Code, true, m_mono);
        modifyRange(document(), from, to, [&](QTextCharFormat &f) { applyInline(f, InlineStyle::Code, true, m_mono); });
    }
    fixMargin(bf, b.textList() != nullptr);
    QTextCursor bc(b);
    bc.setBlockFormat(bf);
    bc.setBlockCharFormat(cf);
}

void VisualEdit::setBlockStyle(BlockStyle s)
{
    auto blocks = selectedBlocks();
    auto sc = m_rec->begin(TxKind::Format, WindowMode::Selection);
    for (const auto &b : blocks) if (!isRule(b)) styleBlock(b, s);
}

void VisualEdit::applyList(QList<QTextBlock> blocks, ListKind kind, int start)
{
    if (blocks.isEmpty()) return;
    if (kind == ListKind::None) {
        for (const auto &b : blocks) {
            if (auto *l = b.textList()) l->remove(b);
            QTextBlockFormat bf = b.blockFormat();
            bf.setMarker(MT::NoMarker);
            bf.setIndent(0);
            QTextCursor(b).setBlockFormat(bf);
        }
        return;
    }
    const auto st = kind == ListKind::Ordered ? QTextListFormat::ListDecimal : QTextListFormat::ListDisc;
    QTextList *cur = nullptr;
    const QTextBlock prev = blocks.first().previous();
    for (const auto &b : blocks) {
        if (isCode(b) || isRule(b)) continue;
        const int indent = b.textList() ? listIndent(b) : 1;
        const MT oldMarker = b.blockFormat().marker();
        if (auto *l = b.textList()) l->remove(b);
        QTextListFormat lf;
        lf.setStyle(st);
        lf.setIndent(indent);
        if (kind == ListKind::Ordered && start > 1) lf.setStart(start);
        if (!cur && prev.isValid() && prev.textList() && listIndent(prev) == indent
            && orderedStyle(prev.textList()->format().style()) == (kind == ListKind::Ordered) && (listKindOf(prev) == ListKind::Check) == (kind == ListKind::Check))
            cur = prev.textList();
        if (cur && cur->format().indent() == indent) cur->add(b);
        else cur = QTextCursor(b).createList(lf);
        QTextBlockFormat bf = b.blockFormat();
        if (bf.hasProperty(kContProp)) { bf.clearProperty(kContProp); fixMargin(bf, true); }   // becomes a list item itself
        bf.setMarker(kind == ListKind::Check ? (oldMarker == MT::NoMarker ? MT::Unchecked : oldMarker) : MT::NoMarker);
        QTextCursor(b).setBlockFormat(bf);
    }
}

void VisualEdit::toggleList(ListKind k)
{
    auto blocks = selectedBlocks();
    blocks.erase(std::remove_if(blocks.begin(), blocks.end(), [](const QTextBlock &b) { return isCode(b) || isRule(b); }), blocks.end());
    if (blocks.isEmpty()) return;
    bool all = true;
    for (const auto &b : blocks) all = all && listKindOf(b) == k;
    auto sc = m_rec->begin(TxKind::Format, WindowMode::Selection);
    applyList(blocks, all ? ListKind::None : k);
}

bool VisualEdit::toggleCheck(const QTextBlock &b)
{
    if (!isCheck(b)) return false;
    auto sc = m_rec->beginBlocks(TxKind::Format, b.blockNumber(), b.blockNumber());
    QTextBlockFormat bf = b.blockFormat();
    bf.setMarker(bf.marker() == MT::Checked ? MT::Unchecked : MT::Checked);
    QTextCursor(b).setBlockFormat(bf);   // textCursor() untouched: caret preserved
    return true;
}

bool VisualEdit::toggleCheckAtCursor() { return toggleCheck(textCursor().block()); }

bool VisualEdit::indentList(int dir)
{
    QList<QTextBlock> todo;
    QSet<int> seen;
    for (const auto &b : selectedBlocks()) {
        if (!b.textList()) continue;
        const int ind = listIndent(b);
        if (!seen.contains(b.blockNumber())) { seen.insert(b.blockNumber()); todo.append(b); }
        for (QTextBlock n = b.next(); n.isValid() && n.textList() && listIndent(n) > ind; n = n.next())
            if (!seen.contains(n.blockNumber())) { seen.insert(n.blockNumber()); todo.append(n); }
    }
    if (todo.isEmpty()) return false;
    std::sort(todo.begin(), todo.end(), [](const QTextBlock &a, const QTextBlock &b) { return a.blockNumber() < b.blockNumber(); });
    const QTextBlock first = todo.first();
    if (dir > 0) {
        const QTextBlock p = first.previous();
        if (!p.isValid() || !p.textList() || listIndent(p) < listIndent(first)) return true;   // no previous sibling to nest under
    } else if (listIndent(first) <= 1) {
        return true;
    }
    auto sc = m_rec->beginBlocks(TxKind::Format, todo.first().blockNumber(), todo.last().blockNumber());
    for (const auto &b : todo) {
        QTextList *L = b.textList();
        if (!L) continue;
        QTextListFormat fmt = L->format();
        const int t = qMax(1, fmt.indent() + dir);
        fmt.setIndent(t);
        QTextList *target = nullptr;
        for (QTextBlock p = b.previous(); p.isValid() && p.textList(); p = p.previous()) {
            const int pi = listIndent(p);
            if (pi == t && orderedStyle(p.textList()->format().style()) == orderedStyle(fmt.style())) { target = p.textList(); break; }
            if (pi < t) break;
        }
        L->remove(b);
        if (target) target->add(b);
        else QTextCursor(b).createList(fmt);
    }
    return true;
}

QString VisualEdit::linkAtCursor() const { return textCursor().charFormat().anchorHref(); }

void VisualEdit::setLink(const QString &url)
{
    QTextCursor c = textCursor();
    auto sc = m_rec->begin(TxKind::Format, WindowMode::Selection);
    if (!c.hasSelection()) {
        const int p = c.position();
        bool found = false;
        for (auto it = c.block().begin(); !it.atEnd() && !found; ++it) {
            const QTextFragment fr = it.fragment();
            if (fr.charFormat().isAnchor() && p >= fr.position() && p <= fr.position() + fr.length()) {
                c.setPosition(fr.position());
                c.setPosition(fr.position() + fr.length(), QTextCursor::KeepAnchor);
                found = true;
            }
        }
        if (!found) {
            if (url.isEmpty()) return;
            QTextCharFormat f = c.charFormat();
            f.setAnchor(true);
            f.setAnchorHref(url);
            f.setFontUnderline(true);
            if (m_link.isValid()) f.setForeground(m_link);
            c.insertText(url, f);
            setTextCursor(c);
            return;
        }
    }
    const int a = c.selectionStart(), b = c.selectionEnd();
    modifyRange(document(), a, b, [&](QTextCharFormat &f) {
        if (url.isEmpty()) {
            f.setAnchor(false);
            f.clearProperty(QTextFormat::AnchorHref);
            f.clearProperty(QTextFormat::AnchorName);
            f.clearProperty(QTextFormat::TextUnderlineStyle);
            f.clearForeground();
        } else {
            f.setAnchor(true);
            f.setAnchorHref(url);
            f.setFontUnderline(true);
            if (m_link.isValid()) f.setForeground(m_link);
        }
    });
}

// ---- recorded native routes ---------------------------------------------------------------------

void VisualEdit::cutRecorded()
{
    auto s = m_rec->begin(TxKind::Cut, WindowMode::Selection);
    cut();
    healRules();
}

void VisualEdit::insertFromMimeData(const QMimeData *src)
{
    if (!src->hasText()) return;
    const QString text = src->text();
    if (qint64(text.size()) * 2 > TxHistory::kMaxBytes / 2) {
        QString err;
        if (!m_rec->history().canSpill(&err)) {   // would not be recoverable: refuse before applying
            emit m_rec->recoveryWriteFailed(err);
            return;
        }
    }
    QMimeData plain;
    plain.setText(text);
    auto s = m_rec->begin(TxKind::Paste, WindowMode::Selection);
    QTextEdit::insertFromMimeData(&plain);
    healRules();
}

void VisualEdit::insertMarkdownFragment(const QTextDocument &parsed)
{
    auto s = m_rec->begin(TxKind::Paste, WindowMode::Selection);
    QTextCursor c = textCursor();
    const int b0 = c.selectionStart() >= 0 ? document()->findBlock(c.selectionStart()).blockNumber() : 0;
    c.insertFragment(QTextDocumentFragment(&parsed));
    setTextCursor(c);
    normalizeRange(b0, document()->findBlock(c.position()).blockNumber());
}

void VisualEdit::dropEvent(QDropEvent *e)
{
    auto s = m_rec->begin(TxKind::Drop, WindowMode::WholeDoc);
    QTextEdit::dropEvent(e);
    healRules();
}

void VisualEdit::inputMethodEvent(QInputMethodEvent *e)
{
    m_preedit = !e->preeditString().isEmpty();
    const bool edits = !e->commitString().isEmpty() || e->replacementLength() != 0;
    // Preedit-only events still touch the layout (markContentsDirty): scope them so they are not seen as stray edits.
    if (edits && textCursor().block().isValid() && isRule(textCursor().block()) && !textCursor().hasSelection()) {
        auto s = m_rec->begin(TxKind::Structure, WindowMode::Wide);   // text cannot live in a rule: type on a new line after it
        openLineAfterRule();
    }
    auto s = m_rec->begin(TxKind::Typing, edits ? WindowMode::Wide : WindowMode::Selection);
    QTextEdit::inputMethodEvent(e);
    if (edits) healRules();
}

// ---- horizontal rules -------------------------------------------------------------------------

void VisualEdit::openLineAfterRule()
{
    QTextCursor c = textCursor();
    QTextBlockFormat bf = c.blockFormat();
    bf.clearProperty(kHrProp);
    bf.setTopMargin(9);
    bf.setBottomMargin(9);
    c.movePosition(QTextCursor::EndOfBlock);
    c.insertBlock(bf, QTextCharFormat());
    QTextBlockFormat nb = c.blockFormat();
    if (m_lh > 0) nb.setLineHeight(m_lh * 100, QTextBlockFormat::ProportionalHeight);
    c.setBlockFormat(nb);
    setTextCursor(c);
}

// A rule holds no text: anything that ends up inside one (merges, pastes) turns it back into an ordinary paragraph.
void VisualEdit::healRules()
{
    const int at = textCursor().blockNumber();
    QTextBlock b = document()->findBlockByNumber(qMax(0, at - 1));
    for (int i = 0; i < 3 && b.isValid(); ++i, b = b.next()) {
        if (!isRule(b) || b.text().isEmpty()) continue;
        QTextBlockFormat bf = b.blockFormat();
        bf.clearProperty(kHrProp);
        bf.setTopMargin(9);
        bf.setBottomMargin(9);
        QTextCursor c(b);
        c.setBlockFormat(bf);
        c.setBlockCharFormat(QTextCharFormat());
    }
}

bool VisualEdit::guardRule(QKeyEvent *e)
{
    QTextCursor c = textCursor();
    if (c.hasSelection() || e->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) return false;
    const QTextBlock b = c.block();
    const bool onRule = isRule(b);
    const bool backAfterRule = e->key() == Qt::Key_Backspace && c.atBlockStart() && b.previous().isValid() && isRule(b.previous());
    const bool del = e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace;
    if ((onRule && del) || backAfterRule) {   // remove the rule block itself
        const QTextBlock r = onRule ? b : b.previous();
        auto s = m_rec->beginBlocks(TxKind::Delete, r.blockNumber() - 1, r.blockNumber() + 1);
        QTextCursor d(r);
        if (r.next().isValid()) { d.setPosition(r.position()); d.setPosition(r.next().position(), QTextCursor::KeepAnchor); }
        else if (r.previous().isValid()) { d.setPosition(r.previous().position() + r.previous().length() - 1); d.setPosition(r.position() + r.length() - 1, QTextCursor::KeepAnchor); }
        else { d.setBlockFormat(QTextBlockFormat()); d.setBlockCharFormat(QTextCharFormat()); return true; }
        const bool atEnd = !r.next().isValid();
        d.removeSelectedText();
        QTextCursor n(document()->findBlockByNumber(qMin(r.blockNumber(), document()->blockCount() - 1)));
        if (atEnd || e->key() == Qt::Key_Backspace) n.movePosition(onRule && !atEnd ? QTextCursor::StartOfBlock : QTextCursor::EndOfBlock);
        if (!onRule) n = c;   // caret stays where it was
        setTextCursor(n);
        return true;
    }
    if (onRule && (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter || isPrintableText(e->text()))) {
        {
            auto s = m_rec->beginBlocks(TxKind::Structure, b.blockNumber(), b.blockNumber() + 1);
            openLineAfterRule();
        }
        if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) return true;
    }
    return false;
}

// ---- keyboard -----------------------------------------------------------------------------------

void VisualEdit::keyPressEvent(QKeyEvent *e)
{
    if (e->matches(QKeySequence::Undo)) { emit undoRequested(); return; }
    if (e->matches(QKeySequence::Redo) || (e->key() == Qt::Key_Y && e->modifiers() == Qt::ControlModifier)) { emit redoRequested(); return; }
    if (e->matches(QKeySequence::Bold)) { toggleInline(InlineStyle::Bold); return; }
    if (e->matches(QKeySequence::Italic)) { toggleInline(InlineStyle::Italic); return; }
    if (e->key() == Qt::Key_K && e->modifiers() == Qt::ControlModifier) { emit linkRequested(); return; }
    if (guardRule(e)) return;
    if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
        if (e->modifiers() & Qt::ControlModifier) {
            if (!toggleCheckAtCursor()) {
                const QString href = linkAtCursor();
                if (!href.isEmpty()) emit linkActivated(QUrl(href));
            }
            return;
        }
        handleEnter(e->modifiers() & Qt::ShiftModifier);
        e->accept();
        return;
    }
    if ((e->key() == Qt::Key_Tab || e->key() == Qt::Key_Backtab) && !(e->modifiers() & (Qt::ControlModifier | Qt::AltModifier))) {
        e->ignore();   // outside lists Tab keeps focus navigation; never inserts a tab character
        return;
    }
    TxKind kind = TxKind::Structure;
    bool edits = true;
    if (e->matches(QKeySequence::Cut)) kind = TxKind::Cut;
    else if (e->matches(QKeySequence::Paste)) kind = TxKind::Paste;
    else if (e->key() == Qt::Key_Backspace || e->key() == Qt::Key_Delete) kind = TxKind::Delete;
    else if (isPrintableText(e->text()) && !(e->modifiers() & (Qt::ControlModifier | Qt::AltModifier))) kind = TxKind::Typing;
    else edits = false;
    if (!edits) { QTextEdit::keyPressEvent(e); return; }
    {
        auto s = m_rec->begin(kind, WindowMode::Selection);
        QTextEdit::keyPressEvent(e);
        healRules();
    }
    if (kind == TxKind::Typing && !m_preedit && e->text().size() == 1 && !textCursor().hasSelection()) {
        const QChar ch = e->text().at(0);
        if (ch == QLatin1Char(' ')) tryBlockShortcut();
        else if (QStringLiteral("*_~`").contains(ch)) tryInlineShortcut();
        if (afterTyping) afterTyping();
    }
}

void VisualEdit::handleEnter(bool shift)
{
    QTextCursor c = textCursor();
    QTextBlock b = c.block();
    if (!c.hasSelection() && !shift && (tryFenceShortcut() || tryRuleShortcut())) return;
    const bool code = isCode(b);
    if (!c.hasSelection() && !shift && b.textList() && b.text().isEmpty() && listIndent(b) > 1) {
        indentList(-1);   // empty nested item: leave one level
        return;
    }
    {
        auto s = m_rec->begin(TxKind::Structure, WindowMode::Selection);
        if (shift && !code) {
            c.insertText(QString(QChar(QChar::LineSeparator)));
            setTextCursor(c);
            ensureCursorVisible();
            return;
        }
        const bool hadSel = c.hasSelection();
        if (hadSel) c.removeSelectedText();
        b = c.block();
        const bool empty = b.text().isEmpty() && !hadSel;
        if (b.textList() && empty) {   // empty item exits the list
            b.textList()->remove(b);
            QTextBlockFormat bf = b.blockFormat();
            bf.setMarker(MT::NoMarker);
            bf.setIndent(0);
            QTextCursor(b).setBlockFormat(bf);
            setTextCursor(QTextCursor(b));
            return;
        }
        if (isQuote(b) && empty) { styleBlock(b, BlockStyle::Paragraph); return; }
        if (!b.textList() && !code && empty && contDepth(b.blockFormat()) > 0) {   // empty second paragraph of an item: leave the item
            QTextBlockFormat bf = b.blockFormat();
            bf.clearProperty(kContProp);
            fixMargin(bf, false);
            QTextCursor(b).setBlockFormat(bf);
            return;
        }
        if (code && empty && b.previous().isValid() && isCode(b.previous()) && !(b.next().isValid() && isCode(b.next()))) {
            styleBlock(b, BlockStyle::Paragraph);   // second Enter on an empty last line leaves the fence
            return;
        }
        const bool atEnd = c.atBlockEnd();
        const bool heading = b.blockFormat().headingLevel() > 0;
        c.insertBlock();
        QTextBlock nb = c.block();
        if (heading && atEnd) styleBlock(nb, BlockStyle::Paragraph);
        QTextBlockFormat bf = nb.blockFormat();
        if (nb.textList() && bf.marker() != MT::NoMarker) {
            bf.setMarker(MT::Unchecked);
            QTextCursor(nb).setBlockFormat(bf);
        }
        if (!code && c.charFormat().fontFixedPitch()) {   // inline code must not leak into the next paragraph
            QTextCharFormat f = c.charFormat();
            applyInline(f, InlineStyle::Code, false, m_mono);
            c.setCharFormat(f);
        }
        setTextCursor(c);
        ensureCursorVisible();
    }
}

// ---- markdown shortcuts --------------------------------------------------------------------------

bool VisualEdit::tryFenceShortcut()
{
    QTextCursor c = textCursor();
    const QTextBlock b = c.block();
    if (isCode(b) || b.textList() || !c.atBlockEnd()) return false;
    static const QRegularExpression re(QStringLiteral("^```\\s*([A-Za-z0-9_+.#-]*)\\s*$"));
    const auto m = re.match(b.text());
    if (!m.hasMatch()) return false;
    auto s = m_rec->begin(TxKind::Auto, WindowMode::Selection);
    QTextCursor d(b);
    d.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
    d.removeSelectedText();
    styleBlock(b, BlockStyle::Code);
    if (!m.captured(1).isEmpty()) {
        QTextBlockFormat bf = b.blockFormat();
        bf.setProperty(QTextFormat::BlockCodeLanguage, m.captured(1));
        QTextCursor(b).setBlockFormat(bf);
    }
    setTextCursor(QTextCursor(b));
    return true;
}

bool VisualEdit::tryBlockShortcut()
{
    QTextCursor c = textCursor();
    const QTextBlock b = c.block();
    if (isCode(b)) return false;
    const QString t = b.text().left(c.positionInBlock());
    if (!t.endsWith(QLatin1Char(' '))) return false;
    const QString p = t.chopped(1);
    static const QRegularExpression head(QStringLiteral("^#{1,6}$")), ord(QStringLiteral("^(\\d{1,9})[.)]$"));
    enum { None, Head, Bullet, Ordered, Quote, Task } what = None;
    int arg = 0;
    if (head.match(p).hasMatch() && b.blockFormat().headingLevel() == 0 && !b.textList()) { what = Head; arg = p.size(); }
    else if ((p == QLatin1String("-") || p == QLatin1String("*") || p == QLatin1String("+")) && !b.textList() && b.blockFormat().headingLevel() == 0) what = Bullet;
    else if (auto m = ord.match(p); m.hasMatch() && !b.textList() && b.blockFormat().headingLevel() == 0) { what = Ordered; arg = m.captured(1).toInt(); }
    else if (p == QLatin1String(">") && !isQuote(b) && !b.textList()) what = Quote;
    else if ((p == QLatin1String("[ ]") || p == QLatin1String("[x]") || p == QLatin1String("[X]")) && b.textList() && !isCheck(b)) { what = Task; arg = p == QLatin1String("[ ]") ? 0 : 1; }
    if (what == None) return false;
    auto s = m_rec->begin(TxKind::Auto, WindowMode::Selection);
    QTextCursor d(b);
    d.setPosition(b.position());
    d.setPosition(b.position() + t.size(), QTextCursor::KeepAnchor);
    d.removeSelectedText();
    switch (what) {
    case Head: styleBlock(b, BlockStyle(int(BlockStyle::H1) + arg - 1)); break;
    case Bullet: applyList({b}, ListKind::Bullet); break;
    case Ordered: applyList({b}, ListKind::Ordered, arg); break;
    case Quote: styleBlock(b, BlockStyle::Quote); break;
    case Task: {
        QTextBlockFormat bf = b.blockFormat();
        bf.setMarker(arg ? MT::Checked : MT::Unchecked);
        QTextCursor(b).setBlockFormat(bf);
        break;
    }
    default: break;
    }
    return true;
}

bool VisualEdit::tryInlineShortcut()
{
    QTextCursor c = textCursor();
    const QTextBlock b = c.block();
    if (isCode(b) || c.charFormat().fontFixedPitch()) return false;
    const QString t = b.text().left(c.positionInBlock());
    if (t.isEmpty()) return false;
    const QChar last = t.back();
    int open = -1, dlen = 0;
    InlineStyle kind = InlineStyle::Bold;
    if (last == QLatin1Char('*')) {
        if (findSpan(t, last, 2, open)) { dlen = 2; kind = InlineStyle::Bold; }
        else if (findSpan(t, last, 1, open)) { dlen = 1; kind = InlineStyle::Italic; }
    } else if (last == QLatin1Char('_')) {
        if (findSpan(t, last, 2, open)) { dlen = 2; kind = InlineStyle::Bold; }
        else if (findSpan(t, last, 1, open)) { dlen = 1; kind = InlineStyle::Italic; }
    } else if (last == QLatin1Char('~')) {
        if (findSpan(t, last, 2, open)) { dlen = 2; kind = InlineStyle::Strike; }
    } else if (last == QLatin1Char('`')) {
        if (findSpan(t, last, 1, open)) { dlen = 1; kind = InlineStyle::Code; }
    }
    if (open < 0) return false;
    auto s = m_rec->begin(TxKind::Auto, WindowMode::Selection);
    const int base = b.position(), n = t.size(), end = n - dlen;
    QTextCursor e(document());
    e.setPosition(base + end);
    e.setPosition(base + n, QTextCursor::KeepAnchor);
    e.removeSelectedText();
    modifyRange(document(), base + open + dlen, base + end, [&](QTextCharFormat &f) { applyInline(f, kind, true, m_mono); });
    QTextCursor o(document());
    o.setPosition(base + open);
    o.setPosition(base + open + dlen, QTextCursor::KeepAnchor);
    o.removeSelectedText();
    QTextCursor after(document());
    after.setPosition(base + open + (end - open - dlen));
    QTextCharFormat pf = after.charFormat();
    applyInline(pf, kind, false, m_mono);
    after.setCharFormat(pf);   // typing continues unformatted
    setTextCursor(after);
    return true;
}

// ---- mouse ----------------------------------------------------------------------------------------

bool VisualEdit::tryRuleShortcut()
{
    QTextCursor c = textCursor();
    const QTextBlock b = c.block();
    static const QRegularExpression re(QStringLiteral("^(-{3,}|\\*{3,}|_{3,})$"));
    if (isCode(b) || b.textList() || isQuote(b) || b.blockFormat().headingLevel() > 0 || !c.atBlockEnd() || !re.match(b.text()).hasMatch()) return false;
    auto s = m_rec->begin(TxKind::Auto, WindowMode::Wide);
    QTextCursor d(b);
    d.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
    d.removeSelectedText();
    QTextBlockFormat bf = b.blockFormat();
    bf.setProperty(kHrProp, true);
    bf.setTopMargin(12);
    bf.setBottomMargin(12);
    QTextCursor(b).setBlockFormat(bf);
    QTextCharFormat cf;
    cf.setFontPointSize(2);
    QTextCursor(b).setBlockCharFormat(cf);
    setTextCursor(QTextCursor(b));
    openLineAfterRule();
    return true;
}

// Checklist marker: Qt paints its own native box (an "X in a box") in the item's indent; hide that strip with the
// theme background and draw a square hairline box (accent + white tick when checked) in the same 16px slot.
QRect VisualEdit::checkRect(const QTextBlock &b) const
{
    const QRect lr = cursorRect(QTextCursor(b));
    const int x = lr.left() - kCheckGap - kCheckBox;
    return QRect(x, lr.top() + (lr.height() - kCheckBox) / 2, kCheckBox, kCheckBox);
}

void VisualEdit::paintChecks(QPainter &p)
{
    const QColor bg = m_bg.isValid() ? m_bg : palette().color(QPalette::Base);
    const QColor line = m_rule.isValid() ? m_rule : palette().color(QPalette::Mid);
    const QColor accent = m_link.isValid() ? m_link : palette().color(QPalette::Highlight);
    const int vh = viewport()->height();
    for (QTextBlock b = cursorForPosition(QPoint(0, 0)).block(); b.isValid(); b = b.next()) {
        const QRect lr = cursorRect(QTextCursor(b));
        if (lr.top() > vh) break;
        if (!isCheck(b)) continue;
        const QRect box = checkRect(b);
        const bool on = b.blockFormat().marker() == MT::Checked;
        p.setRenderHint(QPainter::Antialiasing, false);
        p.fillRect(QRect(qMax(0, lr.left() - kIndentPx), lr.top(), qMin(kIndentPx, lr.left()), lr.height()), bg);
        p.setPen(QPen(on ? accent : line, 1));
        p.setBrush(on ? QBrush(accent) : QBrush(Qt::NoBrush));
        p.drawRect(box.adjusted(0, 0, -1, -1));
        if (on) {
            p.setRenderHint(QPainter::Antialiasing, true);
            QPen tp(Qt::white, 1.75, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
            p.setPen(tp);
            p.setBrush(Qt::NoBrush);
            const QPointF o = box.topLeft();
            p.drawPolyline(QPolygonF{o + QPointF(3.5, 8.5), o + QPointF(6.5, 11.5), o + QPointF(12, 4.5)});
        }
    }
}

void VisualEdit::paintEvent(QPaintEvent *e)
{
    QTextEdit::paintEvent(e);
    if (!document()->blockCount()) return;
    QPainter p(viewport());
    p.setClipRect(e->rect());
    paintChecks(p);
    const QColor col = m_rule.isValid() ? m_rule : palette().color(QPalette::Mid);
    const int dy = verticalScrollBar()->value(), vh = viewport()->height(), vw = viewport()->width();
    const qreal margin = document()->documentMargin();
    auto *lay = document()->documentLayout();
    for (QTextBlock b = cursorForPosition(QPoint(0, 0)).block(); b.isValid(); b = b.next()) {
        const QRectF r = lay->blockBoundingRect(b);
        if (r.top() - dy > vh) break;
        if (!isRule(b)) continue;
        const int x0 = qRound(margin + b.blockFormat().leftMargin()), y = qRound(r.center().y()) - dy;
        p.fillRect(QRect(x0, y, qMax(0, int(vw - margin) - x0), 1), col);
    }
}

void VisualEdit::mousePressEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton && !(e->modifiers() & Qt::ControlModifier)) {
        const QPoint p = e->position().toPoint();
        const QTextBlock b = cursorForPosition(p).block();
        if (isCheck(b)) {
            if (checkRect(b).adjusted(-kCheckGap / 2, -2, kCheckGap, 2).contains(p)) {
                toggleCheck(b);
                if (!hasFocus()) setFocus(Qt::MouseFocusReason);
                e->accept();
                return;
            }
        }
    }
    QTextEdit::mousePressEvent(e);
}

void VisualEdit::mouseReleaseEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton && (e->modifiers() & Qt::ControlModifier)) {
        const QString href = anchorAt(e->position().toPoint());
        if (!href.isEmpty()) {
            emit linkActivated(QUrl(href));
            e->accept();
            return;
        }
    }
    QTextEdit::mouseReleaseEvent(e);
}

// ---- normalization (theme/dialect fixups; never part of saved markdown) ----------------------------

void VisualEdit::normalizeDocument() { normalizeRange(0, document()->blockCount() - 1); }

void VisualEdit::normalizeRange(int b0, int b1)
{
    EditRecorder::Quiet q(m_rec);
    QTextDocument *doc = document();
    b0 = qMax(0, b0);
    b1 = qMin(b1, doc->blockCount() - 1);
    QTextCursor edit(doc);
    edit.beginEditBlock();
    for (QTextBlock b = doc->findBlockByNumber(b0); b.isValid() && b.blockNumber() <= b1; b = b.next()) {
        QTextBlockFormat bf = b.blockFormat();
        bool ch = false;
        if (bf.marker() != MT::NoMarker && !b.textList()) { bf.setMarker(MT::NoMarker); ch = true; }   // Qt importer leaks markers
        if (bf.lineHeightType() != QTextBlockFormat::ProportionalHeight || qAbs(bf.lineHeight() - m_lh * 100) > 0.01) {
            bf.setLineHeight(m_lh * 100, QTextBlockFormat::ProportionalHeight);
            ch = true;
        }
        if (isQuote(b) || bf.hasProperty(kContProp)) {
            const int want = (b.textList() ? 0 : kIndentPx * contDepth(bf)) + 16 * quoteLevelOf(bf);
            if (bf.leftMargin() != want) { bf.setLeftMargin(want); ch = true; }
        }
        if (ch) QTextCursor(b).setBlockFormat(bf);
        if (isCode(b)) {
            QTextCharFormat cf = b.charFormat();
            if (!m_mono.isEmpty() && (!cf.fontFixedPitch() || cf.fontFamilies().toStringList() != QStringList{m_mono})) {
                applyInline(cf, InlineStyle::Code, true, m_mono);
                QTextCursor(b).setBlockCharFormat(cf);
            }
        }
        struct R { int a, e; QTextCharFormat f; };
        QList<R> rs;
        for (auto it = b.begin(); !it.atEnd(); ++it) {
            const QTextFragment fr = it.fragment();
            QTextCharFormat f = fr.charFormat();
            bool c2 = false;
            if (f.fontFixedPitch() && !m_mono.isEmpty() && f.fontFamilies().toStringList() != QStringList{m_mono}) { applyInline(f, InlineStyle::Code, true, m_mono); c2 = true; }
            if (f.isAnchor() && m_link.isValid() && f.foreground().color() != m_link) { f.setForeground(m_link); c2 = true; }
            if (c2) rs.append({fr.position(), fr.position() + fr.length(), f});
        }
        for (auto &r : rs) {
            QTextCursor c(doc);
            c.setPosition(r.a);
            c.setPosition(r.e, QTextCursor::KeepAnchor);
            c.setCharFormat(r.f);
        }
    }
    edit.endEditBlock();
}

// ---------------------------------------------------------------------------------------------------
// Source mode
// ---------------------------------------------------------------------------------------------------

SourceEdit::SourceEdit(EditRecorder *rec, QWidget *parent) : QPlainTextEdit(parent), m_rec(rec)
{
    setUndoRedoEnabled(false);
    document()->setUndoRedoEnabled(false);
    setFrameShape(QFrame::NoFrame);
    setContextMenuPolicy(Qt::CustomContextMenu);
    setLineWrapMode(QPlainTextEdit::WidgetWidth);
}

void SourceEdit::cutRecorded()
{
    auto s = m_rec->begin(TxKind::Cut, WindowMode::Selection);
    cut();
}

void SourceEdit::keyPressEvent(QKeyEvent *e)
{
    if (e->matches(QKeySequence::Undo)) { emit undoRequested(); return; }
    if (e->matches(QKeySequence::Redo) || (e->key() == Qt::Key_Y && e->modifiers() == Qt::ControlModifier)) { emit redoRequested(); return; }
    TxKind kind = TxKind::Structure;
    if (e->matches(QKeySequence::Cut)) kind = TxKind::Cut;
    else if (e->matches(QKeySequence::Paste)) kind = TxKind::Paste;
    else if (e->key() == Qt::Key_Backspace || e->key() == Qt::Key_Delete) kind = TxKind::Delete;
    else if (isPrintableText(e->text()) && !(e->modifiers() & (Qt::ControlModifier | Qt::AltModifier))) kind = TxKind::Typing;
    else if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter || e->key() == Qt::Key_Tab) kind = TxKind::Structure;
    else { QPlainTextEdit::keyPressEvent(e); return; }
    {
        auto s = m_rec->begin(kind, WindowMode::Selection);
        QPlainTextEdit::keyPressEvent(e);
    }
    if (kind == TxKind::Typing && !m_preedit && afterTyping && e->text().size() == 1 && !textCursor().hasSelection()) afterTyping();
}

void SourceEdit::inputMethodEvent(QInputMethodEvent *e)
{
    m_preedit = !e->preeditString().isEmpty();
    const bool edits = !e->commitString().isEmpty() || e->replacementLength() != 0;
    auto s = m_rec->begin(TxKind::Typing, edits ? WindowMode::Wide : WindowMode::Selection);
    QPlainTextEdit::inputMethodEvent(e);
}

void SourceEdit::insertFromMimeData(const QMimeData *src)
{
    if (qint64(src->text().size()) * 2 > TxHistory::kMaxBytes / 2) {
        QString err;
        if (!m_rec->history().canSpill(&err)) { emit m_rec->recoveryWriteFailed(err); return; }
    }
    auto s = m_rec->begin(TxKind::Paste, WindowMode::Selection);
    QPlainTextEdit::insertFromMimeData(src);
}

void SourceEdit::dropEvent(QDropEvent *e)
{
    auto s = m_rec->begin(TxKind::Drop, WindowMode::WholeDoc);
    QPlainTextEdit::dropEvent(e);
}

} // namespace hn::editor
