#include "plugins_test_util.h"
#include <QElapsedTimer>
#include <malloc.h>

using namespace hn::plugins;
using namespace hn::plugins::test;

namespace {
QStringList lines(const QStringList &l, const QString &prefix) {
    QStringList r;
    for (const auto &x : l) if (x.startsWith(prefix)) r << x;
    return r;
}
}  // namespace

class RuntimeTest : public QObject {
    Q_OBJECT
private slots:
    // ---- idle behaviour ----
    void idleNothingEnabledNoStateNoTimer() {
        Rig r;
        QVERIFY(r.mgr->install(fixture("hello")).ok);
        QVERIFY(r.mgr->install(fixture("api-probe")).ok);  // installed, not consented, not enabled
        r.mgr->scan();
        QCOMPARE(r.mgr->plugins().size(), 2);
        QCOMPARE(r.mgr->loadedStates(), 0);
        QVERIFY(!r.mgr->timerActive());
        QVERIFY(r.mgr->registry()->commands().isEmpty());
        // events with no subscriber: still nothing
        r.mgr->post("note.changed", "a.md", &r.note);
        r.mgr->post("note.opened", "a.md", &r.note);
        QCOMPARE(r.mgr->preSave("text", &r.note), QString("text"));
        QVERIFY(!r.mgr->matchTrigger("::date", &r.note));
        QVERIFY(!r.mgr->timerActive());
        QCOMPARE(r.mgr->loadedStates(), 0);
        QVERIFY(r.logs.isEmpty());
    }
    void enabledPluginWithoutSubscriptionStaysLazyAfterRestart() {
        Rig r;
        QVERIFY(r.addLua("cmd-only", {"ui"}, "hn.command{id='c',title='C',run=function() hn.ui.notify('ran') end}\n"));
        r.mgr->scan();  // simulated app restart
        QCOMPARE(r.mgr->loadedStates(), 0);
        QCOMPARE(r.mgr->registry()->commands().size(), 1);  // visible from the registration cache
        for (int i = 0; i < 5; ++i) r.mgr->post("note.changed", "a.md", &r.note);  // nobody subscribed
        QVERIFY(!r.mgr->timerActive());
        QCOMPARE(r.mgr->loadedStates(), 0);
        QVERIFY(r.mgr->runCommand("cmd-only:c", &r.note));  // first hook creates the state
        QCOMPARE(r.mgr->loadedStates(), 1);
        QCOMPARE(r.ui.notes, QStringList{"ran"});
        QVERIFY(r.mgr->disable("cmd-only"));
        QCOMPARE(r.mgr->loadedStates(), 0);  // destroyed on disable
        QVERIFY(r.mgr->registry()->commands().isEmpty());
        QCOMPARE(r.mgr->info("cmd-only").status, Status::Disabled);
    }
    void memoryPerEnabledTrivialPlugin() {
        Rig r;
        const int N = 50;
        for (int i = 0; i < N; ++i)
            QVERIFY(r.addLua(QString("triv-%1").arg(i), {"ui"}, "hn.command{id='c',title='C',run=function() end}\n"));
        r.mgr->scan();
        QCOMPARE(r.mgr->loadedStates(), 0);
        malloc_trim(0);  // return priming garbage to the OS so the delta is honest
        const qint64 before = pssKb();
        for (int i = 0; i < N; ++i) QVERIFY(r.mgr->runCommand(QString("triv-%1:c").arg(i), &r.note));
        const qint64 after = pssKb();
        QCOMPARE(r.mgr->loadedStates(), N);
        quint64 luaBytes = 0;
        for (int i = 0; i < N; ++i) luaBytes += r.mgr->host()->memoryUsed(QString("triv-%1").arg(i));
        qInfo().noquote() << QString("memory: %1 trivial plugins loaded: PSS delta %2 KiB total (%3 KiB/plugin); Lua allocator %4 bytes/plugin")
                                 .arg(N).arg(after - before).arg(double(after - before) / N, 0, 'f', 1).arg(luaBytes / N);
        QVERIFY(luaBytes / N < 64 * 1024);
        for (int i = 0; i < N; ++i) r.mgr->disable(QString("triv-%1").arg(i));
        QCOMPARE(r.mgr->loadedStates(), 0);
        QVERIFY(!r.mgr->timerActive());
    }

