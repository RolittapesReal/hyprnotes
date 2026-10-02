// Link graph, frontmatter feed, tasks, resolution, dirty merge, rename rewrite, schema migration + 10k-note performance.
#include "hn/core/library_index.h"
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QtTest>
#include <algorithm>
#include <random>
using namespace hn::core;

static void writeFile(const QString &p, const QByteArray &b) {
    QDir().mkpath(QFileInfo(p).absolutePath());
    QFile f(p);
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(b);
}
static QByteArray readFile(const QString &p) { QFile f(p); return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray(); }

class GraphTest : public QObject {
    Q_OBJECT
    QTemporaryDir m_root, m_cache;
    QString abs(const QString &r) { return m_root.path() + "/" + r; }
    std::unique_ptr<LibraryIndex> newIndex() { return std::make_unique<LibraryIndex>(m_root.path(), m_cache.path()); }
    static bool syncAndWait(LibraryIndex &idx, int timeoutMs = 300000) {
        QSignalSpy sp(&idx, &LibraryIndex::synced);
        quint64 gen = idx.sync();
        return sp.wait(timeoutMs) && sp.at(0).at(0).toULongLong() == gen;
    }
    static bool updateAndWait(LibraryIndex &idx, const QString &rel) {
        QSignalSpy sp(&idx, &LibraryIndex::pathUpdated);
        idx.updatePath(rel);
        return sp.wait(10000);
    }
    static QStringList paths(const QList<LinkRow> &rows) { QStringList p; for (auto &r : rows) p << r.srcPath + ":" + QString::number(r.line); return p; }
    void baseLibrary() {
        writeFile(abs("a.md"), "# A\n\nSee [[b]] and [[Sub/C]] and [[missing]] and [[dup]] and [[ali]]\nthen [[B#Head|bee]] and ![[b]] and `[[code]]`\n");
        writeFile(abs("b.md"), "# B\n\n## Head\n\n- [ ] open task in b\n- [x] done task in b\n");
        writeFile(abs("Sub/c.md"), "# C\n\nback to [[../a]] and [[./z]]\n");
        writeFile(abs("Sub/z.md"), "# Z\n");
        writeFile(abs("x/dup.md"), "# dup x\n");
        writeFile(abs("y/dup.md"), "# dup y\n");
        writeFile(abs("d.md"), "---\ntitle: Dee\naliases: [ali, \"Another Name\"]\ntags: [T1, t2]\nrating: 7\ncolor: red\n---\n# Heading Ignored\n\ntext #body-tag #t1\n");
    }
private slots:
    void init() {
        QDir(m_root.path()).removeRecursively();
        QDir(m_cache.path()).removeRecursively();
        QDir().mkpath(m_root.path());
    }

