#include "hn/core/frontmatter.h"
#include "hn/core/markdown_codec.h"
#include <QChar>
#include <QRegularExpression>

namespace hn::core {
namespace {

constexpr int kMaxFields = 256, kMaxItems = 256;

bool isTagChar(uint c) { return QChar::isLetterOrNumber(c) || QChar::isMark(c) || c == '_' || c == '-' || c == '/'; }

// Parses one scalar starting at s[i] (quoted or plain). Plain scalars stop at ',' / ']' when inFlow.
// Returns false on malformed input; leaves i just past the scalar.
bool scalar(const QString &s, qsizetype &i, bool inFlow, QString *out) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    out->clear();
    if (i < s.size() && (s[i] == '"' || s[i] == '\'')) {
        const QChar q = s[i++];
        for (;;) {
            if (i >= s.size()) return false; // unterminated
            QChar c = s[i++];
            if (c == q) {
                if (q == '\'' && i < s.size() && s[i] == '\'') { *out += '\''; ++i; continue; }
                break;
            }
            if (q == '"' && c == '\\') {
                if (i >= s.size()) return false;
                QChar e = s[i++];
                switch (e.unicode()) {
                case 'n': *out += '\n'; break;
                case 't': *out += '\t'; break;
                case '"': case '\\': case '/': *out += e; break;
                default: return false;
                }
            } else *out += c;
        }
        return true;
    }
    qsizetype st = i, endv = -1;
    while (i < s.size()) {
        QChar c = s[i];
        if (inFlow && (c == ',' || c == ']')) break;
        if (inFlow && (c == '[' || c == '{')) return false;
        if (c == '#' && i > st && (s[i - 1] == ' ' || s[i - 1] == '\t')) { endv = i; i = s.size(); break; } // comment
        ++i;
    }
    *out = s.mid(st, (endv >= 0 ? endv : i) - st).trimmed();
    if (inFlow && i >= s.size()) return false; // unclosed flow list (checked by caller too)
    if (!out->isEmpty() && (out->startsWith('|') || out->startsWith('>') || out->startsWith('{') || out->startsWith('&') || out->startsWith('*') || out->startsWith('!')))
        return false;
    return true;
}

// Rest-of-line must be empty or a comment.
bool onlyTrailing(const QString &s, qsizetype i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return i >= s.size() || s[i] == '#';
}

bool inlineList(const QString &s, qsizetype i, QStringList *out) {
    ++i; // '['
    for (;;) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
        if (i >= s.size()) return false;
        if (s[i] == ']') break;
        QString v;
        if (!scalar(s, i, true, &v)) return false;
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
        if (i >= s.size()) return false;
        if (!v.isEmpty()) out->append(v);
        if (s[i] == ',') { ++i; continue; }
        if (s[i] != ']') return false;
        break;
    }
    return onlyTrailing(s, i + 1) && out->size() <= kMaxItems;
}

void addUnique(QStringList &l, const QString &v) { if (!v.isEmpty() && !l.contains(v)) l << v; }

