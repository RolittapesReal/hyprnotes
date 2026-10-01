// P8.2 repository: library scale, watcher burst, concurrent autosave on a flaky directory, conflict storm,
// kill -9 during save, disk-full (RLIMIT_FSIZE) simulation, recovery cap. Standalone; METRIC/PASS/FAIL lines.
#include "hn/core/library_index.h"
#include "hn/core/note_repository.h"
#include "hn/core/recovery_store.h"
#include "stress_util.h"
#include <QCoreApplication>
#include <QDirIterator>
#include <QEventLoop>
#include <QProcess>
#include <QSet>
#include <QTimer>
#include <atomic>
#include <csignal>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <thread>

using namespace st;
using namespace hn::core;

template <class F> static bool waitFor(F cond, int timeoutMs) {
    QElapsedTimer t; t.start();
    while (!cond()) {
        if (t.elapsed() > timeoutMs) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        std::this_thread::sleep_for(std::chrono::microseconds(300));
    }
    return true;
}

// Versioned payload: every 4 KiB block starts with "V<version>:" so partial/mixed files are detectable.
static QByteArray versioned(int v, qsizetype size) {
    QByteArray out; out.reserve(size);
    const QByteArray tag = "V" + QByteArray::number(v) + ":";
    while (out.size() < size) { QByteArray blk = tag; blk += QByteArray(4096 - blk.size(), char('a' + v % 26)); out += blk; }
    out.truncate(size);
    return out;
}
// Returns version id when the whole file is one intact version, -1 when mixed/short/garbled.
static int versionOf(const QByteArray &b, qsizetype size) {
    if (b.size() != size || !b.startsWith('V')) return -1;
    const int colon = b.indexOf(':');
    bool ok = false; const int v = b.left(colon).mid(1).toInt(&ok);
    return ok && b == versioned(v, size) ? v : -1;
}

// ------------------------------------------------------------------ 2.1 library scale
static void libraryScale() {
    note("== 2.1 10k-note library create + list + index (2 KiB bodies; 20 KiB x 10k is in stress_index)");
    Sandbox sb("repo-lib");
    NoteRepository repo(sb.notes, sb.dir.filePath("rec"));
    const Mem m0 = mem();
    Stopwatch sw;
    for (int i = 0; i < 10000; ++i) {
        QString err;
        const QString rel = repo.create(QString("f%1").arg(i % 40), QString("Note %1").arg(i), mixedNote(2048, i), &err);
        if (rel.isEmpty()) { check(false, "create failed: " + err); return; }
    }
    metric("repo.lib10k.create_total", sw.ms(), "ms");
    sw.reset(); const auto list = repo.list(); metric("repo.lib10k.list", sw.ms(), "ms");
    check(list.size() == 10000, QString("list returns all 10000 (%1)").arg(list.size()));
    sw.reset(); const auto folders = repo.folders(); metric("repo.lib10k.folders", sw.ms(), "ms");
    check(folders.size() == 40, "40 folders");
    LibraryIndex idx(sb.notes, sb.cache);
    sw.reset();
    int added = 0; bool done = false;
    QObject::connect(&idx, &LibraryIndex::synced, [&](quint64, int a, int, int) { added = a; done = true; });
    // sample peak PSS during indexing from the GUI thread timer
    double peak = 0; QTimer sampler; QObject::connect(&sampler, &QTimer::timeout, [&] { peak = qMax(peak, mem().total()); }); sampler.start(100);
    idx.sync();
    check(waitFor([&] { return done; }, 300000), "initial index sync completes");
    metric("repo.lib10k.index_initial", sw.ms(), "ms");
    metric("repo.lib10k.index_peak_pss", peak, "MiB");
    check(added == 10000 && idx.count() == 10000, QString("indexed %1 / count %2").arg(added).arg(idx.count()));
    done = false; sw.reset(); idx.sync(); waitFor([&] { return done; }, 60000);
    metric("repo.lib10k.index_noop_resync", sw.ms(), "ms");
    sampler.stop();
    metric("repo.lib10k.pss_after", mem().total(), "MiB");
    metric("repo.lib10k.pss_growth", mem().total() - m0.total(), "MiB");
    note(QString("db size %1 MiB").arg(QFileInfo(idx.databasePath()).size() / 1048576.0));
}

