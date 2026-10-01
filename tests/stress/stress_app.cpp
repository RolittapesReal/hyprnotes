// P8.4 whole-app load: idle PSS/CPU for W1/W10/30/100 windows (in-process AppController and the real binary),
// repeated open/close cycles, and a compressed randomized soak with per-minute invariants.
// OFFSCREEN: windows have QImage backing stores in-process, but no compositor/shm/GPU buffers (labelled in the report).
#include "controller.h"
#include "editor_bridge.h"
#include "hn/core/worker.h"
#include "hn/editor/note_editor.h"
#include "organizer_window.h"
#include <QJsonArray>
#include <QLineEdit>
#include <QLabel>
#include "hn/core/markdown_codec.h"
#include "stress_util.h"
#include "sticky_window.h"
#include <QApplication>
#include <QDirIterator>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QProcess>
#include <QTest>
#include <QTimer>
#include <csignal>
#include <malloc.h>
#include <execinfo.h>
#include <pthread.h>
#include <atomic>
#include <thread>

using namespace st;
using namespace hn::app;

// ---------------------------------------------------------------- warnings captured from the app under test
static QMap<QString, int> g_warnings;
static void msgHandler(QtMsgType t, const QMessageLogContext &, const QString &m) {
    if (t == QtDebugMsg || t == QtInfoMsg) return;
    QString key = m.left(140);
    key.replace(QRegularExpression("[0-9]+"), "#");
    ++g_warnings[key];
}

static void runFor(int ms) {
    QEventLoop loop; QTimer::singleShot(ms, &loop, &QEventLoop::quit); loop.exec();
}
template <class F> static bool until(F cond, int timeoutMs) {
    QElapsedTimer t; t.start();
    while (!cond()) { if (t.elapsed() > timeoutMs) return false; runFor(10); }
    return true;
}

// ---------------------------------------------------------------- world: library of 1000 x 20 KiB notes + controller
struct World {
    Sandbox sb;
    int libN;
    QStringList rels;
    ControllerOptions opt;
    std::unique_ptr<AppController> c;
    explicit World(const QString &tag, int lib = 1000, bool withMod = false) : sb(tag), libN(lib) {
        for (int i = 0; i < lib; ++i) {
            const QString rel = QString("d%1/note-%2.md").arg(i % 25).arg(i, 4, 10, QChar('0'));
            writeFile(sb.notes + "/" + rel, mixedNote(20 * 1024, i));
            rels << rel;
        }
        opt.notesDir = sb.notes; opt.stateDir = sb.state; opt.cacheDir = sb.cache;
        opt.configPath = sb.config + "/config.json"; opt.modsDir = sb.data + "/mods"; opt.modsEnabledPath = sb.config + "/mods-enabled.json";
        opt.useTray = false; opt.useHyprland = false;
        opt.confirm = [](const QString &, const QString &) { return true; };
        opt.recoveryPrompt = [](const QList<hn::core::RecoveryEntry> &) { return RecoveryChoice::Later; };
        if (withMod) {
            const QString so = QString(HN_EXAMPLE_MOD_DIR) + "/libuppercase_selection.so";
            if (QFile::exists(so)) {
                const QString md = sb.data + "/mods/uppercase-selection";
                QDir().mkpath(md);
                QFile::copy(so, md + "/libuppercase_selection.so"); QFile::copy(QString(HN_EXAMPLE_MOD_SRC) + "/mod.json", md + "/mod.json");
                writeFile(opt.modsEnabledPath, R"({"version":1,"enabled":["uppercase-selection"]})");
            }
        }
        c = std::make_unique<AppController>(opt);
    }
};

static int liveEditors() { int n = 0; for (QWidget *w : QApplication::allWidgets()) if (qobject_cast<hn::editor::NoteEditor *>(w)) ++n; return n; }
static int liveStickies() { int n = 0; for (QWidget *w : QApplication::allWidgets()) if (qobject_cast<StickyWindow *>(w)) ++n; return n; }
static int liveWidgets() { return QApplication::allWidgets().size(); }

static void typeInto(NoteSession *s, int chars, Rng &rng) {
    QWidget *w = s->editor()->activeEdit();
    for (int i = 0; i < chars; ++i) {
        const int k = rng.bounded(100);
        if (k < 80) QTest::keyClick(w, char('a' + rng.bounded(26)));
        else if (k < 88) QTest::keyClick(w, Qt::Key_Return);
        else if (k < 95) QTest::keyClick(w, Qt::Key_Backspace);
        else s->editor()->undo();
    }
}

