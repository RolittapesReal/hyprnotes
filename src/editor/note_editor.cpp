#include "hn/editor/note_editor.h"

#include <QApplication>
#include <QClipboard>
#include <QFontDatabase>
#include <QInputDialog>
#include <QMenu>
#include <QStackedLayout>
#include <QTextBlock>
#ifdef __GLIBC__
#include <malloc.h>
#endif

#include "hn/core/markdown_codec.h"
#include "md_io.h"
#include "visual_edit.h"

namespace hn::editor {

static constexpr qsizetype kVisualLimit = 256 * 1024;

NoteEditor::NoteEditor(QWidget *parent) : QWidget(parent)
{
    m_vis = new VisualEdit(&m_rec, this);
    m_src = new SourceEdit(&m_rec, this);
    m_stack = new QStackedLayout(this);
    m_stack->setContentsMargins(0, 0, 0, 0);
    m_stack->addWidget(m_vis);
    m_stack->addWidget(m_src);
    setFocusProxy(m_vis);

    QFont f = font();
    f.setWeight(QFont::Normal);   // Qt's writer treats weight > default as bold: keep the document default Normal
    m_vis->document()->setDefaultFont(f);
    const QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    m_src->document()->setDefaultFont(mono);
    m_vis->setStyleContext(f.family(), mono.family(), m_lh, QColor());
    m_rec.setBlockFixup([this](QTextBlockFormat &bf) { bf.setLineHeight(m_lh * 100, QTextBlockFormat::ProportionalHeight); });

    m_vis->afterTyping = [this] { checkTrigger(); };
    m_src->afterTyping = [this] { checkTrigger(); };
    connect(&m_rec, &EditRecorder::changed, this, [this](int r) { if (!m_loading) emit contentChanged(r); });
    connect(&m_rec, &EditRecorder::recoveryWriteFailed, this, &NoteEditor::historyWarning);
    connect(&m_rec, &EditRecorder::historyInvalidated, this, &NoteEditor::historyWarning);
    connect(m_vis, &VisualEdit::undoRequested, this, &NoteEditor::undo);
    connect(m_vis, &VisualEdit::redoRequested, this, &NoteEditor::redo);
    connect(m_src, &SourceEdit::undoRequested, this, &NoteEditor::undo);
    connect(m_src, &SourceEdit::redoRequested, this, &NoteEditor::redo);
    connect(m_vis, &VisualEdit::linkActivated, this, &NoteEditor::linkActivated);
    connect(m_vis, &VisualEdit::linkRequested, this, &NoteEditor::editLink);
    connect(m_vis, &QTextEdit::cursorPositionChanged, this, [this] { emitCursorInfo(); emit formatStateChanged(); });
    connect(m_vis, &QTextEdit::currentCharFormatChanged, this, [this] { emit formatStateChanged(); });
    connect(m_src, &QPlainTextEdit::cursorPositionChanged, this, [this] { emitCursorInfo(); });
    connect(m_vis, &QWidget::customContextMenuRequested, this, [this](const QPoint &p) { showContextMenu(m_vis, p); });
    connect(m_src, &QWidget::customContextMenuRequested, this, [this](const QPoint &p) { showContextMenu(m_src, p); });
    switchTo(Mode::Visual, {});
    m_stack->setCurrentWidget(m_vis);
}

NoteEditor::~NoteEditor() = default;

QTextEdit *NoteEditor::visualEdit() const { return m_vis; }
QPlainTextEdit *NoteEditor::sourceEdit() const { return m_src; }
QWidget *NoteEditor::activeEdit() const { return m_mode == Mode::Visual ? static_cast<QWidget *>(m_vis) : m_src; }
void NoteEditor::focusEditor() { activeEdit()->setFocus(); }

void NoteEditor::emitCursorInfo()
{
    const QTextCursor c = m_mode == Mode::Visual ? m_vis->textCursor() : m_src->textCursor();
    emit cursorInfoChanged(c.blockNumber() + 1, c.positionInBlock() + 1, c.selectionEnd() - c.selectionStart());
}

// ---- encoding helpers ---------------------------------------------------------------------------

QString NoteEditor::decoded(const QByteArray &bytes) const
{
    QString t = QString::fromUtf8(bytes);
    t.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    t.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    return t;
}

QByteArray NoteEditor::encode(QString text) const
{
    if (m_nl == 1) text.replace(QLatin1Char('\n'), QStringLiteral("\r\n"));
    else if (m_nl == 2) text.replace(QLatin1Char('\n'), QLatin1Char('\r'));
    QByteArray out = text.toUtf8();
    if (m_bom) out.prepend("\xEF\xBB\xBF");
    return out;
}

// ---- load / save -------------------------------------------------------------------------------

void NoteEditor::load(const QByteArray &fileBytes)
{
    m_loading = true;
    m_orig = fileBytes;
    m_bom = fileBytes.startsWith("\xEF\xBB\xBF");
    const QByteArray body = m_bom ? fileBytes.mid(3) : fileBytes;
    const int lf = body.indexOf('\n');
    m_nl = lf > 0 && body[lf - 1] == '\r' ? 1 : (lf < 0 && body.contains('\r') ? 2 : 0);
    m_rec.history().clear();
    m_rec.newEpoch();
    m_invalid = false;
    m_src->setReadOnly(false);

    auto toSource = [&](const QString &reason, bool ro) {
        setSourceContent(decoded(body));
        m_invalid = ro;
        m_src->setReadOnly(ro);
        switchTo(Mode::Source, reason);
    };
    if (!hn::core::isValidUtf8(body)) {
        toSource(tr("Invalid UTF-8: opened read-only. Use Convert to make it editable (undecodable bytes become U+FFFD)."), true);
    } else if (auto cls = hn::core::classifyMarkdown(body, kVisualLimit); !cls.visual) {
        toSource(cls.reason, false);
    } else {
        const QString text = decoded(body);
        setVisualContent(text);
        if (const QString problem = roundTripProblem(text); !problem.isEmpty()) toSource(problem, false);
        else switchTo(Mode::Visual, {});
    }
    m_rec.resetRevision();
    m_origRev = m_savedRev = 0;
    m_loading = false;
    m_vis->moveCursor(QTextCursor::Start);
    m_src->moveCursor(QTextCursor::Start);
    emitCursorInfo();
}

QString NoteEditor::roundTripProblem(const QString &md) const
{
    const QString out = exportMarkdown(m_vis->document());
    const QStringList a = hn::core::semanticTokens(md), b = hn::core::semanticTokens(out);
    if (a == b) return {};
    int i = 0;
    while (i < a.size() && i < b.size() && a[i] == b[i]) ++i;
    return tr("Visual import/export check failed near token %1 (%2 vs %3); opened in source mode to preserve the file.")
        .arg(i).arg(i < a.size() ? a[i] : QStringLiteral("<end>")).arg(i < b.size() ? b[i] : QStringLiteral("<end>"));
}

QByteArray NoteEditor::toMarkdownBytes() const
{
    if (m_rec.revision() == m_origRev) return m_orig;   // never rewrite what was not edited
    QString text;
    if (m_mode == Mode::Visual) {
        text = exportMarkdown(m_vis->document());
        if (!text.isEmpty() && !text.endsWith(QLatin1Char('\n'))) text += QLatin1Char('\n');
    } else {
        text = m_src->toPlainText();
    }
    return encode(text);
}

void NoteEditor::markSaved(const QByteArray &savedBytes)
{
    m_orig = savedBytes.isNull() ? toMarkdownBytes() : savedBytes;
    m_origRev = m_savedRev = m_rec.revision();
}

void NoteEditor::convertInvalidUtf8()
{
    if (!m_invalid) return;
    m_invalid = false;
    m_src->setReadOnly(false);
    m_origRev = -1;
    m_rec.history().clear();
    m_rec.touch();
    m_reason = tr("Converted from invalid UTF-8: undecodable bytes were replaced with U+FFFD. Source mode.");
    emit modeChanged(Mode::Source, m_reason);
}

// ---- modes -----------------------------------------------------------------------------------------

void NoteEditor::switchTo(Mode m, const QString &reason)
{
    const bool focus = hasFocus() || activeEdit()->hasFocus();
    m_mode = m;
    m_reason = reason;
    if (m == Mode::Visual) {
        m_stack->setCurrentWidget(m_vis);
        m_rec.attach(m_vis->document(), [this] { return m_vis->textCursor(); },
                     [this](int pos, int anchor) {
                         QTextCursor c(m_vis->document());
                         c.setPosition(anchor);
                         c.setPosition(pos, QTextCursor::KeepAnchor);
                         m_vis->setTextCursor(c);
                     });
    } else {
        m_stack->setCurrentWidget(m_src);
        m_rec.attach(m_src->document(), [this] { return m_src->textCursor(); },
                     [this](int pos, int anchor) {
                         QTextCursor c(m_src->document());
                         c.setPosition(anchor);
                         c.setPosition(pos, QTextCursor::KeepAnchor);
                         m_src->setTextCursor(c);
                     });
    }
    setFocusProxy(activeEdit());
    if (focus) activeEdit()->setFocus();
    emit modeChanged(m, reason);
    emit formatStateChanged();
}

void NoteEditor::setVisualContent(const QString &md)
{
    EditRecorder::Quiet q(&m_rec);
    importMarkdown(m_vis->document(), md);
    m_vis->normalizeDocument();
    m_rec.newEpoch();
}

void NoteEditor::setSourceContent(const QString &text)
{
    EditRecorder::Quiet q(&m_rec);
    m_src->setPlainText(text);
}

void NoteEditor::mapCursor(int fromBlock, int fromBlocks, QTextDocument *to, QWidget *edit)
{
    const int n = to->blockCount();
    const int target = fromBlocks > 1 ? qRound(double(fromBlock) / (fromBlocks - 1) * (n - 1)) : 0;
    QTextCursor c(to->findBlockByNumber(qBound(0, target, n - 1)));
    if (auto *v = qobject_cast<QTextEdit *>(edit)) v->setTextCursor(c);
    else if (auto *s = qobject_cast<QPlainTextEdit *>(edit)) s->setTextCursor(c);
}

void NoteEditor::applyModeState(Mode m, const QString &text)
{
    const QTextDocument *fromDoc = m_mode == Mode::Visual ? m_vis->document() : m_src->document();
    const int fb = (m_mode == Mode::Visual ? m_vis->textCursor() : m_src->textCursor()).blockNumber();
    const int fn = fromDoc->blockCount();
    if (m == Mode::Visual) setVisualContent(text);
    else setSourceContent(text);
    switchTo(m, m == Mode::Source ? tr("Source mode (selected)") : QString());
    mapCursor(fb, fn, m == Mode::Visual ? m_vis->document() : m_src->document(), activeEdit());
}

bool NoteEditor::setMode(Mode target)
{
    if (target == m_mode) return true;
    if (m_invalid) {
        emit switchRefused(tr("Invalid UTF-8 note: convert it before switching modes."));
        return false;
    }
    QString before, after;
    if (target == Mode::Source) {
        before = after = m_rec.revision() == m_origRev ? decoded(m_orig.mid(m_bom ? 3 : 0))
                                                        : exportMarkdown(m_vis->document());
    } else {
        before = m_src->toPlainText();
        if (auto cls = hn::core::classifyMarkdown(before.toUtf8(), kVisualLimit); !cls.visual) {
            emit switchRefused(cls.reason);
            return false;
        }
        setVisualContent(before);   // the hidden visual document is rebuilt; the source stays current on failure
        if (const QString p = roundTripProblem(before); !p.isEmpty()) {
            emit switchRefused(p);
            return false;
        }
        after = exportMarkdown(m_vis->document());
    }
    if (qint64(before.size() + after.size()) * 2 > TxHistory::kMaxBytes / 2) {
        QString err;
        if (!m_rec.history().canSpill(&err)) {
            emit switchRefused(tr("Cannot keep a recovery copy on disk (%1); mode switch not applied.").arg(err));
            emit historyWarning(err);
            return false;
        }
    }
    const QTextDocument *fromDoc = m_mode == Mode::Visual ? m_vis->document() : m_src->document();
    const int fb = (m_mode == Mode::Visual ? m_vis->textCursor() : m_src->textCursor()).blockNumber();
    const int fn = fromDoc->blockCount();
    if (target == Mode::Source) setSourceContent(after);
    Tx tx;
    tx.kind = TxKind::ModeSwitch;
    tx.modeFrom = int(m_mode);
    tx.modeTo = int(target);
    tx.textBefore = before;
    tx.textAfter = after;
    switchTo(target, target == Mode::Source ? tr("Source mode (selected)") : QString());
    mapCursor(fb, fn, target == Mode::Visual ? m_vis->document() : m_src->document(), activeEdit());
    m_rec.pushModeTx(std::move(tx));
    m_rec.touch();
    return true;
}

// ---- history ------------------------------------------------------------------------------------------

bool NoteEditor::undo()
{
    auto &h = m_rec.history();
    if (m_invalid || !h.canUndo()) return false;
    if (h.peekUndo()->kind == TxKind::ModeSwitch) {
        const Tx t = h.takeForUndo();
        applyModeState(Mode(t.modeFrom), t.textBefore);
        m_rec.touch();
        return true;
    }
    return m_rec.undoRegion();
}

bool NoteEditor::redo()
{
    auto &h = m_rec.history();
    if (m_invalid || !h.canRedo()) return false;
    if (h.peekRedo()->kind == TxKind::ModeSwitch) {
        const Tx t = h.takeForRedo();
        applyModeState(Mode(t.modeTo), t.textAfter);
        m_rec.touch();
        return true;
    }
    return m_rec.redoRegion();
}

void NoteEditor::releaseResources()
{
    m_rec.history().clear();
    m_vis->document()->clearUndoRedoStacks();
    m_src->document()->clearUndoRedoStacks();
#ifdef __GLIBC__
    malloc_trim(0);
#endif
}

// ---- commands -------------------------------------------------------------------------------------------

#define VIS_ONLY(expr) do { if (m_mode == Mode::Visual) { expr; } } while (0)
void NoteEditor::toggleInline(InlineStyle s) { VIS_ONLY(m_vis->toggleInline(s)); }
bool NoteEditor::inlineActive(InlineStyle s) const { return m_mode == Mode::Visual && m_vis->inlineActive(s); }
void NoteEditor::setBlockStyle(BlockStyle s) { VIS_ONLY(m_vis->setBlockStyle(s)); }
BlockStyle NoteEditor::blockStyle() const { return m_mode == Mode::Visual ? m_vis->blockStyle() : BlockStyle::Paragraph; }
void NoteEditor::toggleList(ListKind k) { VIS_ONLY(m_vis->toggleList(k)); }
ListKind NoteEditor::listKind() const { return m_mode == Mode::Visual ? m_vis->listKind() : ListKind::None; }
bool NoteEditor::toggleCheck() { return m_mode == Mode::Visual && m_vis->toggleCheckAtCursor(); }
void NoteEditor::indent() { VIS_ONLY(m_vis->indentList(+1)); }
void NoteEditor::outdent() { VIS_ONLY(m_vis->indentList(-1)); }
void NoteEditor::setLink(const QString &url) { VIS_ONLY(m_vis->setLink(url)); }
QString NoteEditor::linkAtCursor() const { return m_mode == Mode::Visual ? m_vis->linkAtCursor() : QString(); }

void NoteEditor::editLink()
{
    if (m_mode != Mode::Visual) return;
    bool ok = false;
    const QString url = QInputDialog::getText(this, tr("Link"), tr("Address (empty removes the link)"), QLineEdit::Normal,
                                              m_vis->linkAtCursor(), &ok);
    if (ok) m_vis->setLink(url.trimmed());
    m_vis->setFocus();
}

void NoteEditor::cut()
{
    if (m_invalid) return;
    if (m_mode == Mode::Visual) m_vis->cutRecorded(); else m_src->cutRecorded();
}
void NoteEditor::copy() { if (m_mode == Mode::Visual) m_vis->copy(); else m_src->copy(); }
void NoteEditor::paste()
{
    if (m_invalid) return;
    if (m_mode == Mode::Visual) m_vis->paste(); else m_src->paste();
}
void NoteEditor::selectAll() { if (m_mode == Mode::Visual) m_vis->selectAll(); else m_src->selectAll(); }

void NoteEditor::pasteAsMarkdown()
{
    if (m_invalid) return;
    if (m_mode != Mode::Visual) { m_src->paste(); return; }
    const QString text = QApplication::clipboard()->text();
    if (text.isEmpty()) return;
    const auto cls = hn::core::classifyMarkdown(text.toUtf8(), kVisualLimit);
    if (!cls.visual) {
        emit pasteNeedsSource(cls.reason);   // cannot be represented visually: insert literally as plain text
        m_vis->paste();
        return;
    }
    QTextDocument tmp;
    QFont f = font();
    f.setWeight(QFont::Normal);
    tmp.setDefaultFont(f);
    importMarkdown(&tmp, text);
    m_vis->insertMarkdownFragment(tmp);
}

bool NoteEditor::caretInCode() const
{
    if (m_mode == Mode::Visual) {
        const QTextCursor c = m_vis->textCursor();
        return c.block().blockFormat().hasProperty(QTextFormat::BlockCodeFence) || c.charFormat().fontFixedPitch();
    }
    const QTextCursor c = m_src->textCursor();
    int fences = 0;
    for (QTextBlock b = m_src->document()->begin(); b.isValid() && b.blockNumber() < c.blockNumber(); b = b.next())
        if (b.text().trimmed().startsWith(QStringLiteral("```"))) ++fences;
    if (fences % 2) return true;
    if (c.block().text().trimmed().startsWith(QStringLiteral("```"))) return true;
    return c.block().text().left(c.positionInBlock()).count(QLatin1Char('`')) % 2 == 1;   // inside an inline code span
}

void NoteEditor::checkTrigger()
{
    if (!m_trigger || m_invalid || m_inTrigger) return;
    const bool vis = m_mode == Mode::Visual;
    if (vis && m_vis->imePreedit()) return;
    QTextCursor c = vis ? m_vis->textCursor() : m_src->textCursor();
    if (c.hasSelection()) return;
    const QString line = c.block().text().left(c.positionInBlock());
    int s = line.size();
    while (s > 0 && !line[s - 1].isSpace()) --s;   // token boundary: whitespace or start of the block
    const QString token = line.mid(s);
    if (token.size() < 2 || token.size() > 64 || caretInCode()) return;
    const auto r = m_trigger(token);
    if (!r || r->matchLen != token.size()) return;   // only a whole token is a trigger
    m_inTrigger = true;
    {
        auto scope = m_rec.begin(TxKind::Format, WindowMode::Selection);   // not mergeable with typing: undo restores the literal
        QTextCursor e = c;
        e.setPosition(c.position() - token.size());
        e.setPosition(c.position(), QTextCursor::KeepAnchor);
        e.insertText(r->replacement);
        if (vis) m_vis->setTextCursor(e); else m_src->setTextCursor(e);
    }
    m_inTrigger = false;
}

void NoteEditor::insertMarkdown(const QString &md, bool replaceAll)
{
    if (m_invalid) return;
    auto scope = m_rec.begin(TxKind::Format, replaceAll ? WindowMode::WholeDoc : WindowMode::Selection);
    if (m_mode == Mode::Source || !hn::core::classifyMarkdown(md.toUtf8(), kVisualLimit).visual) {
        QTextCursor c = m_mode == Mode::Visual ? m_vis->textCursor() : m_src->textCursor();
        if (replaceAll) c.select(QTextCursor::Document);
        c.insertText(md);
        if (m_mode == Mode::Visual) m_vis->setTextCursor(c); else m_src->setTextCursor(c);
        return;
    }
    QTextDocument tmp;
    QFont f = font();
    f.setWeight(QFont::Normal);
    tmp.setDefaultFont(f);
    importMarkdown(&tmp, md);
    if (replaceAll) { QTextCursor c = m_vis->textCursor(); c.select(QTextCursor::Document); m_vis->setTextCursor(c); }
    m_vis->insertMarkdownFragment(tmp);
}

void NoteEditor::showContextMenu(QWidget *edit, const QPoint &p)
{
    QMenu menu(this);
    const bool ro = m_invalid;
    menu.addAction(tr("Undo"), this, &NoteEditor::undo)->setEnabled(canUndo());
    menu.addAction(tr("Redo"), this, &NoteEditor::redo)->setEnabled(canRedo());
    menu.addSeparator();
    const bool sel = m_mode == Mode::Visual ? m_vis->textCursor().hasSelection() : m_src->textCursor().hasSelection();
    menu.addAction(tr("Cut"), this, &NoteEditor::cut)->setEnabled(sel && !ro);
    menu.addAction(tr("Copy"), this, &NoteEditor::copy)->setEnabled(sel);
    menu.addAction(tr("Paste"), this, &NoteEditor::paste)->setEnabled(!ro);
    if (m_mode == Mode::Visual) menu.addAction(tr("Paste as Markdown"), this, &NoteEditor::pasteAsMarkdown)->setEnabled(!ro);
    menu.addAction(tr("Select All"), this, &NoteEditor::selectAll);
    menu.exec(edit->mapToGlobal(p));
}

// ---- theme ------------------------------------------------------------------------------------------------

void NoteEditor::setTheme(const hn::theme::Theme &t)
{
    m_theme = t;
    m_lh = t.lineHeight;
    QFont f = font();
    if (!t.fontFamily.isEmpty()) f.setFamily(t.fontFamily);
    f.setPixelSize(t.baseSize);
    f.setWeight(QFont::Normal);
    QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    if (!t.monoFamily.isEmpty()) mono.setFamily(t.monoFamily);
    mono.setPixelSize(t.baseSize);
    mono.setWeight(QFont::Normal);
    const QString css = QStringLiteral("a { color: %1; } code, pre { font-family: '%2'; }").arg(t.accent.name(), mono.family());
    for (QTextDocument *d : {m_vis->document(), m_src->document()}) {
        d->setDocumentMargin(t.padding);
        d->setDefaultStyleSheet(css);
    }
    m_vis->document()->setDefaultFont(f);
    m_src->document()->setDefaultFont(mono);
    m_vis->setStyleContext(f.family(), mono.family(), t.lineHeight, t.accent);
    m_vis->setRuleColor(t.border);
    m_vis->setBackgroundColor(t.bg);
    const QString ss = QStringLiteral(
        "QTextEdit, QPlainTextEdit { background: %1; color: %2; border: none; padding: 0; selection-background-color: %3; selection-color: %2; }")
        .arg(t.bg.name(), t.text.name(), t.selection.name());
    m_vis->setStyleSheet(ss);
    m_src->setStyleSheet(ss);
    m_vis->setFont(f);
    m_src->setFont(mono);
    m_vis->normalizeDocument();
}

} // namespace hn::editor
