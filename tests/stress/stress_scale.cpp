#include "controller.h"
#include "hn/core/library_index.h"
#include "hn/editor/note_editor.h"
#include "hn/plugins/runtime.h"
#include "sticky_window.h"
#include "stress_util.h"
#include <QApplication>
#include <QDirIterator>
#include <QEventLoop>
#include <QProcess>
#include <QTextBlock>
#include <QTest>
#include <algorithm>
#include <sys/types.h>
#include <malloc.h>
#include <unistd.h>
#if __has_include(<valgrind/callgrind.h>)
#include <valgrind/callgrind.h>
#else
#define CALLGRIND_ZERO_STATS do {} while (false)
#define CALLGRIND_START_INSTRUMENTATION do {} while (false)
#define CALLGRIND_STOP_INSTRUMENTATION do {} while (false)
#endif

using namespace st;
using namespace hn::app;

namespace {
QString scratchRoot;
template <typename T> int widgetCount() {
    int n = 0;
    for (QWidget *w : QApplication::allWidgets()) if (qobject_cast<T *>(w)) ++n;
    return n;
}
void isolateProcessHome(const QString &root) {
    scratchRoot = root;
    QDir().mkpath(root + "/home");
    qputenv("HOME", (root + "/home").toUtf8());
    qputenv("XDG_CONFIG_HOME", (root + "/xdg-config").toUtf8());
    qputenv("XDG_DATA_HOME", (root + "/xdg-data").toUtf8());
    qputenv("XDG_STATE_HOME", (root + "/xdg-state").toUtf8());
    qputenv("XDG_CACHE_HOME", (root + "/xdg-cache").toUtf8());
    qputenv("HN_CONFIG_DIR", (root + "/config").toUtf8());
    qputenv("HN_DATA_DIR", (root + "/data").toUtf8());
    qputenv("HN_STATE_DIR", (root + "/state").toUtf8());
    qputenv("HN_CACHE_DIR", (root + "/cache").toUtf8());
    qputenv("HN_NOTES_DIR", (root + "/notes").toUtf8());
    QDir().mkpath(root + "/notes");
}
bool waitForIndex(hn::core::LibraryIndex &idx, int count, int timeoutMs = 3600000) {
    bool done = false;
    QObject::connect(&idx, &hn::core::LibraryIndex::synced, &idx,
        [&](quint64, int, int, int) { done = true; }, Qt::SingleShotConnection);
    idx.sync();
    QElapsedTimer t; t.start();
    while (!done && t.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        usleep(1000);
    }
    return done && idx.count() == count;
}
int scaleFixtureSize(int i) { return 1024 + (i * 7919) % (19 * 1024 + 1); }
void writeLibrary(const QString &root, int count, int size, bool varySize = false) {
    for (int i = 0; i < count; ++i) {
        const int target = varySize ? scaleFixtureSize(i) : size;
        QByteArray body = "---\ntitle: Scale note " + QByteArray::number(i)
            + "\ntags: [scale, batch" + QByteArray::number(i % 97) + "]\n---\n"
            + "# Note " + QByteArray::number(i) + " uniq" + QByteArray::number(i)
            + "\n[[Note " + QByteArray::number((i + 1) % count) + "]]\n\n";
        if (body.size() < target) body += mixedNote(target - body.size(), i);
        if (body.size() < target) body += QByteArray(target - body.size(), 'x');
        body.truncate(target);
        writeFile(root + QString("/d%1/n%2.md").arg(i % 100).arg(i, 6, 10, QChar('0')), body);
    }
}
void waitForFirstRow(OrganizerWindow *organizer, int timeoutMs = 3600000) {
    QElapsedTimer timer; timer.start();
    while (organizer->model()->rowCount() == 0 && timer.elapsed() < timeoutMs)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    check(organizer->model()->rowCount() > 0, "organizer produced first actual library row");
}
bool repositorySave(Sandbox &box, const QString &name, const QByteArray &bytes, double *elapsedMs) {
    hn::core::NoteRepository repo(box.notes, box.state + "/recovery");
    const QString rel = name + ".md";
    bool saved = false, failed = false;
    QObject::connect(&repo, &hn::core::NoteRepository::saved, &repo,
        [&](const QString &r, quint64) { if (r == rel) saved = true; });
    QObject::connect(&repo, &hn::core::NoteRepository::saveFailed, &repo,
        [&](const QString &r, const QString &, quint64) { if (r == rel) failed = true; });
    Stopwatch sw; repo.save(rel, bytes, 0, true);
    QElapsedTimer wait; wait.start();
    while (!saved && !failed && wait.elapsed() < 30000) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    *elapsedMs = sw.ms();
    return saved && !failed && readFile(repo.root() + "/" + rel) == bytes;
}
void scaleStartup(const QString &notes, const QString &state, const QString &cache,
                  const QString &config, const QString &data, int libraryCount, bool quick) {
    Dist cold, warm;
    const int runs = quick ? 1 : 20;
    note("startup definition: cold = fresh controller with app index cache deleted; warm = fresh controller with same index cache retained; OS page cache is never cleared");
    for (int i = 0; i < runs; ++i) {
        if (!quick) QDir(cache).removeRecursively(); // app index cache only; never drop host page cache
        QDir().mkpath(cache);
        ControllerOptions opt;
        opt.notesDir = notes; opt.stateDir = state; opt.cacheDir = cache;
        opt.configPath = config + "/config.json"; opt.modsDir = data + "/mods";
        opt.useTray = false; opt.useHyprland = false;
        Stopwatch sw;
        AppController c(opt);
        c.showOrganizer(false);
        waitForFirstRow(c.organizer());
        cold.add(sw.ms());
    }
    if (quick) { // one cold sample still exercises path; don't claim cold/warm distribution in smoke
        cold.report(QString("scale.startup.library%1.cold_smoke").arg(libraryCount));
        skip("quick smoke has one cold startup sample; full mode requires 20 cold and 20 warm runs");
        return;
    }
    // Cold cache is cleared for each cold launch. Warm launches retain app index cache and OS page cache.
    for (int i = 0; i < 20; ++i) {
        ControllerOptions opt;
        opt.notesDir = notes; opt.stateDir = state; opt.cacheDir = cache;
        opt.configPath = config + "/config.json"; opt.modsDir = data + "/mods";
        opt.useTray = false; opt.useHyprland = false;
        Stopwatch sw;
        AppController c(opt); c.showOrganizer(false); waitForFirstRow(c.organizer());
        warm.add(sw.ms());
    }
    cold.report(QString("scale.startup.library%1.cold").arg(libraryCount));
    warm.report(QString("scale.startup.library%1.warm").arg(libraryCount));
    check(cold.n() == 20 && warm.n() == 20,
          QString("%1-note library startup has 20 cold and 20 warm observations").arg(libraryCount));
}
void scaleLibrary(int count, bool quick) {
    note(QString("== library scale %1 notes, deterministic scratch fixtures").arg(count));
    Sandbox sb("scale-library");
    const int each = quick ? 1024 : 20 * 1024;
    Stopwatch gen; writeLibrary(sb.notes, count, each, true);
    metric("scale.library.fixture_generation", gen.ms(), "ms");
    int minBytes = INT_MAX, maxBytes = 0, fixtureCount = 0;
    QDirIterator fixtureFiles(sb.notes, {"*.md"}, QDir::Files, QDirIterator::Subdirectories);
    while (fixtureFiles.hasNext()) {
        fixtureFiles.next(); const int bytes = fixtureFiles.fileInfo().size();
        minBytes = qMin(minBytes, bytes); maxBytes = qMax(maxBytes, bytes); ++fixtureCount;
    }
    metric("scale.library.fixture_min_sample_bytes", minBytes, "bytes");
    metric("scale.library.fixture_max_sample_bytes", maxBytes, "bytes");
    check(fixtureCount == count && minBytes >= 1024 && maxBytes <= 20 * 1024 && minBytes < maxBytes,
          QString("all %1 deterministic library fixture sizes vary within 1–20 KiB (%2..%3 bytes)").arg(count).arg(minBytes).arg(maxBytes));
    scaleStartup(sb.notes, sb.state, sb.cache, sb.config, sb.data, count, quick);
    const Mem before = mem();
    hn::core::LibraryIndex idx(sb.notes, sb.cache);
    Stopwatch build;
    check(waitForIndex(idx, count), QString("library indexed all %1 notes").arg(count));
    metric("scale.library.index_build", build.ms(), "ms");
    metric("scale.library.index_pss", mem().total(), "MiB");
    metric("scale.library.index_pss_delta", mem().total() - before.total(), "MiB");
    Dist d[10];
    const QStringList queries{"uniq42", "Scale note", "batch17", "[[Note", "nonexistent-token", "title:scale", "note", "#scale", "d42", "uniq"};
    const int reps = quick ? 1 : 20;
    for (int r = 0; r < reps; ++r)
        for (int i = 0; i < queries.size(); ++i) {
            Stopwatch sw; const auto result = idx.searchNow({queries[i], {}, {}, 50, 0});
            d[i].add(sw.ms());
            check(true, QString("scale query completed with %1 rows").arg(result.rows.size()));
        }
    for (int i = 0; i < queries.size(); ++i) d[i].report(QString("scale.library.query%1").arg(i));
    Dist mutation;
    const int mutations = quick ? 10 : 100;
    for (int i = 0; i < mutations; ++i) {
        const QString oldRel = QString("d%1/n%2.md").arg(i % 100).arg(i, 6, 10, QChar('0'));
        const QString newRel = oldRel + ".renamed";
        Stopwatch sw;
        QFile::rename(sb.notes + "/" + oldRel, sb.notes + "/" + newRel);
        QFile::remove(sb.notes + "/" + newRel);
        writeFile(sb.notes + "/" + oldRel, mixedNote(1024, i + 500000));
        idx.updatePath(oldRel);
        mutation.add(sw.ms());
    }
    mutation.report("scale.library.rename_delete_create");
    const int burst = quick ? 100 : 10000;
    hn::core::NoteRepository repo(sb.notes, sb.state + "/recovery");
    repo.watchLibrary(true);
    int watcherSignals = 0;
    QObject::connect(&repo, &hn::core::NoteRepository::libraryChanged, &repo, [&] { ++watcherSignals; });
    Stopwatch burstSw;
    for (int i = 0; i < burst; ++i)
        writeFile(sb.notes + QString("/external-%1.md").arg(i), "external change\n");
    metric("scale.library.watcher_burst_write", burstSw.ms(), "ms");
    metric("scale.library.watcher_burst_items", burst);
    check(QDir(sb.notes).entryList({"external-*.md"}, QDir::Files).size() == burst,
          "external watcher burst fixtures all materialized");
    QElapsedTimer watcherWait; watcherWait.start();
    while (watcherWait.elapsed() < 30000 && watcherSignals == 0) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    metric("scale.library.watcher_signals", watcherSignals);
    check(watcherSignals > 0, "library watcher observed external burst without polling");
}

void stickyScale(bool quick, int onlyCount = 0) {
    const QList<int> counts = onlyCount ? QList<int>{onlyCount}
        : quick ? QList<int>{1, 10} : QList<int>{10, 50, 100, 200};
    for (int n : counts) {
        note(QString("== sticky scale %1 windows").arg(n));
        Sandbox sb(QString("scale-windows-%1").arg(n));
        ControllerOptions opt;
        opt.notesDir = sb.notes; opt.stateDir = sb.state; opt.cacheDir = sb.cache;
        opt.configPath = sb.config + "/config.json"; opt.modsDir = sb.data + "/mods";
        opt.modsEnabledPath = sb.config + "/mods-enabled.json";
        opt.useTray = false; opt.useHyprland = false;
        opt.confirm = [](const QString &, const QString &) { return true; };
        for (int i = 0; i < n; ++i) writeFile(sb.notes + QString("/n%1.md").arg(i), mixedNote(1024, i));
        AppController c(opt);
        Dist opens, closes, cycleOpens, cycleCloses;
        for (int i = 0; i < n; ++i) { Stopwatch one; c.openSticky(QString("n%1.md").arg(i), false); opens.add(one.ms()); }
        QCoreApplication::processEvents();
        opens.report(QString("scale.windows.%1.open").arg(n));
        metric(qPrintable(QString("scale.windows.%1.pss_total").arg(n)), mem().total(), "MiB");
        metric(qPrintable(QString("scale.windows.%1.pss_per_window").arg(n)), mem().total() / n, "MiB");
        check(c.stickies().size() == n && c.windowCount() == n,
              QString("%1 sticky windows opened and tracked").arg(n));
        const int cycles = quick ? 20 : 10000;
        const int warmupCycles = qMin(cycles / 2, qMax(n, cycles / 4));
        int baselineWidgets = -1;
        QList<double> curve;
        Dist life;
        for (int i = 0; i < cycles; ++i) {
            const QString rel = QString("cycle-%1.md").arg(i % n);
            if (!QFile::exists(sb.notes + "/" + rel)) writeFile(sb.notes + "/" + rel, "cycle\n");
            Stopwatch sw;
            auto *s = c.openSticky(rel, false);
            cycleOpens.add(sw.ms());
            sw.reset(); if (s) c.closeNote(rel); cycleCloses.add(sw.ms());
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            if (i + 1 == warmupCycles) baselineWidgets = QApplication::allWidgets().size();
            if (i >= warmupCycles && (i - warmupCycles) % qMax(1, (cycles - warmupCycles) / 15) == 0) {
                const int point = curve.size();
                const double pss = mem().total();
                curve << pss;
                const struct mallinfo2 mi = mallinfo2();
                metric(qPrintable(QString("scale.windows.%1.cycle_pss_point%2").arg(n).arg(point)), pss, "MiB");
                metric(qPrintable(QString("scale.windows.%1.cycle_heap_point%2").arg(n).arg(point)),
                       (mi.uordblks + mi.hblkhd) / 1048576.0, "MiB");
                metric(qPrintable(QString("scale.windows.%1.cycle_widgets_point%2").arg(n).arg(point)), QApplication::allWidgets().size());
                metric(qPrintable(QString("scale.windows.%1.cycle_editors_point%2").arg(n).arg(point)), widgetCount<hn::editor::NoteEditor>());
                metric(qPrintable(QString("scale.windows.%1.cycle_stickies_point%2").arg(n).arg(point)), widgetCount<StickyWindow>());
                check(c.openNotes().size() == n, QString("%1-window cycle keeps original sessions live").arg(n));
                check(QApplication::allWidgets().size() == baselineWidgets,
                      QString("%1-window cycle leaves no widget objects retained (%2 vs %3)").arg(n).arg(QApplication::allWidgets().size()).arg(baselineWidgets));
            }
        }
        cycleOpens.report(QString("scale.windows.%1.cycle_open").arg(n));
        cycleCloses.report(QString("scale.windows.%1.cycle_close").arg(n));
        bool flat = true;
        if (curve.size() > 1) {
            const auto [lo, hi] = std::minmax_element(curve.cbegin(), curve.cend());
            flat = *hi - *lo <= 2.0;
            metric(qPrintable(QString("scale.windows.%1.cycle_pss_range").arg(n)), *hi - *lo, "MiB");
        }
        check(flat, QString("%1-window open/close PSS curve flat after warm-up").arg(n));
        for (const QString &rel : c.openNotes()) { Stopwatch one; c.closeNote(rel, true); closes.add(one.ms()); }
        closes.report(QString("scale.windows.%1.close").arg(n));
        check(c.openNotes().isEmpty(), "all scaled windows closed");
    }
}

void bigNoteCases(bool quick) {
    const QList<int> mibs = quick ? QList<int>{1} : QList<int>{1, 5, 20};
    for (int mib : mibs) {
        const QByteArray bytes = mixedNote(mib * 1024 * 1024, mib);
        hn::editor::NoteEditor ed; ed.resize(800, 600); ed.show();
        QElapsedTimer eventGap; qint64 lastTick = 0, maxGap = 0; int heartbeatTicks = 0;
        QTimer heartbeat; heartbeat.setInterval(1);
        QObject::connect(&heartbeat, &QTimer::timeout, [&] { const qint64 now = eventGap.elapsed(); if (lastTick) maxGap = qMax(maxGap, now - lastTick); lastTick = now; ++heartbeatTicks; });
        eventGap.start(); heartbeat.start();
        QElapsedTimer ready; ready.start();
        while (!heartbeatTicks && ready.elapsed() < 100) { QCoreApplication::processEvents(QEventLoop::AllEvents, 5); usleep(100); }
        lastTick = eventGap.elapsed(); const int ticksBeforeLoad = heartbeatTicks;
        Stopwatch sw; ed.load(bytes); const double loadMs = sw.ms();
        QElapsedTimer resume; resume.start();
        while (heartbeatTicks == ticksBeforeLoad && resume.elapsed() < 200) QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        heartbeat.stop();
        const bool heartbeatObserved = ticksBeforeLoad > 0 && heartbeatTicks > ticksBeforeLoad;
        metric(qPrintable(QString("scale.big.%1MiB.load_block").arg(mib)), loadMs, "ms");
        metric(qPrintable(QString("scale.big.%1MiB.event_loop_stall_max").arg(mib)), maxGap, "ms");
        metric(qPrintable(QString("scale.big.%1MiB.mode_source").arg(mib)), ed.mode() == hn::editor::Mode::Source ? 1 : 0);
        check(heartbeatObserved, QString("%1MiB live event-loop heartbeat sampled across load").arg(mib));
        check(heartbeatObserved && (maxGap < 100 || ed.mode() == hn::editor::Mode::Source),
              QString("%1MiB load event loop under 100ms or actual source-mode fallback").arg(mib));
        metric(qPrintable(QString("scale.big.%1MiB.pss").arg(mib)), mem().total(), "MiB");
        if (ed.activeEdit()) { ed.setFocus(); QTest::keyClick(ed.activeEdit(), Qt::Key_X); QCoreApplication::processEvents(); }
        const QByteArray serialized = ed.toMarkdownBytes();
        check(serialized.size() >= bytes.size(), QString("%1MiB document retained through serialization").arg(mib));
        Sandbox saveBox("scale-big-save-" + QString::number(mib)); double saveMs = 0;
        const bool saved = repositorySave(saveBox, "big", serialized, &saveMs);
        metric(qPrintable(QString("scale.big.%1MiB.save").arg(mib)), saveMs, "ms");
        check(saved, QString("%1MiB actual repository save persisted serialized bytes").arg(mib));
    }
    QList<QByteArray> pathological{
        QByteArray(100000, 'x') + "\n",
        QByteArray(1000000, 'x') + "\n",
        QByteArray(100000, 'x') + QByteArray(100000, '\n')
    };
    if (quick) pathological = {pathological[0], pathological[2]};
    QByteArray table; while (table.size() < 10 * 1024 * 1024) table += "| cell text | more cell text |\n| --- | --- |\n";
    table.truncate(10 * 1024 * 1024);
    QByteArray nested; for (int i = 0; i < 5000; ++i) nested += QByteArray(i * 2, ' ') + "- level\n";
    if (!quick) pathological << table << nested;
    const QStringList names = quick ? QStringList{"100k_line", "100k_lines"}
                                   : QStringList{"100k_line", "1m_line", "100k_lines", "10MiB_table", "5000_nested"};
    if (pathological.size() != names.size()) {
        check(false, "pathological fixture names and payloads have identical cardinality");
        return;
    }
    for (int i = 0; i < pathological.size(); ++i) {
        hn::editor::NoteEditor ed; ed.resize(800, 600); ed.show();
        QElapsedTimer eventGap; qint64 lastTick = 0, maxGap = 0; int heartbeatTicks = 0;
        QTimer heartbeat; heartbeat.setInterval(1);
        QObject::connect(&heartbeat, &QTimer::timeout, [&] { const qint64 now = eventGap.elapsed(); if (lastTick) maxGap = qMax(maxGap, now - lastTick); lastTick = now; ++heartbeatTicks; });
        eventGap.start(); heartbeat.start();
        QElapsedTimer ready; ready.start();
        while (!heartbeatTicks && ready.elapsed() < 100) { QCoreApplication::processEvents(QEventLoop::AllEvents, 5); usleep(100); }
        lastTick = eventGap.elapsed(); const int ticksBeforeLoad = heartbeatTicks;
        Stopwatch sw; ed.load(pathological[i]); const double loadMs = sw.ms();
        QElapsedTimer resume; resume.start();
        while (heartbeatTicks == ticksBeforeLoad && resume.elapsed() < 200) QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        heartbeat.stop();
        const bool heartbeatObserved = ticksBeforeLoad > 0 && heartbeatTicks > ticksBeforeLoad;
        metric(qPrintable("scale.pathological." + names[i] + ".load_block"), loadMs, "ms");
        metric(qPrintable("scale.pathological." + names[i] + ".event_loop_stall_max"), maxGap, "ms");
        metric(qPrintable("scale.pathological." + names[i] + ".mode_source"), ed.mode() == hn::editor::Mode::Source ? 1 : 0);
        check(heartbeatObserved, names[i] + " live event-loop heartbeat sampled across load");
        check(heartbeatObserved && (maxGap < 100 || ed.mode() == hn::editor::Mode::Source),
              QString("%1 event-loop load stall under 100ms or source-mode fallback").arg(names[i]));
        metric(qPrintable("scale.pathological." + names[i] + ".pss"), mem().total(), "MiB");
        const QByteArray roundTrip = ed.toMarkdownBytes();
        Sandbox saveBox("scale-pathological-save-" + names[i]); double saveMs = 0;
        const bool saved = repositorySave(saveBox, "fixture", roundTrip, &saveMs);
        metric(qPrintable("scale.pathological." + names[i] + ".save"), saveMs, "ms");
        check(saved, names[i] + " actual repository save persisted serialized bytes");
    }
}

void pathologicalFallback(const QByteArray &input, const QString &name) {
    hn::editor::NoteEditor ed; ed.resize(800, 600); ed.show();
    Stopwatch sw; ed.load(input); QCoreApplication::processEvents();
    metric(qPrintable("scale.pathological." + name + ".open"), sw.ms(), "ms");
    metric(qPrintable("scale.pathological." + name + ".mode_source"), ed.mode() == hn::editor::Mode::Source ? 1 : 0);
    check(ed.mode() == hn::editor::Mode::Source, name + " automatically falls back to source mode");
    check(ed.toMarkdownBytes() == input, name + " source-mode fallback preserves exact bytes");
}

void listImportBenchmark() {
    QByteArray markdown = "# List\n\n";
    for (int i = 0; i < 10000; ++i) markdown += "- item " + QByteArray::number(i) + " lorem ipsum\n";
    Dist load;
    for (int i = 0; i < 3; ++i) {
        hn::editor::NoteEditor ed; ed.resize(640, 480);
        Stopwatch sw;
        CALLGRIND_ZERO_STATS;
        CALLGRIND_START_INSTRUMENTATION;
        ed.load(markdown);
        CALLGRIND_STOP_INSTRUMENTATION;
        QCoreApplication::processEvents(); load.add(sw.ms());
        check(ed.toMarkdownBytes() == markdown, "10k list import preserves exact original bytes");
        if (i == 0) {
            const bool switched = ed.setMode(hn::editor::Mode::Source);
            check(switched, "10k list can switch to source mode after chunked visual import");
            check(ed.sourceEdit()->toPlainText().toUtf8() == markdown,
                  "chunked list groups preserve source marker alternation on export");
        }
    }
    load.report("scale.editor.list10k_import");
    check(load.pct(.5) < 2093.0, QString("10k-item visual import improved by at least 10%% from measured 2326ms baseline (%1ms median)").arg(load.pct(.5)));
}

void searchProfile(int count) {
    Sandbox sb(QString("search-profile-%1").arg(count));
    writeLibrary(sb.notes, count, 20 * 1024);
    hn::core::LibraryIndex idx(sb.notes, sb.cache);
    check(waitForIndex(idx, count), QString("profile index contains all %1 notes").arg(count));
    const QStringList queries{"uniq", "uniq77", "Paragraph", "nested item", "\"quoted paragraph\"", "link example", "Section", "tag5", "#tag7", "stress",
        "naïve", "日本語", "answer", "pend*", "numbered AND item", "bold OR italic", "wrap NEAR layout", "zzzznotfound", "int answer", "C++"};
    QList<Dist> perQuery(queries.size());
    CALLGRIND_ZERO_STATS;
    CALLGRIND_START_INSTRUMENTATION;
    for (int repeat = 0; repeat < 10; ++repeat)
        for (int q = 0; q < queries.size(); ++q) {
            Stopwatch sw; const auto page = idx.searchNow({queries[q], {}, {}, 50, 0});
            perQuery[q].add(sw.ms());
            check(true, QString("profile query completed, rows=%1").arg(page.rows.size()));
        }
    CALLGRIND_STOP_INSTRUMENTATION;
    Dist aggregate;
    for (int i = 0; i < perQuery.size(); ++i) {
        perQuery[i].report(QString("scale.search.%1.query%2").arg(count).arg(i));
        for (double sample : perQuery[i].v) aggregate.add(sample);
    }
    aggregate.report(QString("scale.search.%1.aggregate").arg(count));
    const double threshold = count == 1000 ? 17.7 : 46.0;
    check(aggregate.pct(.95) < threshold,
          QString("%1-note warm-search p95 %2 ms remains below immutable %3 ms baseline").arg(count).arg(aggregate.pct(.95)).arg(threshold));
}

void startup(bool quick) {
    Sandbox sb("scale-startup-100k");
    writeLibrary(sb.notes, 100000, 20 * 1024, true);
    scaleStartup(sb.notes, sb.state, sb.cache, sb.config, sb.data, 100000, quick);
}

void plugins() {
    const QString dir = qEnvironmentVariable("HN_PLUGINS_DIR", QStringLiteral(HN_PLUGINS_DIR));
    if (dir.isEmpty() || !QDir(dir).exists()) { skip("official plugin latency requires HN_PLUGINS_DIR"); return; }
    const QStringList ids{"slash-menu", "journal", "tasks", "wiki-links"};
    for (const QString &id : ids)
        if (!QFileInfo::exists(dir + "/" + id + "/plugin.json")) { skip("official plugin checkout incomplete: " + id); return; }
    Sandbox sb("scale-plugins");
    ControllerOptions opt;
    opt.notesDir = sb.notes; opt.stateDir = sb.state; opt.cacheDir = sb.cache;
    opt.configPath = sb.config + "/config.json"; opt.modsDir = sb.data + "/mods";
    opt.pluginsDir = sb.data + "/plugins"; opt.modsEnabledPath = sb.config + "/mods-enabled.json";
    opt.useTray = false; opt.useHyprland = false;
    opt.pluginHooks.consent = [](const ConsentRequest &) { return true; };
    for (int i = 0; i < 20; ++i) writeFile(sb.notes + QString("/n%1.md").arg(i), mixedNote(20 * 1024, i));
    AppController c(opt);
    auto *manager = c.plugins().manager();
    for (const QString &id : ids) {
        const auto installed = manager->install(dir + "/" + id, true);
        check(installed.ok, QString("install official plugin %1 to scratch profile").arg(id));
        if (!installed.ok) continue;
        const auto manifest = manager->info(id).manifest;
        QString error;
        check(manager->consent(id, manifest.permissions, &error), QString("consent official plugin %1").arg(id));
        check(c.plugins().enable(id, nullptr, &error), QString("enable official plugin %1: %2").arg(id, error));
    }
    auto *session = c.openSticky("n0.md", false);
    check(session != nullptr, "open 20KiB typing fixture with all four official plugins enabled");
    if (session) {
        Dist typing;
        QWidget *edit = session->editor()->activeEdit();
        for (int i = 0; i < 500; ++i) {
            Stopwatch sw; QTest::keyClick(edit, char('a' + i % 26));
            QCoreApplication::processEvents(); typing.add(sw.ms());
        }
        typing.report("scale.official_plugins.typing20k");
    }
}

void wakeups(bool quick) {
    for (int n : quick ? QList<int>{1} : QList<int>{1, 10, 100}) {
        Sandbox sb(QString("scale-wakeup-%1").arg(n));
        ControllerOptions opt;
        opt.notesDir = sb.notes; opt.stateDir = sb.state; opt.cacheDir = sb.cache;
        opt.configPath = sb.config + "/config.json"; opt.modsDir = sb.data + "/mods";
        opt.useTray = false; opt.useHyprland = false;
        for (int i = 0; i < n; ++i) {
            writeFile(sb.notes + QString("/n%1.md").arg(i), "idle\n");
        }
        AppController c(opt);
        for (int i = 0; i < n; ++i) c.openSticky(QString("n%1.md").arg(i), false);
        QCoreApplication::processEvents();
        auto switches = [] {
            QFile f("/proc/self/status");
            if (!f.open(QIODevice::ReadOnly)) return qint64(-1);
            qint64 total = 0;
            for (const QByteArray &line : f.readAll().split('\n'))
                if (line.startsWith("voluntary_ctxt_switches:") || line.startsWith("nonvoluntary_ctxt_switches:"))
                    total += line.mid(line.indexOf(':') + 1).trimmed().toLongLong();
            return total;
        };
        const qint64 sw0 = switches();
        const double cpu0 = cpuSeconds();
        const double wall = quick ? 1000 : 60000;
        QElapsedTimer elapsed; elapsed.start();
        QEventLoop loop; QTimer::singleShot(int(wall), &loop, &QEventLoop::quit); loop.exec();
        const double seconds = elapsed.elapsed() / 1000.0;
        const double pct = seconds > 0 ? (cpuSeconds() - cpu0) / seconds * 100.0 : 0;
        const qint64 sw1 = switches();
        metric(qPrintable(QString("scale.wakeups.%1.cpu").arg(n)), pct, "percent");
        metric(qPrintable(QString("scale.wakeups.%1.per_second").arg(n)), seconds > 0 && sw0 >= 0 && sw1 >= sw0 ? (sw1 - sw0) / seconds : -1, "wakeups/s");
        check(sw0 >= 0 && sw1 >= sw0, "proc context-switch counters sampled without wrap/read failure");
        if (quick) skip(QString("%1-second smoke sample cannot certify exact-zero idle CPU budget").arg(seconds));
        else check(pct <= 0.0, QString("idle CPU remains exactly zero for %1 windows").arg(n));
    }
}
}

