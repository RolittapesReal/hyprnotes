#pragma once
#include <QString>
#include <QTextCursor>
#include <memory>
#include <QTextDocument>
namespace hn::editor {
// Private block properties (QTextBlockFormat user properties).
// kHrProp: bool, the block is a thematic break (empty text, painted by VisualEdit, exported as "---").
// kContProp: int list depth d >= 1, the block is a non-first child (paragraph/code/quote/heading/rule) of the
// list item at depth d that precedes it.
inline constexpr int kHrProp = QTextFormat::UserProperty + 1;
inline constexpr int kContProp = QTextFormat::UserProperty + 2;
// One list nesting level, on the 8px grid (QTextDocument::indentWidth and the kContProp margin step).
inline constexpr int kIndentPx = 24;
// Qt import/export dialect: CommonMark + strikethrough + task lists (MD4C flag values), raw HTML never produced.
QTextDocument::MarkdownFeatures mdFeatures();
void importMarkdown(QTextDocument *doc, const QString &md);
// Canonical export. Shift+Enter breaks (U+2028) are exported as CommonMark hard breaks ("\" + newline) because
// Qt's writer would emit a bare newline (soft break).
QString exportMarkdown(const QTextDocument *doc);
} // namespace hn::editor
