#include "hn/core/note_repository.h"
#include <QtTest>
#include <unistd.h>
using namespace hn::core;

static QByteArray readFile(const QString &p) { QFile f(p); return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray("<missing>"); }

class RecoveryTest : public QObject {
    Q_OBJECT
private slots:
    void storeRoundTrip() {
        QTemporaryDir d;
        RecoveryStore s(d.path());
        QByteArray prev("disk");
        QString err;
        QVERIFY(s.writePending("/lib", "a.md", "local1", &prev, 7, &err));
        QVERIFY(s.writePending("/lib", "a.md", "local2", &prev, 8, &err)); // latest wins
        QVERIFY(s.writePending("/lib", "sub/b.md", "b", nullptr, 0, &err));
        QVERIFY(s.writePending("/other", "a.md", "zzz", nullptr, 0, &err));
        QCOMPARE(s.unresolved("/lib").size(), 2);
        QCOMPARE(s.unresolved().size(), 3);
        auto list = s.unresolved("/lib");
        auto a = list[0].relPath == "a.md" ? list[0] : list[1];
        QCOMPARE(a.local, QByteArray("local2"));
        QCOMPARE(a.previousDisk, QByteArray("disk"));
        QVERIFY(a.hadDisk);
        QCOMPARE(a.baseRevision, quint64(8));
        QVERIFY(a.time.isValid());
        QVERIFY(!a.conflict);
        QVERIFY(s.writeExternal("/lib", "a.md", "ext", &err));
        QVERIFY(s.writePending("/lib", "a.md", "local3", &prev, 9, &err)); // keeps conflict flag
        for (auto &e : s.unresolved("/lib")) if (e.relPath == "a.md") { QVERIFY(e.conflict); QCOMPARE(e.external, QByteArray("ext")); QCOMPARE(e.local, QByteArray("local3")); }
        s.resolve("/lib", "a.md");
        QCOMPARE(s.unresolved("/lib").size(), 1);
        QVERIFY(s.resolvedBytes() > 0);
    }
    void incompleteEntryIgnored() {
        QTemporaryDir d;
        RecoveryStore s(d.path());
        QDir().mkpath(d.path() + "/unresolved/deadbeef");
        QFile f(d.path() + "/unresolved/deadbeef/local");
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("x");
        f.close();
        QVERIFY(s.unresolved().isEmpty()); // crash between payload and meta: not offered, no crash
    }
    void resolvedCapEvictsOldestOnly() {
        QTemporaryDir d;
        RecoveryStore s(d.path(), 10 * 1024); // 10 KiB cap for the test
        QString err;
        QByteArray blob(3000, 'x');
        // one unresolved entry must survive any pruning
        QVERIFY(s.writePending("/lib", "keep.md", blob, nullptr, 0, &err));
        for (int i = 0; i < 12; ++i) {
            QVERIFY(s.writePending("/lib", QString("n%1.md").arg(i), blob, nullptr, 0, &err));
            s.resolve("/lib", QString("n%1.md").arg(i));
            QTest::qWait(2);
        }
        QVERIFY2(s.resolvedBytes() <= 10 * 1024, qPrintable(QString::number(s.resolvedBytes())));
        QVERIFY(s.resolvedBytes() >= 6000); // newest entries are kept
        auto left = s.unresolved("/lib");
        QCOMPARE(left.size(), 1);
        QCOMPARE(left[0].relPath, QString("keep.md"));
        // newest survived
        QStringList names = QDir(d.path() + "/resolved").entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        QVERIFY(!names.isEmpty());
    }
    void defaultCapIs64MiB() {
        QTemporaryDir d;
        RecoveryStore s(d.path());
        QString err;
        QByteArray blob(1 << 20, 'y');
        for (int i = 0; i < 70; ++i) {
            QVERIFY(s.writePending("/lib", QString("n%1.md").arg(i), blob, nullptr, 0, &err));
            s.resolve("/lib", QString("n%1.md").arg(i));
            if (i % 10 == 0) QTest::qWait(1);
        }
        QVERIFY(s.resolvedBytes() <= (64LL << 20));
        QVERIFY(s.resolvedBytes() > (60LL << 20));
    }

