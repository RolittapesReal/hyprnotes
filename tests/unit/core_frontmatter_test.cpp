#include "hn/core/frontmatter.h"
#include <QtTest>
#include <random>
using namespace hn::core;

class FmTest : public QObject {
    Q_OBJECT
private slots:
    void scalarsAndLists() {
        auto f = parseFrontmatter("---\ntitle: My Note\naliases: [Alt One, \"Alt, Two\", 'It''s']\ntags:\n  - Work\n  - '#Deep-Dive'\n  - a/b\nauthor: me # trailing comment\nstatus: \"draft # not a comment\"\n# full comment\nempty:\ncount: 3\n---\nbody\n");
        QVERIFY(f.present);
        QCOMPARE(f.title, QString("My Note"));
        QCOMPARE(f.aliases, (QStringList{"Alt One", "Alt, Two", "It's"}));
        QCOMPARE(f.tags, (QStringList{"work", "deep-dive", "a/b"}));
        QCOMPARE(f.fields["author"].scalar(), QString("me"));
        QCOMPARE(f.fields["status"].scalar(), QString("draft # not a comment"));
        QCOMPARE(f.fields["empty"].scalar(), QString(""));
        QVERIFY(!f.fields["empty"].isList);
        QCOMPARE(f.fields["count"].scalar(), QString("3"));
        QVERIFY(f.fields["tags"].isList);
        QCOMPARE(f.endLine, 13);
        QCOMPARE(QByteArray("---\ntitle: My Note\naliases: [Alt One, \"Alt, Two\", 'It''s']\ntags:\n  - Work\n  - '#Deep-Dive'\n  - a/b\nauthor: me # trailing comment\nstatus: \"draft # not a comment\"\n# full comment\nempty:\ncount: 3\n---\nbody\n").mid(f.endOffset), QByteArray("body\n"));
    }
    void scalarTagsAndAliases() {
        auto f = parseFrontmatter("---\ntags: one, Two three\naliases: Solo Alias\n---\n");
        QCOMPARE(f.tags, (QStringList{"one", "two", "three"}));
        QCOMPARE(f.aliases, QStringList{"Solo Alias"});
        auto g = parseFrontmatter("---\ntags: [ ]\naliases:\n---\n");
        QVERIFY(g.present); QVERIFY(g.tags.isEmpty()); QVERIFY(g.aliases.isEmpty());
        auto h = parseFrontmatter("---\ntags: [a, bad tag!, ok]\n---\n");
        QCOMPARE(h.tags, (QStringList{"a", "ok"}));
    }
    void crlfBomDots() {
        auto f = parseFrontmatter("\xEF\xBB\xBF---\r\ntitle: Win\r\ntags: [x]\r\n...\r\ntext");
        QVERIFY(f.present); QCOMPARE(f.title, QString("Win")); QCOMPARE(f.tags, QStringList{"x"});
        QCOMPARE(f.endLine, 4);
    }
    void notFrontmatter() {
        for (QByteArray b : {QByteArray(""), QByteArray("text\n---\na: b\n---\n"), QByteArray(" ---\na: b\n---\n"), QByteArray("----\na: b\n---\n"),
                             QByteArray("---\na: b\n"), QByteArray("---\n"), QByteArray("---")})
            QVERIFY2(!parseFrontmatter(b).present, b.constData());
    }
    void malformedMeansNone() {
        for (QByteArray b : {"---\ntitle: \"unterminated\n---\n", "---\nnested:\n  key: v\n---\n", "---\n- orphan\n---\n", "---\nk: [a, b\n---\n",
                             "---\nk: {a: 1}\n---\n", "---\nk: |\n  text\n---\n", "---\njust text\n---\n", "---\nk: [[a]]\n---\n", "---\na:b\n---\n",
                             "---\ntitle: x\n  indented: 1\n---\n", "---\nk: \"a\" junk\n---\n", "---\nk: \"bad \\q escape\"\n---\n"}) {
            QVERIFY2(!parseFrontmatter(b).present, b.constData());
        }
        QVERIFY(!parseFrontmatter(QByteArray("---\ntitle: \xE9\n---\n")).present); // invalid UTF-8
        QVERIFY(!parseFrontmatter(QByteArray("---\ntitle: a\0b\n---\n", 20)).present);
    }
    void sizeCap() {
        QByteArray ok = "---\nk: " + QByteArray(15 * 1024, 'x') + "\n---\n";
        QVERIFY(parseFrontmatter(ok).present);
        QByteArray big = "---\nk: " + QByteArray(17 * 1024, 'x') + "\n---\n";
        QVERIFY(!parseFrontmatter(big).present);
        QByteArray many = "---\n";
        for (int i = 0; i < 300; ++i) many += "k" + QByteArray::number(i) + ": v\n";
        QVERIFY(!parseFrontmatter(many + "---\n").present);
        QByteArray noClose = "---\n" + QByteArray("a: b\n").repeated(100000);
        QVERIFY(!parseFrontmatter(noClose).present);
    }
    void fuzzNeverThrows() {
        std::mt19937 rng(7);
        const char *atoms[] = {"---\n", "...\n", "title: ", "tags: ", "aliases:\n", "  - ", "- ", "[", "]", ",", "\"", "'", "#", ":", " ", "\n", "\r\n", "x", "é", "\\", "{", "}", "|", ">", "&", "*", "\xFF", "\xEF\xBB\xBF"};
        for (int i = 0; i < 5000; ++i) {
            QByteArray b = rng() % 2 ? "---\n" : "";
            int n = rng() % 40;
            for (int k = 0; k < n; ++k) b += atoms[rng() % (sizeof(atoms) / sizeof(*atoms))];
            if (rng() % 2) b += "---\n";
            auto f = parseFrontmatter(b);
            if (f.present) QVERIFY(f.endOffset <= b.size());
        }
    }
};
QTEST_MAIN(FmTest)
#include "core_frontmatter_test.moc"