// ---------------------------------------------------------------- 4.1 idle load (in-process)
static void idleLoad(int N, bool edit, int settleS, int cpuS, int sampleS) {
    const QString tag = QString("app.inproc.%1%2").arg(N == 1 ? "W1" : N == 10 ? "W10" : QString("%1win").arg(N)).arg(edit ? ".edited" : "");
    note(QString("== 4.1 in-process idle: %1 sticky windows%2, library 1000 x 20 KiB, settle %3 s, CPU window %4 s, PSS sampling %5 s")
         .arg(N).arg(N == 1 ? " (W1: no organizer, no index)" : " + organizer").arg(settleS).arg(cpuS).arg(sampleS));
    World w(QString("idle%1").arg(N));
    AppController &c = *w.c;
    Stopwatch open;
    const int base = N == 10 ? 1 : N;   // W10: organizer holds the eleventh note, stickies hold ten
    for (int i = 0; i < N; ++i) { c.openSticky(w.rels[i], false); if (i % 10 == 9) QCoreApplication::processEvents(); }
    if (N != 1) {
        c.showOrganizer(false);
        c.openInOrganizer(w.rels[N]);   // the eleventh note
        bool synced = false;
        QObject::connect(c.index(), &hn::core::LibraryIndex::synced, [&] { synced = true; });
        until([&] { return synced; }, 120000);
        check(synced, "organizer index sync completed (library 1000)");
    }
    (void)base;
    runFor(300);
    metric(qPrintable(tag + ".open_all_ms"), open.ms(), "ms");
    check(liveStickies() == N, QString("%1 sticky windows alive (%2)").arg(N).arg(liveStickies()));
    if (edit) {   // populate histories (spec: idle budget must hold after an editing workload)
        Rng rng(7);
        Stopwatch e;
        for (auto *s : c.stickies().isEmpty() ? QList<StickyWindow *>() : c.stickies()) { typeInto(s->session(), 300, rng); }
        if (c.organizer() && c.organizer()->currentSession()) typeInto(c.organizer()->currentSession(), 300, rng);
        metric(qPrintable(tag + ".edit_workload_ms"), e.ms(), "ms");
        until([&] { for (auto *s : c.stickies()) if (!s->session()->settled()) return false; return true; }, 30000);
    }
    const Mem peakish = mem();
    metric(qPrintable(tag + ".pss_after_open"), peakish.total(), "MiB");
    runFor(settleS * 1000);
    malloc_trim(0);   // report both: the figure below is taken without any extra trimming after this single point
    // CPU window: two reads only (sampling smaps would charge its own cost to this process)
    const double cpu0 = cpuSeconds(); Stopwatch wall;
    runFor(cpuS * 1000);
    const double cpuPct = (cpuSeconds() - cpu0) / (wall.ms() / 1000.0) * 100.0;
    metric(qPrintable(tag + ".idle_cpu_pct_of_one_core"), cpuPct, "%");
    check(cpuPct < 0.1, QString("[%1] idle CPU %2% of one core over %3 s (< 0.1%%, spec 9.2)").arg(tag).arg(cpuPct).arg(cpuS));
    // memory sampling once per second
    Dist pss, rss, priv, swap;
    for (int i = 0; i < sampleS; ++i) { runFor(1000); const Mem m = mem(); pss.add(m.total()); rss.add(m.rssMiB); priv.add(m.privateMiB); swap.add(m.swapPssMiB); }
    pss.report(tag + ".pss_plus_swappss", "MiB"); rss.report(tag + ".rss", "MiB"); priv.report(tag + ".private", "MiB"); swap.report(tag + ".swappss", "MiB");
    metric(qPrintable(tag + ".threads"), threadCount()); metric(qPrintable(tag + ".fds"), openFds());
    metric(qPrintable(tag + ".live_widgets"), liveWidgets());
    const double target = N == 1 ? 50 : (N == 10 ? 100 : 0);
    if (target > 0) {
        const bool ok = pss.pct(.95) <= target;
        // honest label: in-process offscreen run; includes the Qt libs mapped by the test binary and per-window QImage backing stores,
        // excludes compositor/shm buffers and GPU memory. Failing here is a real finding, passing is only an upper-bound-ish indicator.
        if (edit) soft(ok, QString("[%1] p95 PSS+SwapPss %2 MiB vs %3 MiB target (offscreen, no compositor buffers)").arg(tag).arg(pss.pct(.95)).arg(target));
        else check(ok, QString("[%1] p95 PSS+SwapPss %2 MiB <= %3 MiB target (offscreen, no compositor buffers)").arg(tag).arg(pss.pct(.95)).arg(target));
    }
    // theme switch cost with all windows open (done last: it perturbs nothing measured above)
    {
        Dist th;
        for (int i = 0; i < 4; ++i) { auto st = c.settings(); st.colorScheme = (i % 2) ? "light" : "dark"; Stopwatch t; c.applySettings(st, c.prefs()); th.add(t.ms()); runFor(200); }
        metric(qPrintable(tag + ".theme_switch_ms_p50"), th.pct(.5), "ms"); metric(qPrintable(tag + ".theme_switch_ms_max"), th.mx(), "ms");
        check(th.mx() < 100.0, QString("[D10-THEME-SWITCH-BLOCKS] ") + QString("[%1] light/dark switch with %2 windows returns within 100 ms (took %3 ms median, %4 ms max, synchronous on the GUI thread)").arg(tag).arg(N + (N != 1)).arg(th.pct(.5)).arg(th.mx()));
    }
    // history/queue bounds after the workload
    int hmax = 0; qint64 bmax = 0;
    for (auto *s : c.stickies()) { hmax = qMax(hmax, s->session()->editor()->history().count()); bmax = qMax<qint64>(bmax, s->session()->editor()->history().bytes()); }
    metric(qPrintable(tag + ".max_history_count"), hmax); metric(qPrintable(tag + ".max_history_bytes"), double(bmax), "bytes");
    check(hmax <= 500 && bmax <= (2 << 20), "history bounds hold across all windows");
    Stopwatch q; hn::core::Worker::shared().waitIdle(); metric(qPrintable(tag + ".worker_waitIdle_ms"), q.ms(), "ms");
}

