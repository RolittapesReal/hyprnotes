// Import (Qt/MD4C) -> our exporter -> semantic equality against the codec, for constructs the editor supports.
#include <QTextDocument>

#include "editor_test_util.h"

using namespace hntest;

class RoundTripTest : public QObject {
    Q_OBJECT
private slots:
    void supportedConstructsRoundTrip_data()
    {
        QTest::addColumn<QString>("md");
        auto row = [](const char *n, const char *md) { QTest::newRow(n) << QString::fromUtf8(md); };
        row("paragraphs", "one\n\ntwo\n\nthree\n");
        row("headings", "# H1\n\n## H2\n\n### H3\n\n#### H4\n\n##### H5\n\n###### H6\n");
        row("inline", "a **b** *c* ~~d~~ `e` f\n");
        row("nested-inline", "**a *b* c** and ~~x **y**~~ and ***z***\n");
        row("underscores", "snake_case_word _em_ __strong__ and \\_lit\\_\n");
        row("escapes", "a \\* b \\[c\\] \\<d\\> \\\\ \\# \\~\\~ &amp; 1\\. x\n");
        row("link", "see [site](https://example.org/a_b) and [p](<https://x.y/a(b)>)\n");
        row("bold-link", "**[bold link](https://a.b)** and *[em link](https://a.b)* and [a **b**](https://a.b)\n");
        row("lists", "- a\n- b\n- c\n");
        row("ordered", "1. a\n2. b\n3. c\n");
        row("ordered-start", "3. a\n4. b\n");
        row("nested", "- a\n  - b\n    - c\n  - d\n- e\n");
        row("nested-ordered", "1. a\n   1. b\n   2. c\n2. d\n");
        row("tasks", "- [ ] a\n- [x] b\n  - [ ] c\n");
        row("quote", "> quote\n");
        row("quote2", "> a\n>\n> b\n\nafter\n");
        row("code", "```py\nx = 1\n\ny = 2\n```\n");
        row("code-nolang", "```\nraw *text* <b>\n```\n");
        row("code-fence-in-code", "````\n```\ninner\n```\n````\n");
        row("codespan-backtick", "use `` a`b `` here\n");
        row("mixed", "# T\n\nintro\n\n- a\n- b\n\n> q\n\n```\ncode\n```\n\nend\n");
        row("list-then-para", "- a\n- b\n\npara\n");
        row("unicode", "caf\xC3\xA9 \xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x92\xBB \xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D\n");
        row("line-start-specials", "\\# not heading\n\n\\- not list\n\n1\\. not ordered\n\n\\> not quote\n");
    }
    void supportedConstructsRoundTrip()
    {
        QFETCH(QString, md);
        Fx f(md.toUtf8());
        QVERIFY2(f.ed.mode() == Mode::Visual, qPrintable(f.ed.modeReason()));
        f.moveEnd();
        f.text("z");
        f.key(Qt::Key_Backspace);   // edit + revert: force the serializer
        QVERIFY(f.ed.isModified());
        const QString out = f.md();
        QVERIFY2(sameSemantics(md, out), qPrintable(md + "\n--- exported ---\n" + out));
        // idempotent: a second generation is semantically identical and stays visual
        Fx g(out.toUtf8());
        QVERIFY2(g.ed.mode() == Mode::Visual, qPrintable(g.ed.modeReason()));
    }
    void hardBreakExport()
    {
        Fx f;
        f.text("a");
        f.key(Qt::Key_Return, Qt::ShiftModifier);
        f.text("b");
        f.key(Qt::Key_Return);
        f.text("- x");
        f.key(Qt::Key_Return, Qt::ShiftModifier);
        f.text("y");
        const QString out = f.md();
        QCOMPARE(out, QStringLiteral("a\\\nb\n\n- x\\\n  y\n"));
        QVERIFY(sameSemantics(out, QStringLiteral("a  \nb\n\n- x  \n  y\n")));
    }
    void emphasisWhitespaceStaysOutsideDelimiters()
    {
        Fx f;
        f.text("a b c");
        f.select(1, 4);   // " b "
        f.ed.toggleInline(InlineStyle::Bold);
        QCOMPARE(f.md(), QStringLiteral("a **b** c\n"));
    }
    void adjacentListsStayApart()
    {
        Fx f;
        f.text("- a\n\n");   // exit list via Enter on empty item
        f.key(Qt::Key_Return);
        f.text("- b");
        const QString m = f.md();
        QVERIFY2(m.contains(QStringLiteral("- a")) && (m.contains(QStringLiteral("* b")) || m.contains(QStringLiteral("\n\n- b"))), qPrintable(m));
    }
};

QTEST_MAIN(RoundTripTest)
#include "editor_roundtrip_test.moc"
