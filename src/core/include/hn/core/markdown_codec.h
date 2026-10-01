// Pinned cross-module contract. Implemented in src/core/markdown_codec.cpp (MD4C callbacks).
#pragma once
#include <QByteArray>
#include <QString>
#include <QStringList>

namespace hn::core {

struct MarkdownClass {
    bool visual = true;   // false => whole note must open in source mode
    QString reason;       // human-readable, empty when visual
};

bool isValidUtf8(const QByteArray &bytes);

// Classifies raw file bytes: invalid UTF-8, size > visualLimit, or any unsupported construct
// (tables, images, raw HTML, front matter, footnotes, math) => visual=false with a reason.
MarkdownClass classifyMarkdown(const QByteArray &utf8, qsizetype visualLimit = 256 * 1024);

// Normalized semantic token stream (CommonMark + strikethrough + task lists): block/list structure,
// decoded text, task state, link targets, code contents, significant breaks. Two markdown strings
// are semantically equal iff their token lists are equal. Used for import/export round-trip checks.
QStringList semanticTokens(const QString &markdown);

} // namespace hn::core
