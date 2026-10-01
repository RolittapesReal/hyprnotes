#include "md_io.h"

#include <QGuiApplication>
#include <QHash>
#include <QPalette>
#include <QTextListFormat>
#include <md4c.h>
#include "markdown_internal.h"
#include <QRegularExpression>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextList>
#include <QVector>
#include <algorithm>

// Import uses Qt's bundled MD4C (QTextDocument::setMarkdown). Export is our own serializer: Qt's writer drops hard
// breaks, leaks list indentation into following code blocks, bolds everything when the default weight is not Normal,
// and adds stray blank lines, so it cannot satisfy the round-trip contract of spec 5.3.

namespace hn::editor {

QTextDocument::MarkdownFeatures mdFeatures()
{
    // MD_FLAG_STRIKETHROUGH = 0x0200, MD_FLAG_TASKLISTS = 0x0800 (Qt forwards the value to its bundled MD4C).
    return QTextDocument::MarkdownFeatures(0x0200 | 0x0800) | QTextDocument::MarkdownNoHTML;
}

namespace {

// Custom MD4C -> QTextDocument importer. Qt's own importer turns hard breaks into paragraph splits, which cannot
// round-trip. Here: hard break -> U+2028 inside the block; soft break -> one space (wrap positions are normalized,
// spec 5.3); loose list items collapse to tight; multi-block list items / nested quotes import in a shape the
// exporter cannot reproduce, so the round-trip check sends them to source mode.
struct Importer {
    struct L { QTextList *list = nullptr; bool ordered = false; int start = 1; };
    QTextDocument *doc;
    QTextCursor c;
    bool fresh = true, liFresh = false, inCode = false, inLink = false;
    int quote = 0, heading = 0, strong = 0, em = 0, del = 0, code = 0;
    QString lang, codeText, href, title;
    QList<L> lists;
    QTextCharFormat cur, blockCf;

    explicit Importer(QTextDocument *d) : doc(d), c(d) {}

    static QTextCharFormat codeFmt()
    {
        QTextCharFormat f;
        f.setFontFixedPitch(true);
        f.setFontFamilies({QStringLiteral("monospace")});
        f.setFontWeight(QFont::Normal);
        return f;
    }
    void refresh()
    {
        QTextCharFormat f = blockCf;
        if (strong) f.setFontWeight(heading ? QFont::ExtraBold : QFont::Bold);   // heading base is Bold; explicit bold is one step heavier
        if (em) f.setFontItalic(true);
        if (del) f.setFontStrikeOut(true);
        if (code) { f.setFontFixedPitch(true); f.setFontFamilies({QStringLiteral("monospace")}); f.setFontWeight(QFont::Normal); }
        if (inLink) {
            f.setAnchor(true);
            f.setAnchorHref(href);
            if (!title.isEmpty()) f.setToolTip(title);
            f.setForeground(QGuiApplication::palette().link());
        }
        cur = f;
    }
    // `cont`: the block is a non-first child of the enclosing list item (kContProp).
    QTextBlockFormat baseBf(bool cont = false) const
    {
        QTextBlockFormat bf;
        bf.setTopMargin(9);
        bf.setBottomMargin(9);
        int m = 0;
        if (quote) { bf.setProperty(QTextFormat::BlockQuoteLevel, quote); m += 16 * quote; }
        if (cont && !lists.isEmpty()) { bf.setProperty(kContProp, int(lists.size())); m += kIndentPx * int(lists.size()); }
        if (m) bf.setLeftMargin(m);
        return bf;
    }
    void startBlock(const QTextBlockFormat &bf, const QTextCharFormat &cf)
    {
        if (fresh) { c.setBlockFormat(bf); c.setBlockCharFormat(cf); fresh = false; }
        else c.insertBlock(bf, cf);
        blockCf = cf;
        refresh();
    }
    void text(const QString &t) { if (!t.isEmpty()) c.insertText(t, cur); }
    static QString attr(const MD_ATTRIBUTE &a) { return hn::core::detail::attrText(a); }

