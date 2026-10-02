#pragma once
#include <QByteArray>
#include <QList>
#include <QString>

namespace hn::core {

// One `[[target]]`, `[[target|alias]]`, `[[target#anchor]]` or `![[target]]` occurrence in prose
// (never inside code spans/fences, markdown link text, raw HTML, or after an escaping backslash).
struct LinkRef {
    enum Kind { Link, Embed };
    Kind kind = Link;
    QString target;  // trimmed text before '#'/'|' (e.g. "Notes/Alpha" or "alpha.md")
    QString alias;   // text after the first '|', trimmed (empty if none)
    QString anchor;  // text after '#' in the target part, trimmed (empty if none)
    int line = 1;    // 1-based line of the opening "[["
    QString context; // the surrounding source line, simplified, <= ~160 chars, centred on the link
    // Byte offsets into the input (UTF-8): whole "[[...]]"/"![[...]]", and the target text only.
    qsizetype start = 0, end = 0, targetStart = 0, targetEnd = 0;
    QString kindName() const { return kind == Embed ? "embed" : "link"; }
};

// '- [ ] text' / '- [x] text' list items outside code (first paragraph of the item as text).
struct TaskItem {
    int line = 1; // 1-based line of the checkbox
    bool done = false;
    QString text;
};

struct NoteGraph {
    QList<LinkRef> links;
    QList<TaskItem> tasks;
};

// One md4c pass; a leading frontmatter block and BOM are skipped (line numbers stay absolute).
// Linear in input size (100k links in one line is fine). Never throws.
NoteGraph analyzeNote(const QByteArray &utf8);
inline QList<LinkRef> extractLinks(const QByteArray &utf8) { return analyzeNote(utf8).links; }
inline QList<LinkRef> extractLinks(const QString &markdown) { return analyzeNote(markdown.toUtf8()).links; }
inline QList<TaskItem> extractTasks(const QByteArray &utf8) { return analyzeNote(utf8).tasks; }

} // namespace hn::core
