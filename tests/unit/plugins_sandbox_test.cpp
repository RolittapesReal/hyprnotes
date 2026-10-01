#include "plugins_test_util.h"
#include <QElapsedTimer>
#include <QSignalSpy>

using namespace hn::plugins;
using namespace hn::plugins::test;

// A command "c" that runs `body`.
static QByteArray cmd(const QByteArray &body) { return "hn.command{id='c',title='C',run=function()\n" + body + "\nend}\n"; }

class SandboxTest : public QObject {
    Q_OBJECT
    // runs command c of a fresh plugin, returns wall ms; err filled on failure
    qint64 timed(Rig &r, const QString &id, const QByteArray &body, QString *err, bool *ok = nullptr, const QStringList &perms = {}) {
        if (!r.addLua(id, perms, cmd(body))) { *err = "setup failed"; return -1; }
        QElapsedTimer t;
        t.start();
        const bool b = r.mgr->runCommand(id + ":c", &r.note, err);
        const qint64 ms = t.elapsed();
        if (ok) *ok = b;
        return ms;
    }
private slots:
    void escapeAttemptsAllFail() {
        Rig r;
        QString err;
        QVERIFY2(r.setup(fixture("escape-probe"), {}, true, &err), qPrintable(err));
        QVERIFY2(r.mgr->runCommand("escape-probe:escape", &r.note, &err), qPrintable(err));
        QMap<QString, QStringList> res;
        for (const auto &l : r.logsOf("escape-probe")) { const auto p = l.split('|'); res[p[0]] = p.mid(1); }
        for (const char *g : {"io", "os", "debug", "package", "dofile", "loadfile", "string.dump", "warn"}) {
            QCOMPARE(res[g].value(0), QString("true"));
            QCOMPARE(res[g].value(1), QString("nil"));  // the global simply does not exist
        }
        for (const char *f : {"io.open", "os.execute", "require ../", "require /etc", "require os", "require io", "require missing", "gc stop", "gc setpause", "string.rep bomb", "load function"})
            QVERIFY2(res[f].value(0) == "false", f);
        QVERIFY(res["require ../"][1].contains("invalid module name"));
        QVERIFY(res["require os"][1].contains("not found in the plugin folder"));
        QVERIFY(res["string.rep bomb"][1].contains("too large"));
        QCOMPARE(res["load io"], (QStringList{"true", "nil", "nil"}));          // chunk sees no io
        QCOMPARE(res["load bytecode"].value(1), QString("nil"));                // binary chunks refused
        QVERIFY(res["load bytecode"].value(2).contains("binary", Qt::CaseInsensitive) || res["load bytecode"].value(2).contains("attempt to load"));
        QCOMPARE(res["rawget io"].value(1), QString("nil"));
        QCOMPARE(res["_G.io"].value(1), QString("nil"));
        QCOMPARE(res["coroutine io"].value(1), QString("nil"));
        QCOMPARE(res["gc count"].value(1), QString("number"));
        QCOMPARE(res["string.rep ok"].value(1), QString("2999"));
        qInfo().noquote() << "escape-probe: 28 attempts, io/os/debug/package/dofile/loadfile/string.dump/warn all absent; require ../ /etc os io rejected";
    }
    void requireStaysInsidePluginFolder() {
        Rig r;
        const QString dir = writePlugin(r.env.src(), "req", {}, "hn.command{id='c',title='C',run=function()\n"
            "  hn.log('inner=' .. require('sub.inner').v)\n"
            "  hn.log('again=' .. tostring(require('sub.inner') == require('sub.inner')))\n"
            "  hn.log(tostring(pcall(require, 'sub..inner')))\n"
            "  hn.log(tostring(pcall(require, 'sub/inner')))\n"
            "  hn.log(tostring(pcall(require, '.hidden')))\n"
            "  hn.log(tostring(pcall(require, string.rep('a', 200))))\n"
            "end}\n");
        writeFile(dir + "/sub/inner.lua", "return {v = 'ok', where = (...)}\n");
        writeFile(dir + "/.hidden.lua", "return 1\n");
        QString err;
        QVERIFY2(r.setup(dir, {}, true, &err), qPrintable(err));
        QVERIFY(r.mgr->runCommand("req:c", &r.note, &err));
        QCOMPARE(r.logsOf("req"), (QStringList{"inner=ok", "again=true", "false", "false", "false", "false"}));
    }
    void symlinkPlantedAfterInstallIsCaught() {
        Rig r;
        QVERIFY(r.addLua("plant", {}, cmd("hn.log('x')")));
        QVERIFY(QFile::link("/etc/hostname", r.env.plugins() + "/plant/evil.lua"));
        QString err;
        QVERIFY(!r.mgr->runCommand("plant:c", &r.note, &err));
        QVERIFY2(err.contains("changed") || err.contains("link"), qPrintable(err));
        QVERIFY(r.logsOf("plant").isEmpty());
    }
    void statesAreIsolatedBetweenPlugins() {
        Rig r;
        QVERIFY(r.addLua("evil", {}, cmd(
            "leak = 'from evil'\n"
            "getmetatable('').__index = function() return function() return 'pwned' end end\n"
            "string.upper = function() return 'pwned' end\n"
            "math.floor = nil\n"
            "table.insert = nil\n"
            "hn.log('evil: ' .. ('x'):upper())\n")));
        QVERIFY(r.addLua("victim", {}, cmd(
            "hn.log('leak=' .. tostring(rawget(_G, 'leak')))\n"
            "hn.log(('x'):upper() .. '|' .. string.upper('y') .. '|' .. math.floor(2.5) .. '|' .. tostring(table.insert ~= nil))\n"
            "hn.log(tostring(getmetatable('').__index == string))\n")));
        QString err;
        QVERIFY(r.mgr->runCommand("evil:c", &r.note, &err));
        QVERIFY(r.mgr->runCommand("victim:c", &r.note, &err));
        QCOMPARE(r.logsOf("evil"), QStringList{"evil: pwned"});
        QCOMPARE(r.logsOf("victim"), (QStringList{"leak=nil", "X|Y|2|true", "true"}));
        // and the evil plugin itself stays broken only for itself, even after another run
        QVERIFY(r.mgr->runCommand("victim:c", &r.note, &err));
        QCOMPARE(r.logsOf("victim").last(), QString("true"));
    }
    void printRedirectedToLog() {
        Rig r;
        QVERIFY(r.addLua("pr", {}, cmd("print('a', 1, nil, true) hn.log('b')\nhn.log(tostring(pcall(error, {})))\nlocal ok, e = pcall(function() error(setmetatable({}, {__tostring = function() return 'custom' end})) end) hn.log(tostring(e))")));
        QVERIFY(r.mgr->runCommand("pr:c", &r.note));
        QCOMPARE(r.logsOf("pr"), (QStringList{"a\t1\tnil\ttrue", "b", "false", "custom"}));
    }
    void nonStringErrorsAreReported() {
        Rig r;
        QString err;
        bool ok;
        timed(r, "errobj", "error({code = 1})", &err, &ok);
        QVERIFY(!ok);
        QVERIFY2(err.contains("error object is a table value"), qPrintable(err));
        QVERIFY(r.mgr->trust()->record("errobj").failures == 1);
    }