// ------------------------------------------------------------------ 2.2 watcher burst
static void metricT(const char *tag, const char *n, double v, const char *u = "") { metric(qPrintable(QString("repo.watch.%1.%2").arg(tag, n)), v, u); }
static void watcherBurst(int paceUs, const char *tag) {
    note(QString("== 2.2 watcher burst: 2000 external file events (%1)").arg(tag));
    Sandbox sb("repo-watch");
    NoteRepository repo(sb.notes, sb.dir.filePath("rec"));
    const int kFiles = 100;
    QStringList rels;
    for (int i = 0; i < kFiles; ++i) { QString r = repo.create("", QString("w%1").arg(i), "init\n"); rels << r; repo.read(r); repo.watch(r); }
    repo.watchLibrary(true);
    int ext = 0, lib = 0; QSet<QString> extRels; qint64 firstSig = -1; QElapsedTimer sinceStart; sinceStart.start();
    QObject::connect(&repo, &NoteRepository::externalChange, [&](const QString &r) { ++ext; extRels.insert(r); if (firstSig < 0) firstSig = sinceStart.elapsed(); });
    QObject::connect(&repo, &NoteRepository::libraryChanged, [&] { ++lib; });
    // event-loop latency probe
    qint64 maxGap = 0, last = QDateTime::currentMSecsSinceEpoch();
    QTimer probe; QObject::connect(&probe, &QTimer::timeout, [&] { const qint64 n = QDateTime::currentMSecsSinceEpoch(); maxGap = qMax(maxGap, n - last); last = n; });
    probe.start(5);
    const double cpu0 = cpuSeconds();
    Stopwatch sw;
    // 2000 events from a separate thread as fast as possible: in-place rewrites, atomic replaces, creates, deletes.
    QHash<QString, QByteArray> finalContent;
    std::atomic<bool> wdone{false};
    std::thread writer([&] {
        Rng rng(5);
        for (int i = 0; i < 2000; ++i) {
            const QString rel = rels[rng.bounded(kFiles)];
            const QString abs = sb.notes + "/" + rel;
            const QByteArray body = "ext " + QByteArray::number(i) + "\n";
            if (i % 4 == 0) replaceFile(abs, body);   // editor-style replace
            else if (i % 4 == 1) writeFile(abs, body);
            else if (i % 4 == 2) writeFile(sb.notes + "/new-" + QString::number(i) + ".md", "created\n");
            else { QFile f(abs); if (f.open(QIODevice::Append)) f.write("+\n"); }
            finalContent[rel] = readFile(abs);
            if (paceUs) std::this_thread::sleep_for(std::chrono::microseconds(paceUs));
        }
        wdone = true;
    });
    waitFor([&] { return wdone.load(); }, 120000);
    writer.join();
    metricT(tag, "burst_write_ms", sw.ms(), "ms");
    waitFor([&] { return false; }, 1500);   // settle
    probe.stop();
    metricT(tag, "externalChange_signals", ext);
    metricT(tag, "libraryChanged_signals", lib);
    metricT(tag, "first_externalChange_after", double(firstSig), "ms");
    if (paceUs) check(firstSig >= 0 && firstSig < 1000, QString("[D4-DEBOUNCE-STARVE] ") + QString("first externalChange delivered within 1 s of a continuous 3 s write stream (got %1 ms: the 40 ms debounce restarts on every event, so it only fires once the stream pauses)").arg(firstSig));
    metricT(tag, "distinct_notes_reported", extRels.size());
    metricT(tag, "max_eventloop_gap", double(maxGap), "ms");
    metricT(tag, "cpu_during_burst_and_settle", cpuSeconds() - cpu0, "s");
    check(ext < 2000, QString("externalChange coalesced: %1 signals for 2000 file events (< 2000)").arg(ext));
    check(lib < 2000, QString("libraryChanged coalesced: %1 signals for 2000 events").arg(lib));
    check(maxGap < 250, QString("GUI event loop never stalled > 250 ms (max gap %1 ms)").arg(maxGap));
    // every watched file whose content really differs from baseline must have been reported at least once
    int missed = 0;
    for (const QString &r : rels) if (readFile(sb.notes + "/" + r) != "init\n" && !extRels.contains(r)) ++missed;
    check(missed == 0, QString("no externally modified watched note went unreported (missed %1)").arg(missed));
    // idle after burst: no further events
    const int before = ext; waitFor([] { return false; }, 800);
    check(ext == before, "no event storm after burst ends");
}

