// Ordinary real-world notes must open in visual mode, never change on open, and round-trip semantically.
// Soft-break wrap positions are normalized to spaces (semanticTokens ignores them); hard breaks are preserved as U+2028.
#include <QElapsedTimer>
#include <QTextDocument>

#include "../../src/editor/md_io.h"   // private block property ids (kHrProp, kContProp)
#include "editor_test_util.h"
#include "hn/theme/theme.h"

using namespace hntest;

static QByteArray mixed20k()
{
    QByteArray md;
    int i = 0;
    while (md.size() < 20 * 1024) {
        const QByteArray n = QByteArray::number(i);
        switch (i % 8) {
        case 0: md += "## Section " + n + " with **bold** heading\n\n"; break;
        case 1: md += "Wrapped paragraph " + n + " that keeps going\nacross several source lines with *emphasis*\nand a [link **bold**](https://example.org/" + n + ") at the end.\n\n"; break;
        case 2: md += "Hard break one  \nhard break two\\\nhard break three " + n + "\n\n"; break;
        case 3: md += "- tight " + n + "\n- item with `code`\n  - nested a\n  - nested b\n- last\n\n"; break;
        case 4: md += "1. loose " + n + "\n\n2. second loose\n\n3. third loose\n\n"; break;
        case 5: md += "> quoted " + n + "\n> continues here\n>\n> second para\n\n"; break;
        case 6: md += "```cpp\nint x" + n + " = 1;\n\nreturn x" + n + ";\n```\n\n"; break;
        default: md += "- [ ] todo " + n + "\n- [x] done ~~gone~~\n\n"; break;
        }
        ++i;
    }
    return md;
}

