#pragma once
#include <QByteArray>
#include <QPlainTextEdit>
#include <QTextEdit>
#include <QUrl>
#include <QWidget>
#include <functional>
#include <memory>
#include <optional>

#include "hn/editor/history.h"
#include "hn/editor/types.h"
#include "hn/editor/wiki_links.h"
#include "hn/theme/theme.h"

class QStackedLayout;

namespace hn::editor {

class VisualEdit;
class SourceEdit;

// Additive: typing triggers (plugins). The handler gets the whitespace-delimited token that ends at the caret and
// returns how many characters of it to replace (must be the whole token) and the replacement text.
struct TriggerResult { int matchLen = 0; QString replacement; };
using TriggerHandler = std::function<std::optional<TriggerResult>(const QString &token)>;

class NoteEditor : public QWidget {
    Q_OBJECT
public:
    explicit NoteEditor(QWidget *parent = nullptr);
    ~NoteEditor() override;

    // ---- document ----
    void load(const QByteArray &fileBytes);
    QByteArray toMarkdownBytes() const;
    Mode mode() const { return m_mode; }
    QString modeReason() const { return m_reason; }
    bool isReadOnly() const { return m_invalid; }
    bool hasInvalidUtf8() const { return m_invalid; }
    void convertInvalidUtf8();
    bool setMode(Mode m);                       // explicit, undoable; false => state retained (see switchRefused)
    int revision() const { return m_rec.revision(); }
    bool isModified() const { return m_rec.revision() != m_savedRev; }
    void markSaved(const QByteArray &savedBytes = {});

    // ---- history ----
    bool undo();
    // Drop undo history and hand freed heap back to the OS (call when a note is closed/hidden). Content is untouched.
    void releaseResources();
    bool redo();
    bool canUndo() const { return m_rec.history().canUndo(); }
    bool canRedo() const { return m_rec.history().canRedo(); }
    const TxHistory &history() const { return m_rec.history(); }
    TxHistory &history() { return m_rec.history(); }
    EditRecorder &recorder() { return m_rec; }

    // ---- commands (visual mode; no-ops in source mode) ----
    void toggleInline(InlineStyle s);
    bool inlineActive(InlineStyle s) const;
    void setBlockStyle(BlockStyle s);
    BlockStyle blockStyle() const;
    void toggleList(ListKind k);
    ListKind listKind() const;
    bool toggleCheck();
    void indent() ;
    void outdent();
    void setLink(const QString &url);
    QString linkAtCursor() const;
    void editLink();                            // prompts for a URL
    void pasteAsMarkdown();
    void cut();
    void copy();
    void paste();
    void selectAll();
    void focusEditor();

    void setTheme(const hn::theme::Theme &t);
    hn::theme::Theme theme() const { return m_theme; }

    // ---- additive hooks for plugins ----
    // Called after a typed character; never inside code (block, inline or fenced source), during IME composition, or with a
    // selection. A match is applied as ONE undo step (Format tx) so undo restores the typed literal.
    void setTriggerHandler(TriggerHandler h) { m_trigger = std::move(h); }
    bool hasTriggerHandler() const { return bool(m_trigger); }
    // Inserts markdown at the caret (visual) or text (source); replaceAll replaces the whole document. Wrap in a recorder scope
    // (EditRecorder::begin(.., WholeDoc)) to make it one undo step; without one it records its own.
    void insertMarkdown(const QString &md, bool replaceAll);
    bool caretInCode() const;

    // ---- Plugin API v2: wiki-link overlay (all off by default; nothing is allocated, connected or filtered until enabled) ----
    // Ranges of [[x]] / [[x|alias]] / [[x#h]] / ![[x]] in the VISIBLE text (visual and source mode, never in code) are painted
    // by an overlay in paintEvent: resolved = accent tint + solid accent underline, unresolved = dashed muted-danger underline,
    // ambiguous = double accent underline. The document, its formats and the Markdown round trip are never touched. Ranges are
    // computed lazily for the visible blocks only and cached per block (invalidated by edits of that block), so paint cost does
    // not depend on note size.
    // Independent of the overlay: visual-mode saves/mode switches ALWAYS write [[x]] / ![[x]] with unescaped brackets (the exporter
    // would otherwise write "\[\[x\]\]", which hn::core::extractLinks no longer sees as a link). A deliberately escaped
    // "\[\[x]]" is indistinguishable from a link in the visual document and is therefore written unescaped too (source mode
    // keeps the exact bytes).
    void setWikiLinksEnabled(bool on);
    bool wikiLinksEnabled() const;
    // Consulted lazily at paint/hit-test time with the link TARGET; answers are cached until invalidateLinkStates().
    // Without a resolver every link counts as Resolved.
    void setLinkResolver(std::function<LinkState(const QString &target)> r);
    void invalidateLinkStates();                 // drop cached answers and repaint (call when the note index changes)
    // Activation: Ctrl+click, Ctrl+Enter with the caret inside (or at the edge of) a link, and, only when enabled, a plain click
    // that does not drag. Plain clicks always also place the caret. Hovering a link shows a pointing hand while Ctrl is held
    // (always when plain-click activation is on). Emits wikiLinkActivated (NOT linkActivated(QUrl), which stays for http links).
    void setLinkClickActivates(bool on);
    bool linkClickActivates() const;
    // Helpers (also for tests). `viewportPos` is in the active edit's viewport coordinates.
    std::optional<LinkRefInfo> linkAt(const QPoint &viewportPos) const;
    QList<LinkRange> linkRangesInBlock(int blockNumber) const;   // empty unless enabled; block-local UTF-16 offsets, state resolved