    void frontmatterFeedsTitleTagsAliases() {
        baseLibrary();
        auto idx = newIndex();
        QVERIFY(syncAndWait(*idx));
        auto rows = idx->searchNow({.tag = "t2"}).rows;
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows[0].relPath, QString("d.md"));
        QCOMPARE(rows[0].title, QString("Dee"));                        // frontmatter title beats first heading
        QCOMPARE(rows[0].tags, (QStringList{"t1", "t2", "body-tag"}));  // frontmatter first, #tags kept, deduped
        QCOMPARE(idx->searchNow({.tag = "body-tag"}).rows.size(), 1);
        // the '#tag' rule is untouched: headings and "# " never create tags, code is ignored
        QCOMPARE(idx->tags().size(), 3);
        NoteMeta m = extractMeta("n", "---\ntags: [x]\naliases: [al]\n---\n# H\n\n#y `#z`\n");
        QCOMPARE(m.title, QString("H")); QCOMPARE(m.tags, (QStringList{"x", "y"})); QCOMPARE(m.aliases, QStringList{"al"});
        // malformed frontmatter => body parsed normally (no crash, no tags from it)
        NoteMeta bad = extractMeta("n", "---\ntags: [oops\n---\n# H\n");
        QVERIFY(bad.tags.isEmpty());
        // meta rows are queryable
        auto r = idx->query(QJsonObject{{"from", "notes"}, {"where", QJsonArray{QJsonObject{{"field", "meta.rating"}, {"op", ">"}, {"value", 5}}}}, {"select", QJsonArray{"path", "meta.color"}}});
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.rows.size(), 1);
        QCOMPARE(r.rows[0][0].toString(), QString("d.md")); QCOMPARE(r.rows[0][1].toString(), QString("red"));
    }

    void resolutionOrder() {
        baseLibrary();
        auto idx = newIndex();
        QVERIFY(syncAndWait(*idx));
        auto r = idx->resolve("b", "a.md");
        QCOMPARE(r.state, Resolution::Resolved); QCOMPARE(r.path, QString("b.md")); QCOMPARE(r.via, Resolution::Path);              // root-relative exact path wins
        QCOMPARE(idx->resolve("B").via, Resolution::Basename);
        QCOMPARE(idx->resolve("B").path, QString("b.md"));                         // case-insensitive
        r = idx->resolve("b.md");
        QCOMPARE(r.via, Resolution::Path);
        QCOMPARE(idx->resolve("Sub/C").path, QString("Sub/c.md"));                  // path suffix, case-insensitive
        r = idx->resolve("x/dup");
        QCOMPARE(r.state, Resolution::Resolved); QCOMPARE(r.via, Resolution::Path); QCOMPARE(r.path, QString("x/dup.md"));
        r = idx->resolve("dup");
        QCOMPARE(r.state, Resolution::Ambiguous); QCOMPARE(r.candidates, (QStringList{"x/dup.md", "y/dup.md"}));
        r = idx->resolve("ali");
        QCOMPARE(r.state, Resolution::Resolved); QCOMPARE(r.via, Resolution::Alias); QCOMPARE(r.path, QString("d.md"));
        QCOMPARE(idx->resolve("ANOTHER name").path, QString("d.md"));
        QCOMPARE(idx->resolve("missing").state, Resolution::Unresolved);
        QCOMPARE(idx->resolve("").state, Resolution::Unresolved);
        QCOMPARE(idx->resolve("../a", "Sub/c.md").path, QString("a.md"));
        QCOMPARE(idx->resolve("./z", "Sub/c.md").path, QString("Sub/z.md"));
        QCOMPARE(idx->resolve("../../../etc/passwd", "Sub/c.md").state, Resolution::Unresolved);
        // exact path beats basename: root b.md vs. a note named b in a folder
        writeFile(abs("q/b.md"), "# other b\n");
        QVERIFY(updateAndWait(*idx, "q/b.md"));
        QCOMPARE(idx->resolve("b").path, QString("b.md"));   // exact root path still beats the new same-named note
        QCOMPARE(idx->resolve("B").state, Resolution::Ambiguous); // but case-insensitive basename is now ambiguous
        QCOMPARE(idx->resolve("b.md").path, QString("b.md"));
    }

    void linksBacklinksUnresolved() {
        baseLibrary();
        auto idx = newIndex();
        QVERIFY(syncAndWait(*idx));
        auto from = idx->linksFrom("a.md");
        QCOMPARE(from.size(), 7);
        QCOMPARE(from[0].target, QString("b")); QVERIFY(from[0].resolved); QCOMPARE(from[0].destPath, QString("b.md")); QCOMPARE(from[0].line, 3);
        QVERIFY(!from[2].resolved && !from[2].ambiguous);                      // missing
        QVERIFY(from[3].ambiguous && !from[3].resolved);                       // dup
        QCOMPARE(from[4].destPath, QString("d.md"));                           // alias
        QCOMPARE(from[5].anchor, QString("Head")); QCOMPARE(from[5].alias, QString("bee")); QCOMPARE(from[5].line, 4);
        QCOMPARE(from[6].kind, QString("embed"));
        auto bl = idx->backlinks("b.md");
        QCOMPARE(bl.rows.size(), 3);
        QCOMPARE(bl.rows[0].srcPath, QString("a.md"));
        QVERIFY(bl.rows[0].context.contains("[[b]]"));
        QVERIFY(!bl.hasMore);
        auto p = idx->backlinks("b.md", 2, 0);
        QCOMPARE(p.rows.size(), 2); QVERIFY(p.hasMore);
        auto p2 = idx->backlinks("b.md", 2, 2);
        QCOMPARE(p2.rows.size(), 1); QVERIFY(!p2.hasMore);
        QCOMPARE(p2.rows[0].kind, QString("embed"));
        QCOMPARE(idx->backlinks("a.md").rows.size(), 1);                       // from Sub/c.md ([[../a]])
        QCOMPARE(idx->backlinks("Sub/z.md").rows.size(), 1);                   // [[./z]]
        QCOMPARE(idx->backlinks("d.md").rows.size(), 1);                       // via alias
        QCOMPARE(idx->backlinks("nonexistent.md").rows.size(), 0);
        auto un = idx->unresolvedLinks();
        QCOMPARE(un.rows.size(), 2);
        QCOMPARE(un.rows[0].target, QString("missing")); QCOMPARE(un.rows[1].target, QString("dup")); QVERIFY(un.rows[1].ambiguous);
    }

    void incrementalGraphMaintenance() {
        baseLibrary();
        auto idx = newIndex();
        QVERIFY(syncAndWait(*idx));
        // new note resolves a dangling link
        writeFile(abs("missing.md"), "# Missing now exists\n");
        QVERIFY(updateAndWait(*idx, "missing.md"));
        QCOMPARE(idx->backlinks("missing.md").rows.size(), 1);
        QCOMPARE(idx->unresolvedLinks().rows.size(), 1);
        // editing a note replaces its links
        writeFile(abs("a.md"), "# A\n\nonly [[b]] now\n");
        QVERIFY(updateAndWait(*idx, "a.md"));
        QCOMPARE(idx->linksFrom("a.md").size(), 1);
        QCOMPARE(idx->backlinks("b.md").rows.size(), 1);
        QCOMPARE(idx->backlinks("missing.md").rows.size(), 0);
        QCOMPARE(idx->unresolvedLinks().rows.size(), 0);
        // deleting a target makes the link unresolved again; a duplicate basename makes it ambiguous
        QFile::remove(abs("b.md"));
        QVERIFY(updateAndWait(*idx, "b.md"));
        QCOMPARE(idx->backlinks("b.md").rows.size(), 0);
        QCOMPARE(idx->unresolvedLinks().rows.size(), 1);
        writeFile(abs("b.md"), "# B again\n");
        QVERIFY(updateAndWait(*idx, "b.md"));
        QCOMPARE(idx->unresolvedLinks().rows.size(), 0);
        QCOMPARE(idx->backlinks("b.md").rows.size(), 1);
        writeFile(abs("a.md"), "# A\n\nonly [[bb]] now\n");
        writeFile(abs("m/bb.md"), "# bb one\n");
        QVERIFY(updateAndWait(*idx, "a.md"));
        QVERIFY(updateAndWait(*idx, "m/bb.md"));
        QCOMPARE(idx->backlinks("m/bb.md").rows.size(), 1);
        writeFile(abs("n/BB.md"), "# bb two\n");
        QVERIFY(updateAndWait(*idx, "n/BB.md"));
        QCOMPARE(idx->backlinks("m/bb.md").rows.size(), 0);   // now ambiguous
        auto un = idx->unresolvedLinks();
        QCOMPARE(un.rows.size(), 1); QVERIFY(un.rows[0].ambiguous);
        QFile::remove(abs("n/BB.md"));
        QVERIFY(updateAndWait(*idx, "n/BB.md"));
        QCOMPARE(idx->backlinks("m/bb.md").rows.size(), 1);
        writeFile(abs("a.md"), "# A\n\nonly [[b]] now\n");
        QVERIFY(updateAndWait(*idx, "a.md"));
        // alias edits re-resolve links that used the alias
        writeFile(abs("d.md"), "---\naliases: [other]\n---\n# D\n");
        QVERIFY(updateAndWait(*idx, "d.md"));
        QCOMPARE(idx->resolve("ali").state, Resolution::Unresolved);
        QCOMPARE(idx->resolve("other").path, QString("d.md"));
        // a full sync after external edits keeps everything consistent
        writeFile(abs("a.md"), "# A\n\n[[other]] [[b]] [[Sub/c]]\n");
        QVERIFY(syncAndWait(*idx));
        QCOMPARE(idx->backlinks("d.md").rows.size(), 1);
        QCOMPARE(idx->backlinks("Sub/c.md").rows.size(), 1);
    }

    void renamePathKeepsLinksConsistent() {
        baseLibrary();
        auto idx = newIndex();
        QVERIFY(syncAndWait(*idx));
        QSignalSpy up(idx.get(), &LibraryIndex::pathUpdated);
        QVERIFY(QFile::rename(abs("b.md"), abs("moved.md")));
        idx->renamePath("b.md", "moved.md");
        QVERIFY(up.wait(5000));
        QCOMPARE(idx->resolve("b").state, Resolution::Unresolved);   // old basename no longer exists
        QCOMPARE(idx->resolve("moved").path, QString("moved.md"));
        QCOMPARE(idx->backlinks("moved.md").rows.size(), 0);         // links still say [[b]]
        QCOMPARE(idx->tasks({.path = "moved.md"}).rows.size(), 2);   // tasks followed the note
    }

    void tasksApi() {
        baseLibrary();
        writeFile(abs("t.md"), "# T\n\n- [ ] alpha 100%\n- [ ] beta_x\n- [x] gamma\n\n```\n- [ ] ignored\n```\n");
        auto idx = newIndex();
        QVERIFY(syncAndWait(*idx));
        auto all = idx->tasks();
        QCOMPARE(all.rows.size(), 5);
        QCOMPARE(all.rows[0].path, QString("b.md")); QCOMPARE(all.rows[0].line, 5); QVERIFY(!all.rows[0].done);
        QCOMPARE(idx->tasks({.done = 1}).rows.size(), 2);
        QCOMPARE(idx->tasks({.done = 0}).rows.size(), 3);
        QCOMPARE(idx->tasks({.text = "100%"}).rows.size(), 1);   // LIKE wildcards escaped
        QCOMPARE(idx->tasks({.text = "b_ta"}).rows.size(), 0);
        QCOMPARE(idx->tasks({.text = "BETA"}).rows.size(), 1);
        auto p = idx->tasks({.limit = 2, .offset = 1});
        QCOMPARE(p.rows.size(), 2); QVERIFY(p.hasMore);
        // dirty: unsaved checkbox changes win over disk
        auto d = idx->tasks({}, {{"t.md", "# T\n\n- [x] alpha 100%\n- [ ] brand new\n"}, {"fresh.md", "- [ ] fresh task\n"}});
        QStringList texts;
        for (auto &r : d.rows) texts << r.path + ":" + r.text + (r.done ? "!" : "") + (r.dirty ? "*" : "");
        QCOMPARE(texts, (QStringList{"b.md:open task in b", "b.md:done task in b!", "fresh.md:fresh task*", "t.md:alpha 100%!*", "t.md:brand new*"}));
    }

    void dirtyDocsMergeIntoLinks() {
        baseLibrary();
        auto idx = newIndex();
        QVERIFY(syncAndWait(*idx));
        // unsaved a.md no longer links to b; unsaved new.md does
        QList<DirtyDoc> dirty{{"a.md", "# A\n\nno links now\n"}, {"new.md", "# N\n\nlink [[b]] here\n"}};
        auto bl = idx->backlinks("b.md", 100, 0, dirty);
        QCOMPARE(paths(bl.rows), QStringList{"new.md:3"});
        QVERIFY(bl.rows[0].dirty);
        QCOMPARE(idx->backlinks("b.md").rows.size(), 3);   // disk unchanged
        auto lf = idx->linksFrom("a.md", dirty);
        QVERIFY(lf.isEmpty());
        auto un = idx->unresolvedLinks(100, 0, {{"new.md", "[[nothing]] [[b]]"}});
        QStringList t; for (auto &r : un.rows) t << r.target;
        QVERIFY(t.contains("nothing")); QVERIFY(t.contains("missing"));
        auto un2 = idx->unresolvedLinks(100, 0, {{"a.md", "plain"}});
        QVERIFY(un2.rows.isEmpty() || !paths(un2.rows).join(",").contains("a.md"));
        // paging over merged results
        QList<DirtyDoc> many;
        for (int i = 0; i < 7; ++i) many.append({QString("z%1.md").arg(i), "[[b]]"});
        auto p = idx->backlinks("b.md", 4, 8, many);   // 3 disk + 7 dirty = 10 rows
        QCOMPARE(p.rows.size(), 2); QVERIFY(!p.hasMore);
        auto p0 = idx->backlinks("b.md", 4, 0, many);
        QVERIFY(p0.hasMore);
    }

    void asyncSupersedingCancelsOlder() {
        baseLibrary();
        auto idx = newIndex();
        QVERIFY(syncAndWait(*idx));
        QSignalSpy bs(idx.get(), &LibraryIndex::backlinksFinished), ts(idx.get(), &LibraryIndex::tasksFinished), qs(idx.get(), &LibraryIndex::queryFinished);
        quint64 lb = 0, lt = 0, lq = 0;
        for (int i = 0; i < 25; ++i) {
            lb = idx->backlinksAsync("b.md");
            lt = idx->tasksAsync({});
            lq = idx->queryAsync(QJsonObject{{"from", "notes"}});
        }
        QVERIFY(bs.wait(5000)); QVERIFY(ts.size() > 0 || ts.wait(5000)); QVERIFY(qs.size() > 0 || qs.wait(5000));
        QTest::qWait(200);
        QCOMPARE(bs.size(), 1); QCOMPARE(ts.size(), 1); QCOMPARE(qs.size(), 1);
        QCOMPARE(bs.at(0).at(0).value<LinkPage>().id, lb);
        QCOMPARE(ts.at(0).at(0).value<TaskPage>().id, lt);
        QCOMPARE(qs.at(0).at(0).value<QueryResult>().id, lq);
        QCOMPARE(bs.at(0).at(0).value<LinkPage>().rows.size(), 3);
        QVERIFY(qs.at(0).at(0).value<QueryResult>().ok);
    }

    void schemaV1CacheIsDroppedAndRebuilt() {
        baseLibrary();
        QString db;
        {
            auto idx = newIndex();
            db = idx->databasePath();
            QVERIFY(syncAndWait(*idx));
            QCOMPARE(idx->backlinks("b.md").rows.size(), 3);
        }
        {   // pretend it is an old v1 cache (no v2 tables)
            QSqlDatabase d = QSqlDatabase::addDatabase("QSQLITE", "v1fake");
            d.setDatabaseName(db);
            QVERIFY(d.open());
            QSqlQuery q(d);
            QVERIFY(q.exec("DROP TABLE links"));
            QVERIFY(q.exec("PRAGMA user_version=1"));
            d.close();
        }
        QSqlDatabase::removeDatabase("v1fake");
        auto idx = newIndex();
        QSignalSpy sp(idx.get(), &LibraryIndex::synced);
        idx->sync();
        QVERIFY(sp.wait(30000));
        QCOMPARE(sp.last().at(1).toInt(), 7);                         // everything re-added
        QCOMPARE(idx->backlinks("b.md").rows.size(), 3);
        QCOMPARE(idx->searchNow({.text = "done"}).rows.size(), 1);
    }

    // ------------------------- rename rewrite -------------------------
    void rewriteForRename() {
        writeFile(abs("a.md"), "# A\n\n[[b]] and [[b|alias]] and [[B#Head]] and ![[b]] and [[ b | spaced ]] and [[b.md]] and [[ali]]\n"
                               "`[[b]]` ```[[b]]``` \\[[b]] [text [[b]]](u)\n\n```\n[[b]]\n```\nunicode ü [[b#x|日本]] done\r\n");
        writeFile(abs("b.md"), "# B\n\nself [[b]]\n");
        writeFile(abs("d.md"), "---\naliases: [ali]\nrelated: \"[[b]]\"\n---\n# D\n");
        writeFile(abs("f/p.md"), "path style [[b]] [[../b]] [[./../b]] [[f/p]]\n");
        writeFile(abs("other.md"), "no links to b here [[a]]\n");
        auto idx = newIndex();
        QVERIFY(syncAndWait(*idx));
        auto edits = idx->rewriteLinksForRename("b.md", "z/bee.md");
        QHash<QString, FileEdit> by;
        for (auto &e : edits) by[e.path] = e;
        QCOMPARE(by.size(), 3);                                   // a.md, b.md (self), f/p.md
        QVERIFY(!by.contains("d.md") && !by.contains("other.md"));
        QCOMPARE(by["a.md"].changedCount, 7);
        QCOMPARE(by["a.md"].newBytes,
                 QByteArray("# A\n\n[[bee]] and [[bee|alias]] and [[bee#Head]] and ![[bee]] and [[ bee | spaced ]] and [[bee.md]] and [[ali]]\n"
                            "`[[b]]` ```[[b]]``` \\[[b]] [text [[b]]](u)\n\n```\n[[b]]\n```\nunicode ü [[bee#x|日本]] done\r\n"));
        QCOMPARE(by["b.md"].newBytes, QByteArray("# B\n\nself [[bee]]\n"));
        QCOMPARE(by["f/p.md"].changedCount, 3);                    // basename + two relative spellings
        QCOMPARE(by["f/p.md"].newBytes, QByteArray("path style [[bee]] [[../z/bee]] [[../z/bee]] [[f/p]]\n"));
        // the index and the files are untouched
        QCOMPARE(readFile(abs("b.md")), QByteArray("# B\n\nself [[b]]\n"));
        QCOMPARE(idx->backlinks("b.md").rows.size() > 0, true);
        QVERIFY(idx->rewriteLinksForRename("nope.md", "x.md").isEmpty());
        QVERIFY(idx->rewriteLinksForRename("b.md", "b.md").isEmpty());
    }
    void rewriteKeepsStyleAndStaysUnambiguous() {
        writeFile(abs("src.md"), "[[sub/n]] [[sub/n.md]] [[n]] [[N]]\n");
        writeFile(abs("sub/n.md"), "# n\n");
        writeFile(abs("x/taken.md"), "# taken\n");
        auto idx = newIndex();
        QVERIFY(syncAndWait(*idx));
        // path-style stays path-style, extension preserved; basename style becomes the new basename
        auto e = idx->rewriteLinksForRename("sub/n.md", "sub2/m.md");
        QCOMPARE(e.size(), 1);
        QCOMPARE(e[0].newBytes, QByteArray("[[sub2/m]] [[sub2/m.md]] [[m]] [[m]]\n"));
        // new basename collides with another note => basename links become full paths so they stay unambiguous
        e = idx->rewriteLinksForRename("sub/n.md", "q/taken.md");
        QCOMPARE(e[0].newBytes, QByteArray("[[q/taken]] [[q/taken.md]] [[q/taken]] [[q/taken]]\n"));
        // pure move keeping the basename (and unique): basename links need no edit, path links do
        e = idx->rewriteLinksForRename("sub/n.md", "elsewhere/n.md");
        QCOMPARE(e[0].newBytes, QByteArray("[[elsewhere/n]] [[elsewhere/n.md]] [[n]] [[N]]\n"));
        QCOMPARE(e[0].changedCount, 2);
        // ambiguous links are not "resolved to the renamed note": untouched
        writeFile(abs("amb.md"), "[[twin]]\n");
        writeFile(abs("t1/twin.md"), "1");
        writeFile(abs("t2/twin.md"), "2");
        QVERIFY(syncAndWait(*idx));
        QVERIFY(idx->rewriteLinksForRename("t1/twin.md", "t1/new.md").isEmpty());
    }

    // ------------------------- performance: 10,000 notes x ~20 links -------------------------
    void perf10kNotesTwentyLinks() {
        const int N = 10000;
        std::mt19937 rng(4242);
        QElapsedTimer t;
        t.start();
        // note i: folder f(i%100), title "Note i"; ~20 links (80% basename, 10% path style, 5% embeds, 5% unresolved), 2 tasks; every 10th has frontmatter
        QSet<int> hubFans;
        while (hubFans.size() < 500) hubFans.insert(1 + int(rng() % (N - 1)));
        for (int i = 0; i < N; ++i) {
            QByteArray md;
            if (i % 10 == 0) md += QString("---\ntitle: Titled %1\ntags: [t%2, all]\nrating: %3\naliases: [al%1]\n---\n").arg(i).arg(i % 7).arg(i % 10).toUtf8();
            md += QString("# Note %1\n\nIntro paragraph #topic%2 with some words to make the file realistic.\n\n").arg(i).arg(i % 13).toUtf8();
            for (int k = 0; k < 20; ++k) {
                int j = int(rng() % N);
                int kind = int(rng() % 20);
                QString line;
                if (kind < 16) line = QString("Link %1 to [[note%2]] with context words around it.").arg(k).arg(j);
                else if (kind < 18) line = QString("Path link [[f%1/note%2|alias %3]].").arg(j % 100).arg(j).arg(k);
                else if (kind < 19) line = QString("Embed ![[note%1#section]].").arg(j);
                else line = QString("Dangling [[ghost%1]].").arg(rng() % 50);
                md += line.toUtf8() + "\n\n";
            }
            if (hubFans.contains(i)) md += "Hub reference [[note0]] here.\n\n";
            md += QString("- [ ] todo %1\n- [x] finished %1\n").arg(i).toUtf8();
            writeFile(abs(QString("f%1/note%2.md").arg(i % 100).arg(i)), md);
        }
        qInfo().noquote() << QString("MEASURED fixture generation (10000 notes): %1 ms").arg(t.elapsed());

        auto idx = newIndex();
        t.restart();
        QVERIFY(syncAndWait(*idx));
        const qint64 buildMs = t.elapsed();
        qInfo().noquote() << QString("MEASURED initial index build, 10000 notes x ~20 links (links+tasks+meta+FTS): %1 ms").arg(buildMs);
        QCOMPARE(idx->count(), N);
        auto lq = idx->query(QJsonObject{{"from", "links"}, {"select", QJsonArray{"line"}}, {"limit", 1}});
        QVERIFY(lq.ok);

        // correctness spot checks against the generator's own records
        QCOMPARE(idx->linksFrom("f1/note1.md").size() >= 20, true);
        auto hub = idx->backlinks("f0/note0.md", 1000, 0);
        QVERIFY2(hub.rows.size() >= 500, qPrintable(QString::number(hub.rows.size())));

        // incremental per-note update latency
        std::vector<double> inc;
        for (int rep = 0; rep < 30; ++rep) {
            int i = 100 + rep * 37;
            QString rel = QString("f%1/note%2.md").arg(i % 100).arg(i);
            writeFile(abs(rel), QString("# Note %1 edited %2\n\n[[note%3]] [[note%4]] [[brand-new-%2]]\n\n- [ ] edited task\n").arg(i).arg(rep).arg(i + 1).arg(i + 2).toUtf8());
            QElapsedTimer u;
            u.start();
            QVERIFY(updateAndWait(*idx, rel));
            inc.push_back(u.nsecsElapsed() / 1e6);
        }
        std::sort(inc.begin(), inc.end());
        qInfo().noquote() << QString("MEASURED incremental per-note update (edit -> graph consistent): p50=%1 ms p95=%2 ms max=%3 ms").arg(inc[inc.size() / 2], 0, 'f', 2).arg(inc[size_t(inc.size() * 0.95)], 0, 'f', 2).arg(inc.back(), 0, 'f', 2);
        // creating a note that satisfies existing dangling links must repair them
        writeFile(abs("f3/ghost7.md"), "# ghost7\n");
        QVERIFY(updateAndWait(*idx, "f3/ghost7.md"));
        QVERIFY(idx->backlinks("f3/ghost7.md", 1000, 0).rows.size() > 0);
        // incremental sync after touching 5 notes
        for (int i = 0; i < 5; ++i) writeFile(abs(QString("f%1/note%2.md").arg(i + 1).arg(i + 1)), QString("# Touched %1\n[[note%2]]\n").arg(i).arg(i + 50).toUtf8());
        t.restart();
        QVERIFY(syncAndWait(*idx));
        qInfo().noquote() << QString("MEASURED sync after touching 5 of 10000: %1 ms").arg(t.elapsed());

        // backlinks p95 (blocking, includes the worker hop), mixed hub / random / unlinked
        std::vector<double> bl;
        QStringList targets{"f0/note0.md"};
        for (int i = 0; i < 300; ++i) { int j = int(rng() % N); targets << QString("f%1/note%2.md").arg(j % 100).arg(j); }
        idx->backlinks(targets[1]); // warm
        for (int rep = 0; rep < 2; ++rep)
            for (const QString &p : targets) {
                QElapsedTimer q;
                q.start();
                auto page = idx->backlinks(p, 50, 0);
                bl.push_back(q.nsecsElapsed() / 1e6);
                QVERIFY(page.rows.size() <= 50);
            }
        std::sort(bl.begin(), bl.end());
        const double blP50 = bl[bl.size() / 2], blP95 = bl[size_t(bl.size() * 0.95)];
        qInfo().noquote() << QString("MEASURED backlinks first page (%1 calls): p50=%2 ms p95=%3 ms max=%4 ms").arg(bl.size()).arg(blP50, 0, 'f', 2).arg(blP95, 0, 'f', 2).arg(bl.back(), 0, 'f', 2);
        QVERIFY2(blP95 < 50.0, "backlinks p95 target < 50 ms");

        // linksFrom + resolve latency
        std::vector<double> lf;
        for (int i = 0; i < 200; ++i) { QElapsedTimer q; q.start(); idx->linksFrom(QString("f%1/note%2.md").arg(i % 100).arg(i)); idx->resolve(QString("note%1").arg(i * 7), "f1/note1.md"); lf.push_back(q.nsecsElapsed() / 1e6); }
        std::sort(lf.begin(), lf.end());
        qInfo().noquote() << QString("MEASURED linksFrom+resolve pair: p95=%1 ms").arg(lf[size_t(lf.size() * 0.95)], 0, 'f', 2);

        // rename rewrite on a note with 500+ backlinks
        t.restart();
        auto edits = idx->rewriteLinksForRename("f0/note0.md", "renamed/hub-note.md");
        const qint64 rwMs = t.elapsed();
        int changed = 0;
        for (auto &e : edits) changed += e.changedCount;
        qInfo().noquote() << QString("MEASURED rewriteLinksForRename on hub (%1 files, %2 links rewritten): %3 ms").arg(edits.size()).arg(changed).arg(rwMs);
        QVERIFY(edits.size() >= 500);
        QVERIFY(changed >= edits.size());
        QVERIFY(rwMs < 3000);
        for (auto &e : edits) QVERIFY(e.newBytes.contains("hub-note"));

        // (query p95 is measured in core_query_test on the same shape of library; also here for the combined index)
        std::vector<double> qm;
        const QList<QJsonObject> specs{
            QJsonObject{{"from", "notes"}, {"where", QJsonArray{QJsonObject{{"field", "folder"}, {"op", "="}, {"value", "f7"}}}}, {"limit", 50}},
            QJsonObject{{"from", "notes"}, {"where", QJsonArray{QJsonObject{{"field", "tag"}, {"op", "="}, {"value", "t3"}}}}, {"limit", 50}},
            QJsonObject{{"from", "notes"}, {"where", QJsonArray{QJsonObject{{"field", "meta.rating"}, {"op", ">="}, {"value", 8}}}}, {"order", QJsonArray{QJsonObject{{"field", "title"}, {"dir", "asc"}}}}, {"limit", 50}},
            QJsonObject{{"from", "notes"}, {"where", QJsonArray{QJsonObject{{"field", "title"}, {"op", "like"}, {"value", "Note 12%"}}}}, {"limit", 50}},
            QJsonObject{{"from", "notes"}, {"where", QJsonArray{QJsonObject{{"field", "title"}, {"op", "contains"}, {"value", "titled 9"}}}}, {"limit", 50}},
            QJsonObject{{"from", "tasks"}, {"where", QJsonArray{QJsonObject{{"field", "done"}, {"op", "="}, {"value", false}}}}, {"limit", 50}},
            QJsonObject{{"from", "tasks"}, {"where", QJsonArray{QJsonObject{{"field", "text"}, {"op", "contains"}, {"value", "edited"}}}}, {"limit", 50}},
            QJsonObject{{"from", "links"}, {"where", QJsonArray{QJsonObject{{"field", "resolved"}, {"op", "="}, {"value", false}}}}, {"limit", 50}},
            QJsonObject{{"from", "links"}, {"where", QJsonArray{QJsonObject{{"field", "kind"}, {"op", "="}, {"value", "embed"}}}}, {"limit", 50}},
            QJsonObject{{"from", "links"}, {"where", QJsonArray{QJsonObject{{"field", "target"}, {"op", "="}, {"value", "note0"}}}}, {"limit", 50}},
        };
        for (int rep = 0; rep < 5; ++rep)
            for (const auto &s : specs) {
                QElapsedTimer q;
                q.start();
                auto r = idx->query(s);
                qm.push_back(q.nsecsElapsed() / 1e6);
                QVERIFY2(r.ok, qPrintable(r.error));
            }
        std::sort(qm.begin(), qm.end());
        qInfo().noquote() << QString("MEASURED query first page, 10 shapes x5 (%1 calls): p50=%2 ms p95=%3 ms max=%4 ms").arg(qm.size()).arg(qm[qm.size() / 2], 0, 'f', 2).arg(qm[size_t(qm.size() * 0.95)], 0, 'f', 2).arg(qm.back(), 0, 'f', 2);
        QVERIFY2(qm[size_t(qm.size() * 0.95)] < 50.0, "query p95 target < 50 ms");
    }
};
QTEST_MAIN(GraphTest)
#include "core_graph_test.moc"