Frontmatter parseImpl(const QByteArray &bytes) {
    Frontmatter none;
    qsizetype pos = 0;
    if (bytes.startsWith("\xEF\xBB\xBF")) pos = 3;
    const qsizetype limit = qMin(bytes.size(), pos + kFrontmatterMaxBytes + 8);
    // Split lines lazily, bounded by the cap.
    auto nextLine = [&](qsizetype &p, QByteArray *line) {
        if (p >= limit) return false;
        qsizetype nl = bytes.indexOf('\n', p);
        qsizetype end = (nl < 0 || nl >= limit) ? limit : nl;
        *line = bytes.mid(p, end - p);
        if (line->endsWith('\r')) line->chop(1);
        p = (nl < 0 || nl >= limit) ? limit : nl + 1;
        return true;
    };
    QByteArray line;
    qsizetype p = pos;
    if (!nextLine(p, &line)) return none;
    {
        QByteArray t = line.trimmed();
        if (t != "---" || !line.startsWith("---")) return none;
    }
    const qsizetype bodyStart = p;
    int lines = 1;
    qsizetype closeEnd = -1;
    QList<QByteArray> body;
    while (nextLine(p, &line)) {
        ++lines;
        if (line.startsWith("---") || line.startsWith("...")) {
            if (line.trimmed() == line.left(3)) { closeEnd = p; break; }
        }
        body.append(line);
    }
    if (closeEnd < 0 || closeEnd - pos > kFrontmatterMaxBytes) return none;
    (void)bodyStart;

    Frontmatter fm;
    QString curKey;
    bool awaitingBlock = false; // `key:` with empty value seen; accepts '- item' lines
    for (const QByteArray &raw : body) {
        if (raw.contains('\0') || !isValidUtf8(raw)) return none;
        const QString s = QString::fromUtf8(raw);
        const QString tr = s.trimmed();
        if (tr.isEmpty() || tr.startsWith('#')) continue;
        if (tr.startsWith("- ") || tr == "-") {
            if (!awaitingBlock) return none;
            QString v;
            qsizetype i = s.indexOf('-') + 1;
            if (!scalar(s, i, false, &v) || !onlyTrailing(s, i)) return none;
            auto &fv = fm.fields[curKey];
            fv.isList = true;
            if (fv.items.size() >= kMaxItems) return none;
            if (!v.isEmpty()) fv.items << v;
            continue;
        }
        if (s[0] == ' ' || s[0] == '\t') return none; // nested structure: unsupported
        qsizetype colon = s.indexOf(':');
        if (colon <= 0) return none;
        QString key = s.left(colon).trimmed();
        if (key.isEmpty() || key.size() > 128) return none;
        for (QChar c : key) if (!(c.isLetterOrNumber() || c == '_' || c == '-' || c == '.' || c == ' ')) return none;
        if (colon + 1 < s.size() && s[colon + 1] != ' ' && s[colon + 1] != '\t') return none; // "a:b" is not a pair
        key = key.toLower();
        if (fm.fields.size() >= kMaxFields && !fm.fields.contains(key)) return none;
        FrontmatterValue fv;
        qsizetype i = colon + 1;
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
        awaitingBlock = false;
        if (i >= s.size() || s[i] == '#') {
            awaitingBlock = true; // may turn into a block list; otherwise an empty scalar
            fv.items << QString();
        } else if (s[i] == '[') {
            fv.isList = true;
            if (!inlineList(s, i, &fv.items)) return none;
        } else {
            QString v;
            if (!scalar(s, i, false, &v) || !onlyTrailing(s, i)) return none;
            fv.items << v;
        }
        curKey = key;
        fm.fields[key] = fv;
    }
    // `key:` that got no block items stays a scalar "" (items == [""]); block lists drop the placeholder.
    for (auto it = fm.fields.begin(); it != fm.fields.end(); ++it)
        if (it->isList && it->items.size() > 1 && it->items.first().isEmpty()) it->items.removeFirst();
    // The placeholder is replaced when block items arrive: handle the first '- item' case.
    for (auto it = fm.fields.begin(); it != fm.fields.end(); ++it)
        if (it->isList && it->items.size() == 1 && it->items.first().isEmpty()) it->items.clear();

    const auto t = fm.fields.value("title");
    if (!t.isList) fm.title = t.scalar().simplified();
    for (const QString &a : fm.fields.value("aliases").items) addUnique(fm.aliases, a.simplified());
    for (QString raw : fm.fields.value("tags").items) {
        // scalar `tags: a, b c` => split on commas/whitespace; inline/block items are taken whole
        QStringList parts = fm.fields.value("tags").isList ? QStringList{raw} : raw.split(QRegularExpression("[,\\s]+"), Qt::SkipEmptyParts);
        for (QString tg : parts) {
            tg = tg.trimmed();
            if (tg.startsWith('#')) tg.remove(0, 1);
            tg = tg.toLower();
            bool ok = !tg.isEmpty();
            for (uint c : tg.toUcs4()) ok = ok && isTagChar(c);
            if (ok) addUnique(fm.tags, tg);
        }
    }
    fm.present = true;
    fm.endOffset = closeEnd;
    fm.endLine = lines;
    return fm;
}

} // namespace

Frontmatter parseFrontmatter(const QByteArray &bytes) {
    try { return parseImpl(bytes); } catch (...) { return {}; }
}

} // namespace hn::core