    void enterBlock(MD_BLOCKTYPE t, void *d)
    {
        switch (t) {
        case MD_BLOCK_DOC: case MD_BLOCK_QUOTE:
            if (t == MD_BLOCK_QUOTE) { ++quote; liFresh = false; }
            break;
        case MD_BLOCK_UL: case MD_BLOCK_OL: {
            liFresh = false;
            L l;
            l.ordered = t == MD_BLOCK_OL;
            if (l.ordered) l.start = int(static_cast<MD_BLOCK_OL_DETAIL *>(d)->start);
            lists << l;
            break;
        }
        case MD_BLOCK_LI: {
            auto *li = static_cast<MD_BLOCK_LI_DETAIL *>(d);
            QTextBlockFormat bf = baseBf();
            if (li->is_task) bf.setMarker(li->task_mark == ' ' ? QTextBlockFormat::MarkerType::Unchecked : QTextBlockFormat::MarkerType::Checked);
            startBlock(bf, QTextCharFormat());
            L &l = lists.last();
            if (!l.list) {
                QTextListFormat lf;
                lf.setStyle(l.ordered ? QTextListFormat::ListDecimal : QTextListFormat::ListDisc);
                lf.setIndent(int(lists.size()));
                if (l.ordered && l.start != 1) lf.setStart(l.start);
                l.list = c.createList(lf);
            } else {
                // ponytail: O(n) per add (Qt's QTextBlockGroup::markBlocksDirty walks every member): 10k-item list ~2 s, no cheaper Qt API found
                l.list->add(c.block());
            }
            liFresh = true;
            break;
        }
        case MD_BLOCK_H: {
            heading = int(static_cast<MD_BLOCK_H_DETAIL *>(d)->level);
            QTextBlockFormat bf = baseBf(true);
            bf.setHeadingLevel(heading);
            QTextCharFormat cf;
            cf.setFontWeight(QFont::Bold);
            cf.setProperty(QTextFormat::FontSizeAdjustment, qMax(0, 4 - heading));
            startBlock(bf, cf);
            liFresh = false;
            break;
        }
        case MD_BLOCK_CODE:
            inCode = true;
            codeText.clear();
            lang = attr(static_cast<MD_BLOCK_CODE_DETAIL *>(d)->lang);
            liFresh = false;
            break;
        case MD_BLOCK_HR: {
            QTextBlockFormat bf = baseBf(true);
            bf.setProperty(kHrProp, true);
            bf.setTopMargin(12);
            bf.setBottomMargin(12);
            QTextCharFormat cf;
            cf.setFontPointSize(2);   // thin empty block: VisualEdit paints the hairline through its middle
            startBlock(bf, cf);
            liFresh = false;
            break;
        }
        default:   // P, HTML, tables: a plain paragraph (unsupported ones fail the round-trip check)
            if (liFresh && t == MD_BLOCK_P) liFresh = false;   // first paragraph of a list item reuses the item block
            else { startBlock(baseBf(true), QTextCharFormat()); liFresh = false; }
            break;
        }
    }
    void leaveBlock(MD_BLOCKTYPE t)
    {
        switch (t) {
        case MD_BLOCK_QUOTE: --quote; break;
        case MD_BLOCK_LI: liFresh = false; break;
        case MD_BLOCK_UL: case MD_BLOCK_OL: lists.removeLast(); break;
        case MD_BLOCK_H: heading = 0; break;
        case MD_BLOCK_CODE: {
            QStringList lines = codeText.split(QLatin1Char('\n'));
            if (lines.size() > 1 && lines.last().isEmpty()) lines.removeLast();
            QTextBlockFormat bf = baseBf(true);
            bf.setTopMargin(0);
            bf.setBottomMargin(0);
            bf.setProperty(QTextFormat::BlockCodeFence, QStringLiteral("`"));
            if (!lang.isEmpty()) bf.setProperty(QTextFormat::BlockCodeLanguage, lang);
            bf.setNonBreakableLines(true);
            const QTextCharFormat cf = codeFmt();
            for (const QString &l : std::as_const(lines)) {
                startBlock(bf, cf);
                text(l);
            }
            inCode = false;
            break;
        }
        default: break;
        }
    }
    void enterSpan(MD_SPANTYPE t, void *d)
    {
        switch (t) {
        case MD_SPAN_EM: ++em; break;
        case MD_SPAN_STRONG: ++strong; break;
        case MD_SPAN_DEL: ++del; break;
        case MD_SPAN_CODE: ++code; break;
        case MD_SPAN_A: {
            auto *a = static_cast<MD_SPAN_A_DETAIL *>(d);
            href = attr(a->href);
            title = attr(a->title);
            inLink = true;   // an empty target stays an anchor with an empty href ("[x]()")
            break;
        }
        default: break;
        }
        refresh();
    }
    void leaveSpan(MD_SPANTYPE t)
    {
        switch (t) {
        case MD_SPAN_EM: --em; break;
        case MD_SPAN_STRONG: --strong; break;
        case MD_SPAN_DEL: --del; break;
        case MD_SPAN_CODE: --code; break;
        case MD_SPAN_A: href.clear(); title.clear(); inLink = false; break;
        default: break;
        }
        refresh();
    }
    void textEvent(MD_TEXTTYPE t, const MD_CHAR *s, MD_SIZE n)
    {
        QString str;
        switch (t) {
        case MD_TEXT_SOFTBR: str = inCode ? QStringLiteral("\n") : QStringLiteral(" "); break;
        case MD_TEXT_BR: str = QString(QChar(QChar::LineSeparator)); break;
        case MD_TEXT_NULLCHAR: str = QString(QChar(QChar::ReplacementCharacter)); break;
        case MD_TEXT_ENTITY: str = hn::core::detail::decodeEntity(s, n); break;
        default: str = hn::core::detail::u8(s, n); break;
        }
        if (inCode) codeText += str; else text(str);
    }
};

} // namespace

void importMarkdown(QTextDocument *doc, const QString &md)
{
    doc->clear();
    Importer im(doc);
    QTextCursor edit(doc);
    edit.beginEditBlock();
    MD_PARSER p{};
    p.abi_version = 0;
    p.flags = MD_FLAG_STRIKETHROUGH | MD_FLAG_TASKLISTS;
    p.enter_block = [](MD_BLOCKTYPE t, void *d, void *u) { static_cast<Importer *>(u)->enterBlock(t, d); return 0; };
    p.leave_block = [](MD_BLOCKTYPE t, void *, void *u) { static_cast<Importer *>(u)->leaveBlock(t); return 0; };
    p.enter_span = [](MD_SPANTYPE t, void *d, void *u) { static_cast<Importer *>(u)->enterSpan(t, d); return 0; };
    p.leave_span = [](MD_SPANTYPE t, void *, void *u) { static_cast<Importer *>(u)->leaveSpan(t); return 0; };
    p.text = [](MD_TEXTTYPE t, const MD_CHAR *s, MD_SIZE n, void *u) { static_cast<Importer *>(u)->textEvent(t, s, n); return 0; };
    const QByteArray utf8 = md.toUtf8();
    md_parse(utf8.constData(), MD_SIZE(utf8.size()), &p, &im);
    edit.endEditBlock();
}

namespace {

enum Mark { Link = 0, Strike = 1, Bold = 2, Italic = 3 };
struct Mk {
    int m;
    QString href;
    QString title = {};
    bool operator==(const Mk &o) const { return m == o.m && href == o.href && title == o.title; }
};
struct Piece {
    QString text;
    QList<Mk> marks;
    bool code = false;
};
bool isWs(QChar c) { return c == QLatin1Char(' ') || c == QLatin1Char('\t'); }

QString spaces(int n) { return QString(qMax(0, n), QLatin1Char(' ')); }

QString escapeRun(const QString &t)
{
    static const QRegularExpression entity(QStringLiteral("^&[A-Za-z0-9#]+;"));
    QString o;
    o.reserve(t.size() + 4);
    for (int k = 0; k < t.size(); ++k) {
        const QChar c = t[k];
        switch (c.unicode()) {
        case '\\': case '`': case '*': case '[': case ']': case '<': case '~': o += QLatin1Char('\\'); o += c; break;
        case '_':
            if (k > 0 && k + 1 < t.size() && t[k - 1].isLetterOrNumber() && t[k + 1].isLetterOrNumber()) o += c;
            else { o += QLatin1Char('\\'); o += c; }
            break;
        case '&':
            if (entity.match(t.mid(k, 40)).hasMatch()) o += QLatin1Char('\\');
            o += c;
            break;
        default: o += c;
        }
    }
    return o;
}

// `t` is already run-escaped; add the extra escapes that only matter at the start of a line.
QString escapeLineStart(const QString &t)
{
    static const QRegularExpression head(QStringLiteral("^( {0,3})(#{1,6})(?=\\s|$)")), quote(QStringLiteral("^( {0,3})>")),
        bullet(QStringLiteral("^( {0,3})([-+])(?=\\s|$)")), ord(QStringLiteral("^( {0,3})(\\d{1,9})([.)])(?=\\s|$)")),
        rule(QStringLiteral("^( {0,3})([-=])(?=[-=]*\\s*$)"));
    QString r = t;
    if (auto m = head.match(r); m.hasMatch()) r.insert(m.capturedEnd(1), QLatin1Char('\\'));
    else if (auto m2 = quote.match(r); m2.hasMatch()) r.insert(m2.capturedEnd(1), QLatin1Char('\\'));
    else if (auto m3 = bullet.match(r); m3.hasMatch()) r.insert(m3.capturedEnd(1), QLatin1Char('\\'));
    else if (auto m4 = ord.match(r); m4.hasMatch()) r.insert(m4.capturedStart(3), QLatin1Char('\\'));
    else if (auto m5 = rule.match(r); m5.hasMatch()) r.insert(m5.capturedEnd(1), QLatin1Char('\\'));
    return r;
}

QString codeSpan(const QString &raw)
{
    int longest = 0, run = 0;
    for (QChar c : raw) { run = c == QLatin1Char('`') ? run + 1 : 0; longest = qMax(longest, run); }
    const QString fence(longest + 1, QLatin1Char('`'));
    const bool pad = raw.startsWith(QLatin1Char('`')) || raw.endsWith(QLatin1Char('`'))
        || (raw.size() > 1 && raw.startsWith(QLatin1Char(' ')) && raw.endsWith(QLatin1Char(' ')));
    return fence + (pad ? QStringLiteral(" ") : QString()) + raw + (pad ? QStringLiteral(" ") : QString()) + fence;
}

QString markOpen(int m)
{
    switch (m) { case Link: return QStringLiteral("["); case Strike: return QStringLiteral("~~"); case Bold: return QStringLiteral("**"); default: return QStringLiteral("*"); }
}
QString markClose(const Mk &k)
{
    if (k.m == Link) {
        const QString &href = k.href;
        const bool angle = (href.isEmpty() && !k.title.isEmpty()) || href.contains(QLatin1Char(' ')) || href.contains(QLatin1Char('(')) || href.contains(QLatin1Char(')')) || href.contains(QLatin1Char('<'));
        QString h = href;
        h.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
        h.replace(QLatin1Char('>'), QStringLiteral("%3E"));
        QString t = k.title;
        t.replace(QLatin1Char('\\'), QStringLiteral("\\\\")).replace(QLatin1Char('"'), QStringLiteral("\\\""));
        if (!t.isEmpty()) t = QStringLiteral(" \"") + t + QLatin1Char('"');
        return QStringLiteral("](") + (angle ? QStringLiteral("<") + h + QStringLiteral(">") : h) + t + QStringLiteral(")");
    }
    return markOpen(k.m);
}

// Serializes one block's inline content. `cont` is the continuation indent after a hard break.
// Delimiters never touch whitespace (whitespace at a boundary is moved outside), and simultaneously opened marks are
// nested longest-extent first so `**[a](u) rest**` keeps its shape.
QString inlineMarkdown(const QTextBlock &b, bool heading, const QString &cont)
{
    QList<Piece> pieces;
    for (auto it = b.begin(); !it.atEnd(); ++it) {
        const QTextFragment fr = it.fragment();
        const QTextCharFormat f = fr.charFormat();
        Piece p;
        p.text = fr.text();
        p.code = f.fontFixedPitch();
        // tie-break when marks open together and cover the same text: emphasis outside, link inside (**[t](u)**)
        if (f.fontStrikeOut()) p.marks.append(Mk{Strike, QString()});
        // headings are Bold by style; explicit bold inside a heading is ExtraBold (see Importer::refresh)
        if (heading ? f.fontWeight() > QFont::Bold : f.fontWeight() >= QFont::DemiBold) p.marks.append(Mk{Bold, QString()});
        if (f.fontItalic()) p.marks.append(Mk{Italic, QString()});
        if (f.isAnchor() && (!f.anchorHref().isEmpty() || f.anchorNames().isEmpty())) p.marks.append(Mk{Link, f.anchorHref(), f.toolTip()});
        pieces.append(p);
    }
    auto lcp = [](const QList<Mk> &a, const QList<Mk> &b2) {
        QList<Mk> r;
        for (const Mk &x : a) if (b2.contains(x)) r.append(x);
        return r;
    };
    for (int k = 0; k < pieces.size(); ++k) {   // whitespace-only runs cannot carry delimiters
        Piece &p = pieces[k];
        bool allWs = !p.code;
        for (QChar c : p.text) allWs = allWs && isWs(c);
        if (!allWs) continue;
        if (k > 0 && k + 1 < pieces.size()) p.marks = lcp(pieces[k - 1].marks, pieces[k + 1].marks);
        else p.marks.clear();
    }
    QList<Piece> merged;
    for (const Piece &p : std::as_const(pieces)) {
        if (!merged.isEmpty() && merged.last().marks == p.marks && merged.last().code == p.code && !p.code) merged.last().text += p.text;
        else merged.append(p);
    }
    pieces = merged;
    auto extent = [&](int from, const Mk &m) {
        int n = 0;
        for (int k = from; k < pieces.size() && pieces[k].marks.contains(m); ++k) ++n;
        return n;
    };

    QString out, pending;
    QList<Mk> stack;
    bool lineStart = true;
    auto closeTo = [&](int keep) {
        while (stack.size() > keep) { out += markClose(stack.last()); stack.removeLast(); lineStart = false; }
    };
    for (int idx = 0; idx < pieces.size(); ++idx) {
        const Piece &p = pieces[idx];
        int keep = 0;
        while (keep < stack.size() && p.marks.contains(stack[keep])) ++keep;
        closeTo(keep);
        out += pending;
        pending.clear();
        QString text = p.text;
        QList<Mk> toOpen;
        for (const Mk &m : p.marks) if (!stack.contains(m)) toOpen.append(m);
        if (!toOpen.isEmpty() && !p.code) {   // leading whitespace goes before the opening delimiters
            int s = 0;
            while (s < text.size() && isWs(text[s])) ++s;
            out += text.left(s);
            text = text.mid(s);
        }
        std::stable_sort(toOpen.begin(), toOpen.end(), [&](const Mk &x, const Mk &y) { return extent(idx, x) > extent(idx, y); });
        for (const Mk &m : toOpen) { out += markOpen(m.m); stack.append(m); lineStart = false; }
        if (p.code) {
            text.replace(QChar(QChar::LineSeparator), QLatin1Char(' '));
            out += codeSpan(text);
            lineStart = false;
            continue;
        }
        int e = text.size();
        while (e > 0 && isWs(text[e - 1])) --e;
        pending = text.mid(e);
        text.truncate(e);
        const QStringList parts = text.split(QChar(QChar::LineSeparator));
        for (int k = 0; k < parts.size(); ++k) {
            if (k > 0) {
                if (heading) out += QLatin1Char(' ');
                else out += QStringLiteral("\\\n") + cont;
                lineStart = !heading;
            }
            QString esc = escapeRun(parts[k]);
            if (lineStart && !esc.trimmed().isEmpty()) { esc = escapeLineStart(esc); lineStart = false; }
            else if (!esc.trimmed().isEmpty()) lineStart = false;
            out += esc;
        }
    }
    closeTo(0);
    return out;
}

bool isCodeBlock(const QTextBlock &b) { return b.blockFormat().hasProperty(QTextFormat::BlockCodeFence); }
bool isHr(const QTextBlock &b) { return b.blockFormat().boolProperty(kHrProp); }
int quoteLevel(const QTextBlockFormat &bf) { return bf.hasProperty(QTextFormat::BlockQuoteLevel) ? qMax(1, bf.intProperty(QTextFormat::BlockQuoteLevel)) : 0; }
QString rstrip(QString s) { while (s.endsWith(QLatin1Char(' '))) s.chop(1); return s; }
QString quotes(int n) { return QStringLiteral("> ").repeated(n); }

} // namespace

QString exportMarkdown(const QTextDocument *doc)
{
    QString out;
    enum Prev { None, Item, Block } prev = None;
    QVector<int> cols;                       // content column per list level
    QHash<int, QTextList *> lastList;        // per level: to alternate bullet markers between adjacent lists
    QHash<int, int> toggle;
    bool forceSep = false, lastItemEmpty = false;
    int lastItemLv = 0, prevQ = 0, prevD = 0;
    auto blank = [&] { if (!out.isEmpty()) out += QLatin1Char('\n'); };
    // A leading "---" would read as front matter when a second rule follows, so that one is written as "***".
    int rules = 0;
    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) rules += isHr(b) ? 1 : 0;
    bool firstRule = true;

