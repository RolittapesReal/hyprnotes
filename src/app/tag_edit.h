#pragma once
#include "hn/editor/note_editor.h"
#include <QStringList>

namespace hn::app {

// Tags are visible "#tag" tokens in the note text (spec 6.1). Additions are appended as a final
// "#a #b" paragraph; removals delete the tokens (outside monospace/code). One undo step.
// Returns the number of tokens removed.
int applyTagEdit(hn::editor::NoteEditor *ed, const QStringList &add, const QStringList &remove);
// "a b #c, d" -> {"a","b","c","d"} (lowercase, unique, valid tag characters only).
QStringList parseTagInput(const QString &text);

} // namespace hn::app