// ---------------------------------------------------------------- 4.2 real binary, black box
static void binaryLoad(int N, int settleS, int cpuS, int sampleS) {
    const QString tag = QString("app.binary.%1stickies+organizer").arg(N);
    note(QString("== 4.2 real hyprnotes binary (offscreen) with %1 stickies restored from session.json + organizer; library 1000 x 20 KiB").arg(N));
    World w(QString("bin%1").arg(N));
    w.c.reset();   // the library is created by World; the controller itself is not used here
    QJsonArray arr;
    for (int i = 0; i < N; ++i) arr.append(QJsonObject{{"note", w.rels[i]}, {"role", "sticky"}, {"x", (i % 10) * 40}, {"y", (i / 10) * 40}, {"w", 360}, {"h", 300}, {"monitor", ""}, {"workspace", 0}, {"scale", 1}, {"allWorkspaces", false}});
    writeFile(w.sb.state + "/session.json", QJsonDocument(QJsonObject{{"version", 1}, {"windows", arr}}).toJson());
    QProcess p;
    p.setProgram(HN_BIN);
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start();
    if (!p.waitForStarted(10000)) { check(false, "binary starts"); return; }
    const long pid = p.processId();
    Stopwatch up;
    // wait for readiness: process alive and CPU settled
    runFor(3000);
    check(p.state() == QProcess::Running, "binary running after start");
    runFor(settleS * 1000);
    const double c0 = cpuSecondsOf(pid); Stopwatch wall;
    runFor(cpuS * 1000);
    const double pct = (cpuSecondsOf(pid) - c0) / (wall.ms() / 1000.0) * 100.0;
    metric(qPrintable(tag + ".idle_cpu_pct_of_one_core"), pct, "%");
    Dist pss, rss, priv;
    for (int i = 0; i < sampleS; ++i) { runFor(1000); const Mem m = memOf(pid); pss.add(m.total()); rss.add(m.rssMiB); priv.add(m.privateMiB); }
    pss.report(tag + ".pss_plus_swappss", "MiB"); rss.report(tag + ".rss", "MiB"); priv.report(tag + ".private", "MiB");
    check(pct < 0.1, QString("[%1] idle CPU %2% < 0.1%").arg(tag).arg(pct));
    soft(pss.pct(.95) <= 100.0, QString("[%1] p95 PSS+SwapPss %2 MiB vs 100 MiB W10 target (offscreen; organizer has no 11th note open)").arg(tag).arg(pss.pct(.95)));
    // SIGTERM behaviour is exercised in stress_robust; here just stop it cleanly and confirm the session file survived
    p.terminate(); p.waitForFinished(10000);
    metric(qPrintable(tag + ".sigterm_exit_code"), p.exitCode());
    const QByteArray sess = readFile(w.sb.state + "/session.json");
    check(QJsonDocument::fromJson(sess).isObject(), "session.json still valid after SIGTERM");
}

