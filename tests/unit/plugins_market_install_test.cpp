// Package pre-flight: the downloaded file must match its index entry and this app before anything is installed.
#include "hn/plugins/market.h"
#include "hn/plugins/store.h"
#include "market_test_util.h"
#include "plugins_test_util.h"

using namespace hn::plugins;
using namespace hn::plugins::test;

class MarketInstallTest : public QObject {
    Q_OBJECT
    // A packed "demo" plugin (manifest version 1.0.0, permission ui, API 2) and an entry that matches it.
    struct Fx {
        TmpEnv env;
        QString file;
        MarketEntry e;
        Fx(const QStringList &perms = {"ui"}, const QStringList &hosts = {}, const QByteArray &manifestOverride = {}) {
            const QString dir = writePlugin(env.src(), "demo", perms, "hn.log('x')", hosts, 2);
            if (!manifestOverride.isEmpty()) writeFile(dir + "/plugin.json", manifestOverride);
            file = env.tmp.path() + "/demo.hnplugin";
            QList<PluginError> errs;
            packDirectory(dir, file, &errs);
            e = markettest::entryFor("demo", "1.0.0", readFile(file), "https://example.invalid/demo.hnplugin");
            e.permissions = perms;
            e.netHosts = hosts;
        }
    };

private slots:
    void matching_package_passes() {
        Fx f;
        const auto p = preflightPackage(f.file, f.e);
        QVERIFY2(p.ok, qPrintable(p.error));
        QCOMPARE(p.check.manifest.id, QString("demo"));
    }
    void id_mismatch_refused() {
        Fx f;
        f.e.id = "other";
        const auto p = preflightPackage(f.file, f.e);
        QVERIFY(!p.ok);
        QVERIFY2(p.error.contains("other"), qPrintable(p.error));
    }
    void version_mismatch_refused() {
        Fx f;
        f.e.version = "1.0.1";
        const auto p = preflightPackage(f.file, f.e);
        QVERIFY(!p.ok);
        QVERIFY2(p.error.contains("1.0.1") && p.error.contains("1.0.0"), qPrintable(p.error));
    }
    void permissions_mismatch_refused() {
        Fx f({"ui", "storage"});
        f.e.permissions = {"ui"};   // the dangerous direction: the index lists fewer than the manifest asks for
        QVERIFY(!preflightPackage(f.file, f.e).ok);
        f.e.permissions = {"ui", "storage", "clipboard"};
        QVERIFY(!preflightPackage(f.file, f.e).ok);
        f.e.permissions = {"storage", "ui"};   // order does not matter
        QVERIFY(preflightPackage(f.file, f.e).ok);
    }
    void net_hosts_mismatch_refused() {
        Fx f({"ui", "network"}, {"api.example.com"});
        QVERIFY(preflightPackage(f.file, f.e).ok);
        f.e.netHosts = {"evil.example.com"};
        QVERIFY(!preflightPackage(f.file, f.e).ok);
        f.e.netHosts = {};
        QVERIFY(!preflightPackage(f.file, f.e).ok);
    }
    void native_entry_refused() {
        Fx f;
        f.e.tier = "native";
        const auto p = preflightPackage(QString("/does/not/exist.hnplugin"), f.e);   // refused before any file is read
        QVERIFY(!p.ok);
        QVERIFY2(p.error.contains("signed"), qPrintable(p.error));
    }
    void tier_in_manifest_native_refused() {
        Fx f;   // the packer may refuse a native manifest, so build the archive by hand
        QVERIFY(makeArchive(f.file, {{"plugin.json", manifestJson("demo", {}, {}, "native", "main.lua", 2)}, {"main.lua", "hn.log('x')"}}));
        const auto p = preflightPackage(f.file, f.e);
        QVERIFY(!p.ok);
        QVERIFY2(p.error.contains("ative"), qPrintable(p.error));
    }
    void min_app_too_new_refused() {
        QJsonObject o = QJsonDocument::fromJson(manifestJson("demo", {"ui"}, {}, "script", "main.lua", 2)).object();
        o["min_app"] = "99.0.0";
        Fx f;   // packDirectory refuses a too-new manifest itself, so build the archive by hand
        QVERIFY(makeArchive(f.file, {{"plugin.json", QJsonDocument(o).toJson()}, {"main.lua", "hn.log('x')"}}));
        const auto p = preflightPackage(f.file, f.e, "0.1.0");
        QVERIFY(!p.ok);
        QVERIFY2(p.error.contains("99.0.0"), qPrintable(p.error));
    }
    void broken_archive_refused() {
        TmpEnv env;
        const QString file = env.tmp.path() + "/junk.hnplugin";
        writeFile(file, QByteArray(4096, '\x5a'));
        MarketEntry e = markettest::entryFor("demo", "1.0.0", readFile(file), "https://example.invalid/x");
        const auto p = preflightPackage(file, e);
        QVERIFY(!p.ok);
        QVERIFY2(!p.error.isEmpty(), "a refusal must say why");
    }
    void nothing_installed_by_preflight() {
        Fx f;
        const int before = QDir(QDir::tempPath()).entryList({"hn-*"}, QDir::Dirs | QDir::NoDotAndDotDot).size();
        QVERIFY(preflightPackage(f.file, f.e).ok);
        QVERIFY(!QFileInfo::exists(f.env.plugins()));
        QCOMPARE(QDir(QDir::tempPath()).entryList({"hn-*"}, QDir::Dirs | QDir::NoDotAndDotDot).size(), before);
    }
};

QTEST_MAIN(MarketInstallTest)
#include "plugins_market_install_test.moc"
