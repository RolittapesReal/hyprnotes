#include "hn/core/library_index.h"
#include "hn/core/worker.h"
#include <QtTest>
#include <algorithm>
#include <atomic>
#include <random>
using namespace hn::core;

static void writeFile(const QString &p, const QByteArray &b) {
    QDir().mkpath(QFileInfo(p).absolutePath());
    QFile f(p);
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(b);
}

class IndexTest : public QObject {
    Q_OBJECT
    QTemporaryDir m_root, m_cache;
    QString abs(const QString &r) { return m_root.path() + "/" + r; }
    std::unique_ptr<LibraryIndex> newIndex() { return std::make_unique<LibraryIndex>(m_root.path(), m_cache.path()); }
    static bool syncAndWait(LibraryIndex &idx, int *added = nullptr, int *updated = nullptr, int *removed = nullptr, int timeoutMs = 60000) {
        QSignalSpy sp(&idx, &LibraryIndex::synced);
        quint64 gen = idx.sync();
        if (!sp.wait(timeoutMs)) return false;
        if (sp.at(0).at(0).toULongLong() != gen) return false;
        if (added) *added = sp.at(0).at(1).toInt();
        if (updated) *updated = sp.at(0).at(2).toInt();
        if (removed) *removed = sp.at(0).at(3).toInt();
        return true;
    }
private slots:
    void init() {
        QDir(m_root.path()).removeRecursively();
        QDir(m_cache.path()).removeRecursively();
        QDir().mkpath(m_root.path());
    }

    void workerPriorityAndCoalescing() {
        Worker w;
        std::mutex gate;
        gate.lock();
        QStringList order;
        std::mutex om;
        auto rec = [&](QString s) { return [&, s] { std::lock_guard l(om); order << s; }; };
        w.post(Worker::Index, [&] { std::lock_guard l(gate); std::lock_guard l2(om); order << "first"; });
        QTest::qWait(50); // "first" is now running and blocked
        w.post(Worker::Index, rec("index1"));
        w.post(Worker::Index, rec("coalesced-old"), "k");
        w.post(Worker::Index, rec("coalesced-new"), "k");
        w.post(Worker::Save, rec("save1"));
        w.post(Worker::Save, rec("save2"));
        w.post(Worker::Index, rec("index2"));
        gate.unlock();
        w.waitIdle();
        QCOMPARE(order, (QStringList{"first", "save1", "save2", "index1", "coalesced-new", "index2"}));
        QCOMPARE(w.callBlocking(Worker::Save, [] { return 42; }), 42);
    }