// ---------------------------------------------------------------- 4.3 open/close cycles
static void cycles(int warm, int batches, int perBatch, int extra, int distinct) {
    note(QString("== 4.3 lifecycle: %1 warm-up cycles then %2 batches x %3 cycles (open sticky, type, close) over %5 distinct notes, plus %4 extra cycles").arg(warm).arg(batches).arg(perBatch).arg(extra).arg(distinct));
    const bool recycled = distinct <= 20;   // the same notes reopened every cycle: any growth is a true per-cycle leak
    const QString ctag = recycled ? "app.cycles.same_notes" : "app.cycles.fresh_notes";
    World w("cycles", 400);
    AppController &c = *w.c;
    c.showOrganizer(false);
    Rng rng(3);
    int cycleNo = 0; bool typing = true;
    auto cycle = [&](int k) {
        const QString rel = w.rels[k % distinct];
        NoteSession *s = c.openSticky(rel, false);
        if (!s) { check(false, "openSticky failed for " + rel); return; }
        if (typing) typeInto(s, 8, rng);
        if (k % 5 == 0) { c.popIn(rel); }                       // organizer path
        else if (k % 7 == 0) { c.openInOrganizer(w.rels[(k + 1) % w.libN]); c.popOut(w.rels[(k + 1) % w.libN]); c.closeNote(w.rels[(k + 1) % w.libN]); }
        c.closeNote(rel);
        until([&] { return !c.session(rel) && c.openNotes().isEmpty(); }, 15000);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        ++cycleNo;
    };
    for (int i = 0; i < warm; ++i) cycle(i);
    runFor(2000);
    struct Snap { double pss, priv, anon, heapInUse; int widgets, editors, stickies, sessions, threads, fds; };
    auto snap = [&] { malloc_trim(0); const struct mallinfo2 mi = mallinfo2(); const Mem m = mem(); return Snap{m.total(), m.privateMiB, m.anonMiB, (mi.uordblks + mi.hblkhd) / 1048576.0, liveWidgets(), liveEditors(), liveStickies(), int(c.openNotes().size()), threadCount(), openFds()}; };
    const Snap base = snap();
    note(QString("after warm-up: heapInUse=%9 pss=%1 priv=%2 widgets=%3 editors=%4 stickies=%5 sessions=%6 threads=%7 fds=%8").arg(base.pss).arg(base.priv).arg(base.widgets).arg(base.editors).arg(base.stickies).arg(base.sessions).arg(base.threads).arg(base.fds).arg(base.heapInUse));
    QList<Snap> s;
    for (int b = 0; b < batches; ++b) {
        for (int i = 0; i < perBatch; ++i) cycle(warm + b * perBatch + i);
        runFor(2000);
        s << snap();
        const Snap &x = s.last();
        note(QString("batch %1: heapInUse=%11 pss=%2 priv=%3 anon=%4 widgets=%5 editors=%6 stickies=%7 sessions=%8 threads=%9 fds=%10")
             .arg(b + 1).arg(x.pss).arg(x.priv).arg(x.anon).arg(x.widgets).arg(x.editors).arg(x.stickies).arg(x.sessions).arg(x.threads).arg(x.fds).arg(x.heapInUse));
        metric(qPrintable(QString("%1.batch%2.heap_in_use").arg(ctag).arg(b + 1)), x.heapInUse, "MiB");
        metric(qPrintable(QString("%1.batch%2.pss").arg(ctag).arg(b + 1)), x.pss, "MiB"); metric(qPrintable(QString("%1.batch%2.private").arg(ctag).arg(b + 1)), x.priv, "MiB");
    }
    bool countsFlat = true;
    for (const Snap &x : s) if (x.widgets != base.widgets || x.editors != base.editors || x.stickies != base.stickies || x.sessions != base.sessions || x.threads != base.threads || x.fds > base.fds + 2) countsFlat = false;
    check(countsFlat, "live widget/editor/sticky/session/thread/fd counts identical after every batch (no monotonic growth)");
    check(s.last().editors == 0 && s.last().sessions == 0 && s.last().stickies == 0, "no editors, sessions or stickies left after cycles");
    const double slope = (s.last().priv - s.first().priv) / double(qMax<qsizetype>(1, s.size() - 1));
    metric(qPrintable(ctag + ".private_slope_per_batch"), slope, "MiB/batch");
    const double heapSlope = (s.last().heapInUse - s.first().heapInUse) / double(qMax<qsizetype>(1, s.size() - 1));
    metric(qPrintable(ctag + ".heap_in_use_slope_per_batch"), heapSlope, "MiB/batch");
    if (recycled) {
        check(slope < 1.0, QString("[same notes] private memory plateau: %1 MiB/batch across batches 1..5 (< 1 MiB/batch)").arg(slope));
        check(heapSlope < 0.25, QString("[same notes] malloc in-use bytes (after malloc_trim) slope %1 MiB/batch (< 0.25): no per-cycle leak").arg(heapSlope));
    } else {
        soft(slope < 1.0, QString("[fresh notes] private memory slope %1 MiB/batch across batches 1..5").arg(slope));
        xfail(heapSlope < 0.25, "D9-PER-NOTE-RETAINED", QString("[fresh notes] live heap flat across 5 batches of 20 cycles that each open a note never opened before: %1 MiB/batch (~%2 KiB retained per distinct edited note)").arg(heapSlope).arg(heapSlope * 1024.0 / 20.0));
    }
    // Extended runs expose slow leaks the 5x20 protocol cannot see. Phase A reuses 20 already-touched notes (isolates a true
    // per-cycle leak); phase B walks notes never opened before (isolates per-distinct-note state kept after close).
    auto extended = [&](const char *name, int first, int count, int distinct) {
        malloc_trim(0);
        const Snap before = snap();
        for (int i = 0; i < count; ++i) cycle(first + (i % distinct));
        runFor(500);
        const Snap after = snap();
        const double dHeap = after.heapInUse - before.heapInUse, perCycle = dHeap * 1024.0 / count;
        note(QString("extended %1: %2 cycles over %3 distinct notes: heap in-use %4 -> %5 MiB (%6 KiB/cycle), private %7 -> %8, widgets %9").arg(name).arg(count).arg(distinct)
             .arg(before.heapInUse).arg(after.heapInUse).arg(perCycle).arg(before.priv).arg(after.priv).arg(after.widgets));
        metric(qPrintable(QString("app.cycles.extended_%1.heap_in_use_growth").arg(name)), dHeap, "MiB");
        metric(qPrintable(QString("app.cycles.extended_%1.heap_in_use_growth_per_cycle").arg(name)), perCycle, "KiB/cycle");
        metric(qPrintable(QString("app.cycles.extended_%1.private_growth").arg(name)), after.priv - before.priv, "MiB");
        check(after.widgets == base.widgets && after.editors == 0, QString("extended %1: widget count back to baseline (%2 vs %3)").arg(name).arg(after.widgets).arg(base.widgets));
        return perCycle;
    };
    if (extra > 0 && recycled) {
        const double a = extended("same20", 0, extra, 20);
        check(a < 2.0, QString("[same notes] %1 further open/type/close cycles keep live heap flat: %2 KiB/cycle (< 2)").arg(extra).arg(a));
    }
    if (extra > 0 && !recycled) {
        typing = true;
        const double bb = extended("fresh_notes", 130, qMin(extra, 80), 400 - 130);
        typing = false;
        const double cc = extended("fresh_notes_no_typing", 230, qMin(extra, 80), 400 - 230);
        typing = true;
        note(QString("retention per never-before-opened note: after edit+save %1 KiB, open+close without edit %2 KiB (20 KiB notes)").arg(bb).arg(cc));
        xfail(bb < 2.0, "D9-PER-NOTE-RETAINED", QString("opening, editing and closing notes never touched before keeps live heap flat: %1 KiB retained per note after close (a 20 KiB note)").arg(bb));
    }
}

