#include "hn/editor/wiki_links.h"

namespace hn::editor {

static constexpr int kMaxInner = 1024;   // same cap as hn::core::extractLinks

static bool skipped(const QList<QPair<int, int>> &s, int i)
{
    for (const auto &r : s) if (i >= r.first && i < r.second) return true;
    return false;
}

static void backtickSpans(const QString &t, QList<QPair<int, int>> &out)
{
    const int n = t.size();
    for (int i = 0; i < n;) {
        if (t[i] == u'\\') { i += 2; continue; }
        if (t[i] != u'`') { ++i; continue; }
        int run = 0;
        while (i + run < n && t[i + run] == u'`') ++run;
        int j = i + run;
        bool closed = false;
        while (j < n) {
            if (t[j] != u'`') { ++j; continue; }
            int r2 = 0;
            while (j + r2 < n && t[j + r2] == u'`') ++r2;
            if (r2 == run) { out.append({i, j + r2}); i = j + r2; closed = true; break; }
            j += r2;
        }
        if (!closed) i += run;
    }
}

QList<LinkRange> scanWikiLinks(const QString &text, const QList<QPair<int, int>> &skipIn, bool backticks, int line)
{
    QList<LinkRange> out;
    if (!text.contains(QLatin1String("[["))) return out;   // fast path: most blocks
    QList<QPair<int, int>> skip = skipIn;
    if (backticks) backtickSpans(text, skip);
    const int n = text.size();
    int i = 0;
    while ((i = text.indexOf(QLatin1String("[["), i)) >= 0) {
        int bs = 0;
        while (i - 1 - bs >= 0 && text[i - 1 - bs] == u'\\') ++bs;
        if ((bs & 1) || skipped(skip, i)) { i += 1; continue; }
        int j = i + 2;
        const int limit = qMin(n, j + kMaxInner + 2);
        while (j < limit && text[j] != u']' && text[j] != u'[' && text[j] != u'\n' && text[j] != QChar(0x2028)) ++j;
        if (j >= limit || text[j] != u']' || j + 1 >= n || text[j + 1] != u']') { i += 1; continue; }
        const int end = j + 2;
        int ta = i + 2, tb = j;
        auto ws = [&](int k) { return text[k] == u' ' || text[k] == u'\t'; };
        while (ta < tb && ws(ta)) ++ta;
        while (tb > ta && ws(tb - 1)) --tb;
        int bar = -1;
        for (int k = ta; k < tb; ++k) if (text[k] == u'|') { bar = k; break; }
        const int tEnd = bar < 0 ? tb : bar;
        int hash = -1;
        for (int k = ta; k < tEnd; ++k) if (text[k] == u'#') { hash = k; break; }
        int na = ta, nb = hash < 0 ? tEnd : hash;
        while (nb > na && ws(nb - 1)) --nb;
        if (nb <= na) { i = end; continue; }
        LinkRange r;
        const bool embed = i > 0 && text[i - 1] == u'!' && !skipped(skip, i - 1);
        r.start = embed ? i - 1 : i;
        r.end = end;
        r.targetStart = na;
        r.targetEnd = nb;
        r.ref.kind = embed ? LinkRefInfo::Kind::Embed : LinkRefInfo::Kind::Link;
        r.ref.target = text.mid(na, nb - na);
        r.ref.line = line;
        if (hash >= 0) {
            int aa = hash + 1, ab = tEnd;
            while (aa < ab && ws(aa)) ++aa;
            while (ab > aa && ws(ab - 1)) --ab;
            r.ref.anchor = text.mid(aa, ab - aa);
        }
        if (bar >= 0) {
            int aa = bar + 1;
            while (aa < tb && ws(aa)) ++aa;
            r.ref.alias = text.mid(aa, tb - aa);
        }
        out.append(std::move(r));
        i = end;
    }
    return out;
}

} // namespace hn::editor
