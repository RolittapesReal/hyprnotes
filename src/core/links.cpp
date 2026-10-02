#include "hn/core/links.h"
#include "hn/core/frontmatter.h"
#include "markdown_internal.h"
#include <QChar>
#include <algorithm>
#include <string>
#include <vector>

namespace hn::core {
namespace {

constexpr char kHole = '\x01';          // code/link-text/html/entity/span boundary: never part of a link
constexpr qsizetype kMaxInner = 1024;   // longest "[[...]]" body considered a link
constexpr qsizetype kCtx = 80;          // context bytes each side

struct Seg { qsizetype acc, src; };

struct Ctx {
    const char *whole = nullptr; // start of the full input
    qsizetype baseOff = 0, total = 0; // parsed region starts at baseOff (after BOM/frontmatter)
    int inCode = 0, inLink = 0;
    qsizetype bsPos = 0, bsRun = 0; // backslash-run cursor (parity of backslashes before a chunk)
    // inline accumulation for the current block
    std::string acc;
    std::vector<Seg> segs;
    NoteGraph out;
    // monotonic line cursor
    qsizetype curOff = 0;
    int curLine = 1;
    // tasks
    struct Open { TaskItem t; bool isTask = false, capturing = false; };
    std::vector<Open> tasks;

    int lineAt(qsizetype abs) {
        if (abs < curOff) { curOff = 0; curLine = 1; }
        const char *p = whole + curOff, *e = whole + abs;
        while (p < e) {
            const void *nl = memchr(p, '\n', size_t(e - p));
            if (!nl) break;
            ++curLine;
            p = static_cast<const char *>(nl) + 1;
        }
        curOff = abs;
        return curLine;
    }
    void hole() { if (acc.empty() || acc.back() != kHole) acc.push_back(kHole); }
    void add(const char *s, qsizetype n, qsizetype srcAbs) {
        if (srcAbs >= 0) segs.push_back({qsizetype(acc.size()), srcAbs});
        acc.append(s, size_t(n));
    }
    qsizetype srcOf(qsizetype a) const {
        auto it = std::upper_bound(segs.begin(), segs.end(), a, [](qsizetype v, const Seg &s) { return v < s.acc; });
        if (it == segs.begin()) return -1;
        --it;
        return it->src + (a - it->acc);
    }

    QString contextAround(qsizetype s, qsizetype e) const {
        qsizetype a = s, b = e;
        const char *w = whole;
        qsizetype lo = qMax<qsizetype>(0, s - kCtx), hi = qMin<qsizetype>(total, e + kCtx);
        while (a > lo && w[a - 1] != '\n') --a;
        while (b < hi && w[b] != '\n' && w[b] != '\r') ++b;
        while (a < s && (uchar(w[a]) & 0xC0) == 0x80) ++a; // do not start mid-codepoint
        while (b > e && b < total && (uchar(w[b]) & 0xC0) == 0x80) ++b;
        return QString::fromUtf8(w + a, qsizetype(b - a)).simplified();
    }

    void flush() {
        if (!acc.empty()) scan();
        acc.clear();
        segs.clear();
    }