// ---------------------------------------------------------------- 4.4 soak
static void soak(int minutes, bool fast) {
    note(QString("== 4.4 soak: %1 minute(s) of randomized operations (open/close/pop-out/pop-in/edit/search/theme/config reload/mod/external edit/rename/delete)").arg(minutes));
    World w("soak", 1000, true);
    AppController &c = *w.c;
    c.showOrganizer(false);
    Rng rng(20261001);
    QElapsedTimer total; total.start();
    // event-loop gap probe
    qint64 gapMax = 0, lastTick = 0; QElapsedTimer clk; clk.start();
    QTimer probe; QObject::connect(&probe, &QTimer::timeout, [&] { const qint64 n = clk.elapsed(); if (lastTick) gapMax = qMax(gapMax, n - lastTick); lastTick = n; }); probe.start(10);
    QStringList words = {"Section", "uniq", "Paragraph", "nested", "quoted", "tag3", "stress", "answer", "zzz", "\"phrase", "a*", "NEAR(", "item"};
    long ops = 0; QMap<QString, long> opCount; QMap<QString, Dist> opD; QStringList history; int conflictsResolved = 0, modRuns = 0, modOk = 0;
    auto openList = [&] { return c.stickies(); };
    auto pickOpen = [&]() -> NoteSession * {
        QList<NoteSession *> all; for (auto *sx : openList()) all << sx->session();
        if (c.organizer() && c.organizer()->currentSession()) all << c.organizer()->currentSession();
        return all.isEmpty() ? nullptr : all[rng.bounded(all.size())];
    };
    auto resolveStuck = [&](NoteSession *s) {
        using St = NoteSession::State;
        if (s->state() == St::Conflict) { rng.chance(50) ? s->reload() : s->replaceDisk(); ++conflictsResolved; }
        else if (s->state() == St::Removed) s->recreate();
        else if (s->state() == St::Failed) s->retry();
    };
    struct Min { double pss, priv, rssM; int widgets, editors, sessions, windows, threads, fds; qint64 gap; };
    QList<Min> series;
    int nextNew = 0;
    auto invariants = [&](int minute) {
        // drain: finish pending closes/saves, resolve stuck sessions, then compare
        for (int pass = 0; pass < 20; ++pass) {
            for (auto *s : QList<NoteSession *>{}) (void)s;
            for (const QString &r : c.openNotes()) if (auto *s = c.session(r)) resolveStuck(s);
            runFor(150);
        }
        until([&] { for (const QString &r : c.openNotes()) if (c.session(r) && !c.session(r)->settled()) return false; return true; }, 10000);
        Stopwatch q; hn::core::Worker::shared().waitIdle(); const double idleMs = q.ms();
        const int editors = liveEditors(), sessions = c.openNotes().size();
        const Mem m = mem();
        series << Min{m.total(), m.privateMiB, m.rssMiB, liveWidgets(), editors, sessions, c.windowCount(), threadCount(), openFds(), gapMax};
        const Min &x = series.last();
        std::printf("MINUTE %2d ops=%ld pss=%.1f priv=%.1f rss=%.1f widgets=%d editors=%d sessions=%d windows=%d threads=%d fds=%d waitIdle=%.1fms maxLoopGap=%lldms warnings=%d\n",
                    minute, ops, x.pss, x.priv, x.rssM, x.widgets, editors, sessions, x.windows, x.threads, x.fds, idleMs, (long long)gapMax, int(g_warnings.size()));
        std::fflush(stdout);
        check(editors == sessions + 0, QString("minute %1: live NoteEditor widgets (%2) == open sessions (%3): no editor leak").arg(minute).arg(editors).arg(sessions));
        {   // diagnose sessions that no window shows
            QSet<QString> shown; for (auto *sx : c.stickies()) shown.insert(sx->session()->rel());
            if (c.organizer() && c.organizer()->currentSession()) shown.insert(c.organizer()->currentSession()->rel());
            for (const QString &r : c.openNotes()) if (!shown.contains(r)) { std::printf("ORPHAN-SESSION minute %d rel=%s state=%s organizerNote=%s sticky=%d\n", minute, qPrintable(r), qPrintable(c.session(r)->stateLabel()), qPrintable(c.organizerNote()), c.stickyOf(r) != nullptr); if (minute <= 2) for (const QString &h : history) if (h.contains(r)) std::printf("   history: %s\n", qPrintable(h)); }
        }
        check(c.stickies().size() + (c.organizer() && c.organizer()->currentSession() ? 1 : 0) == sessions, QString("[D15-ORPHAN-SESSION] ") + QString("minute %1: every live session is shown in exactly one window (sessions=%2, stickies=%3, organizer shows one=%4)").arg(minute).arg(sessions).arg(c.stickies().size()).arg(c.organizer() && c.organizer()->currentSession() ? 1 : 0));
        check(idleMs < 3000, QString("minute %1: worker queue drains (waitIdle %2 ms)").arg(minute).arg(idleMs));
        bool hist = true; for (const QString &r : c.openNotes()) if (auto *s = c.session(r)) hist &= s->editor()->history().count() <= 500 && s->editor()->history().bytes() <= (2 << 20);
        check(hist, QString("minute %1: every open editor's history within 500 tx / 2 MiB").arg(minute));
        check(x.threads == series.first().threads, QString("minute %1: thread count constant (%2)").arg(minute).arg(x.threads));
        gapMax = 0;
    };
    const qint64 totalMs = qint64(minutes) * (fast ? 6000 : 60000);
    int minute = 0;
    while (total.elapsed() < totalMs) {
        ++ops;
        QString opName, relUsed; Stopwatch opSw;
        const int r = rng.bounded(100);
        const int nOpen = c.openNotes().size();
        const QString rel = w.rels[rng.bounded(300)];
        if (r < 14) { if (c.stickies().size() < 14) { c.openSticky(rel, false); opName = "open"; relUsed = rel; } }
        else if (r < 26) { auto st = c.stickies(); if (!st.isEmpty()) { relUsed = st[rng.bounded(st.size())]->session()->rel(); c.closeNote(relUsed); opName = "close"; } }
        else if (r < 33) {
            if (c.organizer() && c.organizer()->currentSession()) { relUsed = c.organizerNote(); c.popOut(relUsed); opName = "popout"; }
            else { auto st = c.stickies(); if (!st.isEmpty()) { relUsed = st[rng.bounded(st.size())]->session()->rel(); c.popIn(relUsed); opName = "popin"; } }
        }
        else if (r < 41) { c.openInOrganizer(rel); opName = "org-open"; relUsed = rel; }
        else if (r < 66) { if (auto *s = pickOpen()) { typeInto(s, 3 + rng.bounded(30), rng); opName = "edit"; if (rng.chance(5)) { s->editor()->toggleInline(hn::editor::InlineStyle::Bold); s->editor()->setBlockStyle(hn::editor::BlockStyle::H2); } } }
        else if (r < 71) { if (c.organizer()) { c.organizer()->searchEdit()->setText(words[rng.bounded(words.size())]); c.organizer()->runSearchNow(); opName = "search"; } }
        else if (r < 75) {
            auto s = c.settings(); s.colorScheme = rng.chance(50) ? "dark" : "light"; if (rng.chance(25)) s.theme = rng.chance(50) ? "no-such-theme" : "modernist";
            s.reduceMotion = rng.chance(50); c.applySettings(s, c.prefs()); opName = "theme";
        }
        else if (r < 79) {   // external config.json rewrite (watcher path), sometimes malformed
            const QString cfg = w.opt.configPath;
            if (rng.chance(15)) writeFile(cfg, "{ this is : not json,");
            else replaceFile(cfg, QJsonDocument(QJsonObject{{"version", 1}, {"colorScheme", rng.chance(50) ? "dark" : "light"}, {"reduceMotion", rng.chance(50)}, {"notesFolder", w.sb.notes}}).toJson());
            opName = "config-reload";
        }
        else if (r < 83) {
            if (auto *s = pickOpen()) {
                auto *v = s->editor()->visualEdit();
                if (s->editor()->mode() == hn::editor::Mode::Visual && v->document()->characterCount() > 20) {
                    QTextCursor cur(v->document()); cur.setPosition(rng.bounded(v->document()->characterCount() - 12)); cur.setPosition(cur.position() + 8, QTextCursor::KeepAnchor); v->setTextCursor(cur);
                    ++modRuns; if (c.runModCommand(s->rel(), "uppercase-selection")) ++modOk; opName = "mod";
                }
            }
        }
        else if (r < 87) { replaceFile(w.sb.notes + "/" + rel, mixedNote(20 * 1024, int(rng.bounded(100000)))); opName = "external-edit"; relUsed = rel; }
        else if (r < 90) { const QString n = c.newNote(); if (!n.isEmpty()) { opName = "new"; relUsed = n; if (rng.chance(50)) { c.deleteNote(n); opName = "new+delete"; } } }
        else if (r < 93) { const QString n = c.newNote(); if (!n.isEmpty() && !c.session(n)) { QString err; c.renameNote(n, QString("Renamed %1").arg(nextNew++), &err); opName = "rename"; } }
        else if (r < 96) { if (auto *s = pickOpen()) { resolveStuck(s); } }
        else { if (auto *s = pickOpen()) s->flush(); }
        (void)nOpen;
        if (!opName.isEmpty()) { history << QString("%1 %2 %3 (sessions=%4)").arg(ops).arg(opName, relUsed).arg(c.openNotes().size()); if (history.size() > 4000) history.removeFirst(); }
        if (!opName.isEmpty()) { opCount[opName]++; opD[opName].add(opSw.ms()); }
        runFor(rng.chance(20) ? 120 : 15);
        if (total.elapsed() >= qint64(minute + 1) * (fast ? 6000 : 60000)) { ++minute; invariants(minute); }
    }
    // shutdown phase: close everything, verify disk
    for (int pass = 0; pass < 10; ++pass) { for (const QString &r : c.openNotes()) { if (auto *s = c.session(r)) resolveStuck(s); c.closeNote(r); } runFor(300); }
    until([&] { return c.openNotes().isEmpty(); }, 20000);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    note("op counts: " + [&] { QStringList l; for (auto i = opCount.cbegin(); i != opCount.cend(); ++i) l << QString("%1=%2").arg(i.key()).arg(i.value()); return l.join(" "); }());
    for (auto i = opD.cbegin(); i != opD.cend(); ++i) { metric(qPrintable("app.soak.op." + i.key() + ".p95"), i.value().pct(.95), "ms"); metric(qPrintable("app.soak.op." + i.key() + ".max"), i.value().mx(), "ms"); }
    metric("app.soak.ops", double(ops)); metric("app.soak.conflicts_resolved", conflictsResolved);
    metric("app.soak.mod_commands", modRuns); metric("app.soak.mod_commands_ok", modOk);
    check(c.openNotes().isEmpty(), QString("everything closed at the end (%1 sessions left)").arg(c.openNotes().size()));
    check(liveEditors() == 0, QString("no NoteEditor alive after closing everything (%1)").arg(liveEditors()));
    // disk invariants
    int bad = 0, debris = 0, empty = 0;
    QDirIterator it(w.sb.notes, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        if (!it.fileName().endsWith(".md")) { ++debris; continue; }
        const QByteArray b = readFile(it.filePath());
        if (b.isEmpty() && it.fileInfo().size() == 0 && !it.fileName().startsWith("Untitled")) ++empty;
        if (!hn::core::isValidUtf8(b)) ++bad;
    }
    check(bad == 0, QString("all notes valid UTF-8 on disk (%1 invalid)").arg(bad));
    check(debris == 0, QString("no temp/debris files in the library (%1)").arg(debris));
    metric("app.soak.empty_non_untitled_notes", empty);
    check(empty == 0, QString("no unexpectedly empty notes after soak (%1)").arg(empty));
    // plateau analysis over the soak (open-window count is bounded, so PSS must level off)
    if (series.size() >= 6) {
        auto avg = [&](int a, int b) { double s = 0; for (int i = a; i < b; ++i) s += series[i].priv; return s / (b - a); };
        const int n = series.size(), half = n / 2;
        const double early = avg(qMin(5, half - 1), half), late = avg(half, n);
        metric("app.soak.private_early_avg", early, "MiB"); metric("app.soak.private_late_avg", late, "MiB");
        metric("app.soak.private_late_minus_early", late - early, "MiB");
        double mx = 0; for (const auto &x : series) mx = qMax(mx, x.pss);
        metric("app.soak.max_pss", mx, "MiB");
        check(late - early < 15.0, QString("private memory plateau: second-half average %1 MiB higher than first-half (tolerance 15 MiB)").arg(late - early));
    }
    metric("app.soak.max_loop_gap_overall", double(gapMax), "ms");
    note("distinct qWarning/qCritical messages emitted by the app during the soak:");
    for (auto i = g_warnings.cbegin(); i != g_warnings.cend(); ++i) std::printf("WARNLOG %6d x %s\n", i.value(), qPrintable(i.key()));
    metric("app.soak.distinct_warnings", g_warnings.size());
    // leftover recovery drafts
    auto drafts = c.repo().recoverableDrafts();
    metric("app.soak.unresolved_recovery_drafts", drafts.size());
    for (const auto &d : drafts) std::printf("DRAFT %s conflict=%d\n", qPrintable(d.relPath), int(d.conflict));
}