    void basicsTitlesFoldersTags() {
        writeFile(abs("a.md"), "# Alpha Title\n\nsome text #work and `#code` #Ideas\n");
        writeFile(abs("sub/b.md"), "no heading just words #work\n");
        writeFile(abs("sub/deep/c.md"), "## Deep *heading*\n\n```\n#fenced\n```\n[#link](u)\n");
        QDir().mkpath(abs("empty"));
        QDir().mkpath(abs(".hidden"));
        writeFile(abs(".hidden/x.md"), "hidden");
        writeFile(abs("skip.txt"), "no");
        auto idx = newIndex();
        int added = 0, updated = 0, removed = 0;
        QVERIFY(syncAndWait(*idx, &added, &updated, &removed));
        QCOMPARE(added, 3); QCOMPARE(updated, 0); QCOMPARE(removed, 0);
        QCOMPARE(idx->count(), 3);
        QCOMPARE(idx->folders(), (QStringList{"empty", "sub", "sub/deep"}));
        auto all = idx->searchNow({});
        QCOMPARE(all.rows.size(), 3);
        QHash<QString, IndexRow> by;
        for (auto &r : all.rows) by[r.relPath] = r;
        QCOMPARE(by["a.md"].title, QString("Alpha Title"));
        QCOMPARE(by["sub/b.md"].title, QString("b"));
        QCOMPARE(by["sub/deep/c.md"].title, QString("Deep heading"));
        QCOMPARE(by["a.md"].tags, (QStringList{"work", "ideas"}));
        QCOMPARE(by["sub/deep/c.md"].tags, QStringList{});
        QCOMPARE(by["sub/b.md"].folder, QString("sub"));
        QCOMPARE(by["a.md"].folder, QString(""));
        auto tags = idx->tags();
        QCOMPARE(tags, (QList<QPair<QString, int>>{{"ideas", 1}, {"work", 2}}));
        QCOMPARE(idx->searchNow({.tag = "WORK"}).rows.size(), 2);
        QCOMPARE(idx->searchNow({.folder = "sub"}).rows.size(), 2);
        QCOMPARE(idx->searchNow({.folder = "sub/deep"}).rows.size(), 1);
        QCOMPARE(idx->searchNow({.text = "words", .folder = "sub", .tag = "work"}).rows.size(), 1);
        QCOMPARE(idx->searchNow({.text = "fenced"}).rows.size(), 1); // body text of code is searchable
    }
    // D14: punctuation-bearing literals still find the note that has them, ahead of many prefix matches.
    void literalQueriesFindTheirNote() {
        writeFile(abs("probe.md"), "# Probe\n\nwe use C++ and a:b and \"quoted\" and AND here\n");
        for (int i = 0; i < 40; ++i) writeFile(abs(QString("n%1.md").arg(i)), "# N\n\ncats and dogs and c code and a b quoted and\n");
        auto idx = newIndex();
        QVERIFY(syncAndWait(*idx));
        for (const char *q : {"C++", "a:b", "\"quoted\"", "AND"}) {
            auto rows = idx->searchNow({.text = q, .limit = 5}).rows;
            QVERIFY2(!rows.isEmpty() && rows[0].relPath == "probe.md", q);
        }
    }
    void searchSemantics() {
        writeFile(abs("a.md"), "# Café society\n\nthe quick brown fox\n");
        writeFile(abs("b.md"), "# Other\n\nquick thinking about foxes\n");
        writeFile(abs("c.md"), QString::fromUtf8("# 日本語\n\nhello wörld\n").toUtf8());
        auto idx = newIndex();
        QVERIFY(syncAndWait(*idx));
        QCOMPARE(idx->searchNow({.text = "quick"}).rows.size(), 2);
        QCOMPARE(idx->searchNow({.text = "quick fox"}).rows.size(), 2); // last term is a prefix
        QCOMPARE(idx->searchNow({.text = "brown fox"}).rows.size(), 1);
        QCOMPARE(idx->searchNow({.text = "cafe"}).rows.size(), 1);   // diacritics folded
        QCOMPARE(idx->searchNow({.text = "WORLD"}).rows.size(), 1);
        QCOMPARE(idx->searchNow({.text = QString::fromUtf8("日本語")}).rows.size(), 1);
        QCOMPARE(idx->searchNow({.text = "nomatchterm"}).rows.size(), 0);
        QCOMPARE(idx->searchNow({.text = "\"*() OR -"}).rows.size(), 0); // hostile syntax is inert
        QCOMPARE(idx->searchNow({.text = "qui\" OR \"x"}).rows.size(), 0);
        auto p = idx->searchNow({.text = "brown"});
        QVERIFY(p.rows[0].snippet.contains("[brown]"));
    }
    void incrementalUpdates() {
        for (int i = 0; i < 20; ++i) writeFile(abs(QString("n%1.md").arg(i)), QString("# N%1\n\nbody %1 #t%1\n").arg(i).toUtf8());
        auto idx = newIndex();
        int a, u, r;
        QVERIFY(syncAndWait(*idx, &a, &u, &r));
        QCOMPARE(a, 20);
        QVERIFY(syncAndWait(*idx, &a, &u, &r));
        QCOMPARE(a + u + r, 0); // nothing changed => nothing touched
        writeFile(abs("n3.md"), "# Changed three\n\nnew body longer than before\n");
        writeFile(abs("n7.md"), "# Changed seven\n\nsomething else entirely\n");
        QFile::remove(abs("n9.md"));
        writeFile(abs("fresh.md"), "# Fresh\n");
        QVERIFY(syncAndWait(*idx, &a, &u, &r));
        QCOMPARE(a, 1); QCOMPARE(u, 2); QCOMPARE(r, 1);
        QCOMPARE(idx->count(), 20);
        QCOMPARE(idx->searchNow({.text = "entirely"}).rows.size(), 1);
        QCOMPARE(idx->searchNow({.text = "body 3"}).rows.size(), 0);
        QCOMPARE(idx->searchNow({.text = "N9"}).rows.size(), 0);
        QCOMPARE(idx->searchNow({.tag = "t3"}).rows.size(), 0); // old tags dropped
        QCOMPARE(idx->searchNow({.text = "Changed"}).rows.size(), 2);
    }
    void pathUpdatesAndRename() {
        writeFile(abs("a.md"), "# Old\n\nalpha\n");
        auto idx = newIndex();
        QVERIFY(syncAndWait(*idx));
        QSignalSpy up(idx.get(), &LibraryIndex::pathUpdated);
        writeFile(abs("a.md"), "# New title here\n\nbeta #x\n");
        idx->updatePath("a.md");
        QVERIFY(up.wait(5000));
        QCOMPARE(idx->searchNow({.text = "beta"}).rows.first().title, QString("New title here"));
        QCOMPARE(idx->searchNow({.text = "alpha"}).rows.size(), 0);
        writeFile(abs("b.md"), "# B\n\nbrand new #y\n");
        idx->updatePath("b.md");
        QVERIFY(up.wait(5000));
        QCOMPARE(idx->count(), 2);
        // rename = app-driven move
        QDir().mkpath(abs("dir"));
        QVERIFY(QFile::rename(abs("a.md"), abs("dir/a2.md")));
        idx->renamePath("a.md", "dir/a2.md");
        QVERIFY(up.wait(5000));
        QCOMPARE(idx->count(), 2);
        auto hit = idx->searchNow({.text = "beta"});
        QCOMPARE(hit.rows.size(), 1);
        QCOMPARE(hit.rows[0].relPath, QString("dir/a2.md"));
        QCOMPARE(hit.rows[0].folder, QString("dir"));
        // delete
        QFile::remove(abs("b.md"));
        idx->removePath("b.md");
        QVERIFY(up.wait(5000));
        QCOMPARE(idx->count(), 1);
        QCOMPARE(idx->searchNow({.tag = "y"}).rows.size(), 0);
    }
    void rebuildAndCorruptCache() {
        writeFile(abs("a.md"), "# A\n\nhello\n");
        QString db;
        {
            auto idx = newIndex();
            db = idx->databasePath();
            QVERIFY(syncAndWait(*idx));
            QCOMPARE(idx->count(), 1);
        }
        QVERIFY(QFileInfo::exists(db));
        {   // warm start reuses the cache: nothing to do
            auto idx = newIndex();
            int a, u, r;
            QVERIFY(syncAndWait(*idx, &a, &u, &r));
            QCOMPARE(a + u + r, 0);
            QCOMPARE(idx->count(), 1);
            QSignalSpy sp(idx.get(), &LibraryIndex::synced);
            idx->rebuild();
            QVERIFY(sp.wait(10000));
            QCOMPARE(sp.last().at(1).toInt(), 1); // re-added everything
            QCOMPARE(idx->count(), 1);
        }
        {   // garbage cache file => silently rebuilt
            writeFile(db, QByteArray("this is not a sqlite database").repeated(200));
            auto idx = newIndex();
            int a;
            QVERIFY(syncAndWait(*idx, &a));
            QCOMPARE(a, 1);
            QCOMPARE(idx->searchNow({.text = "hello"}).rows.size(), 1);
        }
        {   // cache deleted entirely
            QFile::remove(db);
            auto idx = newIndex();
            QVERIFY(syncAndWait(*idx));
            QCOMPARE(idx->count(), 1);
        }
        // note text untouched by cache loss
        QCOMPARE(QFile(abs("a.md")).size(), qint64(QByteArray("# A\n\nhello\n").size()));
    }
    void mergeDirtyDocs() {
        writeFile(abs("a.md"), "# Disk A\n\nonly on disk apple\n");
        writeFile(abs("b.md"), "# Disk B\n\nbanana on disk\n");
        auto idx = newIndex();
        QVERIFY(syncAndWait(*idx));
        auto disk = idx->searchNow({.text = "apple"}).rows;
        QCOMPARE(disk.size(), 1);
        // unsaved edit removed "apple" from a.md, added it to b.md, and a new unsaved note has it
        QList<DirtyDoc> dirty{{"a.md", "# Disk A\n\nedited, no fruit\n"},
                              {"b.md", "# Disk B\n\nnow with Apple pie #snack\n"},
                              {"new.md", "# Brand new\n\napples everywhere\n"},
                              {"c.md", "unrelated\n"}};
        auto merged = LibraryIndex::mergeDirty(disk, dirty, "apple");
        QStringList paths;
        for (auto &r : merged) paths << r.relPath;
        paths.sort();
        QCOMPARE(paths, (QStringList{"b.md", "new.md"}));
        for (auto &r : merged) QVERIFY(r.dirty);
        for (auto &r : merged) if (r.relPath == "b.md") { QCOMPARE(r.tags, QStringList{"snack"}); QVERIFY(r.snippet.contains("Apple")); }
        for (auto &r : merged) if (r.relPath == "new.md") QCOMPARE(r.title, QString("Brand new"));
        // empty query: only titles of existing rows refresh
        auto all = idx->searchNow({}).rows;
        auto m2 = LibraryIndex::mergeDirty(all, {{"a.md", "# Retitled\n"}, {"zzz.md", "x"}}, "");
        QCOMPARE(m2.size(), 2);
        for (auto &r : m2) if (r.relPath == "a.md") { QCOMPARE(r.title, QString("Retitled")); QVERIFY(r.dirty); }
    }
    void pagingAndRowCache() {
        for (int i = 0; i < 1000; ++i) writeFile(abs(QString("n%1.md").arg(i, 4, 10, QChar('0'))), QString("# Note %1\n\ncommon word\n").arg(i).toUtf8());
        auto idx = newIndex();
        QVERIFY(syncAndWait(*idx));
        QCOMPARE(idx->count(), 1000);
        auto p0 = idx->searchNow({.text = "common"});
        QCOMPARE(p0.rows.size(), 100);
        QVERIFY(p0.hasMore);
        QSet<QString> seen;
        QSignalSpy sp(idx.get(), &LibraryIndex::searchFinished);
        for (int off = 0; off < 1000; off += 100) {
            idx->search({.text = "common", .limit = 100, .offset = off});
            QVERIFY(sp.wait(5000));
            auto page = sp.last().at(0).value<SearchPage>();
            QCOMPARE(page.rows.size(), 100);
            QCOMPARE(page.hasMore, off < 900);
            for (auto &r : page.rows) seen.insert(r.relPath);
            QVERIFY(idx->cachedRows() <= 500);
        }
        QCOMPARE(seen.size(), 1000);       // pages are disjoint and complete
        QCOMPARE(idx->cachedRows(), 500);  // capped at 500
        auto lastPage = sp.last().at(0).value<SearchPage>();
        for (auto &r : lastPage.rows) QVERIFY(idx->cachedRow(r.relPath) != nullptr); // most recent page is cached
        int cached = 0;
        for (auto &r : seen) cached += idx->cachedRow(r) != nullptr;
        QCOMPARE(cached, 500);
    }
    void supersededQueriesCancelled() {
        for (int i = 0; i < 200; ++i) writeFile(abs(QString("n%1.md").arg(i)), QString("# Note %1\n\nalpha beta%2\n").arg(i).arg(i % 3).toUtf8());
        auto idx = newIndex();
        QVERIFY(syncAndWait(*idx));
        QSignalSpy sp(idx.get(), &LibraryIndex::searchFinished);
        quint64 last = 0;
        for (int i = 0; i < 30; ++i) last = idx->search({.text = QString("alpha beta%1").arg(i % 3)});
        QVERIFY(sp.wait(5000));
        QTest::qWait(200);
        QCOMPARE(sp.size(), 1); // only the newest query reports
        QCOMPARE(sp.at(0).at(0).value<SearchPage>().id, last);
    }
    void invalidUtf8AndOddFilesIndexed() {
        writeFile(abs("bad.md"), QByteArray("# Bad \xE9 title\n\ntext caf\xE9 #ok\n"));
        writeFile(abs("empty.md"), "");
        writeFile(abs("dir.md/inner.md"), "# Inside a dir named .md\n");
        auto idx = newIndex();
        int a;
        QVERIFY(syncAndWait(*idx, &a));
        QCOMPARE(a, 3);
        QCOMPARE(idx->searchNow({.tag = "ok"}).rows.size(), 1);
    }

