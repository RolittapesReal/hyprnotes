// P8.5 robustness: mutated config/theme/mod-manifest/session files (500 each), corrupt native libraries, instance-transport flood,
// simultaneous launchers, SIGTERM during editing. Standalone; METRIC/PASS/FAIL/XFAIL lines.
#include "controller.h"
#include "hn/core/markdown_codec.h"
#include "hn/mods/mods.h"
#include "hn/platform/instance_transport.h"
#include "hn/platform/window_placement.h"
#include "signal_bridge.h"
#include "stress_util.h"
#include "sticky_window.h"
#include "hn/theme/theme.h"
#include <QApplication>
#include <QDirIterator>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTest>
#include <QTimer>
#include <arpa/inet.h>
#include <csignal>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <dlfcn.h>
#include <malloc.h>

using namespace st;
using namespace hn;

static int g_styleWarnings = 0, g_otherWarnings = 0;
static void msgHandler(QtMsgType t, const QMessageLogContext &, const QString &m) {
    if (t == QtDebugMsg || t == QtInfoMsg) return;
    if (m.contains("Could not parse")) ++g_styleWarnings; else ++g_otherWarnings;
}
static void runFor(int ms) { QEventLoop l; QTimer::singleShot(ms, &l, &QEventLoop::quit); l.exec(); }
template <class F> static bool until(F cond, int timeoutMs) { QElapsedTimer t; t.start(); while (!cond()) { if (t.elapsed() > timeoutMs) return false; runFor(10); } return true; }

// ---------------------------------------------------------------- mutation engine
static QByteArray mutate(const QByteArray &base, Rng &rng, int *kindOut = nullptr) {
    static const QList<QByteArray> literals = {"null", "true", "false", "0", "-1", "1e999", "99999999999999999999", "\"\"", "\"x\"", "[]", "{}", "[1,2]", "{\"a\":1}",
        "\"\\u0000\"", "\"../../etc/passwd\"", "\"/\"", "[\"a\",\"a\"]", "[100,100]", "[-5,99999]", "[1e30,1e30]", "\"#zzzzzz\"", "\"#12\"", "\"System\"", "-0.0", "\"\\ud800\""};
    QByteArray b = base;
    const int k = rng.bounded(11);
    if (kindOut) *kindOut = k;
    switch (k) {
    case 0: for (int i = 0, n = 1 + rng.bounded(8); i < n && !b.isEmpty(); ++i) b[rng.bounded(b.size())] = char(rng.bounded(256)); break;
    case 1: b.truncate(rng.bounded(b.size() + 1)); break;
    case 2: { if (b.size() > 2) { const int a = rng.bounded(b.size()), n = rng.bounded(qMin<int>(40, b.size() - a)); b.remove(a, n); } break; }
    case 3: { if (b.size() > 2) { const int a = rng.bounded(b.size()), n = rng.bounded(qMin<int>(60, b.size() - a)); b.insert(rng.bounded(b.size()), b.mid(a, n)); } break; }
    case 4: {   // replace one JSON scalar/array/object value with a hostile literal
        QList<int> colons; for (int i = 0; i < b.size(); ++i) if (b[i] == ':') colons << i;
        if (!colons.isEmpty()) {
            const int c = colons[rng.bounded(colons.size())];
            int e = c + 1; int depth = 0; bool str = false;
            for (; e < b.size(); ++e) { const char ch = b[e]; if (str) { if (ch == '\\') ++e; else if (ch == '"') str = false; continue; }
                if (ch == '"') str = true; else if (ch == '[' || ch == '{') ++depth; else if (ch == ']' || ch == '}') { if (depth == 0) break; --depth; } else if (ch == ',' && depth == 0) break; }
            b.replace(c + 1, e - c - 1, " " + literals[rng.bounded(literals.size())]);
        }
        break; }
    case 5: { QByteArray g; for (int i = 0, n = 1 + rng.bounded(40); i < n; ++i) g += char(rng.bounded(256)); b.insert(rng.bounded(b.size() + 1), g); break; }
    case 6: { const int n = 1000 + rng.bounded(60000); b = QByteArray(n, '[') + b + QByteArray(rng.bounded(n), ']'); break; }
    case 7: b += QByteArray(1 + rng.bounded(100), char(rng.bounded(256))); break;
    case 8: { const int v = rng.bounded(5); b = v == 0 ? QByteArray() : v == 1 ? QByteArray("   \n") : v == 2 ? QByteArray("\xEF\xBB\xBF") + b : v == 3 ? QByteArray("\xFF\xFE{\0\"\0", 6) : QByteArray(b.size(), '\0'); break; }
    case 9: { const int at = b.indexOf('"', rng.bounded(b.size() / 2 + 1)); if (at >= 0) b.insert(at + 1, QByteArray(rng.chance(30) ? (4 << 20) : 1 << 16, 'A')); break; }
    default: { QList<int> q; for (int i = 0; i < b.size(); ++i) if (b[i] == '"') q << i; if (!q.isEmpty()) b.remove(q[rng.bounded(q.size())], 1); break; }
    }
    return b;
}