// ---------------------------------------------------------------- 4.5 orphan-session bisect (finds which operation leaves a session without a window)
static int orphans(AppController &c) {
    QSet<QString> shown; for (auto *sx : c.stickies()) shown.insert(sx->session()->rel());
    if (c.organizer() && c.organizer()->currentSession()) shown.insert(c.organizer()->currentSession()->rel());
    int n = 0; for (const QString &r : c.openNotes()) if (!shown.contains(r)) ++n;
    return n;
}
static void orphanProbe() {
    note("== 4.5 orphan-session bisect: which single operation leaves a NoteSession (editor) that no window shows?");
    World w("orphan", 60);
    AppController &c = *w.c;
    c.showOrganizer(false);
    struct Case { const char *name; std::function<void(int)> op; };
    QTimer modalWatch; QObject::connect(&modalWatch, &QTimer::timeout, [&] {   // a modal dialog would freeze this headless run
        if (QWidget *m = QApplication::activeModalWidget()) { std::printf("MODAL-DIALOG %s title='%s' text='%s'\n", m->metaObject()->className(), qPrintable(m->windowTitle()), qPrintable(m->findChildren<QLabel *>().isEmpty() ? QString() : m->findChildren<QLabel *>().first()->text())); std::fflush(stdout); m->close(); }
    }); modalWatch.start(200);
    auto drain = [&] { runFor(60); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); };
    QList<Case> cases = {
        {"openInOrganizer(a); openInOrganizer(b)", [&](int i) { c.openInOrganizer(w.rels[i % 50]); c.openInOrganizer(w.rels[(i + 1) % 50]); }},
        {"openInOrganizer(a); popOut(a); openInOrganizer(b)", [&](int i) { const QString a = w.rels[i % 50]; c.openInOrganizer(a); c.popOut(a); c.openInOrganizer(w.rels[(i + 1) % 50]); }},
        {"openInOrganizer(a); popOut(a); popIn(a)", [&](int i) { const QString a = w.rels[i % 50]; c.openInOrganizer(a); c.popOut(a); c.popIn(a); }},
        {"openSticky(a); openInOrganizer(b); popIn(a)  [organizer occupied]", [&](int i) { const QString a = w.rels[i % 50]; c.openSticky(a, false); c.openInOrganizer(w.rels[(i + 1) % 50]); c.popIn(a); }},
        {"newNote(intoOrganizer=true)", [&](int) { c.newNote({}, true); }},
        {"newNote(); openInOrganizer(new)", [&](int) { const QString n = c.newNote(); c.openInOrganizer(n); }},
        {"openInOrganizer(a); closeNote(a)", [&](int i) { const QString a = w.rels[i % 50]; c.openInOrganizer(a); c.closeNote(a); }},
        {"openInOrganizer(x); n=newNote(); popIn(n); openInOrganizer(y)  [no drain]", [&](int i) { c.openInOrganizer(w.rels[i % 50]); const QString n = c.newNote(); c.popIn(n); c.openInOrganizer(w.rels[(i + 7) % 50]); }},
        {"openInOrganizer(x); n=newNote(); drain; popIn(n); drain; openInOrganizer(y)", [&](int i) { c.openInOrganizer(w.rels[i % 50]); const QString n = c.newNote(); runFor(80); c.popIn(n); runFor(80); c.openInOrganizer(w.rels[(i + 7) % 50]); }},
        {"openSticky(a); popIn(a); openInOrganizer(b); popOut(b)", [&](int i) { const QString a = w.rels[i % 50]; c.openSticky(a, false); c.popIn(a); c.openInOrganizer(w.rels[(i + 3) % 50]); c.popOut(w.rels[(i + 3) % 50]); }},
        {"newNote(); popIn(new); closeNote(new); popOut-less reopen", [&](int i) { const QString n = c.newNote(); c.popIn(n); c.closeNote(n); c.openSticky(n, false); (void)i; }},
        {"newNote() [opens sticky]; deleteNote(new) immediately", [&](int) { const QString n = c.newNote(); c.deleteNote(n); }},
        {"openSticky(a); deleteNote(a)  [existing note]", [&](int i) { const QString a = w.rels[i % 50]; c.openSticky(a, false); c.deleteNote(a); writeFile(w.sb.notes + "/" + a, mixedNote(2048, i)); }},
        {"newNote(); type; deleteNote(new)", [&](int i) { const QString n = c.newNote(); if (auto *sx = c.session(n)) { Rng r(i); typeInto(sx, 5, r); } c.deleteNote(n); }},
        {"openInOrganizer(a); deleteNote(a)", [&](int) { const QString n = c.newNote(); c.openInOrganizer(n); c.deleteNote(n); }},
    };
    for (auto &cs : cases) {
        int bad = 0; QStringList sample;
        for (int i = 0; i < 40; ++i) {
            cs.op(i * 2); drain();
            const int o = orphans(c);
            if (o) { ++bad; if (sample.size() < 2) sample << c.openNotes().join(","); }
            // reset to a clean slate between iterations so a leftover from one case cannot be blamed on the next
            for (const QString &r : c.openNotes()) c.closeNote(r, true);
            drain();
            if (c.organizer() && c.organizer()->currentSession()) c.organizer()->detachSession();
        }
        metric(qPrintable(QString("app.orphan.%1").arg(cs.name).replace(QRegularExpression("[^A-Za-z0-9]+"), "_")), bad);
        note(QString("%1 -> %2 of 40 iterations left a window-less session %3").arg(cs.name).arg(bad).arg(sample.join(" | ")));
        check(bad == 0, QString("[D15-ORPHAN-SESSION] ") + QString("`%1` never leaves a live editor session that no window shows (%2/40)").arg(cs.name).arg(bad));
    }
}

