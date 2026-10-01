#include "plugins_test_util.h"

using namespace hn::plugins;
using namespace hn::plugins::test;

class TrustTest : public QObject {
    Q_OBJECT
private slots:
    void storePersistsAndRequiresConsent() {
        TmpEnv env;
        const QString path = env.state() + "/plugins.json";
        TrustStore t(path);
        QVERIFY(t.load());
        QVERIFY(!t.record("a").enabled);
        QString err;
        QVERIFY(!t.setEnabled("a", true, &err));  // no consent => cannot enable
        QVERIFY(err.contains("consent"));
        QVERIFY(t.recordConsent("a", {"ui", "network"}, "abc123"));
        QVERIFY(!t.record("a").enabled);  // consent alone does not enable
        QVERIFY(t.setEnabled("a", true));
        TrustStore t2(path);
        QVERIFY(t2.load());
        QVERIFY(t2.record("a").enabled);
        QCOMPARE(t2.record("a").permissions, (QStringList{"ui", "network"}));
        QCOMPARE(t2.record("a").hash, QString("abc123"));
        QVERIFY(!t2.record("a").consentedAt.isEmpty());
        QVERIFY(!t2.recordConsent("a", {"ui"}, ""));
    }
    void hashChangeDisablesAndNeedsReconsent() {
        TmpEnv env;
        TrustStore t(env.state() + "/plugins.json");
        t.recordConsent("a", {"ui"}, "h1");
        t.setEnabled("a", true);
        QVERIFY(!t.verifyHash("a", "h1"));
        QVERIFY(t.record("a").enabled);
        QVERIFY(t.verifyHash("a", "h2"));
        QVERIFY(!t.record("a").enabled);
        QVERIFY(t.record("a").needsReconsent);
        QString err;
        QVERIFY(!t.setEnabled("a", true, &err));  // blocked until fresh consent
        QVERIFY(t.recordConsent("a", {"ui"}, "h2"));
        QVERIFY(!t.record("a").needsReconsent);
        QVERIFY(t.setEnabled("a", true));
        // persisted
        TrustStore t2(env.state() + "/plugins.json");
        t2.load();
        QVERIFY(t2.record("a").enabled);
        QVERIFY(!t.verifyHash("unknown", "x"));
    }
    void editedStateFileCannotEnableWithoutConsent() {
        TmpEnv env;
        const QString path = env.state() + "/plugins.json";
        writeFile(path, R"({"version":1,"plugins":{"a":{"enabled":true},"b":{"enabled":true,"hasConsent":true,"hash":"x","needsReconsent":true}}})");
        TrustStore t(path);
        QVERIFY(t.load());
        QVERIFY(!t.record("a").enabled);
        QVERIFY(!t.record("b").enabled);
        writeFile(path, "garbage{");
        TrustStore t3(path);
        QVERIFY(!t3.load());
        QVERIFY(!t3.record("a").enabled);
    }
    void failureCounter() {
        TmpEnv env;
        TrustStore t(env.state() + "/plugins.json");
        QCOMPARE(t.addFailure("a"), 1);
        QCOMPARE(t.addFailure("a"), 2);
        t.resetFailures("a");
        QCOMPARE(t.record("a").failures, 0);
        t.addFailure("a");
        TrustStore t2(env.state() + "/plugins.json");
        t2.load();
        QCOMPARE(t2.record("a").failures, 1);
        t2.remove("a");
        QVERIFY(!t2.known("a"));
    }
    void auditAppendOnlyAndRotation() {
        TmpEnv env;
        const QString p = env.state() + "/audit.jsonl";
        AuditLog log(p);
        log.log("install", "a", "first");
        log.log("consent", "a", "second\nwith newline");
        auto lines = readFile(p).split('\n');
        QCOMPARE(lines.size(), 3);  // two entries + trailing empty
        const auto o = QJsonDocument::fromJson(lines[1]).object();
        QCOMPARE(o["event"].toString(), QString("consent"));
        QCOMPARE(o["plugin"].toString(), QString("a"));
        QVERIFY(!o["detail"].toString().contains('\n'));
        QVERIFY(!o["ts"].toString().isEmpty());
        // rotation: 1 MiB x 3 files, oldest dropped
        const QString big(900, 'x');
        for (int i = 0; i < 6000; ++i) log.log("failure", "a", big + QString::number(i));
        QVERIFY(QFileInfo(p).exists());
        QVERIFY(QFileInfo(p + ".1").exists());
        QVERIFY(QFileInfo(p + ".2").exists());
        QVERIFY(!QFileInfo(p + ".3").exists());
        for (const auto &f : {p, p + ".1", p + ".2"}) QVERIFY2(QFileInfo(f).size() < 1024 * 1024 + 2000, qPrintable(f));
        QVERIFY(!readFile(p + ".2").contains("\"first\""));  // oldest entries rotated away
        qInfo() << "audit rotation: sizes" << QFileInfo(p).size() << QFileInfo(p + ".1").size() << QFileInfo(p + ".2").size();
    }
    void auditDenialFloodIsCapped() {
        TmpEnv env;
        AuditLog log(env.state() + "/audit.jsonl");
        for (int i = 0; i < 1000; ++i) log.log("denied", "spam", "note.read");
        QVERIFY(readFile(env.state() + "/audit.jsonl").count('\n') <= 201);
        log.log("denied", "other", "ui");
        QVERIFY(readFile(env.state() + "/audit.jsonl").contains("other"));
    }
    // ---- through the manager ----
    void managerConsentFlowAndTamper() {
        Rig r;
        const auto dir = writePlugin(r.env.src(), "flow", {"ui", "note.read"}, "hn.command{id='c',title='C',run=function() hn.ui.notify('x') end}\n");
        QVERIFY(r.mgr->install(dir).ok);
        auto info = r.mgr->info("flow");
        QCOMPARE(info.status, Status::NeedsConsent);  // default: not enabled, nothing consented
        QVERIFY(!info.consented);
        QString err;
        QVERIFY(!r.mgr->enable("flow", &err));
        QVERIFY(err.contains("consent"));
        QVERIFY(!r.mgr->consent("flow", {"clipboard"}, &err));  // not requested by the manifest
        QVERIFY(r.mgr->consent("flow", {"ui", "note.read"}, &err));
        QCOMPARE(r.mgr->info("flow").status, Status::Disabled);
        QVERIFY2(r.mgr->enable("flow", &err), qPrintable(err));
        QCOMPARE(r.mgr->info("flow").status, Status::Enabled);
        QVERIFY(r.mgr->runCommand("flow:c", &r.note));
        QCOMPARE(r.ui.notes, QStringList{"x"});

        // tamper with an installed file behind the app's back
        QTest::qWait(5);
        QFile f(r.env.plugins() + "/flow/main.lua");
        QVERIFY(f.open(QIODevice::Append));
        f.write("\n-- evil\n");
        f.close();
        // a fresh app start (scan) detects it
        r.mgr->scan();
        info = r.mgr->info("flow");
        QCOMPARE(info.status, Status::NeedsConsent);
        QVERIFY(info.needsReconsent);
        QVERIFY(!r.mgr->enable("flow", &err));
        QVERIFY(r.mgr->registry()->commands().isEmpty());
        QVERIFY(readFile(r.env.state() + "/plugins-audit.jsonl").contains("\"tamper\""));
        // re-consent restores it
        QVERIFY(r.mgr->consent("flow", {"ui", "note.read"}, &err));
        QVERIFY(r.mgr->enable("flow", &err));
    }
    void tamperBetweenLoadsDetectedAtLazyLoad() {
        Rig r;
        QVERIFY(r.addLua("lazy", {"ui"}, "hn.command{id='c',title='C',run=function() hn.ui.notify('x') end}\n"));
        r.mgr->scan();  // restart: registrations come from the cache, no Lua state
        QCOMPARE(r.mgr->loadedStates(), 0);
        QCOMPARE(r.mgr->registry()->commands().size(), 1);
        QFile f(r.env.plugins() + "/lazy/main.lua");
        QVERIFY(f.open(QIODevice::Append));
        f.write("\n-- changed after scan\n");
        f.close();
        QString err;
        QVERIFY(!r.mgr->runCommand("lazy:c", &r.note, &err));
        QVERIFY2(err.contains("changed"), qPrintable(err));
        QCOMPARE(r.mgr->loadedStates(), 0);
        QVERIFY(r.ui.notes.isEmpty());
        QCOMPARE(r.mgr->info("lazy").status, Status::NeedsConsent);
    }
    void upgradeThroughManager() {
        Rig r;
        const auto dir = writePlugin(r.env.src(), "upgm", {"ui"}, "hn.command{id='c',title='V1',run=function() hn.ui.notify('v1') end}\n");
        QVERIFY(r.setup(dir));
        QVERIFY(r.mgr->runCommand("upgm:c", &r.note));
        auto res = r.mgr->install(dir, true);  // identical
        QVERIFY(res.ok && res.consentKept);
        QCOMPARE(r.mgr->info("upgm").status, Status::Enabled);
        writeFile(dir + "/main.lua", "hn.command{id='c',title='V2',run=function() hn.ui.notify('v2') end}\n");
        res = r.mgr->install(dir, true);
        QVERIFY(res.ok && !res.consentKept);
        QCOMPARE(r.mgr->info("upgm").status, Status::NeedsConsent);
        QVERIFY(r.mgr->registry()->commands().isEmpty());
        QString err;
        QVERIFY(!r.mgr->runCommand("upgm:c", &r.note, &err));
        QVERIFY(r.mgr->consent("upgm", {"ui"}) && r.mgr->enable("upgm"));
        QVERIFY(r.mgr->runCommand("upgm:c", &r.note));
        QCOMPARE(r.ui.notes, (QStringList{"v1", "v2"}));
    }
    void removeCleansEverything() {
        Rig r;
        QVERIFY(r.addLua("rm-me", {"storage"}, "hn.command{id='c',title='C',run=function() hn.storage.set('k', 1) end}\n"));
        QVERIFY(r.mgr->runCommand("rm-me:c", &r.note));
        QVERIFY(QFileInfo::exists(r.env.state() + "/plugin-data/rm-me.json"));
        QString err;
        QVERIFY(r.mgr->remove("rm-me", &err));
        QVERIFY(!QFileInfo::exists(r.env.plugins() + "/rm-me"));
        QVERIFY(!QFileInfo::exists(r.env.state() + "/plugin-data/rm-me.json"));
        QVERIFY(!r.mgr->trust()->known("rm-me"));
        QVERIFY(r.mgr->plugins().isEmpty());
        QVERIFY(readFile(r.env.state() + "/plugins-audit.jsonl").contains("\"remove\""));
    }
    void invalidPluginsListedAsFailed() {
        TmpEnv env;
        writeFile(env.plugins() + "/broken/plugin.json", "{ nope");
        writeFile(env.plugins() + "/mismatch/plugin.json", manifestJson("other-name", {}));
        writeFile(env.plugins() + "/mismatch/main.lua", "");
        ManagerConfig c;
        PluginManager m(c);
        m.scan();
        QCOMPARE(m.plugins().size(), 2);
        for (const auto &p : m.plugins()) { QCOMPARE(p.status, Status::Failed); QVERIFY(!p.detail.isEmpty()); }
        QVERIFY(m.errors().size() >= 2);
    }
    void nativeTierUsesAdapterAndStub() {
        struct Spy : NativePluginAdapter {
            QStringList calls;
            bool activate(const Manifest &m, QString *) override { calls << "activate:" + m.id; return true; }
            void deactivate(const QString &id) override { calls << "deactivate:" + id; }
            bool runCommand(const QString &p, const QString &c, NoteBridge *, QString *) override { calls << "run:" + p + ":" + c; return true; }
            void postEvent(const QString &p, const QString &e, const QString &) override { calls << "event:" + p + ":" + e; }
        };
        TmpEnv env;
        const QString dir = env.src() + "/nat";
        writeFile(dir + "/plugin.json", manifestJson("nat", {"native"}, {}, "native", "libnat.so"));
        writeFile(dir + "/libnat.so", QByteArray("\x7f""ELF", 4) + QByteArray(64, 0));
        {  // stub adapter: enabling fails with a clear message and leaves it disabled
            ManagerConfig c;
            PluginManager m(c);
            m.scan();
            auto res = m.install(dir);
            QVERIFY2(res.ok, res.errors.isEmpty() ? "" : qPrintable(res.errors.first().message));
            QString err;
            QVERIFY(!m.consent("nat", {"ui"}, &err));  // native consent must include 'native'
            QVERIFY(m.consent("nat", {"native"}, &err));
            QVERIFY(!m.enable("nat", &err));
            QVERIFY2(err.contains("not wired"), qPrintable(err));
            QVERIFY(!m.trust()->record("nat").enabled);
            QVERIFY(m.remove("nat"));
        }
        Spy spy;
        ManagerConfig c;
        c.native = &spy;
        PluginManager m(c);
        m.scan();
        QVERIFY(m.install(dir).ok);
        QVERIFY(m.consent("nat", {"native"}));
        QString err;
        QVERIFY2(m.enable("nat", &err), qPrintable(err));
        QVERIFY(m.runCommand("nat:go", nullptr));
        m.post("note.saved", "p", nullptr);
        m.disable("nat");
        QCOMPARE(spy.calls, (QStringList{"activate:nat", "run:nat:go", "event:nat:note.saved", "deactivate:nat"}));
    }
};
QTEST_MAIN(TrustTest)
#include "plugins_trust_test.moc"
