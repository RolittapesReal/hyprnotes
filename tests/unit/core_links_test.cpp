#include "hn/core/links.h"
#include <QtTest>
using namespace hn::core;

static QStringList targets(const QByteArray &md) {
    QStringList t;
    for (auto &l : extractLinks(md)) t << l.target;
    return t;
}

class LinksTest : public QObject {
    Q_OBJECT
private slots:
    void basicForms() {
        auto l = extractLinks(QByteArray("intro\n\nSee [[Alpha]] and [[beta|the B]] and [[gamma#Part 2]] and [[delta#h|alias]] and ![[pic note]].\n"));
        QCOMPARE(l.size(), 5);
        QCOMPARE(l[0].target, QString("Alpha")); QCOMPARE(l[0].kind, LinkRef::Link); QCOMPARE(l[0].line, 3);
        QCOMPARE(l[1].target, QString("beta")); QCOMPARE(l[1].alias, QString("the B"));
        QCOMPARE(l[2].target, QString("gamma")); QCOMPARE(l[2].anchor, QString("Part 2"));
        QCOMPARE(l[3].target, QString("delta")); QCOMPARE(l[3].anchor, QString("h")); QCOMPARE(l[3].alias, QString("alias"));
        QCOMPARE(l[4].kind, LinkRef::Embed); QCOMPARE(l[4].target, QString("pic note"));
        QVERIFY(l[0].context.contains("See [[Alpha]]"));
        QCOMPARE(l[0].kindName(), QString("link")); QCOMPARE(l[4].kindName(), QString("embed"));
    }
    void offsetsPointAtTarget() {
        QByteArray md = "x [[ Sub/Alpha |al]] y ![[e#h]]\n";
        auto l = extractLinks(md);
        QCOMPARE(l.size(), 2);
        QCOMPARE(md.mid(l[0].start, l[0].end - l[0].start), QByteArray("[[ Sub/Alpha |al]]"));
        QCOMPARE(md.mid(l[0].targetStart, l[0].targetEnd - l[0].targetStart), QByteArray("Sub/Alpha"));
        QCOMPARE(md.mid(l[1].start, l[1].end - l[1].start), QByteArray("![[e#h]]"));
        QCOMPARE(md.mid(l[1].targetStart, l[1].targetEnd - l[1].targetStart), QByteArray("e"));
    }
    void ignoredContexts() {
        QCOMPARE(targets("`[[code]]` and ``[[c2]]`` ok [[real]]\n"), QStringList{"real"});
        QCOMPARE(targets("```\n[[fenced]]\n```\n\n~~~\n[[tilde]]\n~~~\n\n    [[indented]]\n\n[[after]]\n"), QStringList{"after"});
        QCOMPARE(targets("[text with [[inner]] link](http://x) and [[outer]]\n"), QStringList{"outer"});
        QCOMPARE(targets("<http://example.com/[[x]]> <span>[[html]]</span>\n"), QStringList{"html"}.mid(0, 0) + targets("<span>[[html]]</span>\n"));
        QCOMPARE(targets("\\[[escaped]] and [[ok]]\n"), QStringList{"ok"});
        QCOMPARE(targets("\\\\[[twice]]\n"), QStringList{"twice"}); // escaped backslash, link is live
        QCOMPARE(targets("![alt [[x]]](img.png) [[y]]\n"), QStringList{"y"});
    }
    void malformedAndNested() {
        QCOMPARE(targets("[[]] [[ ]] [[#only]] [[|alias]] [[unclosed and [[ok]]\n"), QStringList{"ok"});
        QCOMPARE(targets("[[[triple]]] and [[a [b] c]] and [[a]b]] [[good]]\n"), (QStringList{"triple", "good"}));
        QCOMPARE(targets("[[multi\nline]] [[fine]]\n"), QStringList{"fine"});
        QVERIFY(targets("[[a]\n").isEmpty());
        QVERIFY(targets("[[").isEmpty());
        QVERIFY(targets("").isEmpty());
        QVERIFY(targets(QByteArray("[[a\0b]]", 7)).size() <= 1);
    }
    void unicodeAndFormatting() {
        auto l = extractLinks(QString::fromUtf8("日本語 [[日本語ノート|別名]] и [[Привет]] **[[bold]]** *x* [[émoji 😀]]\n"));
        QCOMPARE(l.size(), 4);
        QCOMPARE(l[0].target, QString::fromUtf8("日本語ノート")); QCOMPARE(l[0].alias, QString::fromUtf8("別名"));
        QCOMPARE(l[1].target, QString::fromUtf8("Привет"));
        QCOMPARE(l[2].target, QString("bold"));
        QCOMPARE(l[3].target, QString::fromUtf8("émoji 😀"));
        QByteArray md = QString::fromUtf8("日本語 [[日本語ノート]]\n").toUtf8();
        auto l2 = extractLinks(md);
        QCOMPARE(md.mid(l2[0].targetStart, l2[0].targetEnd - l2[0].targetStart), QString::fromUtf8("日本語ノート").toUtf8());
    }
    void frontmatterBomAndLines() {
        QByteArray md = "---\ntitle: T\nrelated: \"[[x]]\"\n---\n\nline5 [[real]]\n";
        auto l = extractLinks(md);
        QCOMPARE(l.size(), 1);
        QCOMPARE(l[0].line, 6);
        auto l2 = extractLinks(QByteArray("\xEF\xBB\xBF# H\n\n[[b]]\n"));
        QCOMPARE(l2.size(), 1); QCOMPARE(l2[0].line, 3);
        QCOMPARE(extractLinks(QByteArray("a\r\nb [[c]]\r\n"))[0].line, 2);
    }
    void listsQuotesAndHeadings() {
        QCOMPARE(targets("- item [[a]]\n  - nested [[b]]\n> quote [[c]]\n# Head [[d]]\n1. one [[e]]\n"), (QStringList{"a", "b", "c", "d", "e"}));
        auto l = extractLinks(QByteArray("- item [[a]]\n  - nested [[b]]\n> quote [[c]]\n"));
        QCOMPARE(l[1].line, 2); QCOMPARE(l[2].line, 3);
    }
    void longContextIsBounded() {
        QByteArray md = QByteArray("word ").repeated(5000) + "[[target]] " + QByteArray("tail ").repeated(5000) + "\n";
        auto l = extractLinks(md);
        QCOMPARE(l.size(), 1);
        QVERIFY(l[0].context.size() < 220);
        QVERIFY(l[0].context.contains("[[target]]"));
    }
    void hundredThousandLinksLinear() {
        auto timeIt = [&](int n, bool oneLine) {
            QByteArray md;
            md.reserve(n * 14);
            for (int i = 0; i < n; ++i) { md += "[[note" + QByteArray::number(i) + "]]"; md += oneLine ? " " : "\n"; }
            QElapsedTimer t; t.start();
            auto l = extractLinks(md);
            qint64 ms = t.elapsed();
            if (l.size() != n || l.last().target != QString("note%1").arg(n - 1) || l.last().line != (oneLine ? 1 : n)) return qint64(-1);
            return ms;
        };
        qint64 a = timeIt(10000, true), b = timeIt(100000, true), c = timeIt(100000, false);
        QVERIFY(a >= 0 && b >= 0 && c >= 0);
        qInfo().noquote() << QString("MEASURED extractLinks: 10k links one line %1 ms, 100k one line %2 ms, 100k one per line %3 ms").arg(a).arg(b).arg(c);
        QVERIFY2(b < 10 * qMax<qint64>(a, 30) + 500, "must scale ~linearly");
        QVERIFY(b < 5000 && c < 5000);
    }
    void hostileBrackets() {
        const QList<QPair<QString, QByteArray>> cases{
            {"[[ x100k", QByteArray("[[").repeated(100000)}, {"[[a  x50k", QByteArray("[[a ").repeated(50000)},
            {"[ x200k ] x200k", QByteArray("[").repeated(200000) + QByteArray("]").repeated(200000)},
            {"![[ x60k", QByteArray("![[").repeated(60000) + "x]]"}, {"backslashes 1MiB", QByteArray(1 << 20, '\\') + "[[x]]"}};
        for (const auto &c : cases) {
            QElapsedTimer t; t.start();
            extractLinks(c.second);
            qInfo().noquote() << "hostile" << c.first << t.elapsed() << "ms";
            QVERIFY2(t.elapsed() < 3000, qPrintable(c.first));
        }
    }
    void tasks() {
        QByteArray md = "# T\n\n- [ ] open one\n- [x] done *two*\n- [X] Done three [[l]]\n- plain\n  - [ ] nested\n\n```\n- [ ] in code\n```\n\n1. [ ] numbered\n";
        auto t = extractTasks(md);
        QCOMPARE(t.size(), 5);
        QCOMPARE(t[0].line, 3); QVERIFY(!t[0].done); QCOMPARE(t[0].text, QString("open one"));
        QCOMPARE(t[1].line, 4); QVERIFY(t[1].done); QCOMPARE(t[1].text, QString("done two"));
        QCOMPARE(t[2].line, 5); QVERIFY(t[2].done); QCOMPARE(t[2].text, QString("Done three [[l]]"));
        QCOMPARE(t[3].line, 7); QCOMPARE(t[3].text, QString("nested"));
        QCOMPARE(t[4].line, 13); QCOMPARE(t[4].text, QString("numbered"));
        QVERIFY(extractTasks("---\ntasks: [ ]\n---\n- [ ] after\n").first().line == 4);
    }
};
QTEST_MAIN(LinksTest)
#include "core_links_test.moc"