    // ---- fixtures: registration, commands, events, triggers ----
    void helloFixtureRegistersEverything() {
        Rig r;
        QString err;
        QVERIFY2(r.setup(fixture("hello"), {}, true, &err), qPrintable(err));
        auto *reg = r.mgr->registry();
        QCOMPARE(reg->commands().size(), 4);
        QCOMPARE(reg->commands().first().qualifiedId(), QString("hello:shout"));
        QCOMPARE(reg->toolbarButtons().size(), 1);
        QCOMPARE(reg->toolbarButtons().first().icon, QString("star"));
        QCOMPARE(reg->menuItems("note").size(), 1);
        QCOMPARE(reg->menuItems("tools").size(), 0);
        QCOMPARE(reg->triggers().size(), 2);
        QCOMPARE(reg->settings("hello").size(), 2);
        QCOMPARE(reg->settings().first().def.toString(), QString("Hello"));
        QCOMPARE(reg->subscribers("note.saved"), QStringList{"hello"});
        QVERIFY(reg->subscribers("note.closed").isEmpty());
    }
    void commandEditsNoteInOneTransaction() {
        Rig r;
        QVERIFY(r.setup(fixture("hello")));
        QVERIFY(r.mgr->runCommand("hello:shout", &r.note));
        QCOMPARE(r.note.ops, (QStringList{"replace:WORLD", "insert:!"}));
        QCOMPARE(r.note.begins, 1);
        QCOMPARE(r.note.ends, 1);
        QCOMPARE(r.note.t, QString("hello WORLD!"));
        // toolbar + menu items run too
        QVERIFY(r.mgr->runToolbarButton("hello:tb", &r.note));
        QVERIFY(r.mgr->runMenuItem("hello:mi", &r.note));
        QCOMPARE(r.logsOf("hello"), (QStringList{"toolbar", "menu"}));
        QString err;
        QVERIFY(!r.mgr->runCommand("hello:nope", &r.note, &err));
        QVERIFY(!r.mgr->runCommand("nobody:shout", &r.note, &err));
        QVERIFY(!r.mgr->runCommand("garbage", &r.note, &err));
        QVERIFY(!r.mgr->runCommand("hello:tb", &r.note, &err));  // a toolbar id is not a command id
        r.note.begins = r.note.ends = 0;
        QVERIFY(!r.mgr->runCommand("hello:fail", &r.note, &err));
        QVERIFY2(err.contains("boom"), qPrintable(err));
        QCOMPARE(r.note.begins, r.note.ends);
    }
    void storageAndVersionAndSettings() {
        Rig r;
        QVERIFY(r.setup(fixture("hello")));
        for (int i = 0; i < 3; ++i) QVERIFY(r.mgr->runCommand("hello:count", &r.note));
        QVERIFY(r.mgr->runCommand("hello:version", &r.note));
        QCOMPARE(r.logsOf("hello"), (QStringList{"count=1", "count=2", "count=3", "version=0.1.0"}));
        // persisted per plugin in the state dir, and reloaded by a fresh manager
        QVERIFY(QFileInfo::exists(r.env.state() + "/plugin-data/hello.json"));
        r.mgr->host()->unload("hello");
        QVERIFY(r.mgr->runCommand("hello:count", &r.note));
        QVERIFY(r.logsOf("hello").last() == "count=4");
        // settings: default, set, wrong type rejected
        auto *h = r.mgr->host();
        QCOMPARE(h->setting("hello", "greeting").toString(), QString("Hello"));
        QVERIFY(!h->setSetting("hello", "greeting", true));
        QVERIFY(!h->setSetting("hello", "missing", "x"));
        QVERIFY(h->setSetting("hello", "greeting", "Howdy"));
        auto m = r.mgr->matchTrigger("abc ::hello", &r.note);
        QVERIFY(m);
        QCOMPARE(m->replacement, QString("Howdy"));
    }
    void triggers() {
        Rig r;
        QVERIFY(r.setup(fixture("hello")));
        auto m = r.mgr->matchTrigger("today is ::date", &r.note);
        QVERIFY(m);
        QCOMPARE(m->pluginId, QString("hello"));
        QCOMPARE(m->pattern, QString("::date"));
        QCOMPARE(m->replacement, QString("2026-10-01"));
        QVERIFY(!r.mgr->matchTrigger("::dat", &r.note));
        QVERIFY(!r.mgr->matchTrigger("::date more", &r.note));
        QVERIFY(!r.mgr->matchTrigger("", &r.note));
        // longest pattern wins
        QVERIFY(r.addLua("longer", {}, "hn.trigger{pattern='x::date', replace=function() return 'LONG' end}\n"));
        auto m2 = r.mgr->matchTrigger("ax::date", &r.note);
        QVERIFY(m2);
        QCOMPARE(m2->replacement, QString("LONG"));
        // a failing trigger returns nothing
        QVERIFY(r.addLua("badtrig", {}, "hn.trigger{pattern='::bad', replace=function() error('no') end}\n"));
        QVERIFY(!r.mgr->matchTrigger("::bad", &r.note));
        // non-string result is ignored
        QVERIFY(r.addLua("numtrig", {}, "hn.trigger{pattern='::num', replace=function() return 42 end}\n"));
        QVERIFY(!r.mgr->matchTrigger("::num", &r.note));
    }
    void eventDeliveryOnlyToSubscribers() {
        Rig r;
        QVERIFY(r.setup(fixture("hello")));
        QVERIFY(r.addLua("quiet", {}, "hn.command{id='c',title='C',run=function() end}\n"));
        QVERIFY(r.addLua("loud", {}, "hn.on('note.saved', function(p) hn.log('loud saw ' .. p) end)\nhn.on('note.saved', function(p) hn.log('second ' .. p) end)\n"));
        r.logs.clear();
        r.mgr->post("note.opened", "n1.md", &r.note);
        r.mgr->post("note.saved", "n1.md", &r.note);
        r.mgr->post("note.closed", "n1.md", &r.note);  // nobody subscribed
        QCOMPARE(r.logsOf("hello"), (QStringList{"opened n1.md", "saved n1.md"}));
        QCOMPARE(r.logsOf("loud"), (QStringList{"loud saw n1.md", "second n1.md"}));
        QVERIFY(r.logsOf("quiet").isEmpty());
        QVERIFY(!r.mgr->host()->isLoaded("quiet"));
    }
    void noteChangedIsCoalesced() {
        Rig r;
        QVERIFY(r.setup(fixture("hello")));
        r.mgr->host()->unload("hello");
        QCOMPARE(r.mgr->loadedStates(), 0);
        r.logs.clear();
        QElapsedTimer t;
        t.start();
        for (int i = 0; i < 50; ++i) r.mgr->post("note.changed", QString("v%1").arg(i), &r.note);
        QVERIFY(r.mgr->timerActive());
        QVERIFY(r.logs.isEmpty());  // nothing delivered yet, no state created yet
        QCOMPARE(r.mgr->loadedStates(), 0);
        QTRY_VERIFY_WITH_TIMEOUT(!r.logs.isEmpty(), 2000);
        const qint64 ms = t.elapsed();
        QVERIFY2(ms >= 240, qPrintable(QString::number(ms)));
        QCOMPARE(r.logsOf("hello"), QStringList{"changed v49"});  // only the latest
        QVERIFY(!r.mgr->timerActive());  // idle again
        // ordering: pending coalesced event is flushed before a saved event
        r.logs.clear();
        r.mgr->post("note.changed", "c1", &r.note);
        r.mgr->post("note.saved", "s1", &r.note);
        QCOMPARE(r.logsOf("hello"), (QStringList{"changed c1", "saved s1"}));
        QVERIFY(!r.mgr->timerActive());
        // detachNote flushes pending events that reference a bridge about to die
        r.logs.clear();
        r.mgr->post("note.changed", "c2", &r.note);
        r.mgr->detachNote(&r.note);
        QCOMPARE(r.logsOf("hello"), QStringList{"changed c2"});
        QVERIFY(!r.mgr->timerActive());
        qInfo().noquote() << QString("coalescing: 50 note.changed posts -> 1 delivery after %1 ms").arg(ms);
    }
    void preSaveTransformsText() {
        Rig r;
        QVERIFY(r.setup(fixture("hello")));
        QCOMPARE(r.mgr->preSave("trailing   \n\n", &r.note), QString("trailing\n"));
        QVERIFY(r.addLua("upper", {}, "hn.on('note.pre_save', function(t) return t:upper() end)\n"));
        QVERIFY(r.addLua("ret-nil", {}, "hn.on('note.pre_save', function(t) end)\n"));
        QVERIFY(r.addLua("ret-err", {}, "hn.on('note.pre_save', function(t) error('x') end)\n"));
        const QString out = r.mgr->preSave("abc  ", &r.note);
        QVERIFY2(out == "ABC\n" || out == "ABC\n", qPrintable(out));  // plugins run in id order: hello, ret-err, ret-nil, upper
    }
    void hotReload() {
        Rig r;
        const auto dir = writePlugin(r.env.src(), "hot", {"ui"}, "hn.command{id='c',title='C',run=function() hn.ui.notify('one') end}\n");
        QVERIFY(r.setup(dir));
        QVERIFY(r.mgr->runCommand("hot:c", &r.note));
        // developer edits the installed copy and presses Reload
        const QString inst = r.env.plugins() + "/hot/main.lua";
        writeFile(inst, "hn.command{id='c',title='C2',run=function() hn.ui.notify('two') end}\nhn.command{id='d',title='D',run=function() end}\n");
        QString err;
        QVERIFY(r.mgr->reload("hot", false, &err) == false || r.mgr->info("hot").status != Status::Enabled);  // untrusted change: not running
        QCOMPARE(r.mgr->info("hot").status, Status::NeedsConsent);
        QVERIFY(r.mgr->registry()->commands().isEmpty());
        QVERIFY(r.mgr->consent("hot", {"ui"}) && r.mgr->enable("hot"));
        writeFile(inst, "hn.command{id='c',title='C3',run=function() hn.ui.notify('three') end}\nhn.command{id='d',title='D',run=function() end}\n");
        QVERIFY2(r.mgr->reload("hot", true, &err), qPrintable(err));  // explicit developer reload: trusted, no restart
        QCOMPARE(r.mgr->info("hot").status, Status::Enabled);
        QCOMPARE(r.mgr->registry()->commands().size(), 2);
        QVERIFY(r.mgr->runCommand("hot:c", &r.note));
        QCOMPARE(r.ui.notes, (QStringList{"one", "three"}));
        // a reload that adds a permission is NOT silently trusted
        writeFile(r.env.plugins() + "/hot/plugin.json", manifestJson("hot", {"ui", "clipboard"}));
        QVERIFY(!r.mgr->reload("hot", true, &err) || r.mgr->info("hot").status != Status::Enabled);
        QCOMPARE(r.mgr->info("hot").status, Status::NeedsConsent);
        // a reload that breaks the script reports the error and disables
        writeFile(r.env.plugins() + "/hot/plugin.json", manifestJson("hot", {"ui"}));
        QVERIFY(r.mgr->consent("hot", {"ui"}));
        QVERIFY(r.mgr->enable("hot"));
        writeFile(inst, "this is not lua (\n");
        QVERIFY(!r.mgr->reload("hot", true, &err));
        QVERIFY(!err.isEmpty());
        QVERIFY(r.mgr->registry()->commands().isEmpty());
    }
    void settingsAndEventsPersistAcrossRestartViaCache() {
        Rig r;
        QVERIFY(r.setup(fixture("hello")));
        r.mgr->scan();
        QCOMPARE(r.mgr->loadedStates(), 0);
        QCOMPARE(r.mgr->registry()->settings().size(), 2);
        QCOMPARE(r.mgr->registry()->subscribers("note.pre_save"), QStringList{"hello"});
        QCOMPARE(r.mgr->registry()->triggers().size(), 2);
        r.mgr->post("note.opened", "a.md", &r.note);  // first hook => lazily creates the state
        QCOMPARE(r.mgr->loadedStates(), 1);
        QCOMPARE(r.logsOf("hello"), QStringList{"opened a.md"});
    }

