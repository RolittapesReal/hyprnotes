#pragma once
// Plugin API v2, editor half: wiki-link overlay + completion popup value types (see NoteEditor for the API).
#include <QList>
#include <QPair>
#include <QRect>
#include <QString>

namespace hn::editor {

enum class LinkState { Resolved, Unresolved, Ambiguous };

// One `[[target]]`, `[[target|alias]]`, `[[target#anchor]]` or `![[target]]` as seen by the user.
struct LinkRefInfo {
    enum class Kind { Link, Embed };   // mirrors hn::core::LinkRef::Kind
    Kind kind = Kind::Link;
    QString target;    // trimmed text before '#'/'|'
    QString alias;     // text after the first '|', trimmed ("" if none)
    QString anchor;    // text after '#' in the target part, trimmed ("" if none)
    int line = 1;      // 1-based. Source mode: the line of the opening "[[". Visual mode: block index + 1 (blocks are not lines).
    bool operator==(const LinkRefInfo &) const = default;
};

// A link occurrence inside one text block. Offsets are UTF-16 units relative to the block start; [start,end) covers the
// whole "[[...]]" / "![[...]]" text, [targetStart,targetEnd) only the target.
struct LinkRange {
    int start = 0, end = 0, targetStart = 0, targetEnd = 0;
    LinkRefInfo ref;
    LinkState state = LinkState::Resolved;   // filled by NoteEditor::linkRangesInBlock (resolver), not by scanWikiLinks
};

// Pure scanner (same grammar as hn::core::extractLinks on one line: no '[' or ']' inside, body <= 1024 chars, a target is
// required, a backslash before "[[" escapes it). `skip` are code/markdown-link ranges [a,b) to ignore; with `backticks`
// inline `code spans` of `text` are skipped as well (source mode). `line` is stored in each ref.
QList<LinkRange> scanWikiLinks(const QString &text, const QList<QPair<int, int>> &skip = {}, bool backticks = false, int line = 1);

struct CompletionRequest {
    QString triggerId;     // the trigger string that matched, e.g. "[[" or "/"
    QString query;         // text typed after the trigger (spaces allowed for "[[" only)
    QRect caretRect;       // caret rectangle in GLOBAL coordinates (anchor for your own UI)
    bool atLineStart = false;   // only whitespace precedes the trigger on its line/block
    int generation = 0;    // pass back to NoteEditor::showCompletions() so late replies for an older query are dropped
};

struct CompletionItem {
    QString label;         // shown (match highlighted)
    QString detail;        // optional muted text, right aligned
    QString insert;        // REPLACES trigger+query (so include "[[" and "]]" yourself); plain text, no Markdown parsing
    int cursorOffset = -1; // caret position inside `insert` (UTF-16 units); -1 = after it
    bool markdown = false; // insert through the Markdown importer (visual mode) instead of as plain text
};

} // namespace hn::editor