    // ---- Plugin API v2: completion popup ----
    // After a typed character (never inside code, during IME preedit, or with a selection) the text before the caret is matched
    // against the triggers; on a match completionRequested is emitted (once per distinct query, no timers). Rules: a trigger that
    // starts with '[' (e.g. "[[") matches its LAST occurrence on the line and its query may contain spaces until the closing
    // bracket ("]]") or the end of the line; every other trigger (e.g. "/") must begin a whitespace-delimited token (line start or
    // after whitespace) and its query ends at the first whitespace. The session ends (popup closes, completionDismissed) on Esc,
    // click, focus loss, mode switch, the caret leaving trigger..query, or the query becoming invalid (space, "]]").
    void setCompletionTriggers(const QStringList &triggers);
    // Reply to completionRequested. `generation` is CompletionRequest::generation; replies for an older request are ignored
    // (-1 = "for the current request"). An empty list hides the popup but keeps the session. Ignored when no session is active.
    // The popup never takes focus: Up/Down/PageUp/PageDown move, Enter/Tab accept, Esc dismisses (forwarded from the editor).
    void showCompletions(const QList<CompletionItem> &items, int generation = -1);
    void dismissCompletions();
    bool completionActive() const;               // a session is open (popup may still be waiting for items)
    QWidget *completionPopup() const;            // null until first shown; for tests/theming only
    // Replaces trigger+query by item.insert as ONE undo step (undo restores the literal typed text), places the caret at
    // cursorOffset, emits completionAccepted. row < 0 = the highlighted row. False if there is no popup/row.
    bool acceptCompletion(int row = -1);
    // Popup placement: below the caret, above if it does not fit, inside `available` (pure; exposed for tests).
    static QRect placeCompletionPopup(const QRect &caretGlobal, const QSize &size, const QRect &available);

    QTextEdit *visualEdit() const;
    QPlainTextEdit *sourceEdit() const;
    QWidget *activeEdit() const;

signals:
    void contentChanged(int revision);
    void modeChanged(hn::editor::Mode mode, const QString &reason);
    void cursorInfoChanged(int line, int column, int selectedChars);
    void formatStateChanged();
    void linkActivated(const QUrl &url);        // Ctrl+click / Ctrl+Enter only; the app decides how to open it
    void switchRefused(const QString &reason);
    void pasteNeedsSource(const QString &reason);
    void historyWarning(const QString &reason);
    void wikiLinkActivated(const hn::editor::LinkRefInfo &link);                 // see setLinkClickActivates()
    void completionRequested(const hn::editor::CompletionRequest &req);         // see setCompletionTriggers()
    void completionAccepted(const hn::editor::CompletionItem &item);            // after the insertion was applied
    void completionDismissed();

private:
    void switchTo(Mode m, const QString &reason);
    void setVisualContent(const QString &md);
    void setSourceContent(const QString &text);
    void applyModeState(Mode m, const QString &text);
    QString roundTripProblem(const QString &md) const;
    QString decoded(const QByteArray &bytes) const;
    QByteArray encode(QString text) const;
    void mapCursor(int fromBlock, int fromBlocks, QTextDocument *to, QWidget *edit);
    void showContextMenu(QWidget *edit, const QPoint &p);
    void emitCursorInfo();
    void checkTrigger();
    bool eventFilter(QObject *o, QEvent *e) override;
    struct LinksImpl;
    LinkState stateOf(const QString &target) const;
    void evaluateCompletion(bool fromTyping);
    void linksTyped();
    void linksCaretMoved();
    void linksDocChanged(QTextDocument *d, int pos, int added);
    void linksRefreshHooks();
    void paintLinks(QPainter &p);
    bool activateAtCaret();
    LinksImpl *m_l = nullptr;   // null until the plugin API is used (raw: LinksImpl is private to note_editor_links.cpp)

    TriggerHandler m_trigger;
    bool m_inTrigger = false;
    EditRecorder m_rec;
    VisualEdit *m_vis;
    SourceEdit *m_src;
    QStackedLayout *m_stack;
    Mode m_mode = Mode::Visual;
    QString m_reason;
    bool m_invalid = false, m_loading = false, m_bom = false;
    int m_nl = 0;   // 0 LF, 1 CRLF, 2 CR
    QByteArray m_orig;
    int m_origRev = 0, m_savedRev = 0;
    hn::theme::Theme m_theme;
    qreal m_lh = 1.45;
};

} // namespace hn::editor