    // ---- permission denial for EVERY API ----
    void permissionDenialForEveryApi() {
        Rig r;
        QString err;
        QVERIFY2(r.setup(fixture("api-probe"), {}, false, &err), qPrintable(err));  // consent to nothing
        QVERIFY2(r.mgr->runCommand("api-probe:probe", &r.note, &err), qPrintable(err));
        const QMap<QString, QString> need{
            {"note.text", "note.read"}, {"note.selection", "note.read"}, {"note.path", "note.read"}, {"note.title", "note.read"}, {"note.tags", "note.read"},
            {"note.replace_selection", "note.edit"}, {"note.insert", "note.edit"}, {"note.set_text", "note.edit"},
            {"notes.list", "notes.read"}, {"notes.read", "notes.read"},
            {"notes.create", "notes.write"}, {"notes.write", "notes.write"}, {"notes.delete", "notes.write"},
            {"ui.notify", "ui"}, {"ui.prompt", "ui"}, {"ui.confirm", "ui"}, {"ui.pick", "ui"},
            {"storage.get", "storage"}, {"storage.set", "storage"}, {"clipboard.get", "clipboard"}, {"clipboard.set", "clipboard"},
            {"http.get", "network"}, {"http.post", "network"}, {"theme.set_token", "theme"}};
        const auto out = r.logsOf("api-probe");
        QCOMPARE(out.size(), need.size());
        const QString audit = readFile(r.env.state() + "/plugins-audit.jsonl");
        for (const auto &l : out) {
            const auto p = l.split('|');
            QVERIFY2(need.contains(p[0]), qPrintable(l));
            QCOMPARE(p[1], QString("false"));
            QVERIFY2(p[2].contains("permission denied: " + need[p[0]]), qPrintable(l));
        }
        for (const auto &perm : QSet<QString>(need.begin(), need.end())) QVERIFY2(audit.contains("\"denied\"") && audit.contains("\"detail\":\"" + perm + "\""), qPrintable(perm));
        // nothing reached any bridge
        QVERIFY(r.note.ops.isEmpty() && r.lib.calls.isEmpty() && r.ui.notes.isEmpty() && r.net.reqs.isEmpty() && r.theme.sets.isEmpty());
        QCOMPARE(r.clip.v, QString("clip"));
        QCOMPARE(r.ui.prompts, 0);
    }
    void permissionDenialIsPerPermission() {
        // consent only to note.read + ui: exactly those APIs work
        Rig r;
        QString err;
        QVERIFY2(r.setup(fixture("api-probe"), {"note.read", "ui"}, false, &err), qPrintable(err));
        QVERIFY(r.mgr->runCommand("api-probe:probe", &r.note, &err));
        for (const auto &l : r.logsOf("api-probe")) {
            const auto p = l.split('|');
            const bool allowed = p[0].startsWith("note.") && !p[0].contains("replace") && !p[0].contains("insert") && !p[0].contains("set_text") ? true : p[0].startsWith("ui.");
            QCOMPARE(p[1] == "true", allowed);
        }
    }
    void allApisWorkWithConsent() {
        Rig r;
        QString err;
        QVERIFY2(r.setup(fixture("api-probe"), {}, true, &err), qPrintable(err));
        QVERIFY2(r.mgr->runCommand("api-probe:probe", &r.note, &err), qPrintable(err));
        for (const auto &l : r.logsOf("api-probe")) QVERIFY2(l.split('|')[1] == "true", qPrintable(l));
        QCOMPARE(r.note.ops, (QStringList{"replace:x", "insert:x", "set:x"}));
        QCOMPARE(r.note.begins, 1);
        QCOMPARE(r.note.ends, 1);
        QCOMPARE(r.lib.calls, (QStringList{"list:q", "read:a.md", "create:t", "write:a.md", "delete:a.md"}));
        QCOMPARE(r.ui.notes, QStringList{"hi"});
        QCOMPARE(r.ui.prompts, 1);
        QCOMPARE(r.clip.v, QString("c"));
        QCOMPARE(r.theme.sets, QStringList{"accent=#ff0000"});
        QCOMPARE(r.net.reqs.size(), 2);
        QCOMPARE(r.net.reqs[1].method, QString("POST"));
        QCOMPARE(r.net.reqs[1].body, QByteArray("{}"));
    }
    void noActiveNoteIsAnErrorNotACrash() {
        Rig r;
        QVERIFY(r.setup(fixture("api-probe")));
        QVERIFY(r.mgr->runCommand("api-probe:probe", nullptr));  // pcall'd inside: no note => errors, no crash
        int noNote = 0;
        for (const auto &l : r.logsOf("api-probe")) noNote += l.contains("no active note");
        QCOMPARE(noNote, 8);
    }

