#pragma once
#include <QByteArray>
#include <QMap>
#include <QString>
#include <QStringList>

namespace hn::core {

struct FrontmatterValue {
    bool isList = false;
    QStringList items; // scalar => exactly one item
    QString scalar() const { return items.isEmpty() ? QString() : items.first(); }
};

// Read-only view of a leading `---` YAML block (small strict subset). Never rewritten by the app.
struct Frontmatter {
    bool present = false;
    QString title;                          // scalar `title`, else empty
    QStringList aliases;                    // scalar / inline list / block list, unique, trimmed
    QStringList tags;                       // lowercase, no leading '#', unique ('/' allowed)
    QMap<QString, FrontmatterValue> fields; // every key (lowercased), including title/aliases/tags
    qsizetype endOffset = 0;                // bytes occupied by BOM + block (closing delimiter line included)
    int endLine = 0;                        // number of lines the block occupies (0 when absent)
};

constexpr qsizetype kFrontmatterMaxBytes = 16 * 1024;

// Subset: `key: scalar`, `key: [a, "b c"]`, `key:` + `- item` lines, '...'/"..." quotes, `# comments`,
// CRLF, UTF-8 BOM. Anything else (nested maps, block scalars, flow maps, unterminated quotes, invalid
// UTF-8, no closing `---`/`...` within 16 KiB) => present=false. Never throws.
Frontmatter parseFrontmatter(const QByteArray &bytes);

} // namespace hn::core