// ------------------------------------------------------------------ 2.3 200 notes autosave on flaky dir
static void flakyAutosave(bool flaky) {
    note(flaky ? "== 2.3 200 notes autosaving repeatedly while the directory flips read-only" : "== 2.3b same load with a healthy directory (baseline)");
    Sandbox sb("repo-flaky");
    const QString pfx = flaky ? "repo.flaky." : "repo.healthy.";
    NoteRepository repo(sb.notes, sb.dir.filePath("rec"));
    const int N = 200, ROUNDS = 15, SZ = 12 * 1024;
    struct S { QString rel; int round = 0; quint64 rev = 0; bool done = false; QElapsedTimer t; };
    QList<S> notes(N);
    QHash<QString, int> idx;
    for (int i = 0; i < N; ++i) { notes[i].rel = repo.create("", QString("flaky%1").arg(i), versioned(0, SZ)); idx[notes[i].rel] = i; repo.read(notes[i].rel); }
    Rng rng(321);
    Dist lat; int failures = 0, doneCount = 0;
    auto submit = [&](int i) {
        S &s = notes[i];
        ++s.round; s.t.start();
        s.rev = repo.save(s.rel, versioned(i * 1000 + s.round, SZ));
    };
    QObject::connect(&repo, &NoteRepository::saved, [&](const QString &rel, quint64 rev) {
        const int i = idx[rel]; S &s = notes[i];
        if (rev != s.rev || s.done) return;
        lat.add(s.t.nsecsElapsed() / 1e6);
        if (s.round >= ROUNDS) { s.done = true; ++doneCount; return; }
        QTimer::singleShot(rng.bounded(15), [&, i] { submit(i); });
    });
    QObject::connect(&repo, &NoteRepository::saveFailed, [&](const QString &rel, const QString &, quint64) {
        ++failures;
        const int i = idx[rel];
        QTimer::singleShot(150 + rng.bounded(150), [&, rel] { repo.retry(rel); (void)i; });
    });
    int conflicts = 0;
    QObject::connect(&repo, &NoteRepository::conflict, [&] { ++conflicts; });
    // flip: root read-only 60 ms on / 60 ms off
    QTimer flip; bool ro = false; int flips = 0;
    QObject::connect(&flip, &QTimer::timeout, [&] {
        ro = !ro; ++flips;
        QFile::setPermissions(sb.notes, ro ? QFileDevice::Permissions(QFile::ReadOwner | QFile::ExeOwner)
                                           : QFileDevice::Permissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    });
    if (flaky) flip.start(60);
    const Mem m0 = mem(); Stopwatch sw;
    for (int i = 0; i < N; ++i) submit(i);
    const bool finished = waitFor([&] { return doneCount == N; }, 240000);
    flip.stop();
    QFile::setPermissions(sb.notes, QFileDevice::Permissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    // allow stragglers (retry after final flip) to land
    if (!finished) { for (int i = 0; i < N; ++i) if (!notes[i].done) repo.retry(notes[i].rel); waitFor([&] { return doneCount == N; }, 60000); }
    metric(qPrintable(pfx + "total_ms"), sw.ms(), "ms"); metric(qPrintable(pfx + "saves_done"), doneCount * 1.0); metric(qPrintable(pfx + "saves_per_s"), doneCount * double(ROUNDS) * 1000.0 / sw.ms());
    metric(qPrintable(pfx + "failed_signals"), failures); metric(qPrintable(pfx + "dir_flips"), flips); metric(qPrintable(pfx + "conflicts"), conflicts);
    lat.report(pfx + "save_latency");
    metric(qPrintable(pfx + "pss_growth"), mem().total() - m0.total(), "MiB");
    check(doneCount == N, QString("all %1 notes reached their final round (%2 done)").arg(N).arg(doneCount));
    if (flaky) check(failures > 0, QString("fault injection actually produced failures (%1)").arg(failures));
    check(conflicts == 0, QString("no spurious conflicts from our own failed saves (%1)").arg(conflicts));
    int bad = 0, stale = 0;
    for (int i = 0; i < N; ++i) {
        const QByteArray b = readFile(sb.notes + "/" + notes[i].rel);
        const int v = versionOf(b, SZ);
        if (v < 0) ++bad; else if (v != i * 1000 + ROUNDS) ++stale;
    }
    check(bad == 0, QString("no note is partial/corrupt on disk (%1 bad)").arg(bad));
    check(stale == 0, QString("every note holds its LAST version (%1 stale)").arg(stale));
    int tmp = 0; QStringList tmpNames; QDirIterator it(sb.notes, QDir::Files | QDir::Hidden);
    while (it.hasNext()) { it.next(); if (!it.fileName().endsWith(".md")) { ++tmp; tmpNames << it.fileName() + QString(" (%1 B)").arg(it.fileInfo().size()); } }
    check(tmp == 0, QString("[D2-ORPHAN-TMP] ") + QString("no leftover temp files in the library after failed/retried saves (%1: %2)").arg(tmp).arg(tmpNames.join(", ")));
    repo.recovery().prune();
    check(repo.recoverableDrafts().isEmpty(), QString("no unresolved drafts left after everything saved (%1)").arg(repo.recoverableDrafts().size()));
    metric(qPrintable(pfx + "recovery_resolved_bytes"), double(repo.recovery().resolvedBytes()), "bytes");
}

// ------------------------------------------------------------------ 2.4 conflict storm
static QSet<QByteArray> allRecoveryContents(const QString &dir) {
    QSet<QByteArray> s; QDirIterator it(dir, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) { it.next(); if (it.fileName() != "meta.json") s.insert(readFile(it.filePath())); }
    return s;
}
static void conflictStorm() {
    note("== 2.4 conflict storm: external writer racing 1000 saves (jittered 0..6 ms after each save starts)");
    Sandbox sb("repo-storm");
    const QString recDir = sb.dir.filePath("rec");
    NoteRepository repo(sb.notes, recDir);
    const QString rel = repo.create("", "storm", "base\n");
    repo.read(rel);
    const QString abs = sb.notes + "/" + rel;
    std::atomic<int> started{0}, completed{0}; std::atomic<bool> stop{false};
    QHash<int, QByteArray> extVersions; std::mutex mu;
    std::thread writer([&] {
        int seen = 0; Rng rng(99);
        while (!stop) {
            if (started.load() == seen) { std::this_thread::sleep_for(std::chrono::microseconds(50)); continue; }
            seen = started.load();
            std::this_thread::sleep_for(std::chrono::microseconds(rng.bounded(6000)));
            const QByteArray body = "EXT-VERSION-" + QByteArray::number(seen) + "-" + QByteArray(64, 'e') + "\n";
            replaceFile(abs, body);   // atomic replace like most editors
            { std::lock_guard l(mu); extVersions[seen] = body; }
            // Harness fix: a version nobody could observe (overwritten by the writer itself before any save
            // round read the file) is unreachable by construction. Do not write again until a round that
            // started after this landing has finished.
            const int landedAt = started.load();
            while (!stop && completed.load() < landedAt + 1) std::this_thread::sleep_for(std::chrono::microseconds(50));
            seen = qMin(seen, landedAt);
        }
    });
    int saved = 0, conflicts = 0, failed = 0, removed = 0;
    QHash<int, QByteArray> localVersions;
    QByteArray last; bool resolvedFlag = false;
    Dist lat;
    for (int k = 1; k <= 1000; ++k) {
        const QByteArray body = "LOCAL-VERSION-" + QByteArray::number(k) + "-" + QByteArray(64, 'l') + "\n";
        localVersions[k] = body;
        enum { None, Saved, Conflict, Failed } out = None; QByteArray ext;
        auto c1 = QObject::connect(&repo, &NoteRepository::saved, [&] { out = Saved; });
        auto c2 = QObject::connect(&repo, &NoteRepository::conflict, [&](const QString &, const QByteArray &e, const QByteArray &, quint64) { out = Conflict; ext = e; });
        auto c3 = QObject::connect(&repo, &NoteRepository::saveFailed, [&] { if (out == None) out = Failed; });
        Stopwatch sw;
        started.store(k);
        repo.save(rel, body);
        waitFor([&] { return out != None; }, 20000);
        lat.add(sw.ms());
        QObject::disconnect(c1); QObject::disconnect(c2); QObject::disconnect(c3);
        if (out == Saved) ++saved;
        else if (out == Conflict) {
            ++conflicts;
            // "keep both": preserve local text beside the note, then adopt the external version as the new baseline
            writeFile(sb.notes + QString("/storm (local copy %1).md").arg(k), body);
            repo.resolveRecovery(rel);
            repo.read(rel);
        } else { ++failed; if (out == None) ++removed; }
        completed.store(k);
        // let the racing external write land before the next round starts
        std::this_thread::sleep_for(std::chrono::microseconds(300));
    }
    stop = true; writer.join();
    lat.report("repo.storm.round_latency");
    metric("repo.storm.saved", saved); metric("repo.storm.conflicts", conflicts); metric("repo.storm.failed", failed);
    check(failed == 0, QString("no failed saves during storm (%1)").arg(failed));
    check(saved + conflicts == 1000, "every save ended in saved or conflict");
    // accounting: every version either on disk now, in a side copy, or in recovery storage
    QSet<QByteArray> have = allRecoveryContents(recDir);
    QDirIterator it(sb.notes, QDir::Files);
    while (it.hasNext()) { it.next(); have.insert(readFile(it.filePath())); }
    int lostLocal = 0, lostExt = 0;
    for (auto i = localVersions.cbegin(); i != localVersions.cend(); ++i) if (!have.contains(i.value())) ++lostLocal;
    for (auto i = extVersions.cbegin(); i != extVersions.cend(); ++i) if (!have.contains(i.value())) ++lostExt;
    metric("repo.storm.external_versions_written", extVersions.size()); metric("repo.storm.lost_local_versions", lostLocal);
    metric("repo.storm.lost_external_versions", lostExt);
    // Local versions that were saved and then legitimately replaced by a later local save are in resolved history,
    // so "lost" really means unreachable by the user (no recovery copy, no side copy, not on disk).
    check(lostLocal == 0, QString("no local version lost (%1 of 1000 unreachable)").arg(lostLocal));
    check(lostExt == 0, QString("[D1-TOCTOU] ") + QString("no external version silently overwritten: %1 of %2 external versions unreachable after racing saves (conflict check is read -> fsync-heavy recovery writes -> rename, not atomic)").arg(lostExt).arg(extVersions.size()));
    (void)last; (void)resolvedFlag; (void)removed;
}


// Where exactly is the unsafe window? One external write at a fixed delay after save() is called; 40 trials per delay.
static void racingWindowSweep() {
    note("== 2.4b racing-window sweep: external atomic write d microseconds after save() (40 trials each)");
    Sandbox sb("repo-sweep");
    NoteRepository repo(sb.notes, sb.dir.filePath("rec"));
    const QString rel = repo.create("", "sweep", "base\n"), abs = sb.notes + "/" + rel;
    int totalLost = 0, totalDetected = 0, totalTrials = 0; int tag = 0;
    for (int d : {0, 250, 500, 1000, 1500, 2000, 3000, 5000, 20000}) {
        int detected = 0, overwrittenSafely = 0, lost = 0, extAfter = 0;
        for (int t = 0; t < 40; ++t, ++tag) {
            writeFile(abs, "base\n"); repo.read(rel); repo.resolveRecovery(rel);
            const QByteArray E = "EXT-" + QByteArray::number(tag) + "-sweep\n", L = "LOC-" + QByteArray::number(tag) + "-sweep\n";
            enum { None, Saved, Conflict, Failed } out = None;
            auto c1 = QObject::connect(&repo, &NoteRepository::saved, [&] { out = Saved; });
            auto c2 = QObject::connect(&repo, &NoteRepository::conflict, [&] { out = Conflict; });
            auto c3 = QObject::connect(&repo, &NoteRepository::saveFailed, [&] { out = Failed; });
            std::thread w([&] { std::this_thread::sleep_for(std::chrono::microseconds(d)); replaceFile(abs, E); });
            repo.save(rel, L);
            waitFor([&] { return out != None; }, 10000);
            w.join();
            QObject::disconnect(c1); QObject::disconnect(c2); QObject::disconnect(c3);
            const QByteArray disk = readFile(abs);
            QSet<QByteArray> have = allRecoveryContents(sb.dir.filePath("rec")); have.insert(disk);
            if (out == Conflict) ++detected;
            else if (disk == E) ++extAfter;                    // external write landed after our rename: disk holds it, local is in resolved history
            else if (have.contains(E)) ++overwrittenSafely;   // we overwrote it but a recovery copy exists
            else ++lost;
        }
        note(QString("d=%1 us: conflict-detected=%2 external-landed-after-save=%3 overwritten-with-copy=%4 SILENTLY-LOST=%5").arg(d, 6).arg(detected, 2).arg(extAfter, 2).arg(overwrittenSafely, 2).arg(lost, 2));
        metric(qPrintable(QString("repo.sweep.lost_at_%1us").arg(d)), lost);
        totalLost += lost; totalDetected += detected; totalTrials += 40;
    }
    metric("repo.sweep.total_lost", totalLost); metric("repo.sweep.total_trials", totalTrials);
    // sanity: an external write that clearly precedes the save is always detected
    writeFile(abs, "base\n"); repo.read(rel);
    replaceFile(abs, "EXT-early\n");
    bool conflict = false; auto c = QObject::connect(&repo, &NoteRepository::conflict, [&] { conflict = true; });
    repo.save(rel, "LOC-late\n"); waitFor([&] { return conflict; }, 5000); QObject::disconnect(c);
    check(conflict, "sanity: an external write that precedes the save is detected as a conflict");
    check(totalLost == 0, QString("[D1-TOCTOU] ") + QString("racing external writes never silently lost: %1 of %2 sweep trials lost the external version").arg(totalLost).arg(totalTrials));
}

// ------------------------------------------------------------------ 2.5 kill -9 during save (child process mode)
static int childSave(const QString &root, const QString &rec, const QString &rel, qsizetype size, bool viaRepo, int startVersion) {
    QCoreApplication::processEvents();
    NoteRepository repo(root, rec);
    repo.read(rel);
    std::printf("READY\n"); std::fflush(stdout);
    for (int v = startVersion;; ++v) {
        if (viaRepo) {
            bool done = false;
            auto c = QObject::connect(&repo, &NoteRepository::saved, [&] { done = true; });
            auto d = QObject::connect(&repo, &NoteRepository::conflict, [&] { done = true; });
            auto e = QObject::connect(&repo, &NoteRepository::saveFailed, [&] { done = true; });
            repo.save(rel, versioned(v, size));
            std::printf("S%d\n", v); std::fflush(stdout);
            waitFor([&] { return done; }, 60000);
            QObject::disconnect(c); QObject::disconnect(d); QObject::disconnect(e);
        } else {
            std::printf("S%d\n", v); std::fflush(stdout);
            QString err; atomicWrite(root + "/" + rel, versioned(v, size), &err);
        }
    }
}
static void killDuringSave() {
    note("== 2.5 SIGKILL during save (atomicWrite directly, and via NoteRepository with recovery)");
    const QString self = QCoreApplication::applicationFilePath();
    for (int viaRepo = 0; viaRepo < 2; ++viaRepo) {
        Sandbox sb(viaRepo ? "repo-kill-r" : "repo-kill-a");
        const qsizetype SIZE = 6 << 20;
        const QString rel = "victim.md", abs = sb.notes + "/" + rel, rec = sb.dir.filePath("rec");
        writeFile(abs, versioned(0, SIZE));
        Rng rng(viaRepo + 11);
        int kills = 0, intact = 0, partial = 0, draftsListed = 0, draftsBad = 0, noDraft = 0; QSet<int> seenVersions;
        int version = 1;
        int draftsNeeded = 0;
        for (int k = 0; k < 120; ++k) {
            QProcess p;
            p.setProgram(self);
            p.setArguments({"--child-save", sb.notes, rec, rel, QString::number(SIZE), viaRepo ? "1" : "0", QString::number(version)});
            p.start();
            if (!p.waitForStarted(5000) || !p.waitForReadyRead(10000)) { check(false, "child failed to start"); return; }
            // wait for the first save line then kill after random delay (inside the write window)
            p.waitForReadyRead(10000);
            std::this_thread::sleep_for(std::chrono::microseconds(rng.bounded(40000)));
            kill(p.processId(), SIGKILL); p.waitForFinished(5000);
            ++kills;
            const QByteArray out = p.readAllStandardOutput();
            int maxStarted = version - 1;
            for (const QByteArray &l : out.split('\n')) if (l.startsWith('S')) maxStarted = qMax(maxStarted, l.mid(1).toInt());
            const int v = versionOf(readFile(abs), SIZE);
            if (v < 0) { ++partial; note(QString("PARTIAL file after kill #%1: size=%2").arg(k).arg(QFileInfo(abs).size())); }
            else { ++intact; seenVersions.insert(v); }
            version = maxStarted + 1;
            if (viaRepo) {
                NoteRepository r2(sb.notes, rec);
                const auto drafts = r2.recoverableDrafts();
                if (drafts.isEmpty()) ++noDraft;
                for (const auto &d : drafts) {
                    ++draftsListed;
                    if (versionOf(d.local, SIZE) < 0) { ++draftsBad; note(QString("draft local is partial: size %1").arg(d.local.size())); }
                    if (v >= 0 && versionOf(d.local, SIZE) > v) ++draftsNeeded;   // newer than disk: recovery really matters
                    r2.resolveRecovery(d.relPath);
                }
            }
        }
        { NoteRepository reopen(sb.notes, rec); } // D2: opening the library sweeps temp files of killed writers
        int orphans = 0; qint64 orphanBytes = 0;
        QDirIterator it(sb.notes, QDir::Files | QDir::Hidden);
        while (it.hasNext()) { it.next(); if (it.fileName() != rel) { ++orphans; orphanBytes += it.fileInfo().size(); } }
        const QString tag = viaRepo ? "repo" : "atomic";
        metric(qPrintable("repo.kill9." + tag + ".kills"), kills); metric(qPrintable("repo.kill9." + tag + ".intact"), intact);
        metric(qPrintable("repo.kill9." + tag + ".partial"), partial); metric(qPrintable("repo.kill9." + tag + ".distinct_versions_seen"), seenVersions.size());
        metric(qPrintable("repo.kill9." + tag + ".orphan_temp_files"), orphans); metric(qPrintable("repo.kill9." + tag + ".orphan_bytes"), double(orphanBytes), "bytes");
        check(partial == 0, QString("[%1] file is always an intact old-or-new version after %2 SIGKILLs (%3 partial)").arg(tag).arg(kills).arg(partial));
        soft(seenVersions.size() > 1, QString("[%1] kills landed at varying points (%2 distinct versions on disk)").arg(tag).arg(seenVersions.size()));
        if (viaRepo) {
            metric("repo.kill9.repo.drafts_listed", draftsListed); metric("repo.kill9.repo.kills_without_draft", noDraft);
            metric("repo.kill9.repo.drafts_newer_than_disk", draftsNeeded);
            check(draftsBad == 0, QString("every listed draft holds an intact version (%1 partial drafts)").arg(draftsBad));
            check(draftsListed > 0, QString("recovery lists drafts after kill (%1 listed over %2 kills)").arg(draftsListed).arg(kills));
        }
        check(orphans == 0, QString("[D2-ORPHAN-TMP] ") + QString("[%1] killed saves leave no orphan temp files in the library: %2 files / %3 MiB accumulated over %4 kills (QSaveFile temp is never swept)").arg(tag).arg(orphans).arg(orphanBytes >> 20).arg(kills));
    }
}

// ------------------------------------------------------------------ 2.6 disk-full surrogate (RLIMIT_FSIZE)
static int childFsize(const QString &root, const QString &rec, const QString &rel) {
    struct rlimit rl{1 << 20, 1 << 20};   // 1 MiB max file size: writes beyond fail with EFBIG
    signal(SIGXFSZ, SIG_IGN);
    setrlimit(RLIMIT_FSIZE, &rl);
    NoteRepository repo(root, rec);
    repo.read(rel);
    auto run = [&](qsizetype size, int v) {
        QString outcome = "timeout"; bool done = false; QString err;
        auto c = QObject::connect(&repo, &NoteRepository::saved, [&] { outcome = "saved"; done = true; });
        auto d = QObject::connect(&repo, &NoteRepository::saveFailed, [&](const QString &, const QString &e, quint64) { outcome = "failed"; err = e; done = true; });
        repo.save(rel, versioned(v, size));
        waitFor([&] { return done; }, 20000);
        QObject::disconnect(c); QObject::disconnect(d);
        std::printf("RESULT size=%lld outcome=%s err=%s\n", (long long)size, qPrintable(outcome), qPrintable(err));
    };
    run(4 << 20, 2);       // too large for the limit: must fail, original must survive
    run(64 << 10, 3);      // fits: must succeed after the failure (state machine not wedged)
    std::fflush(stdout);
    return 0;
}
static void diskFull() {
    note("== 2.6 disk-full surrogate: RLIMIT_FSIZE=1MiB child saves a 4 MiB note over a 100 KiB one (real ENOSPC needs a size-limited tmpfs => root; skipped)");
    skip("true ENOSPC on a size-limited tmpfs: mount requires root, not available");
    Sandbox sb("repo-fsize");
    const QString rel = "full.md", abs = sb.notes + "/" + rel;
    writeFile(abs, versioned(1, 100 * 1024));
    QProcess p; p.setProgram(QCoreApplication::applicationFilePath());
    p.setArguments({"--child-fsize", sb.notes, sb.dir.filePath("rec"), rel});
    p.start(); p.waitForFinished(60000);
    const QString out = QString::fromUtf8(p.readAllStandardOutput());
    note(out.trimmed());
    check(p.exitStatus() == QProcess::NormalExit, "child did not crash");
    check(out.contains("size=4194304 outcome=failed"), "oversize save reported as failed (not silently saved)");
    check(out.contains("size=65536 outcome=saved"), "next save that fits succeeds afterwards");
    const int v = versionOf(readFile(abs), 64 << 10);
    check(v == 3, "final file is the intact 64 KiB version after the failed 4 MiB attempt");
    int tmp = 0; QDirIterator it(sb.notes, QDir::Files | QDir::Hidden);
    while (it.hasNext()) { it.next(); if (it.fileName() != rel) ++tmp; }
    check(tmp == 0, QString("no temp debris after EFBIG failure (%1)").arg(tmp));
    // original survives when the second save is also too big
    writeFile(abs, versioned(1, 100 * 1024));
}

// ------------------------------------------------------------------ 2.7 recovery cap (64 MiB)
static void recoveryCap() {
    note("== 2.7 recovery store: 64 MiB resolved cap, unresolved never evicted, prune cost");
    Sandbox sb("repo-cap");
    RecoveryStore rs(sb.dir.filePath("rec"));   // default cap 64 MiB
    const QString root = sb.notes;
    const QByteArray mb = QByteArray(1 << 20, 'm');
    // 20 unresolved 1 MiB entries (+1 MiB prev disk each) that must survive everything below
    for (int i = 0; i < 20; ++i) { QString e; check(rs.writePending(root, QString("keep%1.md").arg(i), versioned(i, 1 << 20), &mb, 1, &e), "writePending " + e); }
    Dist resolveMs; qint64 maxResolved = 0;
    for (int i = 0; i < 200; ++i) {   // 200 x (1 MiB local + 1 MiB prev) = 400 MiB through a 64 MiB cap
        const QString rel = QString("hist%1.md").arg(i);
        rs.writePending(root, rel, versioned(i + 100, 1 << 20), &mb, 1, nullptr);
        Stopwatch sw; rs.resolve(root, rel); resolveMs.add(sw.ms());
        maxResolved = qMax(maxResolved, rs.resolvedBytes());
    }
    resolveMs.report("repo.reccap.resolve_ms_big_entries");
    metric("repo.reccap.max_resolved_bytes", double(maxResolved), "bytes");
    check(maxResolved <= (64LL << 20), QString("resolved history never exceeds 64 MiB (max %1 MiB)").arg(maxResolved / 1048576.0));
    check(rs.unresolved(root).size() == 20, QString("unresolved entries never evicted (%1/20 left)").arg(rs.unresolved(root).size()));
    // unresolved beyond the cap: 90 x 1 MiB unresolved > 64 MiB must all remain
    for (int i = 20; i < 110; ++i) rs.writePending(root, QString("keep%1.md").arg(i), versioned(i, 1 << 20), nullptr, 1, nullptr);
    rs.prune();
    check(rs.unresolved(root).size() == 110, QString("110 unresolved (>64 MiB) all retained (%1)").arg(rs.unresolved(root).size()));
    // per-save cost of resolve() as many small histories accumulate (autosave path calls resolve on every save)
    Sandbox sb2("repo-cap2");
    RecoveryStore small(sb2.dir.filePath("rec"));
    const QByteArray kb = QByteArray(4096, 'k');
    QList<double> checkpoints;
    for (int i = 1; i <= 8000; ++i) {
        const QString rel = QString("s%1.md").arg(i);
        small.writePending(sb2.notes, rel, kb, &kb, 1, nullptr);
        Stopwatch sw; small.resolve(sb2.notes, rel); const double ms = sw.ms();
        if (i == 10 || i == 500 || i == 1500 || i == 3000 || i == 8000) { metric(qPrintable(QString("repo.reccap.resolve_ms_at_%1_entries").arg(i)), ms, "ms"); checkpoints << ms; }
    }
    check(checkpoints.last() < checkpoints.first() * 5 + 5, QString("[D3-PRUNE-SCAN] ") + QString("resolve() cost stays flat as history grows: %1 ms at 10 entries vs %2 ms at 8000 (prune() walks the whole resolved tree on every save)").arg(checkpoints.first()).arg(checkpoints.last()));
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const QStringList a = app.arguments();
    if (a.value(1) == "--child-save") return childSave(a[2], a[3], a[4], a[5].toLongLong(), a[6] == "1", a[7].toInt());
    if (a.value(1) == "--child-fsize") return childFsize(a[2], a[3], a[4]);
    if (getuid() == 0) note("running as root: read-only directory fault injection is ineffective");
    const QString which = a.value(1, "all");
    auto want = [&](const char *n) { return which == "all" || which == n; };
    if (want("lib")) libraryScale();
    if (want("watch")) { watcherBurst(0, "instant"); watcherBurst(1500, "paced_3s"); }
    if (want("flaky")) { flakyAutosave(false); flakyAutosave(true); }
    if (want("storm")) conflictStorm();
    if (want("storm")) racingWindowSweep();
    if (want("kill")) killDuringSave();
    if (want("full")) diskFull();
    if (want("cap")) recoveryCap();
    return finish("stress_repo");
}