    // ---- performance: 1000 generated 20 KiB notes ----
    void perf1000Notes() {
        std::mt19937 rng(12345);
        const char *syl[] = {"ka", "lo", "mi", "ra", "te", "su", "vo", "ni", "da", "pe", "xo", "zu", "bri", "tan", "gle", "sho"};
        std::vector<QString> vocab;
        for (int i = 0; i < 3000; ++i) {
            QString w;
            int n = 2 + rng() % 3;
            for (int k = 0; k < n; ++k) w += syl[rng() % 16];
            vocab.push_back(w + QString::number(i % 7 == 0 ? i % 50 : 0));
        }
        QHash<QString, QStringList> expectTags;
        QHash<QString, QString> expectTitle;
        for (int i = 0; i < 1000; ++i) {
            QString md;
            QString title = QString("Generated note %1 %2").arg(i).arg(vocab[rng() % vocab.size()]);
            md += "# " + title + "\n\n";
            QStringList tags{QString("topic%1").arg(i % 13), "all"};
            md += "#" + tags[0] + " #all\n\n";
            int para = 0;
            while (md.toUtf8().size() < 20 * 1024) {
                if (++para % 9 == 0) md += "## Section " + vocab[rng() % vocab.size()] + "\n\n";
                if (para % 11 == 0) md += "```\n#notatag " + vocab[rng() % vocab.size()] + "\n```\n\n";
                for (int w = 0; w < 60; ++w) md += (w % 7 == 0 ? QString("the") : vocab[rng() % vocab.size()]) + (w == 59 ? ".\n\n" : " ");
            }
            md += QString("unique%1marker\n").arg(i);
            QString rel = QString("f%1/note%2.md").arg(i % 10).arg(i);
            writeFile(abs(rel), md.toUtf8());
            expectTags[rel] = tags;
            expectTitle[rel] = title;
        }
        auto idx = newIndex();
        QElapsedTimer t;
        t.start();
        int a = 0;
        QVERIFY(syncAndWait(*idx, &a));
        qint64 indexMs = t.elapsed();
        qInfo().noquote() << QString("MEASURED initial index of 1000 x 20KiB notes: %1 ms").arg(indexMs);
        QCOMPARE(a, 1000);
        QCOMPARE(idx->count(), 1000);
        QCOMPARE(idx->folders().size(), 10);
        // correctness: titles/tags for every note
        auto listing = [&] {
            QList<IndexRow> all;
            for (int off = 0; off < 1000; off += 500) all += idx->searchNow({.limit = 500, .offset = off}).rows;
            return all;
        }();
        QCOMPARE(listing.size(), 1000);
        for (auto &r : listing) {
            QCOMPARE(r.title, expectTitle[r.relPath]);
            QCOMPARE(r.tags, expectTags[r.relPath]);
        }
        QCOMPARE(idx->tags().size(), 14); // 13 topics + all; '#notatag' in fences never indexed
        QCOMPARE(idx->searchNow({.tag = "topic3", .limit = 500}).rows.size(), 77);
        for (int i : {0, 17, 500, 999}) {
            auto r = idx->searchNow({.text = QString("unique%1marker").arg(i)});
            QCOMPARE(r.rows.size(), 1);
            QVERIFY(r.rows[0].relPath.endsWith(QString("note%1.md").arg(i)));
        }
        // first-page search latency (includes worker hop), mixed rare/common/prefix queries
        QStringList queries{"the", "the ka", "kalo", "unique500marker", "generated note", "tan shu", "brigle", "section", "mi", "zuxo1", "topic3", "notatag"};
        for (int i = 0; i < 40; ++i) queries << vocab[(i * 71) % vocab.size()];
        QVERIFY(idx->searchNow({.text = "warmup"}).rows.size() >= 0);
        std::vector<double> ms;
        for (int rep = 0; rep < 5; ++rep)
            for (const auto &q : queries) {
                QElapsedTimer qt;
                qt.start();
                auto page = idx->searchNow({.text = q});
                ms.push_back(qt.nsecsElapsed() / 1e6);
                QVERIFY(page.rows.size() <= 100);
            }
        std::sort(ms.begin(), ms.end());
        double p50 = ms[ms.size() / 2], p95 = ms[size_t(ms.size() * 0.95)], mx = ms.back();
        qInfo().noquote() << QString("MEASURED first-page search over %1 queries: p50=%2 ms p95=%3 ms max=%4 ms").arg(ms.size()).arg(p50, 0, 'f', 2).arg(p95, 0, 'f', 2).arg(mx, 0, 'f', 2);
        QVERIFY2(p95 < 100.0, "first-page p95 must be < 100ms");
        // incremental: touch 5 notes
        for (int i = 0; i < 5; ++i) writeFile(abs(QString("f%1/note%2.md").arg(i).arg(i)), QString("# Touched %1\n\nshort #tt\n").arg(i).toUtf8());
        t.restart();
        int u = 0, r = 0;
        QVERIFY(syncAndWait(*idx, &a, &u, &r));
        qInfo().noquote() << QString("MEASURED incremental sync after touching 5 of 1000: %1 ms").arg(t.elapsed());
        QCOMPARE(a, 0); QCOMPARE(u, 5); QCOMPARE(r, 0);
        QCOMPARE(idx->searchNow({.tag = "tt"}).rows.size(), 5);
    }
    void saveNotStarvedByIndexing() {
        // A big sync yields between 32-entry chunks, so a save task posted mid-sync runs promptly.
        for (int i = 0; i < 600; ++i) writeFile(abs(QString("n%1.md").arg(i)), QByteArray("# n\n\n") + QByteArray("words ").repeated(3000));
        auto idx = newIndex();
        QSignalSpy sp(idx.get(), &LibraryIndex::synced);
        idx->sync();
        QTest::qWait(30);
        QElapsedTimer t;
        t.start();
        Worker::shared().callBlocking(Worker::Save, [] { return 1; });
        qint64 ms = t.elapsed();
        qInfo().noquote() << QString("MEASURED save-task wait while a 600-note sync runs: %1 ms").arg(ms);
        QVERIFY(ms < 500);
        QVERIFY(sp.wait(60000));
    }
};
QTEST_MAIN(IndexTest)
#include "core_index_test.moc"