// ---------------------------------------------------------------- 5.1 config.json
static void configFuzz() {
    note("== 5.1 config.json: 500 mutated files through Config::reload (last-valid retained) + 150 through a live controller watcher");
    Sandbox sb("rb-config");
    const QString path = sb.config + "/config.json";
    theme::Config cfg(path);
    cfg.save(cfg.settings());
    const QByteArray base = readFile(path);
    check(!base.isEmpty(), "valid base config written");
    Rng rng(500);
    int rejected = 0, accepted = 0, keptViolations = 0, invariantViolations = 0; Dist ms; int hardFail = 0;
    theme::Settings prev = cfg.settings();
    for (int i = 0; i < 500; ++i) {
        int kind = 0; const QByteArray m = mutate(base, rng, &kind);
        writeFile(path, m);
        Stopwatch sw; const bool ok = cfg.reload(); ms.add(sw.ms());
        const bool strict = cfg.lastError().startsWith("malformed") || cfg.lastError().startsWith("unsupported");
        if (!ok && strict) { ++rejected; if (!(cfg.settings() == prev)) ++keptViolations; }
        else if (!ok) ++rejected; else ++accepted;
        const auto s = cfg.settings();
        const bool good = s.stickySize.width() >= 100 && s.stickySize.height() >= 100 && s.stickySize.width() <= 10000 && s.stickyMin.width() >= 100 && s.organizerSize.width() >= 200
            && (s.colorScheme == "system" || s.colorScheme == "light" || s.colorScheme == "dark") && !s.theme.isEmpty() && !s.theme.contains('/') && !s.theme.contains("..")
            && QDir::isAbsolutePath(s.notesFolder);
        if (!good) { ++invariantViolations; note(QString("invariant violated after mutation kind %1 (theme='%2' scheme='%3' notes='%4')").arg(kind).arg(s.theme, s.colorScheme, s.notesFolder)); }
        prev = s; (void)hardFail;
    }
    ms.report("robust.config.reload"); metric("robust.config.rejected", rejected); metric("robust.config.accepted", accepted);
    check(true, "500 mutated configs processed without crash");
    check(keptViolations == 0, QString("malformed/unsupported file keeps previous settings (%1 violations)").arg(keptViolations));
    check(invariantViolations == 0, QString("settings always satisfy validation invariants (%1 violations)").arg(invariantViolations));
    check(ms.mx() < 2000, QString("no mutated config takes > 2 s to parse (max %1 ms; includes 4 MiB blobs and 60k-deep nesting)").arg(ms.mx()));

    // live controller watching the same file

    app::ControllerOptions o; o.notesDir = sb.notes; o.stateDir = sb.state; o.cacheDir = sb.cache; o.configPath = path; o.modsDir = sb.data + "/mods"; o.modsEnabledPath = sb.config + "/me.json";
    o.useTray = false; o.useHyprland = false; o.confirm = [](const QString &, const QString &) { return true; };
    o.recoveryPrompt = [](const QList<core::RecoveryEntry> &) { return app::RecoveryChoice::Later; };
    replaceFile(path, base);
    app::AppController c(o);
    writeFile(sb.notes + "/a.md", "# A\n\nhello\n");
    c.openSticky("a.md", false);
    for (int i = 0; i < 150; ++i) { replaceFile(path, mutate(base, rng)); runFor(rng.chance(30) ? 80 : 15); }
    replaceFile(path, base); runFor(300);
    check(c.stickyOf("a.md") != nullptr, "controller still has its window after 150 live config mutations");
    const auto s = c.settings();
    check(s.stickySize.width() >= 100 && (s.colorScheme == "system" || s.colorScheme == "light" || s.colorScheme == "dark"), "controller settings valid after live fuzz");
    check(c.openSticky("a.md", false) != nullptr, "controller still functional");
}