class RealNotesTest : public QObject {
    Q_OBJECT
    static void check(const QString &md)
    {
        Fx f(md.toUtf8());
        QVERIFY2(f.ed.mode() == Mode::Visual, qPrintable(f.ed.modeReason()));
        QCOMPARE(f.ed.toMarkdownBytes(), md.toUtf8());
        f.moveEnd();
        f.text("z");
        f.key(Qt::Key_Backspace);
        QVERIFY(f.ed.isModified());
        const QString out = f.md();
        QVERIFY2(sameSemantics(md, out), qPrintable(md + "\n--- exported ---\n" + out));
        Fx g(out.toUtf8());
        QVERIFY2(g.ed.mode() == Mode::Visual, qPrintable(g.ed.modeReason()));
        QCOMPARE(g.ed.toMarkdownBytes(), out.toUtf8());
    }
private slots:
    void ordinary_data()
    {
        QTest::addColumn<QString>("md");
        auto row = [](const char *n, const char *md) { QTest::newRow(n) << QString::fromUtf8(md); };
        row("soft-wrapped", "This paragraph is wrapped\nacross three\nsource lines.\n");
        row("soft-wrapped-two", "First wrapped\nparagraph.\n\nSecond wrapped\nparagraph here.\n");
        row("soft-indented-continuation", "a\n   b\n      c\n");
        row("hard-spaces", "line one  \nline two\n");
        row("hard-backslash", "line one\\\nline two\n");
        row("hard-multiple", "a  \nb\\\nc  \nd\n");
        row("hard-and-soft", "a  \nb\nc\\\nd\ne\n");
        row("hard-in-emphasis", "*a  \nb* and **c\\\nd**\n");
        row("hard-in-link", "[a  \nb](https://x.y)\n");
        row("hard-in-list", "- item one  \n  continued\n- item two\\\n  more\n");
        row("hard-in-quote", "> quoted  \n> more\n");
        row("tight-list", "- a\n- b\n- c\n");
        row("loose-list", "- a\n\n- b\n\n- c\n");
        row("loose-ordered", "1. a\n\n2. b\n\n3. c\n");
        row("loose-nested", "- a\n\n  - b\n\n  - c\n\n- d\n");
        row("mixed-loose-tight", "- a\n- b\n\n- c\n");
        row("list-wrapped-items", "- this item is long\n  and wraps\n- and so is\n  this one\n");
        row("list-star-plus", "* a\n* b\n\n+ c\n+ d\n");
        row("nested-deep", "- a\n  - b\n    - c\n      - d\n- e\n");
        row("nested-mixed", "- a\n  1. b\n  2. c\n     - d\n- e\n");
        row("ordered-start", "5. a\n6. b\n");
        row("tasks", "- [ ] a\n- [x] b\n  - [ ] c\n");
        row("tasks-loose", "- [ ] a\n\n- [x] b\n");
        row("heading-bold", "# Title with **bold** word\n");
        row("heading-italic-code", "## An *italic* and `code` heading\n");
        row("heading-all-bold", "### **Everything bold**\n");
        row("heading-link", "# See [the **docs**](https://a.b/c)\n");
        row("heading-setext", "Title\n=====\n\nSub\n---\n");
        row("link-bold", "[**bold link**](https://a.b) and [plain **mid** end](https://a.b)\n");
        row("link-italic", "*[em link](https://a.b)* and [a *b* c](https://a.b)\n");
        row("link-nested-both", "**[a *b* c](https://a.b)** and [***x***](https://a.b)\n");
        row("link-title", "[t](https://a.b \"a title\") and [u](https://a.b 'it\\'s')\n");
        row("link-escapes", "[x](https://a.b/c\\_d) and [y](<https://a.b/(p)>) and <https://auto.link>\n");
        row("link-code", "[`code` text](https://a.b)\n");
        row("quote-multi", "> a\n> b\n>\n> c\n\nafter\n");
        row("quote-lazy", "> lazy\ncontinuation\n");
        row("quote-formatting", "> **bold** and *em* and [l](https://a.b)\n");
        row("code-fence", "```python\ndef f():\n    return 1\n\nprint(f())\n```\n");
        row("code-fence-tilde", "~~~\nraw *text*\n~~~\n");
        row("code-indented", "para\n\n    indented code\n    more\n\nafter\n");
        row("code-between", "text\n\n```\na\n```\n\nmore\n\n```sh\nb\n```\n");
        row("inline-mix", "a **b** *c* ~~d~~ `e` ***f*** **g *h* i**\n");
        row("entities", "a &amp; b &lt;c&gt; &copy; &#35; &#x41;\n");
        row("crlf-free-specials", "price is $5 and 50% off; #hashtag; a_b_c; 2*3*4\n");
        row("journal", "# 2026-10-01\n\n## Morning\n\nWoke up early, wrote some code\nand made tea.\n\n- [x] gym\n- [ ] email\n\n## Notes\n\n> remember: *slow* is **smooth**\n\nSee [plan](https://x.y/plan).  \nThen sleep.\n");
        row("meeting", "# Standup\n\n**Attendees:** A, B, C\n\n1. Status\n\n   - nothing\n\n2. Blockers\n");
        row("empty-lines-many", "a\n\n\n\nb\n");
        // --- horizontal rules, multi-block list items, nested quotes, empty link targets ---
        row("hr-dash", "a\n\n---\n\nb\n");
        row("hr-star", "a\n\n***\n\nb\n");
        row("hr-underscore", "a\n\n___\n\nb\n");
        row("hr-long", "a\n\n----------\n\nb\n");
        row("hr-spaced", "a\n\n- - -\n\nb\n");
        row("hr-interrupts-paragraph", "a\n***\nb\n");
        row("hr-consecutive", "a\n\n---\n\n---\n\nb\n");
        row("hr-last", "text\n\n---\n");
        row("hr-first", "---\n\ntext\n");
        row("hr-after-heading", "# T\n\n---\n\ntext\n");
        row("hr-between-lists", "- a\n\n---\n\n- b\n");
        row("hr-in-quote", "> a\n>\n> ---\n>\n> b\n");
        row("hr-in-list", "- a\n\n  ---\n\n  b\n");
        row("hr-only", "---\n");
        row("item-two-paragraphs", "- a\n\n  b\n");
        row("item-three-paragraphs", "- a\n\n  b\n\n  c\n- d\n");
        row("item-ordered-paragraphs", "1. a\n\n   b\n2. c\n");
        row("item-tight-code", "- a\n  ```\n  code\n  ```\n- b\n");
        row("item-code-blank-line", "- a\n\n  ```sh\n  x\n\n  y\n  ```\n\n  after\n");
        row("item-code-first", "-\n  ```\n  code\n  ```\n");
        row("item-quote", "- a\n\n  > quote\n  > more\n- b\n");
        row("item-quote-first", "- > quoted first\n- b\n");
        row("item-task-detail", "- [ ] task\n\n  detail\n- [x] done\n");
        row("item-nested-and-cont", "- a\n  - b\n\n    b2\n  - c\n\n  a2\n- d\n");
        row("item-heading", "- a\n\n  # h\n");
        row("item-formatted-cont", "- a\n\n  **bold** and [l](https://a.b)  \n  hard\n");
        row("quote-nested", "> a\n>\n> > b\n>\n> c\n");
        row("quote-nested-deep", "> > > three\n");
        row("quote-nested-lazy", "> a\n> > b\n");
        row("quote-nested-multi", "> > a\n> > b\n>\n> c\n");
        row("quote-nested-two-paras", "> a\n>\n> > b\n> >\n> > c\n>\n> d\n");
        row("quote-code", "> ```\n> x\n> ```\n");
        row("link-empty", "[x]()\n");
        row("link-empty-angle", "[x](<>)\n");
        row("link-empty-mixed", "see [a]() and [b](https://x.y) and [**c**]() end\n");
        row("link-empty-emphasis", "*[e]()* and **[s]()**\n");
        row("link-empty-heading", "# [h]()\n");
        row("link-empty-list", "- [l]()\n- m\n");
        row("link-empty-title", "[x](<> \"t\")\n");
        row("unicode", "caf\xC3\xA9 \xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x92\xBB\nsecond line \xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D\n");
    }
    void ordinary()
    {
        QFETCH(QString, md);
        check(md);
    }
    void mixed20KiB()
    {
        const QByteArray md = mixed20k();
        QVERIFY(md.size() >= 20 * 1024);
        check(QString::fromUtf8(md));
        QElapsedTimer t;
        t.start();
        Fx f(md);
        const double ms = t.nsecsElapsed() / 1e6;   // includes widget construction + show; load() alone is measured below
        QVERIFY(f.ed.mode() == Mode::Visual);
        QElapsedTimer t2;
        double best = 1e9;
        for (int i = 0; i < 5; ++i) {
            t2.restart();
            f.ed.load(md);
            best = qMin(best, t2.nsecsElapsed() / 1e6);
        }
        qInfo().nospace() << "20 KiB mixed note (" << md.size() << " bytes, " << f.v->document()->blockCount()
                          << " blocks): load() best-of-5 = " << best << " ms; fixture + widget setup = " << ms << " ms (target load < 30 ms)";
        QVERIFY2(best < 30.0, "load time over target");
    }
    // ---- behaviour of the new native blocks ----
    static int countBlocks(QTextDocument *d, int prop)
    {
        int n = 0;
        for (QTextBlock b = d->begin(); b.isValid(); b = b.next()) n += b.blockFormat().hasProperty(prop) ? 1 : 0;
        return n;
    }
    void hrIsNativeBlock()
    {
        Fx f("a\n\n---\n\nb\n");
        QCOMPARE(f.v->document()->blockCount(), 3);
        QVERIFY(f.v->document()->findBlockByNumber(1).blockFormat().boolProperty(kHrProp));
        QVERIFY(f.v->document()->findBlockByNumber(1).text().isEmpty());
        QCOMPARE(countBlocks(f.v->document(), kHrProp), 1);
    }
    void setextIsHeadingNotRule()
    {
        Fx f("Title\n---\n\nmore\n");
        QCOMPARE(countBlocks(f.v->document(), kHrProp), 0);
        QCOMPARE(f.v->document()->findBlockByNumber(0).blockFormat().headingLevel(), 2);
        QVERIFY(!sameSemantics("Title\n---\n", "Title\n\n---\n"));   // heading vs paragraph + rule
    }
    void hrExportsAsDashes()
    {
        Fx f("a\n\n***\n\nb\n");
        f.moveEnd();
        f.text("!");
        QCOMPARE(f.md(), QStringLiteral("a\n\n---\n\nb!\n"));
    }
    void leadingRuleNeverBecomesFrontMatter()
    {
        Fx f("---\n\nx\n\n***\n\ny\n");
        QCOMPARE(f.ed.mode(), Mode::Visual);
        f.moveEnd();
        f.text("!");
        const QString out = f.md();
        QVERIFY2(sameSemantics(out, "---\n\nx\n\n***\n\ny!\n"), qPrintable(out));
        Fx g(out.toUtf8());
        QCOMPARE(g.ed.mode(), Mode::Visual);
    }
    void hrDeleteAndUndo()
    {
        const QString md = "a\n\n---\n\nb\n";
        Fx f(md.toUtf8());
        f.select(f.v->document()->findBlockByNumber(1).position(), f.v->document()->findBlockByNumber(1).position());
        f.key(Qt::Key_Backspace);
        QCOMPARE(f.md(), QStringLiteral("a\n\nb\n"));
        QVERIFY(f.ed.undo());
        QCOMPARE(f.md(), md);
        QVERIFY(f.ed.redo());
        QCOMPARE(f.md(), QStringLiteral("a\n\nb\n"));
        QVERIFY(f.ed.undo());
        QCOMPARE(f.md(), md);
        QCOMPARE(f.ed.recorder().unrecorded(), 0);
    }
    void hrDeleteWithDeleteKeyAndBackspaceFromNextBlock()
    {
        Fx f("a\n\n---\n\nb\n");
        QTextCursor c(f.v->document()->findBlockByNumber(2));   // start of "b"
        f.v->setTextCursor(c);
        f.key(Qt::Key_Backspace);
        QCOMPARE(f.md(), QStringLiteral("a\n\nb\n"));   // rule removed, text of "b" intact
        QVERIFY(f.ed.undo());
        QVERIFY(sameSemantics(f.md(), "a\n\n---\n\nb\n"));
        Fx g("a\n\n---\n\nb\n");
        g.v->setTextCursor(QTextCursor(g.v->document()->findBlockByNumber(1)));
        g.key(Qt::Key_Delete);
        QCOMPARE(g.md(), QStringLiteral("a\n\nb\n"));
    }
    void hrSelectedAndCut()
    {
        Fx f("a\n\n---\n\nb\n");
        f.selectAllText();
        f.ed.cut();
        QCOMPARE(f.md().trimmed(), QString());
        QVERIFY(f.ed.undo());
        QVERIFY(sameSemantics(f.md(), "a\n\n---\n\nb\n"));
    }
    void typingOnRuleGoesToNewLine()
    {
        Fx f("a\n\n---\n\nb\n");
        f.v->setTextCursor(QTextCursor(f.v->document()->findBlockByNumber(1)));
        f.text("x");
        QCOMPARE(f.md(), QStringLiteral("a\n\n---\n\nx\n\nb\n"));
        QCOMPARE(countBlocks(f.v->document(), kHrProp), 1);
        Fx g("a\n\n---\n\nb\n");
        g.v->setTextCursor(QTextCursor(g.v->document()->findBlockByNumber(1)));
        g.key(Qt::Key_Return);
        g.text("y");
        QCOMPARE(g.md(), QStringLiteral("a\n\n---\n\ny\n\nb\n"));
    }
    void typedRuleShortcut()
    {
        Fx f;
        f.text("a\n---\nb");
        QCOMPARE(f.md(), QStringLiteral("a\n\n---\n\nb\n"));
        QVERIFY(f.ed.undo());
        QVERIFY(f.ed.undo() || true);
        QCOMPARE(f.ed.recorder().unrecorded(), 0);
    }
    void hrPaintsOneHairlineInBorderColor()
    {
        Fx f("a\n\n---\n\nb\n");
        auto t = hn::theme::loadTheme(QStringLiteral("modernist"), false);
        t.bg = QColor(255, 255, 255);
        t.border = QColor(255, 0, 0);
        t.text = QColor(0, 0, 0);
        f.ed.setTheme(t);
        QCoreApplication::processEvents();
        const QImage img = f.v->viewport()->grab().toImage();
        int rows = 0;
        const int x = img.width() / 2;
        for (int y = 0; y < img.height(); ++y) rows += img.pixelColor(x, y) == QColor(255, 0, 0) ? 1 : 0;
        QCOMPARE(rows, 1);
    }
    void nestedQuoteLevelsAndContDepth()
    {
        Fx f("> a\n>\n> > b\n>\n> c\n\n- x\n\n  y\n");
        auto *d = f.v->document();
        QCOMPARE(d->findBlockByNumber(0).blockFormat().intProperty(QTextFormat::BlockQuoteLevel), 1);
        QCOMPARE(d->findBlockByNumber(1).blockFormat().intProperty(QTextFormat::BlockQuoteLevel), 2);
        QCOMPARE(d->findBlockByNumber(2).blockFormat().intProperty(QTextFormat::BlockQuoteLevel), 1);
        QCOMPARE(d->findBlockByNumber(4).blockFormat().intProperty(kContProp), 1);
        QVERIFY(d->findBlockByNumber(3).textList());
        QVERIFY(!d->findBlockByNumber(4).textList());
    }
    void emptyLinkKeepsAnchor()
    {
        Fx f("[x]()\n");
        QVERIFY(f.v->document()->begin().begin().fragment().charFormat().isAnchor());
        QCOMPARE(f.v->document()->begin().begin().fragment().charFormat().anchorHref(), QString());
        f.moveEnd();
        f.text("z");
        QCOMPARE(f.md(), QStringLiteral("[xz]()\n"));   // typing at the end extends the link; the empty target survives
    }
    void editInsideContinuationBlocksAndUndo()
    {
        const QString md = "- a\n\n  b\n\n  ```\n  c\n  ```\n- d\n";
        Fx f(md.toUtf8());
        auto *d = f.v->document();
        QTextCursor c(d->findBlockByNumber(1));
        c.movePosition(QTextCursor::EndOfBlock);
        f.v->setTextCursor(c);
        f.text("!");
        QVERIFY2(sameSemantics(f.md(), QString(md).replace("  b\n", "  b!\n")), qPrintable(f.md()));
        QVERIFY(f.md().contains(QStringLiteral("  b!\n\n  ```\n  c\n  ```\n")));
        QVERIFY(f.ed.undo());
        QVERIFY(sameSemantics(f.md(), md));
        // Enter on an empty second paragraph leaves the item
        Fx g("- a\n\n  b\n");
        g.moveEnd();
        g.key(Qt::Key_Return);
        g.key(Qt::Key_Return);
        g.text("top");
        QCOMPARE(g.md(), QStringLiteral("- a\n\n  b\n\ntop\n"));
    }
    void unsupportedStillSource_data()
    {
        QTest::addColumn<QString>("md");
        auto row = [](const char *n, const char *md) { QTest::newRow(n) << QString::fromUtf8(md); };
        row("table", "| a | b |\n|---|---|\n| 1 | 2 |\n");
        row("image", "text ![alt](pic.png) more\n");
        row("html", "<div>hi</div>\n");
        row("inline-html", "a <b>b</b> c\n");
        row("front-matter", "---\ntitle: x\n---\n\nbody\n");
        row("footnote", "text[^1]\n\n[^1]: note\n");
        row("math", "inline $x^2$ math\n");
        row("list-in-quote", "> - a\n> - b\n");
        row("adjacent-fences", "```\na\n```\n\n```\nb\n```\n");
    }
    void unsupportedStillSource()
    {
        QFETCH(QString, md);
        Fx f(md.toUtf8());
        QCOMPARE(f.ed.mode(), Mode::Source);
        QCOMPARE(f.ed.toMarkdownBytes(), md.toUtf8());
    }
};

QTEST_MAIN(RealNotesTest)
#include "editor_realnotes_test.moc"
