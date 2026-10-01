#pragma once
#include "hn/editor/note_editor.h"
#include "hn/mods/mods.h"

namespace hn::app {

// Mod DocumentBridge over the editor's transaction recorder: one begin/end pair is one undo step
// and, because it goes through the normal edit path, it also triggers autosave.
class EditorBridge : public hn::mods::DocumentBridge {
public:
    explicit EditorBridge(hn::editor::NoteEditor *ed) : m_ed(ed) {}
    ~EditorBridge() override { endTransaction(); }
    QString selectionText() override;
    void beginTransaction(const QString &name) override;
    void replaceSelection(const QString &utf8) override;
    void insertText(const QString &utf8) override;
    void endTransaction() override;
private:
    QTextCursor cursor() const;
    void setCursor(const QTextCursor &c);
    hn::editor::NoteEditor *m_ed;
    hn::editor::EditRecorder::Scope m_scope;
};

} // namespace hn::app
