#include "hn/core/markdown_codec.h"
#include "markdown_internal.h"
#include <QRegularExpression>
#include <md4c.h>

namespace hn::core {
namespace detail {
QString attrText(const MD_ATTRIBUTE &a) {
    QString r;
    if (!a.text || !a.size) return r;
    for (int i = 0; a.substr_offsets[i] < a.size; ++i) {
        const MD_OFFSET o = a.substr_offsets[i], e = a.substr_offsets[i + 1];
        if (a.substr_types[i] == MD_TEXT_ENTITY) r += decodeEntity(a.text + o, e - o);
        else if (a.substr_types[i] == MD_TEXT_NULLCHAR) r += QChar(QChar::ReplacementCharacter);
        else r += u8(a.text + o, e - o);
    }
    return r;
}
QString decodeEntity(const MD_CHAR *text, MD_SIZE size) {
    QString e = u8(text, size); // "&...;"
    if (e.size() >= 4 && e[1] == '#') {
        bool ok = false;
        uint cp = (e[2] == 'x' || e[2] == 'X') ? e.mid(3, e.size() - 4).toUInt(&ok, 16)
                                                : e.mid(2, e.size() - 3).toUInt(&ok, 10);
        if (ok && cp && QChar::isNonCharacter(cp) == false && cp <= 0x10FFFF && !(cp >= 0xD800 && cp < 0xE000))
            return QString::fromUcs4(reinterpret_cast<const char32_t *>(&cp), 1);
        return QString(QChar::ReplacementCharacter);
    }
    static const QHash<QString, QString> named{{"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""},
                                               {"&apos;", "'"}, {"&nbsp;", QString(QChar(0xA0))}};
    return named.value(e, e);
}
} // namespace detail

bool isValidUtf8(const QByteArray &b) {
    const auto *p = reinterpret_cast<const uchar *>(b.constData());
    const auto *end = p + b.size();
    while (p < end) {
        uchar c = *p;
        int n;
        uint cp;
        if (c < 0x80) { ++p; continue; }
        else if (c >= 0xC2 && c <= 0xDF) { n = 1; cp = c & 0x1F; }
        else if (c >= 0xE0 && c <= 0xEF) { n = 2; cp = c & 0x0F; }
        else if (c >= 0xF0 && c <= 0xF4) { n = 3; cp = c & 0x07; }
        else return false;
        if (end - p <= n) return false;
        for (int i = 1; i <= n; ++i) {
            if ((p[i] & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (p[i] & 0x3F);
        }
        if ((n == 2 && cp < 0x800) || (n == 3 && (cp < 0x10000 || cp > 0x10FFFF)) || (cp >= 0xD800 && cp <= 0xDFFF))
            return false;
        p += n + 1;
    }
    return true;
}

namespace {
using detail::u8;

struct ClassCtx {
    QString reason, text, linkText;
    bool inLink = false; // text: ordinary (non-code) text of the current block; MD4C splits it at '['
    void hit(const char *r) { if (reason.isEmpty()) reason = QString::fromLatin1(r); }
    void flush() {
        static const QRegularExpression fn(R"(\[\^[^\]\s]+\])");
        if (fn.match(text).hasMatch()) hit("Contains footnotes");
        text.clear();
    }
};

int cEnterBlock(MD_BLOCKTYPE t, void *, void *u) {
    auto *c = static_cast<ClassCtx *>(u);
    c->flush();
    if (t == MD_BLOCK_TABLE) c->hit("Contains a table");
    else if (t == MD_BLOCK_HTML) c->hit("Contains raw HTML");
    return 0;
}
int cLeaveBlock(MD_BLOCKTYPE, void *, void *u) { static_cast<ClassCtx *>(u)->flush(); return 0; }
int cEnterSpan(MD_SPANTYPE t, void *, void *u) {
    auto *c = static_cast<ClassCtx *>(u);
    if (t == MD_SPAN_IMG) c->hit("Contains an image");
    else if (t == MD_SPAN_LATEXMATH || t == MD_SPAN_LATEXMATH_DISPLAY) c->hit("Contains math");
    else if (t == MD_SPAN_A) { c->inLink = true; c->linkText.clear(); }
    return 0;
}
int cLeaveSpan(MD_SPANTYPE t, void *, void *u) {
    auto *c = static_cast<ClassCtx *>(u);
    if (t == MD_SPAN_A) {
        c->inLink = false;
        // "[^1]" with a matching "[^1]: ..." definition is parsed as a shortcut reference link.
        if (c->linkText.startsWith('^')) c->hit("Contains footnotes");
    }
    return 0;
}
int cText(MD_TEXTTYPE t, const MD_CHAR *s, MD_SIZE n, void *u) {
    auto *c = static_cast<ClassCtx *>(u);
    if (t == MD_TEXT_HTML) c->hit("Contains raw HTML");
    else if (t == MD_TEXT_NORMAL) (c->inLink ? c->linkText : c->text) += u8(s, n);
    return 0;
}

bool hasFrontMatter(const QByteArray &b) {
    QByteArray s = b.startsWith("\xEF\xBB\xBF") ? b.mid(3) : b;
    if (!(s.startsWith("---\n") || s.startsWith("---\r\n"))) return false;
    static const QRegularExpression close(R"((^|\n)(---|\.\.\.)[ \t]*\r?(\n|$))");
    return close.match(QString::fromUtf8(s.mid(3))).hasMatch();
}

// ---- semantic tokens ----
struct TokCtx {
    QStringList out;
    QString text, textSig;
    QString kind = "T";
    // Inline spans are a canonical *set* of active marks per text run (nesting order and span boundaries are not
    // significant: `[**a**](u)` == `**[a](u)**`), so the editor's flat character formats compare equal.
    QStringList active;
    // Block stack: the first paragraph of a list item is dropped so tight and loose items compare equal
    // (the editor cannot represent looseness; spec 5.3 canonicalization).
    QList<int> bstack, kids;
    QList<bool> dropped;
    QString sig() const { QStringList s = active; s.sort(); return s.join(QLatin1Char(',')); }
    void flush() { if (!text.isEmpty()) { out << kind + "[" + textSig + "]:" + text; text.clear(); } }
    void run(const QString &t, const QString &g) { if (g != textSig) { flush(); textSig = g; } text += t; }
    // Spaces/tabs outside code spans carry no marks: serializers must keep delimiters off whitespace, so
    // emphasis on boundary whitespace is not significant.
    void add(const QString &t) {
        const QString g = sig();
        if (active.contains("CODE")) { run(t, g); return; }
        int i = 0;
        while (i < t.size()) {
            const bool ws = t[i] == QLatin1Char(' ') || t[i] == QLatin1Char('\t');
            int j = i;
            while (j < t.size() && (t[j] == QLatin1Char(' ') || t[j] == QLatin1Char('\t')) == ws) ++j;
            run(t.mid(i, j - i), ws ? QString() : g);
            i = j;
        }
    }
    void push(const QString &s) { flush(); out << s; }
};

QString attr(const MD_ATTRIBUTE &a) { return detail::attrText(a); }

int tEnterBlock(MD_BLOCKTYPE t, void *d, void *u) {
    auto *c = static_cast<TokCtx *>(u);
    const bool drop = t == MD_BLOCK_P && !c->bstack.isEmpty() && c->bstack.last() == MD_BLOCK_LI && c->kids.last() == 0;
    if (!c->kids.isEmpty()) ++c->kids.last();
    c->bstack << int(t); c->kids << 0; c->dropped << drop;
    if (drop) return 0;
    switch (t) {
    case MD_BLOCK_DOC: break;
    case MD_BLOCK_QUOTE: c->push("+QUOTE"); break;
    case MD_BLOCK_UL: c->push("+UL"); break;
    case MD_BLOCK_OL: c->push("+OL:" + QString::number(static_cast<MD_BLOCK_OL_DETAIL *>(d)->start)); break;
    case MD_BLOCK_LI: {
        auto *li = static_cast<MD_BLOCK_LI_DETAIL *>(d);
        c->push(li->is_task ? (li->task_mark == ' ' ? "+LI[ ]" : "+LI[x]") : "+LI");
        break;
    }
    case MD_BLOCK_HR: c->push("HR"); break;
    case MD_BLOCK_H: c->push("+H" + QString::number(static_cast<MD_BLOCK_H_DETAIL *>(d)->level)); break;
    case MD_BLOCK_CODE: c->push("+CODE:" + attr(static_cast<MD_BLOCK_CODE_DETAIL *>(d)->lang)); c->kind = "C"; break;
    case MD_BLOCK_HTML: c->push("+HTML"); c->kind = "C"; break;
    case MD_BLOCK_P: c->push("+P"); break;
    default: c->push("+BLOCK"); break;
    }
    return 0;
}
int tLeaveBlock(MD_BLOCKTYPE t, void *, void *u) {
    auto *c = static_cast<TokCtx *>(u);
    c->bstack.removeLast(); c->kids.removeLast();
    if (c->dropped.takeLast()) return 0;
    if (t == MD_BLOCK_DOC) { c->flush(); return 0; }
    c->push("-" + QString::number(int(t)));
    c->kind = "T";
    return 0;
}
int tEnterSpan(MD_SPANTYPE t, void *d, void *u) {
    auto *c = static_cast<TokCtx *>(u);
    switch (t) {
    case MD_SPAN_EM: c->active << "EM"; break;
    case MD_SPAN_STRONG: c->active << "STRONG"; break;
    case MD_SPAN_DEL: c->active << "DEL"; break;
    case MD_SPAN_CODE: c->active << "CODE"; break;
    case MD_SPAN_A: {
        auto *a = static_cast<MD_SPAN_A_DETAIL *>(d);
        c->active << "A:" + attr(a->href) + "|" + attr(a->title);
        break;
    }
    case MD_SPAN_IMG: {
        auto *a = static_cast<MD_SPAN_IMG_DETAIL *>(d);
        c->push("+IMG:" + attr(a->src) + "|" + attr(a->title));
        break;
    }
    default: c->push("+SPAN"); break;
    }
    return 0;
}
int tLeaveSpan(MD_SPANTYPE t, void *, void *u) {
    auto *c = static_cast<TokCtx *>(u);
    switch (t) {
    case MD_SPAN_EM: c->active.removeOne("EM"); break;
    case MD_SPAN_STRONG: c->active.removeOne("STRONG"); break;
    case MD_SPAN_DEL: c->active.removeOne("DEL"); break;
    case MD_SPAN_CODE: c->active.removeOne("CODE"); break;
    case MD_SPAN_A:
        for (int i = c->active.size() - 1; i >= 0; --i) if (c->active[i].startsWith("A:")) { c->active.removeAt(i); break; }
        break;
    default: c->push("-SPAN" + QString::number(int(t))); break;
    }
    return 0;
}
int tText(MD_TEXTTYPE t, const MD_CHAR *s, MD_SIZE n, void *u) {
    auto *c = static_cast<TokCtx *>(u);
    switch (t) {
    case MD_TEXT_SOFTBR: c->add(QStringLiteral(" ")); break;   // wrap positions are not significant
    case MD_TEXT_BR: c->push("BR"); break;
    case MD_TEXT_NULLCHAR: c->add(QString(QChar(QChar::ReplacementCharacter))); break;
    case MD_TEXT_ENTITY: c->add(detail::decodeEntity(s, n)); break;
    default: c->add(u8(s, n)); break;
    }
    return 0;
}

template <class Ctx>
bool parse(const QByteArray &utf8, unsigned flags, Ctx *ctx, MD_PARSER p) {
    p.abi_version = 0;
    p.flags = flags;
    p.debug_log = nullptr;
    p.syntax = nullptr;
    return md_parse(utf8.constData(), MD_SIZE(utf8.size()), &p, ctx) == 0;
}
} // namespace

MarkdownClass classifyMarkdown(const QByteArray &utf8, qsizetype visualLimit) {
    if (!isValidUtf8(utf8)) return {false, "File is not valid UTF-8"};
    if (utf8.size() > visualLimit) return {false, QString("Note is larger than %1 KiB").arg(visualLimit / 1024)};
    if (hasFrontMatter(utf8)) return {false, "Contains front matter"};
    ClassCtx ctx;
    MD_PARSER p{};
    p.enter_block = cEnterBlock; p.leave_block = cLeaveBlock; p.enter_span = cEnterSpan;
    p.leave_span = cLeaveSpan; p.text = cText;
    if (!parse(utf8, MD_FLAG_TABLES | MD_FLAG_STRIKETHROUGH | MD_FLAG_TASKLISTS | MD_FLAG_LATEXMATHSPANS, &ctx, p))
        return {false, "Markdown could not be parsed"};
    if (!ctx.reason.isEmpty()) return {false, ctx.reason};
    return {};
}

QStringList semanticTokens(const QString &markdown) {
    TokCtx ctx;
    MD_PARSER p{};
    p.enter_block = tEnterBlock; p.leave_block = tLeaveBlock; p.enter_span = tEnterSpan;
    p.leave_span = tLeaveSpan; p.text = tText;
    if (!parse(markdown.toUtf8(), MD_FLAG_STRIKETHROUGH | MD_FLAG_TASKLISTS, &ctx, p))
        return {"PARSE_ERROR"};
    return ctx.out;
}

} // namespace hn::core