    // ---- resource limits, measured ----
    void eventBudget50ms() {
        Rig r;
        QVERIFY(r.addLua("evloop", {}, "hn.on('note.opened', function() while true do end end)\n"));
        QElapsedTimer t;
        t.start();
        r.mgr->post("note.opened", "a", &r.note);
        const qint64 ms = t.elapsed();
        qInfo().noquote() << QString("budget: infinite loop in an event handler aborted after %1 ms (budget 50 ms)").arg(ms);
        QVERIFY2(ms >= 45 && ms < 200, qPrintable(QString::number(ms)));
        QCOMPARE(r.mgr->trust()->record("evloop").failures, 1);
        QVERIFY(readFile(r.env.state() + "/plugins-audit.jsonl").contains("time budget exceeded"));
    }
    void preSaveBudget20ms() {
        Rig r;
        QVERIFY(r.addLua("psloop", {}, "hn.on('note.pre_save', function(t) while true do end end)\n"));
        QElapsedTimer t;
        t.start();
        const QString out = r.mgr->preSave("text", &r.note);
        const qint64 ms = t.elapsed();
        qInfo().noquote() << QString("budget: infinite loop in note.pre_save aborted after %1 ms (budget 20 ms); text left unchanged").arg(ms);
        QCOMPARE(out, QString("text"));
        QVERIFY2(ms >= 18 && ms < 150, qPrintable(QString::number(ms)));
    }
    void commandBudget2s() {
        Rig r;
        QString err;
        bool ok;
        const qint64 ms = timed(r, "cmdloop", "local i = 0 while true do i = i + 1 end", &err, &ok);
        qInfo().noquote() << QString("budget: infinite loop in a command aborted after %1 ms (budget 2000 ms)").arg(ms);
        QVERIFY(!ok);
        QVERIFY2(err.contains("time budget exceeded"), qPrintable(err));
        QVERIFY2(ms >= 1950 && ms < 2400, qPrintable(QString::number(ms)));
    }
    void abortCannotBeSwallowedByPcall() {
        Rig r;
        QString err;
        bool ok;
        qint64 ms = timed(r, "swallow1", "while true do pcall(function() while true do end end) end", &err, &ok);
        qInfo().noquote() << QString("budget: while true do pcall(<infinite loop>) end aborted after %1 ms").arg(ms);
        QVERIFY(!ok);
        QVERIFY2(ms < 2400, qPrintable(QString::number(ms)));
        ms = timed(r, "swallow2", "local function nest(n) if n == 0 then while true do end end pcall(nest, n - 1) end\n while true do pcall(nest, 50) end", &err, &ok);
        QVERIFY(!ok);
        QVERIFY2(ms < 2400, qPrintable(QString::number(ms)));
        ms = timed(r, "swallow3", "local co = coroutine.wrap(function() while true do pcall(function() while true do end end) end end)\n co()", &err, &ok);
        QVERIFY(!ok);
        QVERIFY2(ms < 2400, qPrintable(QString::number(ms)));
        qInfo().noquote() << QString("budget: nested-pcall and coroutine variants also aborted (last %1 ms)").arg(ms);
    }
    void legitimateWorkFitsAndThroughput() {
        Rig r;
        QString err;
        bool ok;
        const qint64 ms = timed(r, "work", "local t = {} for i = 1, 200000 do t[#t + 1] = i * 2 end local s = 0 for _, v in ipairs(t) do s = s + v end hn.log(s)\n"
                                           "local parts = {} for i = 1, 2000 do parts[#parts + 1] = ('line %d'):format(i) end hn.log(#table.concat(parts, '\\n'))", &err, &ok);
        QVERIFY2(ok, qPrintable(err));
        QCOMPARE(r.logsOf("work"), (QStringList{"40000200000", "18892"}));
        qInfo().noquote() << QString("throughput: 200k-element build+sum and 2k-line format took %1 ms inside the hooked VM").arg(ms);
        QElapsedTimer t;
        t.start();
        QVERIFY(r.addLua("spin", {}, cmd("local x = 0 for i = 1, 20000000 do x = x + i end hn.log(x)")));
        QVERIFY(r.mgr->runCommand("spin:c", &r.note, &err));
        const qint64 sm = t.elapsed();
        qInfo().noquote() << QString("throughput: 20M-iteration loop in %1 ms (%2 M iter/s with the budget hook active)").arg(sm).arg(20000.0 / double(qMax<qint64>(sm, 1)), 0, 'f', 0);
    }
    void deepRecursion() {
        Rig r;
        QString err;
        bool ok;
        qint64 ms = timed(r, "rec1", "local function f(n) return 1 + f(n + 1) end f(1)", &err, &ok);
        QVERIFY(!ok);
        QVERIFY2(err.contains("depth") || err.contains("overflow"), qPrintable(err));
        QVERIFY2(ms < 500, qPrintable(QString::number(ms)));
        qInfo().noquote() << QString("recursion: unbounded Lua recursion stopped in %1 ms: %2").arg(ms).arg(err.left(80));
        ms = timed(r, "rec2", "local function f() return coroutine.wrap(f)() end f()", &err, &ok);
        QVERIFY(!ok);
        QVERIFY2(ms < 500, qPrintable(QString::number(ms)));
        ms = timed(r, "rec3", "setmetatable(_G, {__index = function(t, k) return t[k] end}) return undefined_global", &err, &ok);
        QVERIFY(!ok);
        QVERIFY2(ms < 500, qPrintable(QString::number(ms)));
        ms = timed(r, "rec4", "local a = {} setmetatable(a, {__index = a}) getmetatable(a).__index = a return a.x", &err, &ok);
        QVERIFY(!ok);
        // moderately deep legitimate recursion works
        timed(r, "rec5", "local function f(n) if n == 0 then return 0 end return 1 + f(n - 1) end hn.log(f(300))", &err, &ok);
        QVERIFY2(ok, qPrintable(err));
        QCOMPARE(r.logsOf("rec5"), QStringList{"300"});
    }
    void memoryBombs() {
        Rig r;
        QString err;
        bool ok;
        qint64 ms = timed(r, "m1", "local t = {} for i = 1, 1e9 do t[i] = i end", &err, &ok);
        QVERIFY(!ok);
        QVERIFY2(err.contains("not enough memory") || err.contains("budget"), qPrintable(err));
        QVERIFY2(r.mgr->host()->memoryUsed("m1") <= 16u * 1024 * 1024, "allocator cap");
        qInfo().noquote() << QString("memory: huge-table loop stopped after %1 ms with '%2'; Lua heap %3 KiB (cap 16384 KiB)")
                                 .arg(ms).arg(err.left(40)).arg(r.mgr->host()->memoryUsed("m1") / 1024);
        ms = timed(r, "m2", "local s = 'x' while true do s = s .. s end", &err, &ok);
        QVERIFY(!ok);
        QVERIFY2(err.contains("not enough memory") || err.contains("budget"), qPrintable(err));
        QVERIFY(r.mgr->host()->memoryUsed("m2") <= 16u * 1024 * 1024);
        ms = timed(r, "m3", "return string.rep('x', 5e9)", &err, &ok);
        QVERIFY(!ok);
        QVERIFY2(err.contains("too large"), qPrintable(err));
        QVERIFY(ms < 200);
        ms = timed(r, "m4", "return string.rep('abcdefgh', 1e6, ',')", &err, &ok);  // 9 MB > 4 MiB result cap, still far under the 16 MiB heap cap
        QVERIFY(!ok);
        timed(r, "m5", "return table.unpack({}, 1, 1e8)", &err, &ok);
        QVERIFY(!ok);
        timed(r, "m6", "local t = {} for i = 1, 1e6 do t[i] = coroutine.create(function() end) end", &err, &ok);
        QVERIFY(!ok);
        QVERIFY(r.mgr->host()->memoryUsed("m6") <= 16u * 1024 * 1024);
        timed(r, "m7", "local t = {} for i = 1, 1e6 do t[i] = {i, i, i, i} end", &err, &ok);
        QVERIFY(!ok);
        timed(r, "m8", "return ('x'):rep(1e6):rep(1e6)", &err, &ok);
        QVERIFY(!ok);
        timed(r, "m9", "local t = {} for i = 1, 200 do t[i] = string.rep('y', 100000) .. i end hn.log(#t)", &err, &ok);  // 20 MB of strings
        QVERIFY(!ok);
        // other plugins are unaffected, and a bombed plugin can still be used afterwards
        QVERIFY(r.addLua("fine", {}, cmd("hn.log('fine')")));
        QVERIFY(r.mgr->runCommand("fine:c", &r.note));
        QVERIFY(r.mgr->runCommand("m1:c", &r.note, &err) == false);
    }
    void bigStringsAtTheBoundary() {
        Rig r;
        QString err;
        bool ok;
        timed(r, "big1", "hn.note.set_text(string.rep('x', 3000000))", &err, &ok, {"note.edit"});
        QVERIFY2(ok, qPrintable(err));
        QCOMPARE(r.note.t.size(), 3000000);
        timed(r, "big2", "hn.note.set_text(string.rep('x', 3000000) .. string.rep('y', 3000000))", &err, &ok, {"note.edit"});
        QVERIFY(!ok);
        QVERIFY2(err.contains("too large"), qPrintable(err));
        QCOMPARE(r.note.t.size(), 3000000);  // unchanged
    }
    void coroutineAbuse() {
        Rig r;
        QString err;
        bool ok;
        qint64 ms = timed(r, "co1", "local co = coroutine.wrap(function() while true do end end) co()", &err, &ok);
        QVERIFY(!ok);
        QVERIFY2(ms < 2400, qPrintable(QString::number(ms)));
        ms = timed(r, "co2", "local co = coroutine.wrap(function() while true do coroutine.yield() end end) while true do co() end", &err, &ok);
        QVERIFY(!ok);
        QVERIFY2(ms < 2400, qPrintable(QString::number(ms)));
        ms = timed(r, "co3", "local co = coroutine.create(function() error('x') end) for i = 1, 1000 do coroutine.resume(co) end hn.log(coroutine.status(co))", &err, &ok);
        QVERIFY2(ok, qPrintable(err));
        // a coroutine created in one callback keeps running under budget in a later one
        QVERIFY(r.addLua("co4", {}, "co = coroutine.wrap(function() while true do coroutine.yield() end end)\nhn.command{id='a',title='A',run=function() co() end}\n"
                                    "hn.command{id='b',title='B',run=function() local c2 = coroutine.wrap(function() while true do end end) c2() end}\n"));
        QVERIFY(r.mgr->runCommand("co4:a", &r.note));
        QElapsedTimer t;
        t.start();
        QVERIFY(!r.mgr->runCommand("co4:b", &r.note, &err));
        QVERIFY(t.elapsed() < 2400);
        timed(r, "co5", "local co = coroutine.wrap(function() local x = {} for i = 1, 1e9 do x[i] = i end end) co()", &err, &ok);
        QVERIFY(!ok);
    }
    void gcFinalizerCannotHangUnload() {
        // Lua turns hooks off while a __gc runs, so a looping finalizer would be uninterruptible: setmetatable refuses __gc.
        Rig r;
        QString err;
        bool ok;
        timed(r, "gcl", "setmetatable({}, {__gc = function() while true do end end})", &err, &ok);
        QVERIFY(!ok);
        QVERIFY2(err.contains("__gc finalizers are not allowed"), qPrintable(err));
        // adding __gc after the fact never marks the object for finalization, so it cannot run either
        timed(r, "gcl2", "local mt = {} local t = setmetatable({}, mt) mt.__gc = function() while true do end end t = nil collectgarbage() collectgarbage() hn.log('survived')", &err, &ok);
        QVERIFY2(ok, qPrintable(err));
        QElapsedTimer t;
        t.start();
        QVERIFY(r.mgr->disable("gcl2"));
        const qint64 ms = t.elapsed();
        qInfo().noquote() << QString("finalizer: __gc rejected at setmetatable; late-added __gc never fires; unload took %1 ms").arg(ms);
        QVERIFY2(ms < 500, qPrintable(QString::number(ms)));
        QVERIFY(!r.mgr->host()->isLoaded("gcl2"));
    }
    void ordinaryPatternsStayLinear() {
        // Pathological patterns are covered in plugins_hardening_test (bounded matcher); this checks the ordinary linear cases.
        Rig r;
        QString err;
        bool ok;
        const qint64 ms = timed(r, "pat", "hn.log(tostring(('a'):rep(100000):find('b'))) hn.log(tostring((('a'):rep(50000)):match('^(a*)$') ~= nil))", &err, &ok);
        QVERIFY2(ok, qPrintable(err));
        QVERIFY(ms < 500);
    }

