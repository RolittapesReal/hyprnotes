#include "hn/core/note_repository.h"
#include <QtTest>
#include <unistd.h>
using namespace hn::core;

static void writeFile(const QString &p, const QByteArray &b) {
    QDir().mkpath(QFileInfo(p).absolutePath());
    QFile f(p);
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(b);
}
static QByteArray readFile(const QString &p) { QFile f(p); return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray("<missing>"); }
static void replaceAtomically(const QString &p, const QByteArray &b) {
    QSaveFile f(p);
    f.setDirectWriteFallback(false);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(b);
    QVERIFY(f.commit());
}

class RepositoryTest : public QObject {
    Q_OBJECT
    QTemporaryDir m_root, m_state;
    std::unique_ptr<NoteRepository> repo;
    QString abs(const QString &r) { return m_root.path() + "/" + r; }
private slots:
    void init() {
        QDir(m_root.path()).removeRecursively();
        QDir().mkpath(m_root.path());
        QDir(m_state.path()).removeRecursively();
        repo = std::make_unique<NoteRepository>(m_root.path(), m_state.path());
        repo->setDebounceMs(30);
    }
    void cleanup() {
        QFile::setPermissions(m_root.path(), QFile::Permissions(0x7755));
        repo.reset();
    }

    void createUniqueNames() {
        QCOMPARE(repo->create("", "Idea", "a"), QString("Idea.md"));
        QCOMPARE(repo->create("", "Idea", "b"), QString("Idea 2.md"));
        QCOMPARE(repo->create("", "Idea", "c"), QString("Idea 3.md"));
        QCOMPARE(readFile(abs("Idea.md")), QByteArray("a"));
        QCOMPARE(repo->create("", "", ""), QString("Untitled.md"));
        QCOMPARE(repo->create("", "../evil/x", ""), QString("evil x.md"));
    }
    void createInFolderAndTraversal() {
        QCOMPARE(repo->create("work/2026", "Plan", ""), QString("work/2026/Plan.md"));
        QVERIFY(QFileInfo::exists(abs("work/2026/Plan.md")));
        QString err;
        QVERIFY(repo->create("../outside", "x", "", &err).isEmpty());
        QVERIFY(!err.isEmpty());
        QVERIFY(!QFileInfo::exists(m_root.path() + "/../outside"));
        QVERIFY(repo->absolutePath("../x.md").isEmpty());
        QVERIFY(repo->absolutePath("/etc/passwd").isEmpty());
        QVERIFY(!repo->read("../x.md").ok);
    }
    void listAndRead() {
        writeFile(abs("a.md"), "alpha");
        writeFile(abs("sub/b.MD"), "beta");
        writeFile(abs("sub/deep/c.md"), "gamma");
        writeFile(abs("note.txt"), "skip");
        writeFile(abs(".hidden/d.md"), "skip");
        QDir().mkpath(abs("empty"));
        QStringList rels;
        for (auto &e : repo->list()) rels << e.relPath;
        rels.sort();
        QCOMPARE(rels, (QStringList{"a.md", "sub/b.MD", "sub/deep/c.md"}));
        QStringList f = repo->folders();
        f.sort();
        QCOMPARE(f, (QStringList{"empty", "sub", "sub/deep"}));
        auto s = repo->read("a.md");
        QVERIFY(s.ok);
        QCOMPARE(s.bytes, QByteArray("alpha"));
        QVERIFY(s.revision > 0);
        QVERIFY(!s.fileId.isEmpty());
        QVERIFY(s.mtime.isValid());
        QVERIFY(repo->read("a.md").revision > s.revision);
        QVERIFY(!repo->read("missing.md").ok);
    }
    void saveBasic() {
        writeFile(abs("n.md"), "v0");
        auto s = repo->read("n.md");
        QSignalSpy saved(repo.get(), &NoteRepository::saved);
        quint64 rev = repo->save("n.md", "v1", s.revision);
        QVERIFY(repo->isSaving("n.md"));
        QVERIFY(saved.wait(5000));
        QCOMPARE(saved.at(0).at(0).toString(), QString("n.md"));
        QCOMPARE(saved.at(0).at(1).toULongLong(), rev);
        QCOMPARE(readFile(abs("n.md")), QByteArray("v1"));
        QVERIFY(!repo->isSaving("n.md"));
        // a second save uses the new baseline without a re-read
        repo->save("n.md", "v2", rev);
        QVERIFY(saved.wait(5000));
        QCOMPARE(readFile(abs("n.md")), QByteArray("v2"));
        // resolved recovery only
        QVERIFY(repo->recoverableDrafts().isEmpty());
    }
    void saveIsAtomicReplace() {
        writeFile(abs("n.md"), "v0");
        auto s = repo->read("n.md");
        QSignalSpy saved(repo.get(), &NoteRepository::saved);
        repo->save("n.md", "v1", s.revision);
        QVERIFY(saved.wait(5000));
        QVERIFY(fileIdOf(abs("n.md")) != s.fileId); // new inode: written via temp + rename
        QCOMPARE(QDir(m_root.path()).entryList(QDir::Files | QDir::Hidden).size(), 1); // no temp leftovers
    }
    void coalescesPending() {
        writeFile(abs("n.md"), "v0");
        repo->read("n.md");
        QSignalSpy saved(repo.get(), &NoteRepository::saved);
        quint64 r1 = repo->save("n.md", "v1");
        repo->save("n.md", "v2");
        quint64 r3 = repo->save("n.md", "v3");
        QVERIFY(repo->hasPending("n.md"));
        QTRY_VERIFY_WITH_TIMEOUT(saved.size() >= 2, 5000);
        QCOMPARE(saved.size(), 2); // r1 in flight, r2 replaced by r3
        QCOMPARE(saved.at(0).at(1).toULongLong(), r1);
        QCOMPARE(saved.at(1).at(1).toULongLong(), r3);
        QCOMPARE(readFile(abs("n.md")), QByteArray("v3"));
        QVERIFY(!repo->isSaving("n.md"));
    }
    void saveFailureReadOnlyDir() {
        if (geteuid() == 0) QSKIP("root ignores permissions");
        writeFile(abs("n.md"), "v0");
        repo->read("n.md");
        QVERIFY(QFile::setPermissions(m_root.path(), QFile::ReadOwner | QFile::ExeOwner));
        QSignalSpy failed(repo.get(), &NoteRepository::saveFailed);
        QSignalSpy saved(repo.get(), &NoteRepository::saved);
        quint64 rev = repo->save("n.md", "local edits");
        QVERIFY(failed.wait(5000));
        QCOMPARE(failed.at(0).at(0).toString(), QString("n.md"));
        QVERIFY(!failed.at(0).at(1).toString().isEmpty());
        QCOMPARE(failed.at(0).at(2).toULongLong(), rev);
        QCOMPARE(saved.size(), 0);
        // the file is writable itself; a direct-write fallback would have changed it
        QCOMPARE(readFile(abs("n.md")), QByteArray("v0"));
        // recovery copy survives, unresolved
        auto drafts = repo->recoverableDrafts();
        QCOMPARE(drafts.size(), 1);
        QCOMPARE(drafts[0].local, QByteArray("local edits"));
        QCOMPARE(drafts[0].previousDisk, QByteArray("v0"));
        // fix the problem and retry
        QVERIFY(QFile::setPermissions(m_root.path(), QFile::Permissions(0x7755)));
        repo->retry("n.md");
        QVERIFY(saved.wait(5000));
        QCOMPARE(readFile(abs("n.md")), QByteArray("local edits"));
        QVERIFY(repo->recoverableDrafts().isEmpty());
    }
    void failedSaveThenNewerPendingRuns() {
        if (geteuid() == 0) QSKIP("root ignores permissions");
        writeFile(abs("n.md"), "v0");
        repo->read("n.md");
        QFile::setPermissions(m_root.path(), QFile::ReadOwner | QFile::ExeOwner);
        QSignalSpy failed(repo.get(), &NoteRepository::saveFailed);
        repo->save("n.md", "v1");
        repo->save("n.md", "v2");
        QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 2, 5000);
        QCOMPARE(repo->recoverableDrafts().first().local, QByteArray("v2"));
    }
    void conflictPreservesExternal() {
        writeFile(abs("n.md"), "base");
        auto s = repo->read("n.md");
        writeFile(abs("n.md"), "external edit");
        QSignalSpy conflict(repo.get(), &NoteRepository::conflict);
        QSignalSpy saved(repo.get(), &NoteRepository::saved);
        quint64 rev = repo->save("n.md", "my edit", s.revision);
        QVERIFY(conflict.wait(5000));
        QCOMPARE(conflict.at(0).at(0).toString(), QString("n.md"));
        QCOMPARE(conflict.at(0).at(1).toByteArray(), QByteArray("external edit"));
        QCOMPARE(conflict.at(0).at(2).toByteArray(), QByteArray("my edit"));
        QCOMPARE(conflict.at(0).at(3).toULongLong(), rev);
        QCOMPARE(saved.size(), 0);
        QCOMPARE(readFile(abs("n.md")), QByteArray("external edit")); // never overwritten
        auto drafts = repo->recoverableDrafts();
        QCOMPARE(drafts.size(), 1);
        QVERIFY(drafts[0].conflict);
        QCOMPARE(drafts[0].external, QByteArray("external edit"));
        QCOMPARE(drafts[0].local, QByteArray("my edit"));
        // explicit replace-disk
        repo->save("n.md", "my edit", s.revision, true);
        QVERIFY(saved.wait(5000));
        QCOMPARE(readFile(abs("n.md")), QByteArray("my edit"));
        QVERIFY(repo->recoverableDrafts().isEmpty());
    }
    void conflictAfterReadResolved() {
        writeFile(abs("n.md"), "base");
        repo->read("n.md");
        writeFile(abs("n.md"), "external");
        auto s2 = repo->read("n.md"); // user reloads external version: new baseline
        QSignalSpy saved(repo.get(), &NoteRepository::saved);
        repo->save("n.md", "merged", s2.revision);
        QVERIFY(saved.wait(5000));
        QCOMPARE(readFile(abs("n.md")), QByteArray("merged"));
    }
    void conflictWhenNeverRead() {
        writeFile(abs("n.md"), "exists");
        QSignalSpy conflict(repo.get(), &NoteRepository::conflict);
        repo->save("n.md", "blind");
        QVERIFY(conflict.wait(5000));
        QCOMPARE(readFile(abs("n.md")), QByteArray("exists"));
    }
    void deletedNoteNotRecreated() {
        writeFile(abs("n.md"), "base");
        repo->read("n.md");
        QFile::remove(abs("n.md"));
        QSignalSpy removed(repo.get(), &NoteRepository::fileRemoved);
        QSignalSpy failed(repo.get(), &NoteRepository::saveFailed);
        QSignalSpy saved(repo.get(), &NoteRepository::saved);
        repo->save("n.md", "dirty");
        QVERIFY(failed.wait(5000));
        QCOMPARE(removed.size(), 1);
        QVERIFY(!QFileInfo::exists(abs("n.md")));
        QCOMPARE(repo->recoverableDrafts().first().local, QByteArray("dirty"));
        repo->save("n.md", "dirty", 0, true); // explicit re-create
        QVERIFY(saved.wait(5000));
        QCOMPARE(readFile(abs("n.md")), QByteArray("dirty"));
    }

    void externalChangeDetected() {
        writeFile(abs("n.md"), "base");
        repo->read("n.md");
        repo->watch("n.md");
        QSignalSpy ext(repo.get(), &NoteRepository::externalChange);
        for (int i = 0; i < 5; ++i) writeFile(abs("n.md"), QByteArray("edit ") + QByteArray::number(i)); // burst
        QVERIFY(ext.wait(3000));
        QTest::qWait(200);
        QCOMPARE(ext.size(), 1); // coalesced
        QCOMPARE(ext.at(0).at(0).toString(), QString("n.md"));
    }
    void ownSaveIsNotExternal() {
        writeFile(abs("n.md"), "base");
        repo->read("n.md");
        repo->watch("n.md");
        QSignalSpy ext(repo.get(), &NoteRepository::externalChange);
        QSignalSpy saved(repo.get(), &NoteRepository::saved);
        repo->save("n.md", "mine");
        QVERIFY(saved.wait(5000));
        QTest::qWait(300);
        QCOMPARE(ext.size(), 0);
        QVERIFY(repo->isArmed("n.md"));
    }
    void atomicReplaceRearmsWatcher() {
        writeFile(abs("n.md"), "base");
        repo->read("n.md");
        repo->watch("n.md");
        QVERIFY(repo->isArmed("n.md"));
        QSignalSpy ext(repo.get(), &NoteRepository::externalChange);
        replaceAtomically(abs("n.md"), "one");
        QVERIFY(ext.wait(3000));
        QTRY_VERIFY_WITH_TIMEOUT(repo->isArmed("n.md"), 2000); // re-armed on the new inode
        replaceAtomically(abs("n.md"), "two");
        QTRY_COMPARE_WITH_TIMEOUT(ext.size(), 2, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(repo->isArmed("n.md"), 2000);
        // and a plain in-place write after two replacements is still seen
        writeFile(abs("n.md"), "three");
        QTRY_COMPARE_WITH_TIMEOUT(ext.size(), 3, 3000);
    }
    void watchedFileRemoved() {
        writeFile(abs("n.md"), "base");
        repo->read("n.md");
        repo->watch("n.md");
        QSignalSpy gone(repo.get(), &NoteRepository::fileRemoved);
        QFile::remove(abs("n.md"));
        QVERIFY(gone.wait(3000));
        QTest::qWait(150);
        QCOMPARE(gone.size(), 1);
        // reappears (e.g. sync tool): re-armed and compared again
        QSignalSpy ext(repo.get(), &NoteRepository::externalChange);
        writeFile(abs("n.md"), "back");
        QVERIFY(ext.wait(3000));
        QVERIFY(repo->isArmed("n.md"));
    }
    void libraryWatch() {
        QDir().mkpath(abs("sub"));
        repo->watchLibrary();
        QSignalSpy lib(repo.get(), &NoteRepository::libraryChanged);
        writeFile(abs("new.md"), "x");
        QVERIFY(lib.wait(3000));
        QCOMPARE(lib.at(0).at(0).toString(), QString());
        lib.clear();
        writeFile(abs("sub/inner.md"), "y");
        QVERIFY(lib.wait(3000));
        QCOMPARE(lib.at(0).at(0).toString(), QString("sub"));
        // newly created subfolders get watched too
        lib.clear();
        QDir().mkpath(abs("fresh"));
        QVERIFY(lib.wait(3000));
        QTest::qWait(100);
        lib.clear();
        writeFile(abs("fresh/z.md"), "z");
        QVERIFY(lib.wait(3000));
        QCOMPARE(lib.at(0).at(0).toString(), QString("fresh"));
    }
    void renameAndRemove() {
        QString a = repo->create("", "A", "text");
        repo->read(a);
        repo->watch(a);
        QSignalSpy ren(repo.get(), &NoteRepository::renamed);
        QString err;
        QVERIFY(repo->rename(a, "moved/B.md", &err));
        QCOMPARE(ren.size(), 1);
        QVERIFY(!QFileInfo::exists(abs(a)));
        QCOMPARE(readFile(abs("moved/B.md")), QByteArray("text"));
        QVERIFY(repo->isArmed("moved/B.md"));
        // baseline moved with the note: saving works without conflict
        QSignalSpy saved(repo.get(), &NoteRepository::saved);
        repo->save("moved/B.md", "new");
        QVERIFY(saved.wait(5000));
        QString c = repo->create("", "C", "");
        QVERIFY(!repo->rename(c, "moved/B.md", &err)); // never clobbers
        QVERIFY(QFileInfo::exists(abs(c)));
        QVERIFY(!repo->rename(c, "../x.md", &err));
        QVERIFY(repo->remove("moved/B.md"));
        QVERIFY(!QFileInfo::exists(abs("moved/B.md")));
        QVERIFY(!repo->remove("moved/B.md", &err));
        // no silent recreation on later events
        repo->reconcileNow();
        QTest::qWait(100);
        QVERIFY(!QFileInfo::exists(abs("moved/B.md")));
    }
    void reconcileNowFindsChangesWithoutEvents() {
        writeFile(abs("n.md"), "base");
        repo->read("n.md");
        repo->watch("n.md");
        QSignalSpy ext(repo.get(), &NoteRepository::externalChange);
        writeFile(abs("n.md"), "changed");
        repo->reconcileNow();
        QCOMPARE(ext.size(), 1);
        repo->reconcileNow();
        QCOMPARE(ext.size(), 1); // same content is not re-announced
    }

    // D1: an external atomic replace landing between the conflict check and the rename must not be lost.
    void externalWriteBetweenCheckAndRenameIsKept() {
        QString rel = repo->create("", "race", "base\n");
        repo->read(rel);
        QSignalSpy conflict(repo.get(), &NoteRepository::conflict), saved(repo.get(), &NoteRepository::saved);
        const QString target = abs(rel); // the hook also fires for recovery-file writes
        beforeRenameHook() = [&](const QString &p) { if (p == target) replaceAtomically(p, "EXT\n"); };
        repo->save(rel, "LOCAL\n");
        QTRY_COMPARE(conflict.size(), 1);
        beforeRenameHook() = nullptr;
        QCOMPARE(saved.size(), 0);
        QCOMPARE(conflict[0][1].toByteArray(), QByteArray("EXT\n"));
        QCOMPARE(readFile(abs(rel)), QByteArray("EXT\n")); // disk untouched: external version stays
        bool localKept = false;
        for (const auto &e : repo->recovery().unresolved()) localKept |= e.local == "LOCAL\n" && e.external == "EXT\n";
        QVERIFY(localKept);
    }
    // D2: temp files of dead writers are swept on open; live ones are not.
    void staleTempsSweptOnOpen() {
        writeFile(abs("a.md"), "x");
        writeFile(abs(".a.md.hn-tmp.2147483646.0"), "orphan");
        writeFile(abs(QString(".a.md.hn-tmp.%1.0").arg(getpid())), "live");
        NoteRepository again(m_root.path(), m_state.path());
        QVERIFY(!QFileInfo::exists(abs(".a.md.hn-tmp.2147483646.0")));
        QVERIFY(QFileInfo::exists(abs(QString(".a.md.hn-tmp.%1.0").arg(getpid()))));
        QVERIFY(QFileInfo::exists(abs("a.md")));
    }
    // D3: pruning tracks the running total instead of re-walking history.
    void pruneEvictsOldestPastCap() {
        RecoveryStore rs(m_state.path() + "/cap", 2500);
        for (int i = 0; i < 5; ++i) { QString e; QVERIFY(rs.writePending("r", "n" + QString::number(i), QByteArray(1000, 'a'), nullptr, 0, &e)); rs.resolve("r", "n" + QString::number(i)); QTest::qWait(2); }
        QVERIFY(rs.resolvedBytes() <= 2500 + 400);
        QVERIFY(QDir(m_state.path() + "/cap/resolved").entryList(QDir::Dirs | QDir::NoDotAndDotDot).size() < 5);
    }
    // D4: a continuous writer is reported within the max wait, not only when it pauses.
    void continuousWriterReportedWithinMaxWait() {
        QString rel = repo->create("", "busy", "0");
        repo->read(rel);
        repo->watch(rel);
        repo->setDebounceMs(200);
        QSignalSpy ext(repo.get(), &NoteRepository::externalChange);
        QElapsedTimer t; t.start();
        int i = 0;
        while (t.elapsed() < 1500 && ext.isEmpty()) { replaceAtomically(abs(rel), QByteArray::number(++i)); QTest::qWait(50); }
        QVERIFY2(!ext.isEmpty() && t.elapsed() < 1200, qPrintable(QString::number(t.elapsed())));
    }
};
QTEST_MAIN(RepositoryTest)
#include "core_repository_test.moc"
