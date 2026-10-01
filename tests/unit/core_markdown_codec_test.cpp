#include "hn/core/library_index.h"
#include "hn/core/markdown_codec.h"
#include <QtTest>
using namespace hn::core;

class MarkdownCodecTest : public QObject {
    Q_OBJECT
private slots:
    void utf8_data() {
        QTest::addColumn<QByteArray>("bytes");
        QTest::addColumn<bool>("valid");
        QTest::newRow("empty") << QByteArray() << true;
        QTest::newRow("ascii") << QByteArray("hello") << true;
        QTest::newRow("unicode") << QString::fromUtf8("héllo wörld 日本語").toUtf8() << true;
        QTest::newRow("emoji") << QString::fromUtf8("👩‍👩‍👧 🎉").toUtf8() << true;
        QTest::newRow("rtl") << QString::fromUtf8("שלום עולם مرحبا").toUtf8() << true;
        QTest::newRow("nul") << QByteArray("a\0b", 3) << true;
        QTest::newRow("bom") << QByteArray("\xEF\xBB\xBFhi") << true;
        QTest::newRow("lone continuation") << QByteArray("a\x80z") << false;
        QTest::newRow("truncated 3") << QByteArray("\xE6\x97") << false;
        QTest::newRow("truncated 4") << QByteArray("ok\xF0\x9F\x8E") << false;
        QTest::newRow("overlong") << QByteArray("\xC0\xAF") << false;
        QTest::newRow("overlong3") << QByteArray("\xE0\x80\x80") << false;
        QTest::newRow("surrogate") << QByteArray("\xED\xA0\x80") << false;
        QTest::newRow("above max") << QByteArray("\xF4\x90\x80\x80") << false;
        QTest::newRow("latin1") << QByteArray("caf\xE9") << false;
        QTest::newRow("FF") << QByteArray("\xFF") << false;
    }
    void utf8() {
        QFETCH(QByteArray, bytes);
        QFETCH(bool, valid);
        QCOMPARE(isValidUtf8(bytes), valid);
    }

    void classify_data() {
        QTest::addColumn<QByteArray>("md");
        QTest::addColumn<bool>("visual");
        QTest::addColumn<QString>("reason");
        auto row = [](const char *n, const QByteArray &m, bool v, const char *r = "") { QTest::newRow(n) << m << v << QString(r); };
        row("plain", "# Title\n\nHello *world* and **bold** and ~~gone~~ `code`.\n", true);
        row("nested lists", "- a\n  - b\n    1. c\n    2. d\n- e\n", true);
        row("tasks", "- [ ] todo\n- [x] done\n", true);
        row("fence", "```cpp\nint x = 1 < 2;\n<div>not html</div>\n| a | b |\n|---|---|\n![x](y)\n```\n", true);
        row("quote+link", "> quote\n\n[text](http://example.com \"t\") <http://auto.link>\n", true);
        row("unicode", QString::fromUtf8("# Привет\n\nעברית مرحبا 日本語 👩‍👩‍👧\n").toUtf8(), true);
        row("underscore emphasis", "_em_ and __strong__ snake_case_word\n", true);
        row("dollar prices", "costs $5 and $6\n", true);
        row("hr not front matter", "intro\n\n---\n\nmore\n", true);
        row("dashes no close", "---\nnot closed\n", true);
        row("invalid utf8", "caf\xE9", false, "UTF-8");
        row("table", "| a | b |\n|---|---|\n| 1 | 2 |\n", false, "table");
        row("image", "see ![alt](pic.png)\n", false, "image");
        row("html block", "<div>\nhi\n</div>\n", false, "HTML");
        row("inline html", "a <b>bold</b> b\n", false, "HTML");
        row("front matter", "---\ntitle: x\n---\n\n# hi\n", false, "front matter");
        row("front matter bom crlf", "\xEF\xBB\xBF---\r\ntitle: x\r\n---\r\nbody\r\n", false, "front matter");
        row("footnote", "text[^1]\n\n[^1]: note\n", false, "footnote");
        row("footnote no def", "text[^note] here\n", false, "footnote");
        row("math inline", "Euler $e^{i\\pi}+1=0$ done\n", false, "math");
        row("math display", "$$\nx^2\n$$\n", false, "math");
    }
    void classify() {
        QFETCH(QByteArray, md);
        QFETCH(bool, visual);
        QFETCH(QString, reason);
        auto c = classifyMarkdown(md);
        QCOMPARE(c.visual, visual);
        if (visual) QVERIFY(c.reason.isEmpty());
        else QVERIFY2(c.reason.contains(reason, Qt::CaseInsensitive), qPrintable(c.reason));
    }
    void classifyLarge() {
        QByteArray big = QByteArray("word ").repeated(256 * 1024 / 5 + 10);
        auto c = classifyMarkdown(big);
        QVERIFY(!c.visual);
        QVERIFY(c.reason.contains("256"));
        QVERIFY(classifyMarkdown(big, 1 << 20).visual);
        QVERIFY(classifyMarkdown(QByteArray("x").repeated(256 * 1024)).visual); // exactly at the limit
    }