// Hang diagnostics: if the GUI thread stops ticking for 20 s, dump its stack (raw addresses; resolve with addr2line) and abort.
static std::atomic<long> g_beat{0};
static void dumpStack(int) { void *bt[64]; const int n = backtrace(bt, 64); backtrace_symbols_fd(bt, n, 2); }
static void startWatchdog() {
    signal(SIGUSR1, dumpStack);
    const pthread_t mainT = pthread_self();
    std::thread([mainT] {
        long last = -1; int still = 0;
        for (;;) { std::this_thread::sleep_for(std::chrono::seconds(1)); const long b = g_beat.load(); still = (b == last) ? still + 1 : 0; last = b;
            if (still == 20) { std::fprintf(stderr, "WATCHDOG: GUI thread stalled for 20 s, stack follows\n"); pthread_kill(mainT, SIGUSR1); std::this_thread::sleep_for(std::chrono::seconds(2)); std::fprintf(stderr, "WATCHDOG: aborting\n"); std::_Exit(4); } }
    }).detach();
}

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    startWatchdog(); QTimer beat; QObject::connect(&beat, &QTimer::timeout, [] { ++g_beat; }); beat.start(100);
    qInstallMessageHandler(msgHandler);
    app.setQuitOnLastWindowClosed(false);
    const QStringList a = app.arguments();
    const QString mode = a.value(1, "help");
    auto num = [&](int i, int def) { return a.value(i).isEmpty() ? def : a.value(i).toInt(); };
    const bool quick = a.contains("--quick");   // 5 s settle / 5 s cpu / 5 samples, for smoke runs only
    if (mode == "idle") idleLoad(num(2, 10), a.contains("--edit"), quick ? 5 : 30, quick ? 5 : 60, quick ? 5 : 60);
    else if (mode == "binary") binaryLoad(num(2, 10), quick ? 5 : 30, quick ? 5 : 60, quick ? 5 : 60);
    else if (mode == "cycles") { cycles(20, 5, 20, quick ? 50 : 300, 20); cycles(20, 5, 20, quick ? 50 : 300, 400); }
    else if (mode == "orphan") orphanProbe();
    else if (mode == "soak") soak(num(2, 20), a.contains("--fast"));
    else { std::fprintf(stderr, "usage: stress_app idle N [--edit] | binary N | cycles | soak MINUTES [--fast] [--quick]\n"); return 2; }
    return finish("stress_app");
}