int main(int argc, char **argv) {
    CALLGRIND_STOP_INSTRUMENTATION;
    const QString scratch = QDir::tempPath() + "/hyprnotes-scale-" + QString::number(getpid());
    QDir().mkpath(scratch); isolateProcessHome(scratch);
    Sandbox sb("scale-root");
    QApplication app(argc, argv);
    const QString mode = app.arguments().value(1, "all");
    const bool quick = app.arguments().contains("--quick");
    if (mode == "all" || mode == "library") scaleLibrary(quick ? 1000 : 100000, quick);
    if (mode == "all" || mode == "windows") stickyScale(quick);
    if (mode == "windows10") stickyScale(false, 10);
    if (mode == "all" || mode == "big") bigNoteCases(quick);
    if (mode == "all" || mode == "list") listImportBenchmark();
    if (mode == "search1000") searchProfile(1000);
    if (mode == "search10000") searchProfile(10000);
    if (mode == "pathological-line") pathologicalFallback(QByteArray(100000, 'x') + "\n", "100k_line");
    if (mode == "pathological-lines") pathologicalFallback(QByteArray(100000, 'x') + QByteArray(100000, '\n'), "100k_lines");
    if (mode == "fixture-cardinality") {
        const int pathologicalCount = 3 + 2;  // three line fixtures, 10MiB table, five-thousand-level list
        const int nameCount = 5;
        check(pathologicalCount == nameCount, "pathological fixture table and names are cardinality-matched");
    }
    if (mode == "nested") {
        QByteArray nested; for (int i = 0; i < 5000; ++i) nested += QByteArray(i * 2, ' ') + "- level\n";
        hn::editor::NoteEditor ed; ed.resize(800, 600); ed.show();
        Stopwatch sw; ed.load(nested); QCoreApplication::processEvents();
        metric("scale.pathological.5000_nested.open", sw.ms(), "ms");
        metric("scale.pathological.5000_nested.mode_source", ed.mode() == hn::editor::Mode::Source ? 1 : 0);
        metric("scale.pathological.5000_nested.pss", mem().total(), "MiB");
        check(!ed.toMarkdownBytes().isEmpty(), "5000 nested levels retained");
    }
    if (mode == "startup") startup(quick);
    if (mode == "all" || mode == "plugins") plugins();
    if (mode == "all" || mode == "wakeups") wakeups(quick);
    const int rc = finish("stress_scale");
    QDir(scratch).removeRecursively();
    return rc;
}