    // ---- circuit breaker ----
    void circuitBreakerAfterThreeConsecutiveFailures() {
        Rig r;
        QVERIFY(r.addLua("flaky", {"ui"}, "n = 0\nhn.command{id='c',title='C',run=function() n = n + 1 if n ~= 3 then error('fail ' .. n) end end}\n"));
        QSignalSpy spy(r.mgr.get(), &PluginManager::pluginAutoDisabled);
        QString err;
        QVERIFY(!r.mgr->runCommand("flaky:c", &r.note, &err));  // 1 fail
        QVERIFY(!r.mgr->runCommand("flaky:c", &r.note, &err));  // 2 fail
        QCOMPARE(r.mgr->trust()->record("flaky").failures, 2);
        QVERIFY(r.mgr->runCommand("flaky:c", &r.note, &err));   // success resets the streak
        QCOMPARE(r.mgr->trust()->record("flaky").failures, 0);
        QCOMPARE(r.mgr->info("flaky").status, Status::Enabled);
        QVERIFY(!r.mgr->runCommand("flaky:c", &r.note, &err));  // 1
        QVERIFY(!r.mgr->runCommand("flaky:c", &r.note, &err));  // 2
        QCOMPARE(spy.count(), 0);
        QVERIFY(!r.mgr->runCommand("flaky:c", &r.note, &err));  // 3 => auto-disable
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.first().first().toString(), QString("flaky"));
        QVERIFY(spy.first().at(1).toString().contains("3 consecutive failures"));
        QCOMPARE(r.mgr->info("flaky").status, Status::Failed);
        QVERIFY(r.mgr->info("flaky").detail.contains("fail 6"));
        QVERIFY(!r.mgr->trust()->record("flaky").enabled);
        QCOMPARE(r.mgr->loadedStates(), 0);
        QVERIFY(r.mgr->registry()->commands().isEmpty());
        QVERIFY(!r.mgr->runCommand("flaky:c", &r.note, &err));
        QVERIFY(err.contains("not enabled"));
        const QString audit = readFile(r.env.state() + "/plugins-audit.jsonl");
        QVERIFY(audit.contains("\"auto-disable\""));
        QCOMPARE(audit.count("\"event\":\"failure\""), 5);
        // survives restart, and enabling again is possible (consent is still valid) and resets the counter
        r.mgr->scan();
        QCOMPARE(r.mgr->info("flaky").status, Status::Disabled);
        QVERIFY(r.mgr->enable("flaky"));
        QCOMPARE(r.mgr->trust()->record("flaky").failures, 0);
    }
    void budgetFailuresAlsoTripBreaker() {
        Rig r;
        QVERIFY(r.addLua("spinner", {}, "hn.on('note.opened', function() while true do end end)\n"));
        QSignalSpy spy(r.mgr.get(), &PluginManager::pluginAutoDisabled);
        QElapsedTimer t;
        t.start();
        for (int i = 0; i < 3; ++i) r.mgr->post("note.opened", "a", &r.note);
        qInfo().noquote() << QString("breaker: 3 runaway event handlers => auto-disabled in %1 ms total").arg(t.elapsed());
        QCOMPARE(spy.count(), 1);
        QCOMPARE(r.mgr->info("spinner").status, Status::Failed);
        QVERIFY(t.elapsed() < 700);
        r.mgr->post("note.opened", "a", &r.note);  // disabled: no more cost
        QVERIFY(t.elapsed() < 800);
    }
    void loadErrorsAreReportedAndDisable() {
        Rig r;
        QString err;
        QVERIFY(!r.addLua("loaderr", {}, "error('top-level boom')\n", {}, &err));
        QVERIFY2(err.contains("top-level boom"), qPrintable(err));
        QVERIFY(!r.mgr->trust()->record("loaderr").enabled);
        QCOMPARE(r.mgr->info("loaderr").status, Status::Failed);
        QCOMPARE(r.mgr->loadedStates(), 0);
        QVERIFY(!r.addLua("loadloop", {}, "while true do end\n", {}, &err));
        QVERIFY2(err.contains("budget"), qPrintable(err));
        QVERIFY(!r.addLua("badreg", {}, "hn.command{id='Bad Id', title='x', run=function() end}\n", {}, &err));
        QVERIFY2(err.contains("invalid id"), qPrintable(err));
        QVERIFY(!r.addLua("badev", {}, "hn.on('not.an.event', function() end)\n", {}, &err));
        QVERIFY(err.contains("unknown event"));
        QVERIFY(!r.addLua("dupcmd", {}, "hn.command{id='a',title='x',run=function() end}\nhn.command{id='a',title='y',run=function() end}\n", {}, &err));
        QVERIFY(err.contains("duplicate"));
    }
    void reentrancyDuringBlockingUiCallIsSafe() {
        struct Evil : FakeUi {
            PluginManager *m = nullptr;
            QStringList got;
            std::optional<QString> prompt(const QString &, const QString &, const QString &, const QString &) override {
                got << (m->runCommand("re:c", nullptr) ? "reentered" : "refused");  // same plugin while it is mid-callback
                m->disable("re");                                                    // disabled while its callback is on the stack
                m->post("note.opened", "x", nullptr);
                return QString("v");
            }
        };
        Evil ui;
        TmpEnv env;
        ManagerConfig c;
        c.bridges.ui = &ui;
        PluginManager m(c);
        ui.m = &m;
        m.scan();
        const auto dir = writePlugin(env.src(), "re", {"ui"}, "hn.command{id='c',title='C',run=function() local v = hn.ui.prompt('a','b','c') hn.ui.notify('after ' .. v) end}\nhn.on('note.opened', function() hn.ui.notify('event') end)\n");
        QVERIFY(m.install(dir).ok && m.consent("re", {"ui"}) && m.enable("re"));
        QVERIFY(m.runCommand("re:c", nullptr));
        QCOMPARE(ui.got, QStringList{"refused"});
        QCOMPARE(ui.notes, QStringList{"after v"});  // callback finished, then the state was closed
        QCOMPARE(m.loadedStates(), 0);
        QCOMPARE(m.info("re").status, Status::Disabled);
    }
};
QTEST_MAIN(SandboxTest)
#include "plugins_sandbox_test.moc"