// ---------------------------------------------------------------- 5.2 theme files
static void themeFuzz() {
    note("== 5.2 theme files: 500 mutated themes + targeted QSS injection cases");
    Sandbox sb("rb-theme");
    const QString dir = sb.dir.filePath("xdg-config/hyprnotes/themes"); QDir().mkpath(dir);
    const QJsonObject tok{{"bg", "#101010"}, {"surface", "#181818"}, {"text", "#eeeeee"}, {"muted", "#999999"}, {"accent", "#ff5522"}, {"accentText", "#000000"}, {"border", "#333333"},
        {"danger", "#cc0000"}, {"success", "#00cc00"}, {"selection", "#444444"}, {"noteAccent", QJsonArray{"#111111", "#222222", "#333333", "#444444", "#555555", "#666666"}},
        {"fontFamily", "Noto Sans"}, {"monoFamily", "monospace"}, {"baseSize", 14}, {"padding", 16}, {"radius", 0}, {"borderWidth", 1}, {"lineHeight", 1.5}};
    const QByteArray base = QJsonDocument(QJsonObject{{"version", 1}, {"base", "modernist"}, {"dark", tok}, {"light", tok}}).toJson();
    Rng rng(501);
    int failedLoads = 0, okLoads = 0, badTheme = 0, noErrMsg = 0; Dist ms;
    for (int i = 0; i < 500; ++i) {
        writeFile(dir + "/fz.json", mutate(base, rng));
        Stopwatch sw; const theme::Theme t = theme::loadTheme("fz", i % 2); ms.add(sw.ms());
        const bool err = !theme::lastThemeError().isEmpty();
        if (err) ++failedLoads; else ++okLoads;
        bool good = t.bg.isValid() && t.surface.isValid() && t.text.isValid() && t.muted.isValid() && t.accent.isValid() && t.accentText.isValid() && t.border.isValid() && t.danger.isValid()
                    && t.success.isValid() && t.selection.isValid() && t.baseSize >= 8 && t.baseSize <= 32 && t.lineHeight >= 1.0 && t.lineHeight <= 3.0 && t.radius >= 0 && !t.fontFamily.isEmpty();
        for (int k = 0; k < 6; ++k) good &= t.noteAccent[k].isValid();
        if (!good) ++badTheme;
        if (!err && t.bg == theme::loadTheme("modernist", i % 2).bg && false) ++noErrMsg;
    }
    ms.report("robust.theme.load"); metric("robust.theme.fell_back_with_error", failedLoads); metric("robust.theme.loaded", okLoads);
    check(badTheme == 0, QString("every loadTheme result is a complete, in-range Theme (%1 bad)").arg(badTheme));
    check(ms.mx() < 2000, QString("no mutated theme takes > 2 s (max %1 ms)").arg(ms.mx()));
    (void)noErrMsg;
    // targeted: font-family strings that could break out of / break the application stylesheet
    int parseBreaks = 0; QStringList breaking;
    for (const QString &ff : QStringList{"Foo\\", "Foo\\\\\\", "}", "Foo; } QWidget { background: red; ", "Foo\nBar", "Foo\x01", QString(200000, 'x'), "a'b", "Foo\\00"}) {
        QJsonObject t2 = tok; t2["fontFamily"] = ff;
        writeFile(dir + "/inj.json", QJsonDocument(QJsonObject{{"version", 1}, {"dark", t2}, {"light", t2}}).toJson());
        const theme::Theme t = theme::loadTheme("inj", true);
        if (!theme::lastThemeError().isEmpty()) continue;   // rejected: fine
        g_styleWarnings = 0;
        theme::applyTheme(t);
        QWidget w; w.show(); QCoreApplication::processEvents();
        if (g_styleWarnings > 0) { ++parseBreaks; breaking << ff.left(20).toHtmlEscaped(); }
        theme::applyTheme(theme::loadTheme("modernist", true));
    }
    check(parseBreaks == 0, QString("[D11-THEME-QSS] ") + QString("theme fontFamily values cannot break the application stylesheet (%1 accepted values made Qt fail to parse the QSS: %2)").arg(parseBreaks).arg(breaking.join(" | ")));
    // theme through the live controller: unknown / broken theme names fall back, app keeps running
    app::ControllerOptions o; o.notesDir = sb.notes; o.stateDir = sb.state; o.cacheDir = sb.cache; o.configPath = sb.config + "/config.json";
    o.useTray = false; o.useHyprland = false; o.confirm = [](const QString &, const QString &) { return true; };
    app::AppController c(o);
    writeFile(sb.notes + "/a.md", "# A\n"); c.openSticky("a.md", false);
    for (int i = 0; i < 100; ++i) { writeFile(dir + "/fz.json", mutate(base, rng)); auto s = c.settings(); s.theme = (i % 3) ? "fz" : "nonexistent"; s.colorScheme = (i % 2) ? "dark" : "light"; c.applySettings(s, c.prefs()); }
    check(c.stickyOf("a.md") != nullptr, "controller survives 100 theme applications of mutated themes");
}

