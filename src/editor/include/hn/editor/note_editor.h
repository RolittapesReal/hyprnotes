#pragma once
#include <QByteArray>
#include <QPlainTextEdit>
#include <QTextEdit>
#include <QUrl>
#include <QWidget>
#include <functional>
#include <optional>

#include "hn/editor/history.h"
#include "hn/editor/types.h"
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