    void tokens_equal_data() {
        QTest::addColumn<QString>("a");
        QTest::addColumn<QString>("b");
        QTest::newRow("emphasis delimiters") << "*a* **b**\n" << "_a_ __b__\n";
        QTest::newRow("list markers") << "- a\n- b\n" << "* a\n* b\n";
        QTest::newRow("setext vs atx") << "Title\n=====\n" << "# Title\n";
        QTest::newRow("indented vs fenced code") << "    x = 1\n" << "```\nx = 1\n```\n";
        QTest::newRow("trailing newline") << "hello" << "hello\n";
        QTest::newRow("entity") << "a &amp; b" << "a & b";
        QTest::newRow("unicode") << QString::fromUtf8("日本語 👩‍👩‍👧 שלום") << QString::fromUtf8("日本語 👩‍👩‍👧 שלום\n");
        QTest::newRow("task") << "- [x] a\n" << "* [X] a\n";
    }
    void tokens_equal() {
        QFETCH(QString, a);
        QFETCH(QString, b);
        QCOMPARE(semanticTokens(a), semanticTokens(b));
    }
    void tokens_differ_data() {
        QTest::addColumn<QString>("a");
        QTest::addColumn<QString>("b");
        QTest::newRow("em vs strong") << "*a*\n" << "**a**\n";
        QTest::newRow("task state") << "- [ ] a\n" << "- [x] a\n";
        QTest::newRow("task vs plain") << "- [ ] a\n" << "- a\n";
        QTest::newRow("link target") << "[a](http://x)\n" << "[a](http://y)\n";
        QTest::newRow("code contents") << "```\na\n```\n" << "```\nb\n```\n";
        QTest::newRow("code lang") << "```c\na\n```\n" << "```d\na\n```\n";
        QTest::newRow("hard break") << "a  \nb\n" << "a\nb\n";
        QTest::newRow("paragraph split") << "a\n\nb\n" << "a\nb\n";
        QTest::newRow("heading level") << "# a\n" << "## a\n";
        QTest::newRow("nesting") << "- a\n  - b\n" << "- a\n- b\n";
        QTest::newRow("ordered vs bullet") << "1. a\n" << "- a\n";
        QTest::newRow("whitespace in code") << "`a b`\n" << "`a  b`\n";
        QTest::newRow("underscore not underline") << "_a_\n" << "a\n";
        QTest::newRow("strike") << "~~a~~\n" << "a\n";
        QTest::newRow("emoji text") << QString::fromUtf8("😀\n") << QString::fromUtf8("😃\n");
        QTest::newRow("quote") << "> a\n" << "a\n";
    }
    void tokens_differ() {
        QFETCH(QString, a);
        QFETCH(QString, b);
        QVERIFY(semanticTokens(a) != semanticTokens(b));
    }
    void tokensDeterministic() {
        QString md = "# H\n\n- [ ] a\n  - b\n\n```x\ncode\n```\n> q\n";
        QVERIFY(semanticTokens(md).size() > 10);
        QCOMPARE(semanticTokens(md), semanticTokens(md));
    }