    // ---- hn.http through the fake NetBridge ----
    void httpAllowlistHttpsOnlyAndAudit() {
        Rig r;
        QString err;
        QVERIFY2(r.setup(fixture("http-probe"), {}, true, &err), qPrintable(err));
        QVERIFY(r.mgr->runCommand("http-probe:allowed", &r.note, &err));
        QCOMPARE(r.net.reqs.size(), 2);
        QCOMPARE(r.net.reqs[0].url, QString("https://api.example.com/v1/items"));
        QCOMPARE(r.net.reqs[0].allowedHosts, (QStringList{"api.example.com", "cdn.example.org"}));
        QCOMPARE(r.net.reqs[0].timeoutMs, 10000);
        QCOMPARE(r.net.reqs[0].maxResponseBytes, qint64(1024 * 1024));
        QCOMPARE(lines(r.logsOf("http-probe"), "res|").size(), 2);
        QVERIFY(r.logsOf("http-probe").first().endsWith("|ok|200|ok"));
        r.net.reqs.clear();
        r.logs.clear();
        QVERIFY(r.mgr->runCommand("http-probe:denied", &r.note, &err));
        QCOMPARE(r.net.reqs.size(), 0);  // none of these reached the bridge
        const auto res = r.logsOf("http-probe");
        QCOMPARE(res.size(), 8);
        for (const auto &l : res) QVERIFY2(l.contains("|error|") && l.contains("permission denied: network"), qPrintable(l));
        const QString audit = readFile(r.env.state() + "/plugins-audit.jsonl");
        QVERIFY(audit.contains("only https"));
        QVERIFY(audit.contains("not listed in net_hosts"));
        // audited network calls carry the host only, never path/query/body
        QVERIFY(audit.contains("\"event\":\"network\""));
        QVERIFY(audit.contains("GET api.example.com\""));
        QVERIFY(!audit.contains("v1/items"));
    }
    void httpRateLimit() {
        Rig r;
        QVERIFY(r.setup(fixture("http-probe")));
        QString err;
        QVERIFY(r.mgr->runCommand("http-probe:flood", &r.note, &err));
        QCOMPARE(r.net.reqs.size(), 20);  // 20 per minute
        const auto res = r.logsOf("http-probe");
        int limited = 0;
        for (const auto &l : res) limited += l.contains("rate limit exceeded");
        QCOMPARE(limited, 5);
        QVERIFY(readFile(r.env.state() + "/plugins-audit.jsonl").contains("rate limit"));
    }
    void httpHeadersAndBridgeErrors() {
        Rig r;
        QVERIFY(r.setup(fixture("http-probe")));
        QString err;
        QVERIFY(r.mgr->runCommand("http-probe:headers", &r.note, &err));
        QCOMPARE(r.net.reqs.size(), 1);  // the Host header attempt was rejected
        QCOMPARE(r.net.reqs[0].headers.size(), 1);
        QCOMPARE(r.net.reqs[0].headers[0].first, QString("X-Token"));
        QVERIFY(r.logsOf("http-probe").last().contains("header 'Host' is not allowed"));
        // transport errors / oversize responses come back as nil,err and do not trip the breaker
        r.logs.clear();
        r.net.error = "timeout";
        QVERIFY(r.mgr->runCommand("http-probe:allowed", &r.note, &err));
        QVERIFY(r.logsOf("http-probe").first().contains("|nil|timeout"));
        r.net.error.clear();
        r.net.body = QByteArray(2 * 1024 * 1024, 'x');
        r.logs.clear();
        QVERIFY(r.mgr->runCommand("http-probe:allowed", &r.note, &err));
        QVERIFY(r.logsOf("http-probe").first().contains("response too large"));
        QCOMPARE(r.mgr->trust()->record("http-probe").failures, 0);
    }
    void notifyRateLimited() {
        Rig r;
        QVERIFY(r.addLua("spam", {"ui"}, "hn.command{id='c',title='C',run=function() local n=0 for i=1,30 do if hn.ui.notify('m'..i) then n=n+1 end end hn.log('sent '..n) end}\n"));
        QVERIFY(r.mgr->runCommand("spam:c", &r.note));
        QCOMPARE(r.ui.notes.size(), 10);
        QCOMPARE(r.logsOf("spam"), QStringList{"sent 10"});
    }
    void noteWritesPerCallbackCapped() {
        Rig r;
        QVERIFY(r.addLua("wr", {"notes.write"}, "hn.command{id='c',title='C',run=function() for i=1,100 do hn.notes.write('a.md','x') end end}\n"));
        QString err;
        QVERIFY(!r.mgr->runCommand("wr:c", &r.note, &err));
        QVERIFY2(err.contains("too many note writes"), qPrintable(err));
        QCOMPARE(r.lib.calls.size(), 50);
        QVERIFY(!r.addLua("wr2", {"notes.read"}, "hn.command{id='c',title='C',run=function() hn.notes.read('../../etc/passwd') end}\n") == false);
        QVERIFY(!r.mgr->runCommand("wr2:c", &r.note, &err));
        QVERIFY(err.contains("invalid note path"));
    }
    void storageCapAndTypes() {
        Rig r;
        QVERIFY(r.addLua("st", {"storage"},
            "hn.command{id='fill',title='F',run=function()\n"
            "  local chunk = string.rep('x', 100000)\n"
            "  for i = 1, 20 do hn.storage.set('k' .. i, chunk) end\n"
            "end}\n"
            "hn.command{id='types',title='T',run=function()\n"
            "  hn.storage.set('t', {a=1, b={true,'x',2.5}, c='s'})\n"
            "  local t = hn.storage.get('t')\n"
            "  hn.log(t.a .. '|' .. tostring(t.b[1]) .. '|' .. t.b[2] .. '|' .. t.b[3] .. '|' .. t.c)\n"
            "  hn.log(tostring(pcall(hn.storage.set, 'f', function() end)))\n"
            "  hn.log(tostring(pcall(hn.storage.set, 'm', {1, 2, x = 3})))\n"
            "  local cyc = {} cyc.self = cyc\n"
            "  hn.log(tostring(pcall(hn.storage.set, 'cyc', cyc)))\n"
            "  hn.storage.set('t', nil)\n"
            "  hn.log(tostring(hn.storage.get('t')))\n"
            "  hn.log(hn.json.encode({1,2,3}) .. hn.json.encode({a='b'}) .. hn.json.encode('s'))\n"
            "  hn.log(tostring(hn.json.decode('{\"a\":[1,2]}').a[2]) .. tostring(hn.json.decode('nope')))\n"
            "end}\n"));
        QString err;
        QVERIFY(r.mgr->runCommand("st:types", &r.note, &err));
        QCOMPARE(r.logsOf("st"), (QStringList{"1|true|x|2.5|s", "false", "false", "false", "nil", "[1,2,3]{\"a\":\"b\"}\"s\"", "2nil"}));
        QVERIFY(!r.mgr->runCommand("st:fill", &r.note, &err));
        QVERIFY2(err.contains("quota"), qPrintable(err));
        const qint64 sz = QFileInfo(r.env.state() + "/plugin-data/st.json").size();
        QVERIFY2(sz <= 1024 * 1024 + 200, qPrintable(QString::number(sz)));
        QVERIFY(sz > 900 * 1024);
        qInfo().noquote() << QString("storage cap: file is %1 bytes after filling past 1 MiB (cap 1048576)").arg(sz);
    }
    void themeTokenValidation() {
        Rig r;
        QVERIFY(r.addLua("th", {"theme"}, "hn.command{id='c',title='C',run=function() hn.log(tostring(pcall(hn.theme.set_token, 'Bad Token', 'x')))\n hn.theme.set_token('accent','red') end}\n"));
        QVERIFY(r.mgr->runCommand("th:c", &r.note));
        QCOMPARE(r.logsOf("th"), QStringList{"false"});
        QCOMPARE(r.theme.sets, QStringList{"accent=red"});
    }
    void uiBlockingDoesNotEatBudget() {
        struct SlowUi : FakeUi {
            std::optional<QString> prompt(const QString &, const QString &, const QString &, const QString &) override { QTest::qWait(300); return QString("done"); }
        };
        SlowUi ui;
        TmpEnv env;
        ManagerConfig c;
        c.bridges.ui = &ui;
        PluginManager m(c);
        m.scan();
        const auto dir = writePlugin(env.src(), "slow", {"ui"}, "hn.on('note.opened', function() local s = hn.ui.prompt('t','l','d') hn.ui.notify(s) end)\n");
        QVERIFY(m.install(dir).ok && m.consent("slow", {"ui"}) && m.enable("slow"));
        m.post("note.opened", "a", nullptr);  // event budget is 50 ms; the prompt blocks 300 ms
        QCOMPARE(ui.notes, QStringList{"done"});
        QCOMPARE(m.trust()->record("slow").failures, 0);
    }
};
QTEST_MAIN(RuntimeTest)
#include "plugins_runtime_test.moc"