    void crashRecoveryListing() {
        if (geteuid() == 0) QSKIP("root ignores permissions");
        QTemporaryDir root, state;
        {
            QFile f(root.path() + "/n.md");
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("on disk");
        }
        {
            // "session 1": save fails (read-only library), app then dies with the draft unsaved
            NoteRepository repo(root.path(), state.path());
            repo.read("n.md");
            QFile::setPermissions(root.path(), QFile::ReadOwner | QFile::ExeOwner);
            QSignalSpy failed(&repo, &NoteRepository::saveFailed);
            QElapsedTimer t;
            t.start();
            repo.save("n.md", "unsaved draft");
            RecoveryStore probe(state.path());
            while (probe.unresolved().isEmpty() && t.elapsed() < 5000) QTest::qWait(1);
            qInfo().noquote() << "MEASURED recovery-copy-on-disk latency after save():" << t.elapsed() << "ms";
            QVERIFY(failed.wait(5000));
        }
        QFile::setPermissions(root.path(), QFile::Permissions(0x7755));
        // "session 2"
        NoteRepository repo2(root.path(), state.path());
        auto drafts = repo2.recoverableDrafts();
        QCOMPARE(drafts.size(), 1);
        QCOMPARE(drafts[0].relPath, QString("n.md"));
        QCOMPARE(drafts[0].local, QByteArray("unsaved draft"));
        QCOMPARE(drafts[0].previousDisk, QByteArray("on disk"));
        // drafts from another library are not offered
        QTemporaryDir root2;
        NoteRepository repo3(root2.path(), state.path());
        QVERIFY(repo3.recoverableDrafts().isEmpty());
        // user discards: no longer listed, kept in history
        repo2.resolveRecovery("n.md");
        QVERIFY(repo2.recoverableDrafts().isEmpty());
        QVERIFY(repo2.recovery().resolvedBytes() > 0);
    }
    void draftEqualToDiskNotOffered() {
        QTemporaryDir root, state;
        QFile f(root.path() + "/n.md");
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("same");
        f.close();
        RecoveryStore s(state.path());
        QString err;
        QVERIFY(s.writePending(QDir::cleanPath(root.path()), "n.md", "same", nullptr, 0, &err)); // saved, then crashed before cleanup
        NoteRepository repo(root.path(), state.path());
        QVERIFY(repo.recoverableDrafts().isEmpty());
        QVERIFY(s.unresolved().isEmpty()); // auto-resolved
    }
    void draftForDeletedFileOffered() {
        QTemporaryDir root, state;
        RecoveryStore s(state.path());
        QString err;
        QVERIFY(s.writePending(QDir::cleanPath(root.path()), "gone.md", "work", nullptr, 0, &err));
        NoteRepository repo(root.path(), state.path());
        QCOMPARE(repo.recoverableDrafts().size(), 1);
    }
    void recoveryWriteFailureBlocksSave() {
        if (geteuid() == 0) QSKIP("root ignores permissions");
        QTemporaryDir root, state;
        {
            QFile f(root.path() + "/n.md");
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("v0");
        }
        QString rec = state.path() + "/rec";
        QDir().mkpath(rec);
        QFile::setPermissions(rec, QFile::ReadOwner | QFile::ExeOwner);
        NoteRepository repo(root.path(), rec);
        repo.read("n.md");
        QSignalSpy failed(&repo, &NoteRepository::saveFailed);
        repo.save("n.md", "v1");
        QVERIFY(failed.wait(5000));
        QVERIFY(failed.at(0).at(1).toString().contains("recovery"));
        QCOMPARE(readFile(root.path() + "/n.md"), QByteArray("v0")); // not written without a recovery copy
        QFile::setPermissions(rec, QFile::Permissions(0x7755));
    }
};
QTEST_MAIN(RecoveryTest)
#include "core_recovery_test.moc"
