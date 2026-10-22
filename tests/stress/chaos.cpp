#include "controller.h"
#include "hn/core/markdown_codec.h"
#include "hn/editor/note_editor.h"
#include "hn/plugins/runtime.h"
#include "sticky_window.h"
#include "stress_util.h"
#include <QApplication>
#include <QDirIterator>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QTextCursor>
#include <QTimer>
#include <QFileInfo>
#include <QDir>
#include <QRegularExpression>
#include <QRandomGenerator>
#include <array>
#include <cmath>
#include <malloc.h>
#include <unistd.h>

using namespace st;
using namespace hn::app;

namespace {
constexpr int kChaosMaxNotes = 128;
constexpr int kChaosMaxOpenSessions = 12;

void pumpDeferredDeletes(int maxMs = 10) {
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents(QEventLoop::AllEvents, maxMs);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

struct World {
    Sandbox sb{"chaos"};
    ControllerOptions opt;
    std::unique_ptr<AppController> app;
    QMap<QString, QByteArray> shadow;
    QMap<QString, bool> officialPlugins;
    QMap<QString, QString> officialSourceHashes;
    bool pluginDestinationIsolated = false;
    int restartCount = 0;
    int capRetirements = 0;
    explicit World() {
        opt.notesDir = sb.notes; opt.stateDir = sb.state; opt.cacheDir = sb.cache;
        opt.configPath = sb.config + "/config.json";
        opt.modsDir = sb.data + "/mods"; opt.modsEnabledPath = sb.config + "/mods-enabled.json";
        const QString root = qEnvironmentVariable("HN_PLUGINS_DIR", QStringLiteral(HN_PLUGINS_DIR));
        opt.pluginsDir = sb.data + "/plugins";
        pluginDestinationIsolated = QDir::cleanPath(QFileInfo(opt.pluginsDir).absoluteFilePath())
            != QDir::cleanPath(QFileInfo(root).absoluteFilePath());
        if (!pluginDestinationIsolated) return;
        opt.useTray = false; opt.useHyprland = false;
        opt.confirm = [](const QString &, const QString &) { return true; };
        opt.recoveryPrompt = [](const QList<hn::core::RecoveryEntry> &) { return RecoveryChoice::Later; };
        opt.pluginHooks.consent = [](const ConsentRequest &) { return true; };
        const QStringList ids{"slash-menu", "journal", "tasks", "wiki-links"};
        bool complete = !root.isEmpty();
        for (const QString &id : ids) complete &= QFileInfo::exists(root + "/" + id + "/plugin.json");
        if (complete)
            for (const QString &id : ids) officialSourceHashes[id] = hn::plugins::hashDirectory(root + "/" + id);
        app = std::make_unique<AppController>(opt);
        if (complete) {
            auto *manager = app->plugins().manager();
            for (const QString &id : ids) {
                const auto installed = manager->install(root + "/" + id, true);
                QString error;
                const bool enabled = installed.ok
                    && manager->consent(id, manager->info(id).manifest.permissions, &error)
                    && app->plugins().enable(id, nullptr, &error);
                check(enabled, "chaos installed and enabled official plugin " + id + ": " + error);
                if (enabled) officialPlugins[id] = true;
                check(QFileInfo::exists(opt.pluginsDir + "/" + id + "/plugin.json"),
                      "official plugin installed under scratch destination: " + id);
            }
            for (const QString &id : ids)
                check(officialSourceHashes.value(id) == hn::plugins::hashDirectory(root + "/" + id),
                      "official plugin source tree remained unchanged: " + id);
        } else {
            skip("chaos official-plugin on/off coverage requires complete slash-menu, journal, tasks, wiki-links checkout");
        }
    }
    QString create(int n) {
        const QByteArray bytes = "# Shadow " + QByteArray::number(n) + "\n\nseeded-content-" + QByteArray::number(n) + "\n";
        QString err;
        const QString rel = app->repo().create("chaos/generated", "n" + QString::number(n), bytes, &err);
        check(!rel.isEmpty(), "repository created chaos note: " + err);
        if (!rel.isEmpty()) shadow[rel] = bytes;
        return rel;
    }
    bool restart() {
        app->requestQuit();
        for (int i = 0; i < 1000 && !app->exiting(); ++i) pumpDeferredDeletes();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        const bool sessionWritten = QFileInfo::exists(sb.state + "/session.json");
        app.reset();
        app = std::make_unique<AppController>(opt);
        app->start(hn::platform::Action::ShowOrganizer);
        ++restartCount;
        const int restored = app->stickies().size() + (app->organizer() && app->organizer()->currentSession() ? 1 : 0);
        const bool consistent = app->openNotes().size() == restored;
        check(app->openNotes().size() <= kChaosMaxOpenSessions,
              "session restore respects chaos open-session cap");
        check(sessionWritten, "actual app quit wrote session.json");
        check(consistent, "actual app restart restored sessions into visible windows");
        return sessionWritten && consistent;
    }
};

int markdownFileCount(const World &w) {
    int count = 0;
    QDirIterator files(w.sb.notes, {"*.md"}, QDir::Files, QDirIterator::Subdirectories);
    while (files.hasNext()) { files.next(); ++count; }
    return count;
}
QString createBounded(World &w, int n) {
    if (w.shadow.size() >= kChaosMaxNotes) {
        QString victim;
        for (const QString &candidate : w.shadow.keys()) {
            if (!w.app->session(candidate)) { victim = candidate; break; }
        }
        if (victim.isEmpty()) { check(false, "note cap has a closed candidate for retirement"); return {}; }
        const QByteArray expected = w.shadow.value(victim);
        if (readFile(w.sb.notes + "/" + victim) != expected) {
            check(false, "retired candidate disk bytes match shadow before deletion");
            return {};
        }
        QString error;
        const bool removed = w.app->deleteNote(victim, &error);
        const bool absent = !QFileInfo::exists(w.sb.notes + "/" + victim);
        check(removed && absent, "controller removed actual retirement candidate before shadow update: " + error);
        if (!removed || !absent) return {};
        w.shadow.remove(victim);
        ++w.capRetirements;
    }
    const QString rel = w.create(n);
    if (!rel.isEmpty())
        check(readFile(w.sb.notes + "/" + rel) == w.shadow.value(rel), "created note matches shadow bytes");
    return rel;
}

NoteSession *openStickyBounded(World &w, const QString &rel) {
    if (!w.app->session(rel) && w.app->openNotes().size() >= kChaosMaxOpenSessions) return nullptr;
    return w.app->openSticky(rel, false);
}

NoteSession *openOrganizerBounded(World &w, const QString &rel) {
    if (!w.app->session(rel) && w.app->openNotes().size() >= kChaosMaxOpenSessions) return nullptr;
    return w.app->openInOrganizer(rel);
}

bool editFromShadow(World &w, const QString &rel, long op, bool injectCorruption = false) {
    NoteSession *session = w.app->session(rel);
    if (!session) return false;
    auto *editor = session->editor();
    const QByteArray before = w.shadow.value(rel);
    QByteArray insertion = "<!-- chaos-edit-" + QByteArray::number(op) + " -->\n";
    if (!before.endsWith('\n')) insertion.prepend('\n');
    const QByteArray expected = before + insertion;
    if (!editor->setMode(hn::editor::Mode::Source)) return false;
    if (editor->sourceEdit()->toPlainText().toUtf8() != before) return false;
    QTextCursor cursor(editor->sourceEdit()->document());
    cursor.movePosition(QTextCursor::End);
    editor->sourceEdit()->setTextCursor(cursor);
    editor->insertMarkdown(QString::fromUtf8(insertion), false);
    if (injectCorruption) editor->insertMarkdown(QStringLiteral("corrupt-export\n"), false);
    const QByteArray actual = editor->toMarkdownBytes();
    if (actual != expected || editor->sourceEdit()->toPlainText().toUtf8() != expected) return false;
    w.shadow[rel] = expected;
    session->flush();
    return true;
}

void verifyDisk(World &w, int minute) {
    int lost = 0;
    for (auto it = w.shadow.cbegin(); it != w.shadow.cend(); ++it) {
        const QByteArray actual = readFile(w.sb.notes + "/" + it.key());
        if (actual != it.value()) {
            ++lost;
            note(QString("SHADOW_MISMATCH minute=%1 rel=%2 expected=%3 actual=%4 expected_prefix=%5 actual_prefix=%6")
                 .arg(minute).arg(it.key()).arg(it.value().size()).arg(actual.size())
                 .arg(QString::fromUtf8(it.value().left(50)).toHtmlEscaped())
                 .arg(QString::fromUtf8(actual.left(50)).toHtmlEscaped()));
        }
    }
    check(lost == 0, QString("minute %1: shadow-model exact bytes match disk (%2 lost/mismatched)").arg(minute).arg(lost));
    const int sessions = w.app->openNotes().size();
    const int noteWindows = w.app->stickies().size() + (w.app->organizer() && w.app->organizer()->currentSession() ? 1 : 0);
    check(noteWindows == sessions, QString("minute %1: sessions equal note windows (%2/%3)").arg(minute).arg(sessions).arg(noteWindows));
    QSet<QString> shown;
    for (auto *s : w.app->stickies()) shown.insert(s->session()->rel());
    if (w.app->organizer() && w.app->organizer()->currentSession()) shown.insert(w.app->organizerNote());
    check(shown.size() == sessions, QString("minute %1: no orphan or multiply-shown sessions").arg(minute));
    const int actualNoteFiles = markdownFileCount(w);
    check(actualNoteFiles == w.shadow.size(), QString("minute %1: actual note file count equals shadow (%2/%3)").arg(minute).arg(actualNoteFiles).arg(w.shadow.size()));
    check(actualNoteFiles <= kChaosMaxNotes, QString("minute %1: note fixture cap %2 holds (%3 files)").arg(minute).arg(kChaosMaxNotes).arg(actualNoteFiles));
    check(sessions <= kChaosMaxOpenSessions, QString("minute %1: open session cap %2 holds (%3)").arg(minute).arg(kChaosMaxOpenSessions).arg(sessions));
    int tempFiles = 0;
    QDirIterator tempScan(w.sb.dir.path(), QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
    while (tempScan.hasNext()) { tempScan.next(); if (tempScan.fileName().contains(".hn-tmp.")) ++tempFiles; }
    int unresolvedEntries = 0;
    QDirIterator unresolved(w.sb.state + "/recovery/unresolved", QDir::Files | QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (unresolved.hasNext()) { unresolved.next(); ++unresolvedEntries; }
    QDir resolved(w.sb.state + "/recovery/resolved");
    const auto histories = resolved.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
    qint64 historyBytes = 0;
    for (const auto &dir : histories) {
        QDirIterator files(dir.filePath(), QDir::Files, QDirIterator::Subdirectories);
        while (files.hasNext()) { files.next(); historyBytes += files.fileInfo().size(); }
    }
    const bool boundedHistory = histories.size() <= 4096 && historyBytes <= (64LL << 20);
    check(tempFiles == 0 && unresolvedEntries == 0 && boundedHistory,
          QString("minute %1: full scratch sandbox has no temp files, unresolved recovery is empty, resolved history <=4096 entries/64MiB (temps=%2 unresolved=%3 history=%4/%5 bytes)")
              .arg(minute).arg(tempFiles).arg(unresolvedEntries).arg(histories.size()).arg(historyBytes));
    const int drafts = w.app->repo().recoverableDrafts().size();
    check(drafts == 0, QString("minute %1: settled chaos state has no unaccounted recovery drafts (%2)").arg(minute).arg(drafts));
}

template <typename T> int liveWidgetsOfType() {
    int count = 0;
    for (QWidget *widget : QApplication::allWidgets())
        if (qobject_cast<T *>(widget)) ++count;
    return count;
}

void reportResources(World &w, int minute, long operations) {
    const QString prefix = QString("chaos.minute%1").arg(minute);
    const Mem m = mem();
    const struct mallinfo2 heap = mallinfo2();
    qint64 shadowBytes = 0;
    for (auto it = w.shadow.cbegin(); it != w.shadow.cend(); ++it) shadowBytes += it.value().size();
    int noteFiles = 0;
    QDirIterator notes(w.sb.notes, {"*.md"}, QDir::Files, QDirIterator::Subdirectories);
    while (notes.hasNext()) { notes.next(); ++noteFiles; }
    metric(qPrintable(prefix + ".pss"), m.total(), "MiB");
    metric(qPrintable(prefix + ".rss"), m.rssMiB, "MiB");
    metric(qPrintable(prefix + ".private"), m.privateMiB, "MiB");
    metric(qPrintable(prefix + ".pss_anon"), m.anonMiB, "MiB");
    metric(qPrintable(prefix + ".heap_in_use"), (heap.uordblks + heap.hblkhd) / 1048576.0, "MiB");
    metric(qPrintable(prefix + ".widgets"), QApplication::allWidgets().size());
    metric(qPrintable(prefix + ".editors"), liveWidgetsOfType<hn::editor::NoteEditor>());
    metric(qPrintable(prefix + ".stickies"), liveWidgetsOfType<StickyWindow>());
    metric(qPrintable(prefix + ".organizers"), liveWidgetsOfType<OrganizerWindow>());
    metric(qPrintable(prefix + ".sessions"), w.app->openNotes().size());
    metric(qPrintable(prefix + ".windows"), w.app->windowCount());
    metric(qPrintable(prefix + ".threads"), threadCount());
    metric(qPrintable(prefix + ".fds"), openFds());
    metric(qPrintable(prefix + ".note_files"), noteFiles);
    metric(qPrintable(prefix + ".shadow_notes"), w.shadow.size());
    metric(qPrintable(prefix + ".shadow_bytes"), shadowBytes, "bytes");
    metric(qPrintable(prefix + ".restarts"), w.restartCount);
    metric(qPrintable(prefix + ".cap_retirements"), w.capRetirements);
    metric(qPrintable(prefix + ".note_cap"), kChaosMaxNotes);
    metric(qPrintable(prefix + ".open_session_cap"), kChaosMaxOpenSessions);
    metric(qPrintable(prefix + ".operations"), operations);
}

struct ThreadSample {
    QString identity;
    double cpuSeconds = 0.0;
};

bool parseThreadStat(const QByteArray &stat, int tid, ThreadSample *sample) {
    if (!sample) return false;
    const qsizetype close = stat.lastIndexOf(')');
    if (close < 0 || close + 2 >= stat.size()) return false;
    const auto fields = stat.mid(close + 2).simplified().split(' ');
    // Suffix starts at proc stat field 3 (state); utime/stime are fields 14/15,
    // and starttime is field 22. Include starttime because Linux can reuse TIDs.
    if (fields.size() < 20 || fields[0].size() != 1) return false;
    bool ticksOk = false;
    bool startOk = false;
    const qulonglong userTicks = fields[11].toULongLong(&ticksOk);
    const qulonglong startTicks = fields[19].toULongLong(&startOk);
    bool systemOk = false;
    const qulonglong systemTicks = fields[12].toULongLong(&systemOk);
    const long ticksPerSecond = sysconf(_SC_CLK_TCK);
    if (!ticksOk || !systemOk || !startOk || ticksPerSecond <= 0) return false;
    sample->identity = QString("%1:%2").arg(tid).arg(startTicks);
    sample->cpuSeconds = double(userTicks + systemTicks) / double(ticksPerSecond);
    return true;
}

QHash<int, ThreadSample> threadCpuSeconds() {
    QHash<int, ThreadSample> result;
    QDir tasks("/proc/self/task");
    for (const QString &tid : tasks.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        QFile stat(tasks.filePath(tid + "/stat"));
        if (!stat.open(QIODevice::ReadOnly)) continue;
        bool tidOk = false;
        const int tidValue = tid.toInt(&tidOk);
        ThreadSample sample;
        if (tidOk && parseThreadStat(stat.readAll(), tidValue, &sample))
            result.insert(tidValue, sample);
    }
    return result;
}

void threadStatParserSmoke() {
    // Suffix fields 3..22: state, ppid..stime, ..., starttime.
    const QByteArray first = "123 (worker (one)) S 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 900";
    const QByteArray second = "123 (worker (one)) S 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 901";
    ThreadSample a, b, malformed;
    check(parseThreadStat(first, 123, &a) && parseThreadStat(second, 123, &b),
          "proc stat fixture parses comm parentheses and counters");
    check(std::abs(a.cpuSeconds - 0.23) < 1e-9,
          "proc stat fixture reads utime/stime positions and converts ticks using clock rate");
    check(a.identity == "123:900" && b.identity == "123:901" && a.identity != b.identity,
          "same TID with changed starttime is a different thread identity");
    check(!parseThreadStat("123 (truncated) S 1 2", 123, &malformed),
          "truncated proc stat fixture is rejected, never treated as zero");
    QHash<int, ThreadSample> before{{123, a}, {456, {"456:800", 2.0}}};
    QHash<int, ThreadSample> after{{123, b}, {789, {"789:1000", 3.0}}};
    int matched = 0, newlySeen = 0, exited = 0;
    for (auto it = after.cbegin(); it != after.cend(); ++it) {
        const auto old = before.constFind(it.key());
        if (old != before.cend() && old->identity == it->identity) ++matched;
        else ++newlySeen;
    }
    for (auto it = before.cbegin(); it != before.cend(); ++it) {
        const auto now = after.constFind(it.key());
        if (now == after.cend() || now->identity != it->identity) ++exited;
    }
    check(matched == 0 && newlySeen == 2 && exited == 2,
          "snapshot fixture classifies TID reuse/new thread and exited identities without fabricated deltas");
}

void idleTimerControl(int seconds) {
    const double cpu0 = cpuSeconds();
    QElapsedTimer wall;
    wall.start();
    QEventLoop loop;
    QTimer::singleShot(seconds * 1000, &loop, &QEventLoop::quit);
    loop.exec();
    const qint64 elapsedMs = wall.elapsed();
    const double cpu = cpuSeconds() - cpu0;
    metric("chaos.control.process_cpu_seconds", cpu, "seconds");
    metric("chaos.control.wall_ms", elapsedMs, "ms");
    metric("chaos.control.wall_resolution_ms", 1, "ms");
    metric("chaos.control.cpu_percent_of_one_core", cpu / (elapsedMs / 1000.0) * 100.0, "percent");
    note("CONTROL=minimal QApplication + singleShot quit event loop; no World/AppController");
}

struct MappingBytes { double pssKiB = 0.0; double privateKiB = 0.0; };

QMap<QString, MappingBytes> mappingClasses() {
    QFile smaps("/proc/self/smaps");
    QMap<QString, MappingBytes> classes;
    if (!smaps.open(QIODevice::ReadOnly)) return classes;
    QString current = "unknown";
    const QRegularExpression header(QStringLiteral("^[0-9a-f]+-[0-9a-f]+\\s"));
    auto add = [&](const QString &line, const char *key, double *value) {
        if (!line.startsWith(QLatin1String(key))) return false;
        bool ok = false;
        const double amount = line.mid(int(strlen(key))).trimmed().split(' ').first().toDouble(&ok);
        if (ok) *value += amount;
        return true;
    };
    for (const QByteArray &raw : smaps.readAll().split('\n')) {
        const QString line = QString::fromLatin1(raw);
        if (header.match(line).hasMatch()) {
            const QStringList fields = line.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
            const QString path = fields.size() > 5 ? fields.mid(5).join(' ') : QString();
            if (path.isEmpty()) current = "anonymous";
            else if (path == "[heap]") current = "heap";
            else if (path.startsWith("[stack")) current = "stack";
            else if (path.startsWith('[')) current = "kernel-mapping";
            else current = "file-backed";
            classes[current];
            continue;
        }
        auto &bytes = classes[current];
        add(line, "Pss:", &bytes.pssKiB);
        add(line, "Private_Clean:", &bytes.privateKiB);
        add(line, "Private_Dirty:", &bytes.privateKiB);
    }
    return classes;
}

void mappingSnapshot(const QString &label) {
    const Mem memory = mem();
    const struct mallinfo2 heap = mallinfo2();
    metric(qPrintable("chaos.smaps." + label + ".process_pss"), memory.total(), "MiB");
    metric(qPrintable("chaos.smaps." + label + ".pss_anon"), memory.anonMiB, "MiB");
    metric(qPrintable("chaos.smaps." + label + ".live_heap"),
           (heap.uordblks + heap.hblkhd) / 1048576.0, "MiB");
    const auto classes = mappingClasses();
    for (auto it = classes.cbegin(); it != classes.cend(); ++it) {
        metric(qPrintable("chaos.smaps." + label + ".class." + it.key() + ".pss"), it->pssKiB / 1024.0, "MiB");
        metric(qPrintable("chaos.smaps." + label + ".class." + it.key() + ".private"), it->privateKiB / 1024.0, "MiB");
    }
}

void mappingStateSmoke() {
    World w;
    if (!w.pluginDestinationIsolated || !w.app) {
        check(false, "mapping smoke refuses unsafe or uninitialized plugin destination");
        return;
    }
    for (int i = 0; i < 12; ++i) w.create(i);
    w.app->start(hn::platform::Action::ShowOrganizer);
    const QStringList notes = w.shadow.keys();
    mappingSnapshot("fixed_before");
    for (int i = 0; i < 6; ++i) check(w.app->openSticky(notes[i], false) != nullptr, "fixed-state session opened");
    mappingSnapshot("fixed_after");
    for (int count = 1; count <= 12; ++count) {
        for (const QString &rel : w.app->openNotes()) w.app->closeNote(rel, true);
        pumpDeferredDeletes();
        for (int i = 0; i < count; ++i) check(w.app->openSticky(notes[i], false) != nullptr, "varying-state session opened");
    }
    mappingSnapshot("varying_after");
    metric("chaos.smaps.varying.max_sessions", w.app->openNotes().size());
    check(w.app->openNotes().size() <= kChaosMaxOpenSessions, "mapping workload respects 12-session cap");
    note("SMAPS snapshots are per mapping class; association is observational, not causal ownership proof");
}

// Short, fixed-cardinality retention experiment. All measurements happen after
// restoring the same 12 note sessions and geometry; this is diagnostic only.
void fixedStateMemory(int cycles) {
    World w;
    if (!w.pluginDestinationIsolated || !w.app) {
        check(false, "fixed-state experiment refuses unsafe or uninitialized plugin destination");
        return;
    }
    for (int i = 0; i < 12; ++i) w.create(i);
    w.app->start(hn::platform::Action::ShowOrganizer);
    const QStringList notes = w.shadow.keys();
    const QSize targetSize(480, 360);
    const auto restore = [&] {
        for (const QString &rel : w.app->openNotes())
            if (!notes.contains(rel)) w.app->closeNote(rel, true);
        for (const QString &rel : notes)
            if (!w.app->session(rel)) w.app->openSticky(rel, false);
        for (int i = 0; i < notes.size(); ++i) if (auto *s = w.app->session(notes[i])) {
            for (auto *win : w.app->stickies()) if (win->session() == s)
                win->setGeometry(20 + (i % 4) * 500, 20 + (i / 4) * 380, targetSize.width(), targetSize.height());
            s->flush();
        }
        for (int n = 0; n < 30; ++n) {
            pumpDeferredDeletes(10);
            bool settled = w.app->openNotes().size() == 12;
            for (const QString &rel : notes) {
                auto *s = w.app->session(rel);
                settled &= s && s->settled() && readFile(w.sb.notes + "/" + rel) == w.shadow.value(rel);
            }
            if (settled) break;
            usleep(10000);
        }
    };
    restore();
    // Discarded fixed-state warm-up: settle constructors, plugin hooks, and allocator paths.
    pumpDeferredDeletes(100);
    restore();
    note("WARMUP complete: discarded measurement after returning to fixed 12-session state");
    for (int i = 0; i < notes.size(); ++i) check(w.app->session(notes[i]) != nullptr, "fixed-state has all 12 sessions");
    auto sample = [&](int i) {
        restore();
        check(w.app->openNotes().size() == 12, QString("sample %1 returns to 12 sessions").arg(i));
        mappingSnapshot(QString("fixed_%1").arg(i));
        struct mallinfo2 h = mallinfo2();
        metric(qPrintable(QString("chaos.fixed.%1.widgets").arg(i)), QApplication::allWidgets().size(), "widgets");
        metric(qPrintable(QString("chaos.fixed.%1.fds").arg(i)), openFds(), "fds");
        metric(qPrintable(QString("chaos.fixed.%1.threads").arg(i)), threadCount(), "threads");
        metric(qPrintable(QString("chaos.fixed.%1.heap_inuse").arg(i)), (h.uordblks + h.hblkhd) / 1048576.0, "MiB");
    };
    for (int i = 0; i < 4; ++i) sample(i);
    QRandomGenerator rng(0x7a5eed);
    for (int cycle = 0; cycle < cycles; ++cycle) {
        QList<int> order; for (int i = 0; i < 12; ++i) order << i;
        for (int i = order.size() - 1; i > 0; --i) order.swapItemsAt(i, int(rng.bounded(quint32(i + 1))));
        const int n = 2 + int(rng.bounded(9u));
        for (int i = 0; i < n; ++i) w.app->closeNote(notes[order[i]], true);
        pumpDeferredDeletes(20);
        for (int i = 0; i < n; ++i) w.app->openSticky(notes[order[i]], false);
        const QString rel = notes[order[n - 1]];
        if (auto *s = w.app->session(rel)) {
            s->editor()->insertMarkdown(QStringLiteral("\ntransient churn edit\n"), false);
            s->flush();
            for (int wait = 0; wait < 30 && readFile(w.sb.notes + "/" + rel) == w.shadow.value(rel); ++wait) {
                pumpDeferredDeletes(10);
                usleep(10000);
                s->flush();
            }
            check(readFile(w.sb.notes + "/" + rel) != w.shadow.value(rel), "edit churn writes transient content");
            s->reload();
        }
        sample(4 + cycle);
    }
    note("fixed-state snapshots only; Qt build version must be recorded externally; no historical Qt 6.11 comparability implied");
}

void runChaos(int minutes, quint64 seed, bool quick, long maxOperations = 0) {
    World w;
    if (!w.pluginDestinationIsolated || !w.app) {
        check(false, "chaos refuses unsafe or uninitialized plugin destination");
        return;
    }
    for (int i = 0; i < kChaosMaxNotes; ++i) createBounded(w, i);
    w.app->start(hn::platform::Action::ShowOrganizer);
    Rng rng(seed);
    note(QString("CHAOS_SEED=%1 CHAOS_MIN=%2 mode=%3").arg(seed).arg(minutes)
         .arg(maxOperations > 0 ? "bounded-operations-smoke" : quick ? "quick-smoke" : "full"));
    if (maxOperations > 0) note("CHAOS_OPERATION_MASK restart action limited to 10 real save/restore cycles; additional restart selections become bounded create/retire operations");
    QElapsedTimer total; total.start();
    const int intervalMs = quick ? 1000 : 60000;
    const int targetMs = minutes * intervalMs;
    QList<double> pss;
    QStringList rels = w.shadow.keys();
    long ops = 0;
    std::array<quint64, 14> exercised{};
    if (quick) {
        const QString made = createBounded(w, 1000000);
        check(w.shadow.contains(made) && readFile(w.sb.notes + "/" + made) == w.shadow.value(made),
              "smoke forces note creation into shadow model and disk");
        w.app->openSticky(made, false);
        w.restart();
        verifyDisk(w, 0);
        for (auto it = w.officialPlugins.begin(); it != w.officialPlugins.end(); ++it) {
            const QString id = it.key(); QString error;
            const bool off = w.app->plugins().manager()->disable(id);
            const bool on = off && w.app->plugins().enable(id, nullptr, &error);
            check(off && on, "smoke toggled official plugin off then on: " + id + ": " + error);
            it.value() = on;
        }
    }
    auto settle = [&] {
        for (const QString &rel : w.app->openNotes()) if (auto *s = w.app->session(rel)) s->flush();
        const qint64 deadline = QDateTime::currentMSecsSinceEpoch() + 20000;
        while (QDateTime::currentMSecsSinceEpoch() < deadline) {
            pumpDeferredDeletes(20);
            bool done = true;
            for (const QString &rel : w.app->openNotes()) if (auto *s = w.app->session(rel)) done &= s->settled();
            if (done) break;
            usleep(1000);
        }
    };
    int minute = 0;
    while (maxOperations > 0 ? ops < maxOperations : total.elapsed() < targetMs) {
        ++ops;
        int action = rng.bounded(14);
        if (maxOperations > 0 && action == 13 && w.restartCount >= 10) action = 12;
        rels = w.shadow.keys();
        if (rels.isEmpty()) rels << createBounded(w, int(ops));
        const QString rel = rels[rng.bounded(rels.size())];
        if (action == 0) {
            if (openStickyBounded(w, rel)) ++exercised[action];
        }
        else if (action == 1 && w.app->session(rel)) { w.app->closeNote(rel); ++exercised[action]; }
        else if (action == 2 && !w.app->session(rel)) {
            if (openOrganizerBounded(w, rel)) ++exercised[action];
        }
        else if (action == 3) {
            const QByteArray external = "# External " + QByteArray::number(ops) + "\nexternal-version\n";
            bool wrote = false;
            if (auto *s = w.app->session(rel)) {
                s->editor()->insertMarkdown(QString("\nlocal-version-%1\n").arg(ops), false);
                wrote = writeFile(w.sb.notes + "/" + rel, external);
                QElapsedTimer conflictWait; conflictWait.start();
                while (conflictWait.elapsed() < 1000 && s->state() != NoteSession::State::Conflict)
                    pumpDeferredDeletes(20);
                check(s->state() == NoteSession::State::Conflict, QString("seed operation %1 observes external-write conflict").arg(ops));
                s->reload();
            } else wrote = writeFile(w.sb.notes + "/" + rel, external);
            check(wrote, QString("seed operation %1 wrote external version").arg(ops));
            if (wrote) { w.shadow[rel] = external; ++exercised[action]; }
        } else if (action == 4) {
            const QString parent = QFileInfo(rel).path() == "." ? QString() : QFileInfo(rel).path() + "/";
            const QString next = parent + QString("renamed-%1.md").arg(ops);
            QString err;
            if (w.app->renameNote(rel, QString("renamed-%1").arg(ops), &err)) {
                w.shadow[next] = w.shadow.take(rel);
                ++exercised[action];
                note(QString("SHADOW_RENAME op=%1 old=%2 new=%3 exists=%4").arg(ops).arg(rel, next).arg(QFileInfo::exists(w.sb.notes + "/" + next)));
                rels.replaceInStrings(rel, next);
            }
        } else if (action == 5 && w.shadow.size() > 15 && !w.app->session(rel)) {
            QString err;
            const bool removed = w.app->deleteNote(rel, &err);
            if (removed && !QFileInfo::exists(w.sb.notes + "/" + rel)) { w.shadow.remove(rel); ++exercised[action]; }
        } else if (action == 6) {
            auto settings = w.app->settings(); settings.colorScheme = settings.colorScheme == "dark" ? "light" : "dark";
            w.app->applySettings(settings, w.app->prefs());
            ++exercised[action];
        } else if (action == 7) {
            if (w.app->session(rel)) {
                const bool matched = editFromShadow(w, rel, ops);
                check(matched, QString("seed operation %1 edit export matches independently computed bytes").arg(ops));
                if (matched) ++exercised[action];
            }
        } else if (action == 8 && w.app->organizer()) {
            w.app->organizer()->searchEdit()->setText("Shadow"); w.app->organizer()->runSearchNow();
            ++exercised[action];
        } else if (action == 9) {
            if (w.app->stickyOf(rel)) { if (w.app->popIn(rel)) ++exercised[action]; }
            else if (w.app->session(rel)) { if (w.app->popOut(rel)) ++exercised[action]; }
            else if (openStickyBounded(w, rel)) ++exercised[action];
        } else if (action == 10 && !w.officialPlugins.isEmpty()) {
            const QStringList ids = w.officialPlugins.keys();
            const QString id = ids[rng.bounded(ids.size())];
            QString error;
            const bool next = !w.officialPlugins.value(id);
            const bool ok = next ? w.app->plugins().enable(id, nullptr, &error)
                                 : w.app->plugins().manager()->disable(id);
            check(ok, QString("seed operation %1 toggles official plugin %2=%3: %4").arg(ops).arg(id).arg(next).arg(error));
            if (ok) { w.officialPlugins[id] = next; ++exercised[action]; }
        } else if (action == 11 && !w.app->session(rel)) {
            const QString moved = QString("moved/n%1-%2.md").arg(ops).arg(QFileInfo(rel).completeBaseName());
            QString error;
            if (w.app->repo().rename(rel, moved, &error)) {
                w.shadow[moved] = w.shadow.take(rel);
                ++exercised[action];
                note(QString("SHADOW_MOVE op=%1 old=%2 new=%3 exists=%4").arg(ops).arg(rel, moved).arg(QFileInfo::exists(w.sb.notes + "/" + moved)));
            }
        } else if (action == 12) {
            const QString created = createBounded(w, int(ops + 1000000));
            check(!created.isEmpty() && w.shadow.contains(created) && readFile(w.sb.notes + "/" + created) == w.shadow.value(created),
                  QString("seed operation %1 creates note represented in shadow model").arg(ops));
            if (!created.isEmpty()) ++exercised[action];
        } else if (action == 13) {
            settle();
            if (w.restart()) ++exercised[action];
            verifyDisk(w, minute);
        }
        pumpDeferredDeletes();
        if (total.elapsed() / intervalMs > minute) {
            minute = int(total.elapsed() / intervalMs);
            settle(); verifyDisk(w, minute);
            const Mem m = mem(); pss << m.total();
            reportResources(w, minute, ops);
            if (!quick && minute > 10 && pss.size() >= 11) {
                const double hours = (minute - 10) / 60.0;
                const double slope = hours > 0 ? (pss.last() - pss[9]) / hours : 0;
                metric("chaos.pss_slope_after_warmup", slope, "MiB/hour");
                check(slope <= 1.0, QString("minute %1: PSS slope %2 MiB/hour <= 1 MiB/hour").arg(minute).arg(slope));
            }
            if (!quick && minute % 30 == 0) {
                w.app->requestQuit();
                for (int i = 0; i < 100 && !w.app->exiting(); ++i) pumpDeferredDeletes(20);
                QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
                const bool saved = QFileInfo::exists(w.sb.state + "/session.json");
                w.app.reset();
                w.app = std::make_unique<AppController>(w.opt);
                w.app->start(hn::platform::Action::ShowOrganizer);
                ++w.restartCount;
                check(saved, QString("minute %1: session state saved before process restart").arg(minute));
                const int restoredWindows = w.app->stickies().size() + (w.app->organizer() && w.app->organizer()->currentSession() ? 1 : 0);
                check(w.app->openNotes().size() == restoredWindows,
                      QString("minute %1: restored sessions equal note windows").arg(minute));
                check(w.app->openNotes().size() <= kChaosMaxOpenSessions,
                      QString("minute %1: restored sessions stay within chaos cap").arg(minute));
            }
        }
    }
    settle(); verifyDisk(w, minute + 1);
    metric("chaos.operations", double(ops));
    metric("chaos.note_files_final", markdownFileCount(w));
    metric("chaos.shadow_notes_final", w.shadow.size());
    metric("chaos.open_sessions_final", w.app->openNotes().size());
    metric("chaos.cap_retirements", w.capRetirements);
    if (maxOperations > 0) {
        metric("chaos.operations_smoke.restart_mask_max", 10);
        check(w.restartCount <= 10, "bounded smoke applies restart operation mask");
    }
    for (size_t i = 0; i < exercised.size(); ++i) {
        metric(qPrintable(QString("chaos.operation%1.count").arg(i)), double(exercised[i]));
        if (maxOperations > 0)
            check(exercised[i] > 0, QString("bounded smoke exercised operation action %1").arg(i));
    }
    if (maxOperations > 0) {
        check(markdownFileCount(w) == w.shadow.size() && w.shadow.size() <= kChaosMaxNotes,
              "bounded smoke actual files and shadow stay within cap");
        check(w.app->openNotes().size() <= kChaosMaxOpenSessions,
              "bounded smoke restored/open notes stay within cap");
    }
    for (const QString &rel : w.app->openNotes()) w.app->closeNote(rel, true);
    pumpDeferredDeletes();
    if (!quick) {
        const auto threadCpuBefore = threadCpuSeconds();
        const double cpu0 = cpuSeconds(); QElapsedTimer wall; wall.start();
        QEventLoop loop; QTimer::singleShot(60000, &loop, &QEventLoop::quit); loop.exec();
        const qint64 wallMs = wall.elapsed();
        const double cpu = (cpuSeconds() - cpu0) / (wallMs / 1000.0) * 100.0;
        const auto threadCpuAfter = threadCpuSeconds();
        QStringList matched, newIdentities, exitedIdentities;
        for (auto it = threadCpuAfter.cbegin(); it != threadCpuAfter.cend(); ++it) {
            const auto before = threadCpuBefore.constFind(it.key());
            if (before == threadCpuBefore.cend() || before->identity != it->identity) {
                newIdentities << it->identity;
                continue;
            }
            matched << it->identity;
            metric(qPrintable(QString("chaos.quiescent_thread.%1_cpu").arg(it->identity)),
                   (it->cpuSeconds - before->cpuSeconds) / (wallMs / 1000.0) * 100.0,
                   "percent-of-one-core");
        }
        for (auto it = threadCpuBefore.cbegin(); it != threadCpuBefore.cend(); ++it) {
            const auto after = threadCpuAfter.constFind(it.key());
            if (after == threadCpuAfter.cend() || after->identity != it->identity)
                exitedIdentities << it->identity;
        }
        note(QString("QUIESCENT_THREAD_IDENTITIES matched=%1 new=%2 exited=%3").arg(matched.join(',')).arg(newIdentities.join(',')).arg(exitedIdentities.join(',')));
        metric("chaos.quiescent_thread_sample_set_changed",
               newIdentities.isEmpty() && exitedIdentities.isEmpty() ? 0 : 1);
        metric("chaos.quiescent_wall_ms", wallMs, "ms");
        metric("chaos.quiescent_wall_resolution_ms", 1, "ms");
        metric("chaos.quiescent_idle_cpu", cpu, "percent");
        check(cpu <= 0.0, QString("quiescent idle CPU exactly 0.0%% (measured %1%%)").arg(cpu));
    }
}

void deferredDeleteSmoke(int cycles) {
    World w;
    if (!w.pluginDestinationIsolated || !w.app) {
        check(false, "deferred-delete smoke refuses unsafe plugin destination");
        return;
    }
    w.app->start(hn::platform::Action::ShowOrganizer);
    const QString rel = w.create(424242);
    pumpDeferredDeletes();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    const int baselineWidgets = QApplication::allWidgets().size();
    const Mem before = mem();
    const struct mallinfo2 heapBefore = mallinfo2();
    for (int i = 0; i < cycles; ++i) {
        check(w.app->openSticky(rel, false) != nullptr, QString("cycle %1 opens note").arg(i));
        w.app->closeNote(rel, true);
        pumpDeferredDeletes();
    }
    const int pendingWidgets = QApplication::allWidgets().size();
    const Mem pending = mem();
    const struct mallinfo2 heapPending = mallinfo2();
    metric("chaos.deferred_delete.cycles", cycles);
    metric("chaos.deferred_delete.widgets_before", baselineWidgets);
    metric("chaos.deferred_delete.widgets_pending", pendingWidgets);
    metric("chaos.deferred_delete.widgets_delta_pending", pendingWidgets - baselineWidgets);
    metric("chaos.deferred_delete.pss_delta_pending", pending.total() - before.total(), "MiB");
    metric("chaos.deferred_delete.heap_delta_pending",
           ((heapPending.uordblks + heapPending.hblkhd) - (heapBefore.uordblks + heapBefore.hblkhd)) / 1048576.0, "MiB");
    check(w.app->openNotes().isEmpty(), "all diagnostic sessions released synchronously");
    check(pendingWidgets == baselineWidgets,
          "processEvents-only chaos pump reclaims deferred-delete windows immediately");
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    const int drainedWidgets = QApplication::allWidgets().size();
    const Mem drained = mem();
    const struct mallinfo2 heapDrained = mallinfo2();
    metric("chaos.deferred_delete.widgets_drained", drainedWidgets);
    metric("chaos.deferred_delete.pss_delta_drained", drained.total() - before.total(), "MiB");
    metric("chaos.deferred_delete.heap_delta_drained",
           ((heapDrained.uordblks + heapDrained.hblkhd) - (heapBefore.uordblks + heapBefore.hblkhd)) / 1048576.0, "MiB");
    check(drainedWidgets == baselineWidgets, "explicit deferred-delete drain restores widget baseline");
}

void boundedWorldSmoke() {
    World w;
    if (!w.pluginDestinationIsolated || !w.app) {
        check(false, "bounded-world smoke refuses unsafe plugin destination");
        return;
    }
    for (int i = 0; i < 160; ++i) createBounded(w, 2000000 + i);
    w.app->start(hn::platform::Action::ShowOrganizer);
    const QStringList candidates = w.shadow.keys();
    Rng rng(202610081337);
    QSet<QString> seen;
    QStringList chosen;
    while (chosen.size() < 20) {
        const QString rel = candidates[rng.bounded(candidates.size())];
        if (!seen.contains(rel)) { seen.insert(rel); chosen << rel; }
    }
    int opened = 0;
    for (const QString &rel : chosen) if (openStickyBounded(w, rel)) ++opened;
    QString closedCandidate;
    for (const QString &rel : candidates) if (!w.app->session(rel)) { closedCandidate = rel; break; }
    const NoteSession *organizerOpen = closedCandidate.isEmpty() ? nullptr : openOrganizerBounded(w, closedCandidate);
    pumpDeferredDeletes();
    metric("chaos.bounded_smoke.create_attempts", 160);
    metric("chaos.bounded_smoke.open_attempts", 21);
    metric("chaos.bounded_smoke.note_files", markdownFileCount(w));
    metric("chaos.bounded_smoke.shadow_notes", w.shadow.size());
    metric("chaos.bounded_smoke.open_sessions", w.app->openNotes().size());
    metric("chaos.bounded_smoke.cap_retirements", w.capRetirements);
    check(opened == kChaosMaxOpenSessions, "sticky route admitted exactly the session cap");
    check(organizerOpen == nullptr, "organizer route refuses an additional closed note at session cap");
    check(markdownFileCount(w) <= kChaosMaxNotes && w.shadow.size() <= kChaosMaxNotes,
          "fixture population and shadow remain within stationary note cap");
    check(w.app->openNotes().size() <= kChaosMaxOpenSessions,
          "sticky and organizer open routes respect shared session cap");
    verifyDisk(w, 0);
}

void shadowOracleSmoke() {
    World w;
    if (!w.pluginDestinationIsolated || !w.app) {
        check(false, "shadow-oracle smoke refuses unsafe plugin destination");
        return;
    }
    w.app->start(hn::platform::Action::ShowOrganizer);
    const QString rel = w.create(515151);
    const QByteArray priorShadow = w.shadow.value(rel);
    const QByteArray expected = priorShadow + "<!-- chaos-edit-17 -->\n";
    check(w.app->openSticky(rel, false) != nullptr, "open fixture in actual NoteEditor");
    const bool accepted = editFromShadow(w, rel, 17, true);
    check(!accepted, "editor serialization corruption is rejected by independent expected bytes");
    check(w.shadow.value(rel) != expected, "shadow oracle must not accept corrupted serialized output");
    check(w.shadow.value(rel) == priorShadow, "rejected edit preserves prior independent shadow");
    check(w.shadow.value(rel) == readFile(w.sb.notes + "/" + rel), "rejected edit leaves disk at prior shadow bytes");
    const QByteArray injected = w.app->session(rel)->editor()->toMarkdownBytes();
    check(injected != expected, "real NoteEditor contains injected corruption for red/green proof");
    w.app->closeNote(rel, true);
    pumpDeferredDeletes();
}
}

int main(int argc, char **argv) {
    const QString scratch = QDir::tempPath() + "/hyprnotes-chaos-" + QString::number(getpid());
    QDir().mkpath(scratch + "/home");
    qputenv("HOME", (scratch + "/home").toUtf8());
    qputenv("XDG_CONFIG_HOME", (scratch + "/xdg-config").toUtf8());
    qputenv("XDG_DATA_HOME", (scratch + "/xdg-data").toUtf8());
    qputenv("XDG_STATE_HOME", (scratch + "/xdg-state").toUtf8());
    qputenv("XDG_CACHE_HOME", (scratch + "/xdg-cache").toUtf8());
    qputenv("HN_CONFIG_DIR", (scratch + "/config").toUtf8());
    qputenv("HN_DATA_DIR", (scratch + "/data").toUtf8());
    qputenv("HN_STATE_DIR", (scratch + "/state").toUtf8());
    qputenv("HN_CACHE_DIR", (scratch + "/cache").toUtf8());
    qputenv("HN_NOTES_DIR", (scratch + "/notes").toUtf8());
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.contains("--thread-stat-parser-smoke")) {
        threadStatParserSmoke();
        const int result = finish("stress_chaos_thread_stat_parser");
        QDir(scratch).removeRecursively();
        return result;
    }
    if (args.contains("--idle-timer-control")) {
        bool ok = false;
        const int seconds = args.value(args.indexOf("--idle-timer-control") + 1, "60").toInt(&ok);
        if (!ok || seconds < 1) { std::fprintf(stderr, "usage: stress_chaos --idle-timer-control SECONDS\n"); return 2; }
        idleTimerControl(seconds);
        const int result = finish("stress_chaos_idle_timer_control");
        QDir(scratch).removeRecursively();
        return result;
    }
    if (args.contains("--mapping-state-smoke")) {
        mappingStateSmoke();
        const int result = finish("stress_chaos_mapping_state");
        QDir(scratch).removeRecursively();
        return result;
    }
    if (args.contains("--fixed-state-memory")) {
        bool ok = false; const int cycles = args.value(args.indexOf("--fixed-state-memory") + 1).toInt(&ok);
        if (!ok || cycles < 1 || cycles > 8) { std::fprintf(stderr, "usage: stress_chaos --fixed-state-memory CYCLES(1..8)\n"); return 2; }
        fixedStateMemory(cycles);
        const int result = finish("stress_chaos_fixed_state_memory");
        return result;
    }
    if (args.contains("--deferred-delete-smoke")) {
        bool ok = false;
        const int cycles = args.value(args.indexOf("--deferred-delete-smoke") + 1, "25").toInt(&ok);
        if (!ok || cycles < 1) { std::fprintf(stderr, "usage: stress_chaos --deferred-delete-smoke CYCLES\n"); return 2; }
        deferredDeleteSmoke(cycles);
        const int result = finish("stress_chaos_deferred_delete");
        QDir(scratch).removeRecursively();
        return result;
    }
    if (args.contains("--bounded-world-smoke")) {
        boundedWorldSmoke();
        const int result = finish("stress_chaos_bounded_world");
        QDir(scratch).removeRecursively();
        return result;
    }
    if (args.contains("--shadow-oracle-smoke")) {
        shadowOracleSmoke();
        const int result = finish("stress_chaos_shadow_oracle");
        QDir(scratch).removeRecursively();
        return result;
    }
    if (args.contains("--operations-smoke")) {
        bool ok = false;
        const long operations = args.value(args.indexOf("--operations-smoke") + 1, "5000").toLong(&ok);
        const quint64 seed = qEnvironmentVariable("CHAOS_SEED", "202610081337").toULongLong();
        if (!ok || operations < 1) { std::fprintf(stderr, "usage: stress_chaos --operations-smoke COUNT\n"); return 2; }
        runChaos(1, seed, true, operations);
        const int result = finish("stress_chaos_operations_smoke");
        QDir(scratch).removeRecursively();
        return result;
    }
    if (args.contains("--plugin-isolation-smoke")) {
        const QString root = qEnvironmentVariable("HN_PLUGINS_DIR", QStringLiteral(HN_PLUGINS_DIR));
        const QStringList ids{"slash-menu", "journal", "tasks", "wiki-links"};
        QMap<QString, QString> before;
        for (const QString &id : ids) before[id] = hn::plugins::hashDirectory(root + "/" + id);
        World w;
        check(w.pluginDestinationIsolated, "plugin installation destination is isolated under scratch data directory");
        check(w.officialPlugins.size() == ids.size(), "all four official plugins installed and enabled in scratch");
        for (const QString &id : ids) {
            check(QFileInfo::exists(w.opt.pluginsDir + "/" + id + "/plugin.json"), "scratch plugin payload exists: " + id);
            check(before.value(id) == hn::plugins::hashDirectory(root + "/" + id), "source plugin tree hash unchanged: " + id);
        }
        if (w.app && w.officialPlugins.size() == ids.size()) {
            auto *manager = w.app->plugins().manager();
            for (const QString &id : ids) {
                const bool disabled = manager->disable(id);
                QString error;
                const bool reenabled = disabled && w.app->plugins().enable(id, nullptr, &error);
                check(disabled && reenabled, "official plugin toggles off/on in scratch: " + id + ": " + error);
                check(before.value(id) == hn::plugins::hashDirectory(root + "/" + id), "source hash stable after toggle: " + id);
            }
        }
        const int result = finish("stress_chaos_plugin_isolation");
        QDir(QDir::tempPath() + "/hyprnotes-chaos-" + QString::number(getpid())).removeRecursively();
        return result;
    }
    bool ok = false;
    const int minutes = args.value(1, "120").toInt(&ok);
    const quint64 seed = qEnvironmentVariable("CHAOS_SEED", "202610081337").toULongLong();
    if (!ok || minutes < 1) { std::fprintf(stderr, "usage: stress_chaos MINUTES\n"); return 2; }
    runChaos(minutes, seed, args.contains("--quick"));
    const int result = finish("stress_chaos");
    QDir(scratch).removeRecursively();
    return result;
}