// ---------------------------------------------------------------- 5.3 mods
namespace {
struct NullBridge : mods::DocumentBridge {
    QString selectionText() override { return "abc"; }
    void beginTransaction(const QString &) override {}
    void replaceSelection(const QString &) override {}
    void insertText(const QString &) override {}
    void endTransaction() override {}
};
}
static int childLib(const QString &modsRoot);
static void modFuzz() {
    note("== 5.3 mod manifests (500), enabled.json (500), corrupt native libraries");
    Sandbox sb("rb-mods");
    QFile mf(QString(HN_EXAMPLE_MOD_SRC) + "/mod.json"); if (!mf.open(QIODevice::ReadOnly)) return;
    const QByteArray base = mf.readAll();
    check(!base.isEmpty(), "example mod.json readable");
    Rng rng(502);
    int accepted = 0, rejected = 0, invariant = 0; Dist ms;
    for (int i = 0; i < 500; ++i) {
        const QString dir = sb.data + "/m/uppercase-selection"; QDir().mkpath(dir);
        const QByteArray m = mutate(base, rng);
        mods::Manifest out; QList<mods::ModError> errs;
        Stopwatch sw; const bool ok = mods::parseManifest(m, dir, &out, &errs); ms.add(sw.ms());
        if (ok) {
            ++accepted;
            static const QRegularExpression idre("^[a-z0-9][a-z0-9._-]*$");
            if (out.id != "uppercase-selection" || out.library.startsWith('/') || out.library.contains("..") || out.hostApiVersion != 1 || out.activation.isEmpty()) ++invariant;
        } else { ++rejected; if (errs.isEmpty()) ++invariant; }
    }
    ms.report("robust.mods.parseManifest"); metric("robust.mods.accepted", accepted); metric("robust.mods.rejected", rejected);
    check(invariant == 0, QString("parseManifest: accepted manifests satisfy invariants, rejections carry an error (%1 violations)").arg(invariant));
    // directory of 500 mutated mods: scanManifests + ModHost keep the valid one loadable
    QDir(sb.data + "/m").removeRecursively();
    const QString modsDir = sb.data + "/mods";
    QStringList enabled;
    for (int i = 0; i < 500; ++i) {
        const QString id = i == 0 ? "uppercase-selection" : QString("fz%1").arg(i);
        const QString d = modsDir + "/" + id; QDir().mkpath(d);
        QByteArray m = i == 0 ? base : mutate(QByteArray(base).replace("uppercase-selection", id.toUtf8()), rng);
        writeFile(d + "/mod.json", m);
        enabled << id;
    }
    QFile::copy(QString(HN_EXAMPLE_MOD_DIR) + "/libuppercase_selection.so", modsDir + "/uppercase-selection/libuppercase_selection.so");
    writeFile(sb.config + "/mods-enabled.json", QJsonDocument(QJsonObject{{"version", 1}, {"enabled", QJsonArray::fromStringList(enabled)}}).toJson());
    QList<mods::ModError> errs; Stopwatch sw;
    const auto manifests = mods::scanManifests(modsDir, &errs);
    metric("robust.mods.scan_500_ms", sw.ms(), "ms"); metric("robust.mods.scan_valid", manifests.size()); metric("robust.mods.scan_errors", errs.size());
    check(manifests.size() >= 1 && std::any_of(manifests.begin(), manifests.end(), [](const mods::Manifest &m) { return m.id == "uppercase-selection"; }), "valid mod still discovered among 499 mutated neighbours");
    mods::ModHost host; host.start(modsDir, sb.config + "/mods-enabled.json");
    NullBridge nb;
    check(host.runCommand("uppercase-selection", &nb), "valid mod command still runs next to 499 mutated mods");
    check(!host.runCommand("../../nope", &nb) && !host.runCommand(QString(100000, 'x'), &nb) && !host.runCommand({}, &nb), "hostile command ids are rejected");
    metric("robust.mods.host_errors", host.errors().size());
    check(host.errors().size() < 20000, "error list is bounded");
    // enabled.json fuzz
    const QByteArray ebase = readFile(sb.config + "/mods-enabled.json").left(200);
    Rng r2(503); int bad = 0;
    for (int i = 0; i < 500; ++i) {
        const QByteArray m = mutate(QJsonDocument(QJsonObject{{"version", 1}, {"enabled", QJsonArray{"uppercase-selection", "x"}}}).toJson(), r2);
        writeFile(sb.config + "/e.json", m);
        QList<mods::ModError> e; const QStringList l = mods::loadEnabled(sb.config + "/e.json", &e);
        for (const QString &s : l) if (s.isEmpty() && false) ++bad;
    }
    check(bad == 0, "500 mutated enabled.json files load without crash");
    (void)ebase;

    // corrupt native libraries: dlopen runs foreign code; isolate each attempt in a child process
    const QByteArray so = readFile(QString(HN_EXAMPLE_MOD_DIR) + "/libuppercase_selection.so");
    auto runHost = [&](const QString &tag, const std::function<void(const QString &)> &place, int waitMs, bool *hungOut) {
        const QString root = sb.dir.filePath("lib-" + tag); QDir().mkpath(root + "/mods/uppercase-selection");
        writeFile(root + "/mods/uppercase-selection/mod.json", base);
        writeFile(root + "/enabled.json", R"({"version":1,"enabled":["uppercase-selection"]})");
        place(root + "/mods/uppercase-selection/libuppercase_selection.so");
        QProcess pr; pr.setProgram(QCoreApplication::applicationFilePath()); pr.setArguments({"--child-lib", root}); pr.start();
        const bool done = pr.waitForFinished(waitMs);
        if (!done) { pr.kill(); pr.waitForFinished(); }
        QDir(root).removeRecursively();
        *hungOut = !done;
        return done ? (pr.exitStatus() == QProcess::CrashExit ? 2 : 1) : 0;
    };
    int crashed = 0, hung = 0, clean = 0, crashTrunc = 0, crashRandom = 0, crashHdr = 0; Rng r3(504);
    for (int i = 0; i < 120; ++i) {
        bool h = false;
        const int res = runHost(QString::number(i), [&](const QString &dst) {
            QByteArray b = so;
            if (i < 20) b.truncate(i * so.size() / 20);
            else if (i < 30) b = QByteArray(r3.bounded(4096), char(r3.bounded(256)));
            else if (i == 30) b.clear();
            else for (int k = 0, n = 1 + r3.bounded(16); k < n; ++k) b[r3.bounded(qMin<int>(b.size(), 4096))] = char(r3.bounded(256));   // damage ELF headers
            writeFile(dst, b);
        }, 8000, &h);
        if (h) ++hung; else if (res == 2) { ++crashed; (i < 20 ? crashTrunc : i <= 30 ? crashRandom : crashHdr)++; } else ++clean;
    }
    metric("robust.mods.corrupt_lib.clean_error", clean); metric("robust.mods.corrupt_lib.crashed", crashed); metric("robust.mods.corrupt_lib.hung", hung);
    note(QString("host crashes by corruption kind: truncated-at-N/20 of a valid .so %1/20, random garbage/empty %2/11, damaged ELF header bytes %3/89").arg(crashTrunc).arg(crashRandom).arg(crashHdr));
    metric("robust.mods.corrupt_lib.crash_truncated", crashTrunc); metric("robust.mods.corrupt_lib.crash_garbage", crashRandom); metric("robust.mods.corrupt_lib.crash_header_damage", crashHdr);
    check(crashTrunc == 0, QString("[D12-CORRUPT-SO] ") + QString("a truncated/partially written mod library (e.g. interrupted upgrade) is rejected without taking the whole app down: %1 of 20 truncations crashed the host inside dlopen").arg(crashTrunc));
    soft(crashed == 0, QString("120 corrupt .so files through the real ModHost: %1 crashed the host inside dlopen/ld.so (native mods are trusted code; informational)").arg(crashed));
    check(hung == 0, QString("corrupt libraries never hang the host (%1 hung)").arg(hung));
    // FIFO as library: dlopen() would block forever on open(2)
    {
        bool h = false;
        runHost("fifo", [&](const QString &dst) { ::mkfifo(QFile::encodeName(dst).constData(), 0600); }, 5000, &h);
        check(!h, "a FIFO named as the mod library does not freeze the host when the mod is activated");
    }
}
static int childLib(const QString &modsRoot) {
    // The real host path: manifest scan + lazy activation (dlopen of the manifest library) on runCommand.
    mods::ModHost host; host.start(modsRoot + "/mods", modsRoot + "/enabled.json");
    NullBridge nb;
    return host.runCommand("uppercase-selection", &nb) ? 0 : 0;   // failure to load is a clean (error-reported) outcome, not a crash
}