    void tags_data() {
        QTest::addColumn<QString>("md");
        QTest::addColumn<QStringList>("tags");
        QTest::newRow("basic") << "hello #Idea and #to-do_1\n" << QStringList{"idea", "to-do_1"};
        QTest::newRow("heading marker") << "# Title\n\n## Sub\n" << QStringList{};
        QTest::newRow("heading with tag") << "# Title #x\n" << QStringList{"x"};
        QTest::newRow("no space is a paragraph tag") << "#tag\n" << QStringList{"tag"};
        QTest::newRow("mid-word") << "abc#tag a#b\n" << QStringList{};
        QTest::newRow("code span") << "`#no` #yes\n" << QStringList{"yes"};
        QTest::newRow("code fence") << "```\n#no\n```\n#yes\n" << QStringList{"yes"};
        QTest::newRow("indented code") << "para\n\n    #no\n" << QStringList{};
        QTest::newRow("link text") << "[#no](http://x/#frag) #yes\n" << QStringList{"yes"};
        QTest::newRow("url fragment") << "see http://x.com/a#frag and <http://y/#z>\n" << QStringList{};
        QTest::newRow("unicode") << QString::fromUtf8("#café #日本語 #Ünï #привет\n") << QStringList{QString::fromUtf8("café"), QString::fromUtf8("日本語"), QString::fromUtf8("ünï"), QString::fromUtf8("привет")};
        QTest::newRow("emoji not tag char") << QString::fromUtf8("#😀 #ok😀\n") << QStringList{"ok"};
        QTest::newRow("lone hash") << "# \n# x\na # b\n" << QStringList{};
        QTest::newRow("double hash") << "##tag\n" << QStringList{};
        QTest::newRow("dedupe") << "#a #A #a\n" << QStringList{"a"};
        QTest::newRow("list item") << "- #one\n  - #two\n" << QStringList{"one", "two"};
        QTest::newRow("after emphasis") << "**#bold** _#it_\n" << QStringList{"bold", "it"};
        QTest::newRow("punct follows") << "(#a) #b, #c.\n" << QStringList{"b", "c"};
        QTest::newRow("after soft break") << "line\n#next\n" << QStringList{"next"};
        QTest::newRow("image alt") << "![#no](x.png) #yes\n" << QStringList{"yes"};
        QTest::newRow("entity") << "&#35;notatag #yes\n" << QStringList{"yes"};
    }
    void tags() {
        QFETCH(QString, md);
        QFETCH(QStringList, tags);
        QCOMPARE(extractMeta("file", md.toUtf8()).tags, tags);
    }
    void titles() {
        QCOMPARE(extractMeta("fn", "intro\n\n## Second *one*\n# Later\n").title, QString("Second one"));
        QCOMPARE(extractMeta("fn", "no heading here\n").title, QString("fn"));
        QCOMPARE(extractMeta("fn", "#\n\ntext\n").title, QString("fn"));
        QCOMPARE(extractMeta("fn", "Setext\n======\n").title, QString("Setext"));
        QCOMPARE(extractMeta("fn", "# `code` and [link](x)\n").title, QString("code and link"));
        QCOMPARE(extractMeta("fn", QString::fromUtf8("# 日本語 👩‍👩‍👧\n").toUtf8()).title, QString::fromUtf8("日本語 👩‍👩‍👧"));
    }
};
QTEST_APPLESS_MAIN(MarkdownCodecTest)
#include "core_markdown_codec_test.moc"
