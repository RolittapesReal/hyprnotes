#include "md_io.h"
#include "hn/editor/wiki_links.h"

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
#include <QVariantList>
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
    struct L {
        QTextList *list = nullptr;
        bool ordered = false;
        int start = 1, runId = 0;
    };
    QTextDocument *doc;
    QTextCursor c;
    bool fresh = true, liFresh = false, inCode = false, inLink = false;
    int quote = 0, heading = 0, strong = 0, em = 0, del = 0, code = 0;
    QString lang, codeText, href, title;
    QList<L> lists;
    QList<int> quoteBlockCounts;
    QList<int> quoteListDepths;
    int nextListRunId = 0, startedBlocks = 0;
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
        if (code) { f.setFontFixedPitch(true); f.setFontFamilies({QStringLiteral("monospace")}); }
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
        if (!quoteListDepths.isEmpty()) {
            QVariantList depths;
            for (const int depth : quoteListDepths) depths.append(depth);
            bf.setProperty(kQuoteListDepthsProp, depths);
        }
        if (cont && !lists.isEmpty()) { bf.setProperty(kContProp, int(lists.size())); m += kIndentPx * int(lists.size()); }
        if (m) bf.setLeftMargin(m);
        return bf;
    }
    void startBlock(const QTextBlockFormat &bf, const QTextCharFormat &cf)
    {
        ++startedBlocks;
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
            if (t == MD_BLOCK_QUOTE) {
                quoteBlockCounts.append(startedBlocks);
                quoteListDepths.append(lists.size());
                ++quote;
                liFresh = false;
            }
            break;
        case MD_BLOCK_UL: case MD_BLOCK_OL: {
            liFresh = false;
            L l;
            l.ordered = t == MD_BLOCK_OL;
            if (l.ordered) l.start = int(static_cast<MD_BLOCK_OL_DETAIL *>(d)->start);
            l.runId = ++nextListRunId;
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
                lf.setProperty(kListRunProp, l.runId);
                l.list = c.createList(lf);
            } else {
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
            if (liFresh && (t == MD_BLOCK_P || t == MD_BLOCK_HTML)) liFresh = false;   // first paragraph of a list item reuses the item block
            else { startBlock(baseBf(true), QTextCharFormat()); liFresh = false; }
            break;
        }
    }
    void leaveBlock(MD_BLOCKTYPE t)
    {
        switch (t) {
        case MD_BLOCK_QUOTE: {
            const int blocksAtEntry = quoteBlockCounts.takeLast();
            const QTextBlockFormat emptyQuoteFormat = baseBf();
            quoteListDepths.removeLast();
            --quote;
            if (startedBlocks == blocksAtEntry) {
                c.movePosition(QTextCursor::End);
                if (!fresh) c.insertBlock();
                c.setBlockFormat(emptyQuoteFormat);
                c.setBlockCharFormat(QTextCharFormat());
                blockCf = QTextCharFormat();
                refresh();
                fresh = false;
            }
            break;
        }
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
    QString normalized = md;
    for (qsizetype i = 0; i < normalized.size(); ++i) {
        const ushort c = normalized[i].unicode();
        if ((c < 0x20 && c != '\t' && c != '\n' && c != '\r') || c == 0x7f)
            normalized[i] = QChar(QChar::ReplacementCharacter);
    }
    const QByteArray utf8 = normalized.toUtf8();
    md_parse(utf8.constData(), MD_SIZE(utf8.size()), &p, &im);
    edit.endEditBlock();
}

namespace {

enum Mark { Link = 0, Strike = 1, Bold = 2, Italic = 3, ItalicStar = 4 };
struct Mk {
    int m = 0;
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

// How md4c (CommonMark 0.31) classifies a character next to a '*' run: whitespace, punctuation (Unicode P* and S*, so a
// symbol such as U+FFFD counts), or anything else. A run opens emphasis only if it is left-flanking and closes only if it is
// right-flanking; both depend on these classes of the characters on each side.
enum class Flank { Space, Punct, Other };
Flank flankClass(char32_t c)
{
    switch (QChar::category(c)) {
    case QChar::Separator_Space: case QChar::Separator_Line: case QChar::Separator_Paragraph: return Flank::Space;
    case QChar::Punctuation_Connector: case QChar::Punctuation_Dash: case QChar::Punctuation_Open: case QChar::Punctuation_Close:
    case QChar::Punctuation_InitialQuote: case QChar::Punctuation_FinalQuote: case QChar::Punctuation_Other:
    case QChar::Symbol_Math: case QChar::Symbol_Currency: case QChar::Symbol_Modifier: case QChar::Symbol_Other: return Flank::Punct;
    default: return (c == '\t' || c == '\n' || c == '\f' || c == '\r') ? Flank::Space : Flank::Other;
    }
}
// A character that can be written as a numeric entity without changing what it means (not a control character).
bool entityEncodable(char32_t c) { return QChar::category(c) != QChar::Other_Control && QChar::category(c) != QChar::Other_Surrogate && c != 0 && !QChar::isNonCharacter(c); }   // numeric entities of noncharacters decode to U+FFFD
char32_t firstCodePoint(QStringView s) { return s.isEmpty() ? 0 : (s.size() > 1 && s[0].isHighSurrogate() && s[1].isLowSurrogate() ? QChar::surrogateToUcs4(s[0], s[1]) : s[0].unicode()); }
char32_t lastCodePoint(QStringView s)
{
    if (s.isEmpty()) return 0;
    const QChar l = s.back();
    return l.isLowSurrogate() && s.size() > 1 && s[s.size() - 2].isHighSurrogate() ? QChar::surrogateToUcs4(s[s.size() - 2], l) : l.unicode();
}
// Delimiter family: runs of '*' (bold, italic) and of '~' (strike) are separate runs to md4c; links have no flanking rules.
int delimFamily(const Mk &m) { return m.m == Link ? -1 : (m.m == Strike ? 1 : (m.m == Italic ? 2 : 0)); }
int delimWidth(const Mk &m) { return m.m == Italic || m.m == ItalicStar ? 1 : 2; }

QString spaces(int n) { return QString(qMax(0, n), QLatin1Char(' ')); }
void trimLeadingMarkdownIndent(QString &s) {
    qsizetype count = 0;
    while (count < s.size() && (s[count] == QLatin1Char(' ') || s[count] == QLatin1Char('\t'))) ++count;
    if (count) s.remove(0, count);
}
void encodeLeadingTextIndent(QString &s) {
    qsizetype leading = 0;
    QString encoded;
    while (leading < s.size() && isWs(s[leading])) {
        encoded += s[leading] == QLatin1Char(' ') ? QStringLiteral("&#32;") : QStringLiteral("&#9;");
        ++leading;
    }
    if (leading) s.replace(0, leading, encoded);
}

// `after` is the literal text that follows this run in the same block (later parts and runs): a '[' here
// opens link syntax when its ']' and '(' / '[' / ':' only appear in a later formatting run.
QString escapeRun(const QString &t, bool escapeClosingBrackets, const QString &after = QString(),
                  bool escapeIntrawordUnderscores = false)
{
    static const QRegularExpression entity(QStringLiteral("^&[A-Za-z0-9#]+;"));
    QString o;
    o.reserve(t.size() + 4);
    // Wiki links ([[a]], ![[a]]) are plain text to CommonMark: keep their brackets unescaped so a visual-mode save does not
    // turn every link into "\[\[a\]\]" (which the link index would no longer see).
    QList<QPair<int, int>> wiki;
    for (const LinkRange &r : scanWikiLinks(t)) wiki.append({r.start, r.end});
    auto inWiki = [&](int k) { for (const auto &w : wiki) if (k >= w.first && k < w.second) return true; return false; };
    const QString lookahead = t + after;
    // A literal '[' is escaped whenever a later literal ']' is directly followed by '(' '[' or ':' (inline link, full/collapsed
    // reference, reference definition). The decision ignores nesting and backslashes on purpose: it depends only on the literal
    // text, so it is the same on every serialization, and an extra escape never changes meaning.
    // Last index of a ']' directly followed by '(' '[' or ':' in the literal text (one scan, so a block of many '[' stays linear).
    int lastLinkClose = -1;
    for (int i = int(lookahead.size()) - 2; i >= 0; --i)
        if (lookahead[i] == QLatin1Char(']')
            && (lookahead[i + 1] == QLatin1Char('(') || lookahead[i + 1] == QLatin1Char('[') || lookahead[i + 1] == QLatin1Char(':'))) {
            lastLinkClose = i;
            break;
        }
    auto opensMarkdownLink = [&](int start) { return lastLinkClose > start; };
    for (int k = 0; k < t.size(); ++k) {
        const QChar c = t[k];
        if ((c == QLatin1Char('[') || c == QLatin1Char(']')) && !wiki.isEmpty() && inWiki(k)) { o += c; continue; }
        switch (c.unicode()) {
        case '\\': case '`': case '*': case '<': case '~': o += QLatin1Char('\\'); o += c; break;
        case '[':
            if (escapeClosingBrackets || opensMarkdownLink(k)) o += QLatin1Char('\\');
            o += c;
            break;
        case ']':
            if (escapeClosingBrackets) o += QLatin1Char('\\');
            o += c;
            break;
        case '_':
            if (!escapeIntrawordUnderscores && k > 0 && k + 1 < t.size()
                && t[k - 1].isLetterOrNumber() && t[k + 1].isLetterOrNumber()) o += c;
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
QString escapeLineStart(const QString &t, const QString &after = QString())
{
    static const QRegularExpression head(QStringLiteral("^( {0,3})(#{1,6})(?=\\s|$)")), quote(QStringLiteral("^( {0,3})>")),
        bullet(QStringLiteral("^( {0,3})([-+])(?=\\s|$)")), ord(QStringLiteral("^( {0,3})(\\d{1,9})([.)])(?=\\s|$)")),
        rule(QStringLiteral("^( {0,3})([-=])(?=[-=]*\\s*$)"));
    QString r = t;
    if (auto m = head.match(r); m.hasMatch()) r.insert(m.capturedEnd(1), QLatin1Char('\\'));
    else if (auto m2 = quote.match(r); m2.hasMatch()) r.insert(m2.capturedEnd(1), QLatin1Char('\\'));
    else if (auto m3 = bullet.match(r); m3.hasMatch()) r.insert(m3.capturedEnd(1), QLatin1Char('\\'));
    else if (auto m4 = ord.match(r); m4.hasMatch()) r.insert(m4.capturedStart(3), QLatin1Char('\\'));
    else if (auto m5 = rule.match(r); m5.hasMatch()) {
        QString lineRemainder = after;
        const qsizetype hardBreak = lineRemainder.indexOf(QChar(QChar::LineSeparator));
        if (hardBreak >= 0) lineRemainder.truncate(hardBreak);
        const bool onlyRuleChars = std::all_of(lineRemainder.cbegin(), lineRemainder.cend(), [](QChar c) {
            return c.isSpace() || c == QLatin1Char('-') || c == QLatin1Char('=');
        });
        if (onlyRuleChars) r.insert(m5.capturedEnd(1), QLatin1Char('\\'));
    }
    return r;
}

QString codeSpan(const QString &raw)
{
    int longest = 0, run = 0;
    for (QChar c : raw) { run = c == QLatin1Char('`') ? run + 1 : 0; longest = qMax(longest, run); }
    const QString fence(longest + 1, QLatin1Char('`'));
    // CommonMark strips one boundary space only when the span contains a non-space character; tabs are content here.
    const bool hasNonSpace = std::any_of(raw.cbegin(), raw.cend(), [](QChar c) { return c != QLatin1Char(' '); });
    const bool pad = raw.startsWith(QLatin1Char('`')) || raw.endsWith(QLatin1Char('`'))
        || (raw.size() > 1 && raw.startsWith(QLatin1Char(' ')) && raw.endsWith(QLatin1Char(' ')) && hasNonSpace);
    return fence + (pad ? QStringLiteral(" ") : QString()) + raw + (pad ? QStringLiteral(" ") : QString()) + fence;
}

QString markOpen(int m)
{
    switch (m) {
    case Link: return QStringLiteral("[");
    case Strike: return QStringLiteral("~~");
    case Bold: return QStringLiteral("**");
    case Italic: return QStringLiteral("_");
    case ItalicStar: return QStringLiteral("*");
    }
    return {};
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
    const bool trailingHeadingSeparator = heading && !pieces.isEmpty() && !pieces.last().code
        && pieces.last().text.endsWith(QChar(QChar::LineSeparator));
    QList<qsizetype> trailingHardBreakStart(pieces.size(), -1);
    if (!heading) {
        for (qsizetype index = pieces.size(); index > 0; --index) {
            const Piece &piece = pieces[index - 1];
            if (piece.code) break;
            qsizetype start = piece.text.size();
            while (start > 0 && piece.text[start - 1] == QChar(QChar::LineSeparator)) --start;
            if (start == piece.text.size()) break;
            trailingHardBreakStart[index - 1] = start;
            if (start > 0) break;
        }
    }
    // A hard-break separator is a delimiter boundary like whitespace, though it is not layout whitespace.
    // Split it here so whitespace-only mark runs can use the same common-mark rule without changing isWs().
    QList<Piece> normalized;
    for (qsizetype pieceIndex = 0; pieceIndex < pieces.size(); ++pieceIndex) {
        const Piece &p = pieces[pieceIndex];
        if (p.code) { normalized.append(p); continue; }
        for (qsizetype begin = 0; begin < p.text.size();) {
            const bool trailingSeparator = trailingHeadingSeparator && pieceIndex == pieces.size() - 1
                && begin == p.text.size() - 1 && p.text[begin] == QChar(QChar::LineSeparator);
            const bool trailingHardBreak = trailingHardBreakStart[pieceIndex] >= 0
                && begin >= trailingHardBreakStart[pieceIndex] && p.text[begin] == QChar(QChar::LineSeparator);
            const bool lineSeparator = p.text[begin] == QChar(QChar::LineSeparator);
            const bool ws = isWs(p.text[begin]) || lineSeparator;
            qsizetype end = begin + 1;
            while (end < p.text.size()) {
                if (trailingHardBreakStart[pieceIndex] >= 0 && end == trailingHardBreakStart[pieceIndex]) break;
                const bool nextLineSeparator = p.text[end] == QChar(QChar::LineSeparator);
                if (trailingHeadingSeparator && pieceIndex == pieces.size() - 1
                    && end == p.text.size() - 1 && nextLineSeparator) break;
                if ((isWs(p.text[end]) || nextLineSeparator) != ws) break;
                ++end;
            }
            Piece part;
            part.text = trailingHardBreak ? QString() : p.text.mid(begin, end - begin);
            part.marks = trailingSeparator || trailingHardBreak ? QList<Mk>{} : p.marks;
            normalized.append(std::move(part));
            begin = end;
        }
    }
    pieces = std::move(normalized);
    auto lcp = [](const QList<Mk> &a, const QList<Mk> &b) {
        QList<Mk> common;
        for (const Mk &m : a) if (b.contains(m)) common.append(m);
        return common;
    };
    auto whitespaceOnly = [](const Piece &p) {
        if (p.code || p.text.isEmpty()) return false;
        for (QChar c : p.text)
            if (!isWs(c) && c != QChar(QChar::LineSeparator)) return false;
        return true;
    };
    for (int k = 0; k < pieces.size();) {
        if (!whitespaceOnly(pieces[k])) { ++k; continue; }
        const int begin = k;
        while (k < pieces.size() && whitespaceOnly(pieces[k])) ++k;
        const QList<Mk> common = begin > 0 && k < pieces.size()
            ? lcp(pieces[begin - 1].marks, pieces[k].marks) : QList<Mk>{};
        for (int n = begin; n < k; ++n) pieces[n].marks = common;
    }
    QList<Piece> merged;
    for (qsizetype i = 0; i < pieces.size(); ++i) {
        const Piece &p = pieces[i];
        const bool finalHeadingSeparator = trailingHeadingSeparator && i == pieces.size() - 1;
        if (!finalHeadingSeparator && !merged.isEmpty()
            && merged.last().marks == p.marks && merged.last().code == p.code && !p.code)
            merged.last().text += p.text;
        else
            merged.append(p);
    }
    pieces = std::move(merged);
    // Use the conventional asterisk spelling for simple italic spans. Keep underscore delimiters for punctuation-heavy or
    // partially overlapping spans, where a separate delimiter family is needed to keep the CommonMark run unambiguous.
    const Mk italicMark{Italic, QString()}, boldMark{Bold, QString()};
    auto simpleStarText = [](const QString &text) {
        return !text.isEmpty() && std::all_of(text.cbegin(), text.cend(), [](QChar c) {
            return c == QLatin1Char(' ') || (c >= QLatin1Char('0') && c <= QLatin1Char('9'))
                || (c >= QLatin1Char('A') && c <= QLatin1Char('Z')) || (c >= QLatin1Char('a') && c <= QLatin1Char('z'));
        });
    };
    for (int start = 0; start < pieces.size();) {
        if (!pieces[start].marks.contains(italicMark)) { ++start; continue; }
        int end = start;
        bool anyBold = false, everyPieceBold = true, safeText = true;
        QString spanText;
        while (end < pieces.size() && pieces[end].marks.contains(italicMark)) {
            const bool bold = pieces[end].marks.contains(boldMark);
            anyBold |= bold;
            everyPieceBold &= bold;
            safeText &= !pieces[end].code;
            spanText += pieces[end].text;
            ++end;
        }
        const bool sameBoldExtent = everyPieceBold
            && (start == 0 || !pieces[start - 1].marks.contains(boldMark))
            && (end == pieces.size() || !pieces[end].marks.contains(boldMark));
        const bool adjacentBoldBoundary = (start > 0
                && pieces[start - 1].marks.contains(boldMark) != pieces[start].marks.contains(boldMark))
            || (end < pieces.size()
                && pieces[end - 1].marks.contains(boldMark) != pieces[end].marks.contains(boldMark));
        if (safeText && (!anyBold || sameBoldExtent) && !adjacentBoldBoundary && simpleStarText(spanText)) {
            for (int index = start; index < end; ++index)
                for (Mk &mark : pieces[index].marks)
                    if (mark == italicMark) mark.m = ItalicStar;
        }
        start = end;
    }
    auto extent = [&](int from, const Mk &m) {
        int n = 0;
        for (int k = from; k < pieces.size() && pieces[k].marks.contains(m); ++k) ++n;
        return n;
    };

    QString out, pending;
    QList<Mk> stack;
    bool lineStart = true;
    bool protectLeadingIndent = false;
    qsizetype openRunStart = -1, openRunEnd = -1;   // the last delimiter run of openers written to `out`
    int closeRun = 0, closeFamily = -1;   // delimiter characters of one family at the very end of `out`, written by the closers of the current piece
    auto closeTo = [&](int keep) {
        closeRun = 0;
        closeFamily = -1;
        while (stack.size() > keep) {
            const int family = delimFamily(stack.last());
            closeRun = family < 0 ? 0 : (family == closeFamily ? closeRun : 0) + delimWidth(stack.last());
            closeFamily = family;
            out += markClose(stack.last());
            stack.removeLast();
            lineStart = false;
            protectLeadingIndent = false;
        }
    };
    auto codeIsEscapedLiteral = [](const Piece &piece) {
        if (!piece.code) return false;
        int longestTickRun = 0, tickRun = 0;
        for (QChar c : piece.text) {
            tickRun = c == QLatin1Char('`') ? tickRun + 1 : 0;
            longestTickRun = qMax(longestTickRun, tickRun);
        }
        return longestTickRun >= 32;
    };
    for (int idx = 0; idx < pieces.size(); ++idx) {
        const Piece &p = pieces[idx];
        int keep = 0;
        while (keep < stack.size() && p.marks.contains(stack[keep])) ++keep;
        closeTo(keep);
        qsizetype adjacentRun = pending.isEmpty() ? closeRun : 0;   // closers directly followed by this piece's delimiters or text
        if (protectLeadingIndent && !pending.isEmpty()) {
            encodeLeadingTextIndent(pending);
            lineStart = false;
            protectLeadingIndent = false;
        }
        if (trailingHeadingSeparator && idx == pieces.size() - 1)
            encodeLeadingTextIndent(pending);
        out += pending;
        pending.clear();
        QString text = p.text;
        QList<Mk> toOpen;
        for (const Mk &m : p.marks) if (!stack.contains(m)) toOpen.append(m);
        if (!toOpen.isEmpty() && !p.code) {   // leading whitespace goes before the opening delimiters
            int s = 0;
            while (s < text.size() && isWs(text[s])) ++s;
            if (s > 0) adjacentRun = 0;
            QString leading = text.left(s);
            if (protectLeadingIndent && !leading.isEmpty()) {
                encodeLeadingTextIndent(leading);
                lineStart = false;
                protectLeadingIndent = false;
            }
            out += leading;
            text = text.mid(s);
        }
        std::stable_sort(toOpen.begin(), toOpen.end(), [&](const Mk &x, const Mk &y) { return extent(idx, x) > extent(idx, y); });
        // A delimiter run can open only if it is left-flanking and close only if it is right-flanking, which depends on the
        // characters on either side of the whole run (closers just written and openers about to be written count as one run when
        // they are of the same family). '~' also follows the stricter intraword rule of '_'. If a letter would stop the run from
        // opening or closing, that letter is written as a numeric entity (a punctuation-class character).
        bool leadEntity = false;
        {
            const bool hasClose = adjacentRun > 0;
            const int openFamily = toOpen.isEmpty() ? -1 : delimFamily(toOpen.first());
            const bool hasOpen = openFamily >= 0;
            if (hasClose || hasOpen) {
                qsizetype lead = 0;   // openers of the first opener's family that form one run
                while (hasOpen && lead < toOpen.size() && delimFamily(toOpen[lead]) == openFamily) ++lead;
                const bool combined = hasClose && hasOpen && closeFamily == openFamily;
                const QStringView head = QStringView(out).left(out.size() - adjacentRun);
                const Flank headCls = head.isEmpty() ? Flank::Space : flankClass(lastCodePoint(head));
                const Flank textCls = text.isEmpty() ? Flank::Space : text[0] == QChar(QChar::LineSeparator) ? Flank::Punct : flankClass(firstCodePoint(text));
                const bool textFollows = lead == toOpen.size() && !p.code;   // the run is followed directly by this piece's text
                const bool headFixable = hasOpen && headCls == Flank::Other && entityEncodable(lastCodePoint(head));
                const bool textFixable = textFollows && textCls == Flank::Other && entityEncodable(firstCodePoint(text));
                auto canOpen = [](int family, Flank prev, Flank next) {
                    const bool left = next != Flank::Space && (next != Flank::Punct || prev != Flank::Other);
                    const bool right = prev != Flank::Space && (prev != Flank::Punct || next != Flank::Other);
                    return left && (family == 0 || !right || prev == Flank::Punct);
                };
                auto canClose = [](int family, Flank prev, Flank next) {
                    const bool left = next != Flank::Space && (next != Flank::Punct || prev != Flank::Other);
                    const bool right = prev != Flank::Space && (prev != Flank::Punct || next != Flank::Other);
                    return right && (family == 0 || !left || next == Flank::Punct);
                };
                // classes as the run will see them: a closer-only run is followed by what comes next (an opener of another family is punctuation)
                Flank prev = headCls, next = lead < toOpen.size() || p.code ? Flank::Punct : textCls;
                const Flank closeNext = combined ? next : (toOpen.isEmpty() ? textCls : Flank::Punct);
                const Flank openPrev = hasClose && !combined ? Flank::Punct : prev;
                bool fixHead = false;
                Flank cn = closeNext, op = openPrev;
                for (int round = 0; round < 2; ++round) {
                    const bool openBad = hasOpen && !canOpen(openFamily, op, next);
                    const bool closeBad = hasClose && !canClose(closeFamily, combined ? op : prev, cn);
                    if (openBad && headFixable && !fixHead && op == Flank::Other) { fixHead = true; op = Flank::Punct; if (combined) prev = Flank::Punct; }
                    else if (closeBad && textFixable && !leadEntity && cn == Flank::Other) { leadEntity = true; cn = Flank::Punct; if (combined) next = Flank::Punct; }
                    else break;
                }
                if (fixHead) {
                    const char32_t ch = lastCodePoint(head);
                    const qsizetype chLen = ch > 0xFFFF ? 2 : 1, chAt = head.size() - chLen;
                    // The letter may be the first character after an opener run, which then sees punctuation next to it and
                    // needs a punctuation-class (or space) neighbour in front too.
                    const char32_t b = openRunEnd == chAt && openRunStart > 0 ? lastCodePoint(QStringView(out).left(openRunStart)) : 0;
                    out.replace(chAt, chLen, QStringLiteral("&#%1;").arg(uint(ch)));
                    if (b && flankClass(b) == Flank::Other && entityEncodable(b)) {
                        const qsizetype bLen = b > 0xFFFF ? 2 : 1;
                        const QString entity = QStringLiteral("&#%1;").arg(uint(b));
                        out.replace(openRunStart - bLen, bLen, entity);
                        openRunStart += entity.size() - bLen;
                        openRunEnd += entity.size() - bLen;
                    }
                }
            }
        }
        int previousOpenFamily = -1;
        for (const Mk &m : toOpen) {
            if (m.m == Link && out.endsWith(QLatin1Char('!'))) out.insert(out.size() - 1, QLatin1Char('\\'));   // "![" would start an image
            if (delimFamily(m) >= 0 && delimFamily(m) != previousOpenFamily) openRunStart = out.size();
            previousOpenFamily = delimFamily(m);
            out += markOpen(m.m);
            if (delimFamily(m) >= 0) openRunEnd = out.size();
            stack.append(m);
            lineStart = false;
            protectLeadingIndent = false;
        }
        if (p.code) {
            text.replace(QChar(QChar::LineSeparator), QLatin1Char(' '));
            int longestTickRun = 0, tickRun = 0;
            for (QChar c : text) {
                tickRun = c == QLatin1Char('`') ? tickRun + 1 : 0;
                longestTickRun = qMax(longestTickRun, tickRun);
            }
            if (longestTickRun >= 32) {
                // The importer does not recognize code-span fences longer than 32 ticks. Preserve text in escaped form.
                const bool inLink = std::any_of(p.marks.cbegin(), p.marks.cend(), [](const Mk &m) { return m.m == Link; });
                QString after;
                for (int next = idx + 1; next < pieces.size(); ++next) {
                    const Piece &np = pieces[next];
                    const bool nextLink = std::any_of(np.marks.cbegin(), np.marks.cend(), [](const Mk &m) { return m.m == Link; });
                    after += (nextLink || (np.code && !codeIsEscapedLiteral(np))) ? QStringLiteral("x") : np.text;
                }
                QString literal = escapeRun(text, inLink, inLink ? QString() : after);
                if (lineStart) encodeLeadingTextIndent(literal);
                out += literal;
            } else {
                out += codeSpan(text);
            }
            lineStart = false;
            protectLeadingIndent = false;
            continue;
        }
        int e = text.size();
        while (e > 0 && isWs(text[e - 1])) --e;
        pending = text.mid(e);
        text.truncate(e);
        const QStringList parts = text.split(QChar(QChar::LineSeparator));
        for (int k = 0; k < parts.size(); ++k) {
            if (k > 0) {
                if (heading) {
                    const bool trailingSeparator = trailingHeadingSeparator && idx == pieces.size() - 1
                        && k == parts.size() - 1 && parts[k].isEmpty();
                    out += trailingSeparator ? QStringLiteral("&#32;") : QStringLiteral(" ");
                }
                else out += QStringLiteral("\\\n") + cont;
                lineStart = !heading;
                protectLeadingIndent = !heading;
            }
            QString part = parts[k];
            QString generatedLeadEntity;
            if (k == 0 && leadEntity && !part.isEmpty() && flankClass(firstCodePoint(part)) == Flank::Other) {
                const char32_t lead = firstCodePoint(part);
                part.remove(0, lead > 0xFFFF ? 2 : 1);
                generatedLeadEntity = QStringLiteral("&#%1;").arg(uint(lead));
            }
            const bool inLink = std::any_of(p.marks.cbegin(), p.marks.cend(), [](const Mk &m) { return m.m == Link; });
            // literal text that follows in this block: later parts of this run, then later runs (code spans and
            // real links are placeholders; their own brackets are syntax)
            QString after;
            for (int rest = k + 1; rest < parts.size(); ++rest) after += parts[rest];
            for (int next = idx + 1; next < pieces.size(); ++next) {
                const Piece &np = pieces[next];
                const bool nextLink = std::any_of(np.marks.cbegin(), np.marks.cend(), [](const Mk &m) { return m.m == Link; });
                after += (nextLink || (np.code && !codeIsEscapedLiteral(np))) ? QStringLiteral("x") : np.text;
            }
            // A literal intraword underscore inside underscore-delimited emphasis can become a delimiter
            // when the following formatted piece starts a different delimiter family. Escape it so the
            // imported text remains literal across that generated mark transition.
            const bool underscoreItalic = p.marks.contains(Mk{Italic, QString()});
            QString esc = escapeRun(part, inLink, inLink ? QString() : after, underscoreItalic);
            // Raw NBSP is Unicode whitespace to CommonMark's delimiter flanking rules, but unlike ASCII spaces
            // it is not recognized as whitespace by md4c's delimiter scanner. Keep marked NBSP as an entity so
            // the delimiters parse; Importer decodes it back to the original U+00A0 character.
            if (!p.marks.isEmpty()) esc.replace(QChar(0x00a0), QStringLiteral("&#160;"));
            if (p.marks.contains(Mk{Strike, QString()})) {
                // Encode escaped underscores consistently across fragment boundaries. Importing a generated
                // strikethrough run can coalesce adjacent fragments, so first/last-fragment special cases
                // otherwise change from numeric entities to backslash escapes on the next export.
                esc.replace(QStringLiteral("\\_"), QStringLiteral("&#95;"));
            }
            // Escape original text before appending serializer-generated syntax. Otherwise escapeRun
            // treats this entity as literal source text and escapes its ampersand.
            esc = generatedLeadEntity + esc;
            if (protectLeadingIndent) {
                encodeLeadingTextIndent(esc);
                if (!esc.isEmpty()) protectLeadingIndent = false;
            }
            if (lineStart && !esc.trimmed().isEmpty()) { esc = escapeLineStart(esc, after); lineStart = false; }
            else if (!esc.trimmed().isEmpty()) lineStart = false;
            out += esc;
        }
    }
    closeTo(0);
    // Markdown strips literal spaces/tabs at line end (and treats two spaces as a hard break).
    // Encode only the final pending run so these text characters survive import without changing mark boundaries.
    for (QChar c : pending)
        out += c == QLatin1Char(' ') ? QStringLiteral("&#32;")
              : c == QLatin1Char('\t') ? QStringLiteral("&#9;") : QString(c);
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
    QHash<int, int> orderedRun, nextOrderedNumber;
    bool forceSep = false, lastItemEmpty = false;
    int lastItemLv = 0, prevQ = 0, prevD = 0;
    auto blank = [&] { if (!out.isEmpty()) out += QLatin1Char('\n'); };
    auto containerPrefix = [&](const QTextBlockFormat &bf, int listDepth, int col, int quoteCount = -1) {
        const int q = quoteCount < 0 ? quoteLevel(bf) : qBound(0, quoteCount, quoteLevel(bf));
        if (!bf.hasProperty(kQuoteListDepthsProp)) return spaces(col) + quotes(q);
        const QVariantList boundaries = bf.property(kQuoteListDepthsProp).toList();
        QString prefix;
        int writtenCol = 0;
        for (int i = 0; i < q; ++i) {
            const int depth = qBound(0, i < boundaries.size() ? boundaries[i].toInt() : listDepth, listDepth);
            const int rawCol = depth > 0 && depth <= cols.size() ? cols[depth - 1] : 0;
            const int boundaryCol = qBound(writtenCol, qMin(rawCol, col), col);
            prefix += spaces(boundaryCol - writtenCol) + QStringLiteral("> ");
            writtenCol = boundaryCol;
        }
        prefix += spaces(col - writtenCol);
        return prefix;
    };
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
            bool separateListRun = false;
            for (auto it = lastList.begin(); it != lastList.end();) it = it.key() > lv ? lastList.erase(it) : std::next(it);
            const bool resumesAfterContinuation = lastList.contains(lv) && prev == Block && prevD >= lv;
            if (lastList.contains(lv) && (lastList.value(lv) != list || resumesAfterContinuation)) {
                const int run = list->format().intProperty(kListRunProp);
                QTextList *previous = lastList.value(lv);
                const int previousRun = previous->format().intProperty(kListRunProp);
                // A bare blank after quoted code ends that quote, so keep the emitted list-run boundary stable.
                const bool quotedCodeBoundary = resumesAfterContinuation && quoteLevel(bf) > 0
                    && b.previous().isValid() && isCodeBlock(b.previous());
                if (run == 0 || run != previousRun || quotedCodeBoundary) {
                    separateListRun = true;
                    const auto previousStyle = previous->format().style();
                    const bool previousOrdered = previousStyle == QTextListFormat::ListDecimal
                        || previousStyle == QTextListFormat::ListLowerAlpha || previousStyle == QTextListFormat::ListUpperAlpha
                        || previousStyle == QTextListFormat::ListLowerRoman || previousStyle == QTextListFormat::ListUpperRoman;
                    if (ordered) toggle[lv] = 0;
                    else if (previousOrdered) toggle[lv] = 0;
                    else toggle[lv] ^= 1; // chunked segments of one source list remain one export run
                }
            }
            lastList[lv] = list;
            QString marker;
            if (ordered) {
                const int run = list->format().intProperty(kListRunProp);
                int number = qMax(1, list->format().start()) + list->itemNumber(b);
                if (run != 0 && orderedRun.value(lv) == run) number = nextOrderedNumber.value(lv, number);
                orderedRun[lv] = run;
                if (run != 0) nextOrderedNumber[lv] = number + 1;
                marker = QString::number(number) + QLatin1Char('.');
            } else {
                marker = QString(QLatin1Char(toggle.value(lv) ? '*' : '-'));
            }
            QString task;
            if (bf.marker() == QTextBlockFormat::MarkerType::Checked) task = QStringLiteral("[x]");
            else if (bf.marker() == QTextBlockFormat::MarkerType::Unchecked) task = QStringLiteral("[ ]");
            const int col = parentCol + int(marker.size()) + 1;
            QString text = inlineMarkdown(b, heading > 0, spaces(col));
            if (!task.isEmpty()) text = text.trimmed();
            else trimLeadingMarkdownIndent(text);
            const bool separateEmptyNestedAfterText = prev == Item && text.isEmpty() && lv > 1
                && lastItemLv < lv && !lastItemEmpty;
            if (prev != Item || forceSep || separateListRun || separateEmptyNestedAfterText) blank();
            if (!text.isEmpty()) text = escapeLineStart(text);
            QString line = containerPrefix(bf, lv - 1, parentCol) + marker;
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
        const QString pre = containerPrefix(bf, d, col);
        const bool hr = isHr(b), code = isCodeBlock(b);
        const int heading = bf.headingLevel();
        const QString blockText = b.text();   // one object: begin()/end() must come from the same string
        const bool onlyHardBreaks = !blockText.isEmpty()
            && std::all_of(blockText.cbegin(), blockText.cend(), [](QChar c) {
                return c == QChar(QChar::LineSeparator);
            });
        if (!hr && !code && !heading && q == 0 && (blockText.isEmpty() || onlyHardBreaks)) {
            forceSep = true;
            continue;   // empty paragraphs and paragraphs containing only an unrepresentable hard break carry no Markdown
        }
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
                   && e.blockFormat().property(kQuoteListDepthsProp) == bf.property(kQuoteListDepthsProp)
                   && qMin(e.blockFormat().intProperty(kContProp), int(cols.size())) == d; e = e.next()) {
                QString t = e.text();
                t.replace(QChar(QChar::LineSeparator), QLatin1Char('\n'));
                lines << t;
            }
            if (lines.isEmpty()) { lines << b.text(); e = b.next(); }   // defensive: a list-member code block still advances
            int longestTicks = 0, longestTildes = 0;
            for (const QString &l : std::as_const(lines)) {
                int ticks = 0, tildes = 0;
                for (QChar c : l) {
                    ticks = c == QLatin1Char('`') ? ticks + 1 : 0;
                    tildes = c == QLatin1Char('~') ? tildes + 1 : 0;
                    longestTicks = qMax(longestTicks, ticks);
                    longestTildes = qMax(longestTildes, tildes);
                }
            }
            // Backtick fences cannot have backticks in their info string (a tilde fence can).
            const QChar fenceChar = lang.contains(QLatin1Char('`')) || longestTildes < longestTicks ? QLatin1Char('~') : QLatin1Char('`');
            const int longest = fenceChar == QLatin1Char('`') ? longestTicks : longestTildes;
            const QString fence(qMax(3, longest + 1), fenceChar);
            if (d == 0) lastList.clear();
            separate();
            QString info = lang;   // the info string is backslash- and entity-decoded on import
            info.replace(QLatin1Char('\\'), QStringLiteral("\\\\")).replace(QLatin1Char('&'), QStringLiteral("\\&"));
            out += pre + fence + info + QLatin1Char('\n');
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
                QString text = inlineMarkdown(b, true, QString());
                encodeLeadingTextIndent(text);
                trimLeadingMarkdownIndent(text);
                if (text.endsWith(QLatin1Char('#'))) {   // "text #" would end in a closing sequence, which the heading drops
                    qsizetype run = text.size();
                    while (run > 0 && text[run - 1] == QLatin1Char('#')) --run;
                    if (run == 0 || isWs(text[run - 1])) text.insert(run, QLatin1Char('\\'));
                }
                out += pre + QString(qBound(1, heading, 6), QLatin1Char('#')) + (text.isEmpty() ? QString() : QLatin1Char(' ') + text) + QLatin1Char('\n');
            } else {
                QString text = inlineMarkdown(b, false, pre);
                encodeLeadingTextIndent(text);
                trimLeadingMarkdownIndent(text);
                if (!text.isEmpty()) text = escapeLineStart(text);
                if (text.isEmpty() && q > 0)
                    out += pre + QStringLiteral("&#32;") + QLatin1Char('\n');
                else
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