    for (QTextBlock b = doc->begin(); b.isValid(); b = b.next()) {
        const QTextBlockFormat bf = b.blockFormat();
        QTextList *list = b.textList();
        if (list && !isCodeBlock(b) && !isHr(b)) {
            const int heading = bf.headingLevel();
            const int level = qMax(1, list->format().indent());
            const int lv = qMin(level, int(cols.size()) + 1);
            const int parentCol = lv > 1 ? cols[lv - 2] : 0;
            const auto style = list->format().style();
            const bool ordered = style == QTextListFormat::ListDecimal || style == QTextListFormat::ListLowerAlpha
                || style == QTextListFormat::ListUpperAlpha || style == QTextListFormat::ListLowerRoman || style == QTextListFormat::ListUpperRoman;
            for (auto it = lastList.begin(); it != lastList.end();) it = it.key() > lv ? lastList.erase(it) : std::next(it);
            if (lastList.contains(lv) && lastList.value(lv) != list) toggle[lv] ^= 1;   // adjacent lists must not merge
            lastList[lv] = list;
            QString marker;
            if (ordered) marker = QString::number(qMax(1, list->format().start()) + list->itemNumber(b)) + QLatin1Char('.');
            else marker = QString(QLatin1Char(toggle.value(lv) ? '*' : '-'));
            if (prev != Item || forceSep) blank();
            QString task;
            if (bf.marker() == QTextBlockFormat::MarkerType::Checked) task = QStringLiteral("[x]");
            else if (bf.marker() == QTextBlockFormat::MarkerType::Unchecked) task = QStringLiteral("[ ]");
            const int col = parentCol + int(marker.size()) + 1;
            QString text = inlineMarkdown(b, heading > 0, spaces(col));
            QString line = spaces(parentCol) + marker;
            if (!task.isEmpty()) line += QLatin1Char(' ') + task;
            if (!text.isEmpty()) line += QLatin1Char(' ') + text;
            out += line + QLatin1Char('\n');
            cols.resize(lv);
            cols[lv - 1] = col;
            prev = Item;
            lastItemEmpty = text.isEmpty();
            lastItemLv = lv;
            prevQ = 0;
            forceSep = false;
            continue;
        }
        // Every other block lives in a container context: list item content (column of its item) plus quote depth.
        const int q = quoteLevel(bf);
        const int cont = bf.intProperty(kContProp);
        const int d = cont > 0 ? qMin(cont, int(cols.size())) : 0;
        const int col = d > 0 ? cols[d - 1] : 0;
        const QString pre = spaces(col) + quotes(q);
        const bool hr = isHr(b), code = isCodeBlock(b);
        const int heading = bf.headingLevel();
        if (!hr && !code && !heading && q == 0 && b.text().isEmpty()) { forceSep = true; continue; }   // empty paragraphs carry no Markdown
        auto separate = [&] {
            const int common = qMin(q, prevQ);
            if (prev == Item && lastItemEmpty && d > 0 && d == lastItemLv && !forceSep) return;   // "-\n  code": a blank would end the empty item
            if (prev == Block && !forceSep && common > 0 && prevD == d) out += rstrip(spaces(col) + quotes(common)) + QLatin1Char('\n');
            else blank();
        };
        if (code) {
            QStringList lines;
            const QString lang = bf.stringProperty(QTextFormat::BlockCodeLanguage);
            QTextBlock e = b;
            for (; e.isValid() && isCodeBlock(e) && !e.textList() && quoteLevel(e.blockFormat()) == q
                   && qMin(e.blockFormat().intProperty(kContProp), int(cols.size())) == d; e = e.next()) {
                QString t = e.text();
                t.replace(QChar(QChar::LineSeparator), QLatin1Char('\n'));
                lines << t;
            }
            if (lines.isEmpty()) { lines << b.text(); e = b.next(); }   // defensive: a list-member code block still advances
            int longest = 0;
            for (const QString &l : std::as_const(lines)) {
                int run = 0;
                for (QChar c : l) { run = c == QLatin1Char('`') ? run + 1 : 0; longest = qMax(longest, run); }
            }
            const QString fence(qMax(3, longest + 1), QLatin1Char('`'));
            if (d == 0) lastList.clear();
            separate();
            out += pre + fence + lang + QLatin1Char('\n');
            for (const QString &l : std::as_const(lines))
                for (const QString &part : l.split(QLatin1Char('\n'))) out += (part.isEmpty() ? rstrip(pre) : pre + part) + QLatin1Char('\n');
            out += pre + fence + QLatin1Char('\n');
            b = e.previous();
        } else {
            if (d == 0) lastList.clear();
            separate();
            if (hr) {
                out += pre + ((firstRule && out.isEmpty() && rules > 1) ? QStringLiteral("***") : QStringLiteral("---")) + QLatin1Char('\n');
                firstRule = false;
            } else if (heading > 0) {
                const QString text = inlineMarkdown(b, true, QString());
                out += pre + QString(qBound(1, heading, 6), QLatin1Char('#')) + (text.isEmpty() ? QString() : QLatin1Char(' ') + text) + QLatin1Char('\n');
            } else {
                const QString text = inlineMarkdown(b, false, pre);
                out += text.isEmpty() ? rstrip(pre) + QLatin1Char('\n') : pre + text + QLatin1Char('\n');
            }
        }
        prev = Block;
        prevQ = q;
        prevD = d;
        forceSep = false;
    }
    return out;
}

} // namespace hn::editor