// ---------------------------------------------------------------- 5.4 session.json
static void sessionFuzz() {
    note("== 5.4 session.json: 500 mutated files via loadSession, 60 via real startup restore");
    Sandbox sb("rb-session");
    for (int i = 0; i < 30; ++i) writeFile(sb.notes + QString("/n%1.md").arg(i), mixedNote(4096, i));
    platform::SessionState st; 
    for (int i = 0; i < 12; ++i) { platform::WindowState w; w.noteKey = QString("n%1.md").arg(i); w.geometry = QRect(10 * i, 10 * i, 360, 300); w.scale = 1; st.windows.push_back(w); }
    const QByteArray base = platform::toJson(st);
    Rng rng(505); int okc = 0, bad = 0; Dist ms;
    for (int i = 0; i < 500; ++i) {
        const QString p = sb.state + "/session.json"; writeFile(p, mutate(base, rng));
        bool ok = false; Stopwatch sw; const auto s = platform::loadSession(p, &ok); ms.add(sw.ms());
        okc += ok;
        for (const auto &w : s.windows) if (w.geometry.width() < 0 || w.geometry.height() < 0) ++bad;
    }
    ms.report("robust.session.loadSession"); metric("robust.session.parsed_ok", okc);
    check(bad == 0, "no negative window sizes survive parsing");
    // through the controller: restore must neither crash nor open windows for escaping/missing notes
    int restoredWeird = 0; Dist startup; int crashes = 0;
    const QStringList evil = {"../../etc/passwd", "/etc/passwd", "n1.md/../../x.md", "", "n0.md\\u0000.txt", "nonexistent.md", QString(5000, 'a') + ".md"};
    for (int i = 0; i < 60; ++i) {
        QByteArray m = mutate(base, rng);
        if (i % 6 == 0) { platform::SessionState s2 = st; for (const QString &e : evil) { platform::WindowState w; w.noteKey = e; w.geometry = QRect(0, 0, 360, 300); s2.windows.push_back(w); } m = platform::toJson(s2); }
        if (i % 10 == 1) { platform::SessionState s2; for (int k = 0; k < 400; ++k) { platform::WindowState w; w.noteKey = QString("n%1.md").arg(k % 30); w.geometry = QRect(k, k, 1 << 30, -5); s2.windows.push_back(w); } m = platform::toJson(s2); }
        writeFile(sb.state + "/session.json", m);
        app::ControllerOptions o; o.notesDir = sb.notes; o.stateDir = sb.state; o.cacheDir = sb.cache; o.configPath = sb.config + "/config.json"; o.modsDir = sb.data + "/mods";
        o.useTray = false; o.useHyprland = false; o.confirm = [](const QString &, const QString &) { return true; };
        o.recoveryPrompt = [](const QList<core::RecoveryEntry> &) { return app::RecoveryChoice::Later; };
        auto c = std::make_unique<app::AppController>(o);
        Stopwatch sw; c->handleAction(platform::Action::ShowOrganizer); runFor(30); startup.add(sw.ms());
        for (const QString &r : c->openNotes()) if (r.contains("..") || r.startsWith('/') || !QFile::exists(c->repo().absolutePath(r))) ++restoredWeird;
        c.reset(); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    startup.report("robust.session.restore_startup");
    check(restoredWeird == 0, QString("restore never opens escaping or missing notes (%1)").arg(restoredWeird));
    check(crashes == 0, "60 startups with mutated session.json: no crash");
    // no cap on restored window count: a corrupt/huge session file opens one window per distinct entry
    {
        for (int i = 30; i < 200; ++i) writeFile(sb.notes + QString("/n%1.md").arg(i), mixedNote(4096, i));
        platform::SessionState s2; for (int k = 0; k < 200; ++k) { platform::WindowState w; w.noteKey = QString("n%1.md").arg(k); w.geometry = QRect(0, 0, 360, 300); s2.windows.push_back(w); }
        writeFile(sb.state + "/session.json", platform::toJson(s2));
        app::ControllerOptions o; o.notesDir = sb.notes; o.stateDir = sb.state; o.cacheDir = sb.cache; o.configPath = sb.config + "/config.json"; o.modsDir = sb.data + "/mods";
        o.useTray = false; o.useHyprland = false; o.recoveryPrompt = [](const QList<core::RecoveryEntry> &) { return app::RecoveryChoice::Later; };
        app::AppController c(o); Stopwatch sw; const Mem m0 = mem();
        c.handleAction(platform::Action::ShowOrganizer); runFor(100);
        metric("robust.session.restore_200_windows_ms", sw.ms(), "ms"); metric("robust.session.restore_200_windows_pss_delta", mem().total() - m0.total(), "MiB");
        metric("robust.session.restore_200_windows_opened", c.stickies().size());
        check(c.stickies().size() <= 30, QString("[D13-RESTORE-UNCAPPED] ") + QString("a session file listing 200 notes restores %1 windows synchronously at startup (%2 ms, no cap or lazy restore)").arg(c.stickies().size()).arg(sw.ms()));
    }
}

// ---------------------------------------------------------------- 5.5 transport flood (child process holds the client side)
static int childFlood(const QString &path, int n, int mode, int holdMs) {
    std::vector<int> fds;
    sockaddr_un a{}; a.sun_family = AF_UNIX; strncpy(a.sun_path, QFile::encodeName(path).constData(), sizeof(a.sun_path) - 1);
    Rng rng(mode * 77 + n);
    int refused = 0, closedByServer = 0, replies = 0, bad = 0;
    for (int i = 0; i < n; ++i) {
        int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (::connect(fd, reinterpret_cast<sockaddr *>(&a), sizeof a) != 0) { ++refused; ::close(fd); continue; }
        QByteArray payload;
        const int kind = mode == 0 ? 99 : rng.bounded(9);   // mode 0: idle slow-loris
        switch (kind) {
        case 0: for (int k = 0, m = 1 + rng.bounded(4000); k < m; ++k) payload += char(rng.bounded(256)); break;
        case 1: payload = QByteArray(1 + rng.bounded(100000), 'A'); break;                      // oversize, no newline
        case 2: payload = "HNIPC 99 show-organizer\n"; break;                                    // wrong version
        case 3: payload = "HNIPC 1 rm -rf /\n"; break;
        case 4: payload = "HNIPC 1 new-note"; break;                                             // partial, never completed
        case 5: payload = "HNIPC 1 new-note\n"; payload = payload.left(payload.size() - 1); break;
        case 6: payload = QByteArray("\0\0\0\n", 4); break;
        case 7: payload = "GET / HTTP/1.1\r\n\r\n"; break;
        case 8: payload = "HNIPC 1 " + QByteArray(300, 'x') + "\n"; break;
        default: break;
        }
        if (!payload.isEmpty()) (void)::send(fd, payload.constData(), payload.size(), MSG_NOSIGNAL);
        fds.push_back(fd);
        if (mode == 2 && i % 3 == 0) { ::close(fds.back()); fds.pop_back(); }   // abrupt disconnects
    }
    // hold, then see which the server closed
    QElapsedTimer t; t.start();
    while (t.elapsed() < holdMs) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    for (int fd : fds) {
        char buf[256]; ::fcntl(fd, F_SETFL, O_NONBLOCK);
        QByteArray all; ssize_t r; bool eof = false;
        while ((r = ::recv(fd, buf, sizeof buf, 0)) > 0) all += QByteArray(buf, int(r));
        if (r == 0) eof = true;
        if (eof) ++closedByServer;
        if (!all.isEmpty()) { ++replies; if (all.startsWith("HNIPC 1 ok")) ++bad; }
        ::close(fd);
    }
    std::printf("FLOOD refused=%d held=%zu closedByServer=%d replies=%d okReplies=%d\n", refused, fds.size(), closedByServer, replies, bad);
    return 0;
}
static void transportFlood() {
    note("== 5.5 instance transport: 1000 garbage connections (mixed payloads), 900 idle slow-loris, abrupt disconnects, 50 valid commands interleaved");
    Sandbox sb("rb-transport");
    const QString ep = sb.runtime + "/hn.sock";
    platform::InstanceServer server(ep);
    int handled = 0; QList<platform::Action> seen;
    server.setHandler([&](platform::Action a) { ++handled; seen << a; return true; });
    QString why; check(server.listen(&why) == platform::InstanceServer::Result::Listening, "server listening " + why);
    const int fd0 = openFds(); const Mem m0 = mem();
    auto runChild = [&](int n, int mode, int hold) {
        auto *p = new QProcess; p->setProgram(QCoreApplication::applicationFilePath());
        p->setArguments({"--child-flood", ep, QString::number(n), QString::number(mode), QString::number(hold)}); p->start(); return p;
    };
    // 1) 1000 garbage (mode 1) while valid launchers hit the server
    QProcess *flood = runChild(1000, 1, 2500);
    Dist validMs; int acked = 0;
    for (int i = 0; i < 50; ++i) { runFor(40); Stopwatch sw; const auto r = platform::sendAction(platform::Action::Background, ep); validMs.add(sw.ms()); acked += r.status == platform::ClientResult::Status::Acked; }
    until([&] { return flood->state() == QProcess::NotRunning; }, 30000);
    const QString out1 = QString::fromUtf8(flood->readAllStandardOutput()).trimmed(); note(out1);
    validMs.report("robust.transport.valid_roundtrip_under_flood");
    metric("robust.transport.valid_acked", acked);
    check(acked == 50, QString("all 50 valid commands acked during the garbage flood (%1/50)").arg(acked));
    check(handled == acked, QString("handler invoked exactly once per valid command and never for garbage (%1 calls, %2 valid)").arg(handled).arg(acked));
    check(!out1.contains("okReplies=") || out1.contains("okReplies=0"), "garbage never receives an ok ack");
    delete flood;
    runFor(2500);
    // 2) 900 idle connections held 4 s (slow-loris) -> server must drop them at the ack deadline
    QProcess *loris = runChild(900, 0, 4000);
    runFor(2000);
    const auto mid = platform::sendAction(platform::Action::Background, ep, 1500);
    until([&] { return loris->state() == QProcess::NotRunning; }, 30000);
    const QString out2 = QString::fromUtf8(loris->readAllStandardOutput()).trimmed(); note(out2);
    note(QString("legit launcher while 900 idle connections were open: status=%1 %2").arg(int(mid.status)).arg(mid.message));
    metric("robust.transport.slowloris_legit_status", int(mid.status));
    check(mid.status == platform::ClientResult::Status::Acked, "legit launcher is still served while 900 idle connections are held (fd exhaustion check)");
    const QRegularExpression re("closedByServer=(\\d+)"); const int closed = re.match(out2).captured(1).toInt();
    check(closed >= 800, QString("server drops idle connections by the deadline (%1 of 900 closed by the server within 4 s)").arg(closed));
    delete loris;
    // 3) abrupt disconnects
    QProcess *abrupt = runChild(600, 2, 300); until([&] { return abrupt->state() == QProcess::NotRunning; }, 30000); delete abrupt;
    runFor(2500);
    const auto after = platform::sendAction(platform::Action::Background, ep);
    check(after.status == platform::ClientResult::Status::Acked, "server still serves a legit launcher after the flood");
    metric("robust.transport.fds_before", fd0); metric("robust.transport.fds_after", openFds());
    check(openFds() <= fd0 + 2, QString("no leaked connection fds after flood (%1 before, %2 after)").arg(fd0).arg(openFds()));
    malloc_trim(0);
    const double g1 = mem().total() - m0.total();
    metric("robust.transport.pss_growth_after_first_flood", g1, "MiB");
    // second identical flood: a leak would grow again, allocator caching would not
    const Mem m1 = mem();
    QProcess *again = runChild(1000, 1, 1500); until([&] { return again->state() == QProcess::NotRunning; }, 30000); delete again;
    QProcess *again2 = runChild(900, 0, 2500); until([&] { return again2->state() == QProcess::NotRunning; }, 30000); delete again2;
    runFor(2000); malloc_trim(0);
    const double g2 = mem().total() - m1.total();
    metric("robust.transport.pss_growth_second_flood", g2, "MiB");
    check(g2 < 3.0, QString("server memory plateaus: first flood +%1 MiB, an identical second flood +%2 MiB (< 3 MiB)").arg(g1).arg(g2));
    check(openFds() <= fd0 + 2, "no leaked fds after the second flood either");
    check(std::all_of(seen.begin(), seen.end(), [](platform::Action a) { return a == platform::Action::Background; }), "only Background actions were ever dispatched");
}

// ---------------------------------------------------------------- 5.6 simultaneous launchers (real binary)
static void launchers() {
    note("== 5.6 12 simultaneous launchers racing to become the single instance (real binary), then 12 simultaneous --new-note against the running instance");
    for (int round = 0; round < 3; ++round) {
        Sandbox sb(QString("rb-launch%1").arg(round));
        QList<QProcess *> ps;
        for (int i = 0; i < 12; ++i) {
            auto *p = new QProcess; p->setProgram(HN_BIN); p->setArguments({"--new-note"}); p->setProcessChannelMode(QProcess::MergedChannels); ps << p;
        }
        for (auto *p : ps) p->start();           // started back to back: genuine race for the endpoint
        runFor(6000);
        int alive = 0, exit0 = 0, exit1 = 0, crashed = 0; QStringList msgs;
        for (auto *p : ps) {
            if (p->state() == QProcess::Running) ++alive;
            else if (p->exitStatus() == QProcess::CrashExit) ++crashed;
            else if (p->exitCode() == 0) ++exit0; else { ++exit1; msgs << QString::fromUtf8(p->readAll()).trimmed().left(120); }
        }
        const int notes = QDir(sb.notes).entryList({"*.md"}, QDir::Files).size();
        note(QString("round %1: alive=%2 exit0=%3 exit1=%4 crashed=%5 notesCreated=%6 %7").arg(round).arg(alive).arg(exit0).arg(exit1).arg(crashed).arg(notes).arg(msgs.join(" | ")));
        metric(qPrintable(QString("robust.launch.round%1.alive").arg(round)), alive); metric(qPrintable(QString("robust.launch.round%1.notes").arg(round)), notes);
        check(alive == 1, QString("round %1: exactly one persistent instance (%2 alive)").arg(round).arg(alive));
        check(crashed == 0, QString("round %1: no launcher crashed").arg(round));
        check(notes == alive + exit0, QString("round %1: each launcher that succeeded created exactly one note, none replayed (%2 notes for %3 successes)").arg(round).arg(notes).arg(alive + exit0));
        if (round == 0) {   // second phase against the now-running instance
            QList<QProcess *> ps2;
            for (int i = 0; i < 12; ++i) { auto *p = new QProcess; p->setProgram(HN_BIN); p->setArguments({"--new-note"}); ps2 << p; }
            for (auto *p : ps2) p->start();
            runFor(5000);
            int ok = 0, fail = 0; for (auto *p : ps2) { if (p->state() == QProcess::NotRunning && p->exitStatus() == QProcess::NormalExit && p->exitCode() == 0) ++ok; else ++fail; }
            const int notes2 = QDir(sb.notes).entryList({"*.md"}, QDir::Files).size();
            note(QString("against running instance: acked=%1 other=%2 notes %3 -> %4").arg(ok).arg(fail).arg(notes).arg(notes2));
            check(notes2 - notes == ok, QString("12 simultaneous --new-note: notes created (%1) == acked launchers (%2): no replay, no loss").arg(notes2 - notes).arg(ok));
            qDeleteAll(ps2);
        }
        for (auto *p : ps) if (p->state() == QProcess::Running) { p->terminate(); if (!p->waitForFinished(8000)) { p->kill(); p->waitForFinished(); } }
        qDeleteAll(ps);
    }
}

// ---------------------------------------------------------------- 5.7 SIGTERM during editing
static int childEdit(const QString &notes, const QString &state, const QString &cache, const QString &cfg, int pauseBeforeReadyMs) {
    app::ControllerOptions o; o.notesDir = notes; o.stateDir = state; o.cacheDir = cache; o.configPath = cfg; o.useTray = false; o.useHyprland = false;
    o.confirm = [](const QString &, const QString &) { return true; };
    app::AppController c(o);
    QObject::connect(&c, &app::AppController::exitRequested, qApp, &QApplication::quit);
    app::SignalBridge bridge;   // same wiring as main.cpp
    QObject::connect(&bridge, &app::SignalBridge::quitSignal, &c, [&](int) { c.requestQuit(); });
    auto *s = c.openSticky("victim.md", false);
    QWidget *w = s->editor()->activeEdit();
    s->editor()->focusEditor();
    QTest::keyClicks(w, "TYPED-TEXT-MARKER");
    std::printf("TYPED\n"); std::fflush(stdout);
    Q_UNUSED(pauseBeforeReadyMs);
    return qApp->exec();
}
static void sigtermMidEdit() {
    note("== 5.7 SIGTERM mid-edit: child with the real controller + SignalBridge types, parent sends SIGTERM after a delay; was the typing saved?");
    for (int delay : {0, 100, 400, 1000, 3000}) {
        Sandbox sb(QString("rb-term%1").arg(delay));
        writeFile(sb.notes + "/victim.md", "# Victim\n\nbase text\n");
        QProcess p; p.setProgram(QCoreApplication::applicationFilePath());
        p.setArguments({"--child-edit", sb.notes, sb.state, sb.cache, sb.config + "/config.json"}); p.setProcessChannelMode(QProcess::MergedChannels); p.start();
        if (!p.waitForReadyRead(20000)) { check(false, "child did not start typing"); continue; }
        runFor(delay);
        Stopwatch sw; kill(p.processId(), SIGTERM);
        const bool exited = p.waitForFinished(15000);
        const double exitMs = sw.ms();
        if (!exited) { p.kill(); p.waitForFinished(); }
        const QByteArray disk = readFile(sb.notes + "/victim.md");
        const bool saved = disk.contains("TYPED-TEXT-MARKER");
        const auto drafts = core::NoteRepository(sb.notes, sb.state + "/recovery").recoverableDrafts();
        const bool inDraft = std::any_of(drafts.begin(), drafts.end(), [](const core::RecoveryEntry &e) { return e.local.contains("TYPED-TEXT-MARKER"); });
        note(QString("SIGTERM %1 ms after typing: exited=%2 in %3 ms code=%4/%5, text on disk=%6, in recovery draft=%7").arg(delay).arg(exited).arg(exitMs).arg(int(p.exitStatus())).arg(p.exitCode()).arg(saved).arg(inDraft));
        metric(qPrintable(QString("robust.sigterm.delay%1.saved_to_disk").arg(delay)), saved); metric(qPrintable(QString("robust.sigterm.delay%1.exit_ms").arg(delay)), exitMs, "ms");
        check(exited, QString("SIGTERM at +%1 ms: process exits (no hang)").arg(delay));
        check(saved || inDraft, QString("SIGTERM at +%1 ms: typed text is on disk or in a recovery draft (never silently lost)").arg(delay));
        check(core::isValidUtf8(disk) && disk.contains("Victim") && disk.contains("base text") && disk.startsWith("# "), QString("SIGTERM at +%1 ms: note file intact (disk now: %2)").arg(delay).arg(QString::fromUtf8(disk.left(80)).replace("\n", "\\n")));
    }
    // SIGKILL for contrast: documents the autosave window
    for (int delay : {0, 300, 1500}) {
        Sandbox sb(QString("rb-kill%1").arg(delay));
        writeFile(sb.notes + "/victim.md", "# Victim\n\nbase text\n");
        QProcess p; p.setProgram(QCoreApplication::applicationFilePath());
        p.setArguments({"--child-edit", sb.notes, sb.state, sb.cache, sb.config + "/config.json"}); p.setProcessChannelMode(QProcess::MergedChannels); p.start();
        if (!p.waitForReadyRead(20000)) continue;
        runFor(delay); kill(p.processId(), SIGKILL); p.waitForFinished(5000);
        const bool saved = readFile(sb.notes + "/victim.md").contains("TYPED-TEXT-MARKER");
        const auto drafts = core::NoteRepository(sb.notes, sb.state + "/recovery").recoverableDrafts();
        const bool inDraft = std::any_of(drafts.begin(), drafts.end(), [](const core::RecoveryEntry &e) { return e.local.contains("TYPED-TEXT-MARKER"); });
        note(QString("(contrast) SIGKILL %1 ms after typing: on disk=%2 recovery draft=%3 -> %4").arg(delay).arg(saved).arg(inDraft).arg(saved || inDraft ? "preserved" : "typing lost (inside the 750 ms idle-autosave window)"));
        metric(qPrintable(QString("robust.sigkill.delay%1.preserved").arg(delay)), saved || inDraft);
    }
}

int main(int argc, char **argv) {
    const QStringList raw = [&] { QStringList l; for (int i = 0; i < argc; ++i) l << QString::fromLocal8Bit(argv[i]); return l; }();
    if (raw.value(1) == "--child-lib") return childLib(raw.value(2));
    if (raw.value(1) == "--child-flood") { return childFlood(raw[2], raw[3].toInt(), raw[4].toInt(), raw[5].toInt()); }
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    qInstallMessageHandler(msgHandler);
    if (raw.value(1) == "--child-edit") return childEdit(raw[2], raw[3], raw[4], raw[5], 0);
    const QString which = raw.value(1, "all");
    auto want = [&](const char *n) { return which == "all" || which == n; };
    if (want("config")) configFuzz();
    if (want("theme")) themeFuzz();
    if (want("mods")) modFuzz();
    if (want("session")) sessionFuzz();
    if (want("transport")) transportFlood();
    if (want("launch")) launchers();
    if (want("sigterm")) sigtermMidEdit();
    metric("robust.other_warnings_seen", g_otherWarnings);
    return finish("stress_robust");
}