    void scan() {
        const qsizetype n = qsizetype(acc.size());
        const char *d = acc.data();
        qsizetype i = 0;
        while (i + 1 < n) {
            const char *f = static_cast<const char *>(memchr(d + i, '[', size_t(n - i - 1)));
            if (!f) break;
            i = f - d;
            if (d[i + 1] != '[') { ++i; continue; }
            qsizetype j = i + 2;
            const qsizetype limit = qMin(n, j + kMaxInner + 2);
            while (j < limit && d[j] != ']' && d[j] != '[' && d[j] != kHole && d[j] != '\n') ++j;
            if (j >= limit || d[j] != ']' || j + 1 >= n || d[j + 1] != ']') { ++i; continue; }
            // inner = [i+2, j)
            bool embed = i > 0 && d[i - 1] == '!';
            qsizetype matchStart = embed ? i - 1 : i, matchEnd = j + 2;
            qsizetype innerA = i + 2, innerB = j;
            // trim
            qsizetype ta = innerA, tb = innerB;
            while (ta < tb && (d[ta] == ' ' || d[ta] == '\t')) ++ta;
            while (tb > ta && (d[tb - 1] == ' ' || d[tb - 1] == '\t')) --tb;
            qsizetype bar = -1;
            for (qsizetype k = ta; k < tb; ++k) if (d[k] == '|') { bar = k; break; }
            qsizetype tEnd = bar < 0 ? tb : bar;
            qsizetype hashp = -1;
            for (qsizetype k = ta; k < tEnd; ++k) if (d[k] == '#') { hashp = k; break; }
            qsizetype nameEnd = hashp < 0 ? tEnd : hashp;
            qsizetype na = ta, nb = nameEnd;
            while (nb > na && (d[nb - 1] == ' ' || d[nb - 1] == '\t')) --nb;
            if (nb <= na) { i = matchEnd; continue; } // "[[#h]]" / "[[|a]]" / "[[ ]]": no target
            qsizetype s0 = srcOf(matchStart), s1 = srcOf(matchEnd - 1), sT0 = srcOf(na), sT1 = srcOf(nb - 1);
            // The whole match must map to contiguous source bytes identical to what we scanned.
            if (s0 < 0 || s1 < 0 || sT0 < 0 || sT1 < 0 || s1 - s0 != matchEnd - 1 - matchStart ||
                memcmp(whole + s0, d + matchStart, size_t(matchEnd - matchStart)) != 0) { i = matchEnd; continue; }
            LinkRef r;
            r.kind = embed ? LinkRef::Embed : LinkRef::Link;
            r.target = QString::fromUtf8(d + na, nb - na);
            if (hashp >= 0) {
                qsizetype aa = hashp + 1, ab = tEnd;
                while (aa < ab && (d[aa] == ' ' || d[aa] == '\t')) ++aa;
                while (ab > aa && (d[ab - 1] == ' ' || d[ab - 1] == '\t')) --ab;
                r.anchor = QString::fromUtf8(d + aa, ab - aa);
            }
            if (bar >= 0) {
                qsizetype aa = bar + 1, ab = tb;
                while (aa < ab && (d[aa] == ' ' || d[aa] == '\t')) ++aa;
                r.alias = QString::fromUtf8(d + aa, ab - aa);
            }
            r.start = s0; r.end = s1 + 1; r.targetStart = sT0; r.targetEnd = sT1 + 1;
            r.line = lineAt(s0);
            r.context = contextAround(s0, s1 + 1);
            out.links.append(std::move(r));
            i = matchEnd;
        }
    }
};

int eb(MD_BLOCKTYPE t, void *d, void *u) {
    auto *c = static_cast<Ctx *>(u);
    c->flush();
    if (t == MD_BLOCK_LI) {
        auto *li = static_cast<MD_BLOCK_LI_DETAIL *>(d);
        Ctx::Open o;
        o.isTask = li->is_task;
        o.capturing = li->is_task;
        if (li->is_task) {
            o.t.done = li->task_mark != ' ';
            o.t.line = c->lineAt(c->baseOff + qsizetype(li->task_mark_offset));
        }
        c->tasks.push_back(std::move(o));
    } else if ((t == MD_BLOCK_UL || t == MD_BLOCK_OL || t == MD_BLOCK_QUOTE || t == MD_BLOCK_CODE || t == MD_BLOCK_HTML) && !c->tasks.empty())
        c->tasks.back().capturing = false;
    return 0;
}
int lb(MD_BLOCKTYPE t, void *, void *u) {
    auto *c = static_cast<Ctx *>(u);
    c->flush();
    if (t == MD_BLOCK_P && !c->tasks.empty()) c->tasks.back().capturing = false;
    if (t == MD_BLOCK_LI && !c->tasks.empty()) {
        Ctx::Open o = std::move(c->tasks.back());
        c->tasks.pop_back();
        if (o.isTask) {
            o.t.text = o.t.text.simplified();
            c->out.tasks.append(std::move(o.t));
        }
    }
    return 0;
}
int es(MD_SPANTYPE t, void *, void *u) {
    auto *c = static_cast<Ctx *>(u);
    c->hole();
    if (t == MD_SPAN_CODE) ++c->inCode;
    else if (t == MD_SPAN_A || t == MD_SPAN_IMG) ++c->inLink;
    return 0;
}
int ls(MD_SPANTYPE t, void *, void *u) {
    auto *c = static_cast<Ctx *>(u);
    c->hole();
    if (t == MD_SPAN_CODE) --c->inCode;
    else if (t == MD_SPAN_A || t == MD_SPAN_IMG) --c->inLink;
    return 0;
}
int tx(MD_TEXTTYPE t, const MD_CHAR *s, MD_SIZE n, void *u) {
    auto *c = static_cast<Ctx *>(u);
    if (!c->tasks.empty() && c->tasks.back().capturing) {
        auto &o = c->tasks.back();
        switch (t) {
        case MD_TEXT_SOFTBR: case MD_TEXT_BR: o.t.text += ' '; break;
        case MD_TEXT_NULLCHAR: o.t.text += QChar::ReplacementCharacter; break;
        case MD_TEXT_ENTITY: o.t.text += detail::decodeEntity(s, n); break;
        case MD_TEXT_HTML: break;
        default: o.t.text += detail::u8(s, n); break;
        }
    }
    switch (t) {
    case MD_TEXT_SOFTBR: case MD_TEXT_BR: c->acc.push_back('\n'); return 0;
    case MD_TEXT_NORMAL: break;
    default: c->hole(); return 0;
    }
    if (c->inCode || c->inLink) { c->hole(); return 0; }
    qsizetype abs = (s >= c->whole && s + n <= c->whole + c->total) ? qsizetype(s - c->whole) : -1;
    if (abs > 0) { // md4c emits an escaped punctuation char as its own chunk: neutralise it
        if (abs < c->bsPos) { c->bsPos = 0; c->bsRun = 0; }
        for (; c->bsPos < abs; ++c->bsPos) c->bsRun = c->whole[c->bsPos] == '\\' ? c->bsRun + 1 : 0; // linear overall
        if (c->bsRun & 1) {
            c->acc.push_back(kHole);
            if (n > 1) c->add(s + 1, n - 1, abs + 1);
            return 0;
        }
    }
    c->add(s, n, abs);
    return 0;
}
} // namespace

NoteGraph analyzeNote(const QByteArray &utf8) {
    try {
        Ctx c;
        c.whole = utf8.constData();
        c.total = utf8.size();
        qsizetype skip = 0;
        Frontmatter fm = parseFrontmatter(utf8);
        if (fm.present) skip = fm.endOffset;
        else if (utf8.startsWith("\xEF\xBB\xBF")) skip = 3;
        c.baseOff = skip;
        MD_PARSER p{};
        p.flags = MD_FLAG_STRIKETHROUGH | MD_FLAG_TASKLISTS;
        p.enter_block = eb; p.leave_block = lb; p.enter_span = es; p.leave_span = ls; p.text = tx;
        md_parse(c.whole + skip, MD_SIZE(utf8.size() - skip), &p, &c);
        c.flush();
        std::stable_sort(c.out.tasks.begin(), c.out.tasks.end(), [](const TaskItem &a, const TaskItem &b) { return a.line < b.line; });
        return std::move(c.out);
    } catch (...) { return {}; }
}

} // namespace hn::core
