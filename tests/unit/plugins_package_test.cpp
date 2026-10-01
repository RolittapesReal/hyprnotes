#include "plugins_test_util.h"
#include <QRandomGenerator>

using namespace hn::plugins;
using namespace hn::plugins::test;

class PackageTest : public QObject {
    Q_OBJECT
    TmpEnv env;
    std::unique_ptr<TrustStore> trust;
    std::unique_ptr<AuditLog> audit;
    std::unique_ptr<PackageInstaller> inst;
    QString arch(const QString &n) const { return env.tmp.path() + "/" + n; }
    QString allErrors(const InstallResult &r) const { QStringList l; for (const auto &e : r.errors) l << e.message; return l.join(" | "); }
    void expectReject(const QString &archive, const QString &needle) {
        const auto r = inst->install(archive);
        QVERIFY2(!r.ok, qPrintable(archive));
        QVERIFY2(allErrors(r).contains(needle, Qt::CaseInsensitive), qPrintable(allErrors(r)));
        // nothing leaked into the plugins dir except possibly hidden staging
        for (const auto &e : QDir(env.plugins()).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) QFAIL(qPrintable("installed: " + e));
        QVERIFY(QDir(env.plugins()).entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden).isEmpty());  // staging cleaned up
    }
private slots:
    void init() {
        QDir(env.plugins()).removeRecursively();
        QDir(env.state()).removeRecursively();
        QDir(env.src()).removeRecursively();
        trust = std::make_unique<TrustStore>(env.state() + "/plugins.json");
        audit = std::make_unique<AuditLog>(env.state() + "/audit.jsonl");
        inst = std::make_unique<PackageInstaller>(env.plugins(), trust.get(), audit.get());
    }
    void installFromFolder() {
        const auto dir = writePlugin(env.src(), "folder-one", {"ui"}, "hn.command{id='a',title='A',run=function() end}\n");
        const auto r = inst->install(dir);
        QVERIFY2(r.ok, qPrintable(allErrors(r)));
        QCOMPARE(r.id, QString("folder-one"));
        QCOMPARE(r.hash.size(), 64);
        QVERIFY(QFileInfo(env.plugins() + "/folder-one/main.lua").isFile());
        QCOMPARE(hashDirectory(env.plugins() + "/folder-one"), r.hash);
        QVERIFY(readFile(audit->path()).contains("\"install\""));
        QVERIFY(!trust->record("folder-one").hasConsent);  // default: nothing consented
        QVERIFY(!trust->record("folder-one").enabled);
    }
    void installFromArchiveAndPackRoundtrip() {
        const auto dir = writePlugin(env.src(), "round-trip", {"ui"}, "-- hi\n");
        QList<PluginError> errs;
        QVERIFY(packDirectory(dir, arch("rt.hnplugin"), &errs));
        QVERIFY(errs.isEmpty());
        QVERIFY(QFileInfo(arch("rt.hnplugin")).size() < 5000);
        const auto r = inst->install(arch("rt.hnplugin"));
        QVERIFY2(r.ok, qPrintable(allErrors(r)));
        QCOMPARE(hashDirectory(env.plugins() + "/round-trip"), hashDirectory(dir));
        // deterministic archives
        QVERIFY(packDirectory(dir, arch("rt2.hnplugin"), &errs));
        QCOMPARE(readFile(arch("rt.hnplugin")), readFile(arch("rt2.hnplugin")));
    }
    void packRejectsInvalidAndOutputInside() {
        const auto dir = writePlugin(env.src(), "p-bad", {"ui"}, "this is not lua (\n");
        QList<PluginError> errs;
        QVERIFY(!packDirectory(dir, arch("bad.hnplugin"), &errs));
        QVERIFY(errs.first().message.contains("syntax", Qt::CaseInsensitive));
        const auto ok = writePlugin(env.src(), "p-ok", {"ui"}, "-- ok\n");
        errs.clear();
        QVERIFY(!packDirectory(ok, ok + "/out.hnplugin", &errs));
    }
    void tarGzAndWrappedFolder() {
        // "archive of the folder itself": single top-level directory
        QList<AEntry> e{{"wrapped/", {}, AE_IFDIR}, {"wrapped/plugin.json", manifestJson("wrapped", {"ui"})}, {"wrapped/main.lua", "-- x\n"}};
        QVERIFY(makeArchive(arch("w.tar"), e, false));
        const auto r = inst->install(arch("w.tar"));
        QVERIFY2(r.ok, qPrintable(allErrors(r)));
        QVERIFY(QFileInfo(env.plugins() + "/wrapped/main.lua").isFile());
    }
    void zipSlip_data() {
        QTest::addColumn<QString>("name");
        QTest::addColumn<bool>("zip");
        QTest::newRow("../ zip") << "../evil.lua" << true;
        QTest::newRow("deep ../ zip") << "a/b/../../../evil.lua" << true;
        QTest::newRow("../ tar") << "../evil.lua" << false;
        QTest::newRow("abs tar") << "/tmp/evil.lua" << false;
        QTest::newRow("abs zip") << "/tmp/evil.lua" << true;
        QTest::newRow("backslash") << "..\\evil.lua" << true;
        QTest::newRow("double slash") << "a//evil.lua" << false;
        QTest::newRow("drive") << "C:evil.lua" << true;
    }
    void zipSlip() {
        QFETCH(QString, name);
        QFETCH(bool, zip);
        auto e = goodEntries("slip");
        e << AEntry{name, "x"};
        QVERIFY(makeArchive(arch("slip.hnplugin"), e, zip));
        const auto r = inst->install(arch("slip.hnplugin"));
        QVERIFY2(!r.ok, qPrintable(name));
        QVERIFY2(allErrors(r).contains("unsafe path") || allErrors(r).contains("escapes") || allErrors(r).contains("corrupt"), qPrintable(allErrors(r)));
        QVERIFY(!QFileInfo::exists(env.tmp.path() + "/evil.lua"));
        QVERIFY(!QFileInfo::exists("/tmp/evil.lua"));
        QVERIFY(!QFileInfo::exists(env.plugins() + "/evil.lua"));
        QVERIFY(!QFileInfo::exists(env.plugins() + "/slip"));
    }
    void symlinkHardlinkDeviceRejected() {
        auto e = goodEntries("links");
        e << AEntry{"link.lua", {}, AE_IFLNK, "/etc/passwd"};
        QVERIFY(makeArchive(arch("sym.hnplugin"), e, false));
        expectReject(arch("sym.hnplugin"), "link");
        e = goodEntries("links");
        e << AEntry{"hard.lua", {}, AE_IFREG, "main.lua", true};
        QVERIFY(makeArchive(arch("hard.hnplugin"), e, false));
        expectReject(arch("hard.hnplugin"), "link");
        e = goodEntries("links");
        e << AEntry{"dev", {}, AE_IFCHR};
        QVERIFY(makeArchive(arch("dev.hnplugin"), e, false));
        expectReject(arch("dev.hnplugin"), "regular file");
        // zip symlink too
        e = goodEntries("links");
        e << AEntry{"link2.lua", {}, AE_IFLNK, "main.lua"};
        QVERIFY(makeArchive(arch("symzip.hnplugin"), e, true));
        expectReject(arch("symzip.hnplugin"), "link");
    }
    void symlinkInFolderRejected() {
        const auto dir = writePlugin(env.src(), "folder-link", {"ui"}, "-- x\n");
        QVERIFY(QFile::link("/etc/passwd", dir + "/passwd.lua"));
        const auto r = inst->install(dir);
        QVERIFY(!r.ok);
        QVERIFY2(allErrors(r).contains("symbolic link"), qPrintable(allErrors(r)));
    }
    void oversizeArchive() {
        QByteArray big(6 * 1024 * 1024, 0);
        QRandomGenerator rng(7);
        for (auto &c : big) c = char(rng.bounded(256));  // incompressible
        auto e = goodEntries("big");
        e << AEntry{"blob.bin", big};
        QVERIFY(makeArchive(arch("big.hnplugin"), e));
        QVERIFY(QFileInfo(arch("big.hnplugin")).size() > kMaxArchiveBytes);
        expectReject(arch("big.hnplugin"), "5 MiB");
    }
    void zipBombRejected() {
        QByteArray zeros(30 * 1024 * 1024, 0);  // compresses to ~30 KiB, expands past the unpack cap
        auto e = goodEntries("bomb");
        for (int i = 0; i < 3; ++i) e << AEntry{QString("z%1.txt").arg(i), zeros};
        QVERIFY(makeArchive(arch("bomb.hnplugin"), e));
        QVERIFY(QFileInfo(arch("bomb.hnplugin")).size() < kMaxArchiveBytes);
        const auto r = inst->install(arch("bomb.hnplugin"));
        QVERIFY(!r.ok);
        QVERIFY2(allErrors(r).contains("larger than") || allErrors(r).contains("expands"), qPrintable(allErrors(r)));
    }
    void tooManyFiles() {
        auto e = goodEntries("many");
        for (int i = 0; i < 201; ++i) e << AEntry{QString("f/%1.txt").arg(i), "x"};
        QVERIFY(makeArchive(arch("many.hnplugin"), e));
        expectReject(arch("many.hnplugin"), "200");
        // 200 total is fine
        e = goodEntries("many");
        for (int i = 0; i < 198; ++i) e << AEntry{QString("f/%1.txt").arg(i), "x"};
        QVERIFY(makeArchive(arch("ok200.hnplugin"), e));
        const auto r = inst->install(arch("ok200.hnplugin"));
        QVERIFY2(r.ok, qPrintable(allErrors(r)));
    }
    void scriptTooLarge() {
        auto e = goodEntries("fat", QByteArray("-- ") + QByteArray(1024 * 1024 + 10, 'a') + "\n");
        QVERIFY(makeArchive(arch("fat.hnplugin"), e));
        expectReject(arch("fat.hnplugin"), "1 MiB");
    }
    void nativeLibsRejectedInScriptTier() {
        auto e = goodEntries("elf");
        e << AEntry{"data.dat", QByteArray("\x7f""ELF\x02\x01\x01", 7) + QByteArray(100, 0)};
        QVERIFY(makeArchive(arch("elf.hnplugin"), e));
        expectReject(arch("elf.hnplugin"), "native");
        e = goodEntries("so");
        e << AEntry{"libx.so", "not really"};
        QVERIFY(makeArchive(arch("so.hnplugin"), e));
        expectReject(arch("so.hnplugin"), "native");
        e = goodEntries("exe");
        e << AEntry{"tool", QByteArray("MZ\x90\x00", 4)};
        QVERIFY(makeArchive(arch("exe.hnplugin"), e));
        expectReject(arch("exe.hnplugin"), "native");
        e = goodEntries("sh");
        e << AEntry{"run.sh", "#!/bin/sh\nrm -rf ~\n"};
        QVERIFY(makeArchive(arch("sh.hnplugin"), e));
        expectReject(arch("sh.hnplugin"), "native");
    }
    void luaMustBeUtf8TextNotBytecode() {
        auto e = goodEntries("latin", QByteArray("-- caf\xe9\n"));
        QVERIFY(makeArchive(arch("latin.hnplugin"), e));
        expectReject(arch("latin.hnplugin"), "UTF-8");
        e = goodEntries("bc", QByteArray("\x1b""Lua\x55\x00\x19\x93", 8));
        QVERIFY(makeArchive(arch("bc.hnplugin"), e));
        expectReject(arch("bc.hnplugin"), "bytecode");
        e = goodEntries("nul", QByteArray("-- a\0b\n", 7));
        QVERIFY(makeArchive(arch("nul.hnplugin"), e));
        expectReject(arch("nul.hnplugin"), "UTF-8");
    }
    void missingEntryAndSyntaxError() {
        QList<AEntry> e{{"plugin.json", manifestJson("noentry", {"ui"})}};
        QVERIFY(makeArchive(arch("noentry.hnplugin"), e));
        expectReject(arch("noentry.hnplugin"), "entry file");
        auto e2 = goodEntries("syn", "function (\n");
        QVERIFY(makeArchive(arch("syn.hnplugin"), e2));
        expectReject(arch("syn.hnplugin"), "syntax");
    }
    void corruptArchive() {
        writeFile(arch("junk.hnplugin"), "this is not an archive at all");
        expectReject(arch("junk.hnplugin"), "archive");
        QVERIFY(!inst->install(arch("missing.hnplugin")).ok);
    }
    void duplicateEntriesRejected() {
        auto e = goodEntries("dup");
        e << AEntry{"main.lua", "-- second\n"};
        QVERIFY(makeArchive(arch("dup.hnplugin"), e));
        expectReject(arch("dup.hnplugin"), "twice");
    }
    void alreadyInstalledNeedsUpgradeFlag() {
        const auto dir = writePlugin(env.src(), "twice-id", {"ui"}, "-- v1\n");
        QVERIFY(inst->install(dir).ok);
        const auto r = inst->install(dir);
        QVERIFY(!r.ok);
        QVERIFY(allErrors(r).contains("already installed"));
    }
    void upgradeKeepsConsentOnlyIfIdentical() {
        const auto dir = writePlugin(env.src(), "upg", {"ui"}, "-- v1\n");
        auto r = inst->install(dir);
        QVERIFY(r.ok);
        QVERIFY(trust->recordConsent("upg", {"ui"}, r.hash));
        QVERIFY(trust->setEnabled("upg", true));
        // identical reinstall: consent and enabled state kept
        r = inst->install(dir, true);
        QVERIFY2(r.ok, qPrintable(allErrors(r)));
        QVERIFY(r.upgraded);
        QVERIFY(r.consentKept);
        QVERIFY(trust->record("upg").enabled);
        QVERIFY(!trust->record("upg").needsReconsent);
        // changed content: disabled + needs re-consent
        writeFile(dir + "/main.lua", "-- v2\n");
        r = inst->install(dir, true);
        QVERIFY(r.ok && r.upgraded);
        QVERIFY(!r.consentKept);
        QVERIFY(!trust->record("upg").enabled);
        QVERIFY(trust->record("upg").needsReconsent);
        QCOMPARE(readFile(env.plugins() + "/upg/main.lua"), QByteArray("-- v2\n"));
        QVERIFY(readFile(audit->path()).contains("\"upgrade\""));
    }
    void failedUpgradeLeavesOldVersion() {
        const auto dir = writePlugin(env.src(), "keep", {"ui"}, "-- good\n");
        QVERIFY(inst->install(dir).ok);
        writeFile(dir + "/main.lua", "syntax error here (\n");
        const auto r = inst->install(dir, true);
        QVERIFY(!r.ok);
        QCOMPARE(readFile(env.plugins() + "/keep/main.lua"), QByteArray("-- good\n"));
    }
    void neverOverwritesDifferentIdFolder() {
        // An installed folder named "victim" that actually holds another plugin must not be replaced by a package claiming id "victim".
        const auto other = writePlugin(env.src(), "other-id", {"ui"}, "-- other\n");
        QVERIFY(inst->install(other).ok);
        QVERIFY(QDir().rename(env.plugins() + "/other-id", env.plugins() + "/victim"));
        const auto evil = writePlugin(env.src(), "victim", {"ui"}, "-- evil\n");
        const auto r = inst->install(evil, true);
        QVERIFY(!r.ok);
        QVERIFY2(allErrors(r).contains("refusing"), qPrintable(allErrors(r)));
        QCOMPARE(readFile(env.plugins() + "/victim/main.lua"), QByteArray("-- other\n"));
    }
    void refusesToOverwriteSymlinkedTarget() {
        QDir().mkpath(env.plugins());
        QDir().mkpath(env.tmp.path() + "/elsewhere");
        writeFile(env.tmp.path() + "/elsewhere/plugin.json", manifestJson("sym-target", {"ui"}));
        QVERIFY(QFile::link(env.tmp.path() + "/elsewhere", env.plugins() + "/sym-target"));
        const auto r = inst->install(writePlugin(env.src(), "sym-target", {"ui"}, "-- x\n"), true);
        QVERIFY(!r.ok);
        QVERIFY(QFileInfo::exists(env.tmp.path() + "/elsewhere/plugin.json"));
    }
    void uninstall() {
        const auto dir = writePlugin(env.src(), "bye", {"ui"}, "-- x\n");
        const auto r = inst->install(dir);
        QVERIFY(trust->recordConsent("bye", {"ui"}, r.hash));
        QString err;
        QVERIFY(inst->uninstall("bye", &err));
        QVERIFY(!QFileInfo::exists(env.plugins() + "/bye"));
        QVERIFY(!trust->known("bye"));
        QVERIFY(!inst->uninstall("bye", &err));
        QVERIFY(!inst->uninstall("../../etc", &err));
        QVERIFY(readFile(audit->path()).contains("\"remove\""));
    }
    void hiddenEntriesIgnored() {
        const auto dir = writePlugin(env.src(), "hid", {"ui"}, "-- x\n");
        writeFile(dir + "/.git/config", "x");
        writeFile(dir + "/.DS_Store", "x");
        const auto r = inst->install(dir);
        QVERIFY2(r.ok, qPrintable(allErrors(r)));
        QVERIFY(!QFileInfo::exists(env.plugins() + "/hid/.git"));
    }
    void templateIsValidAndPacks() {
        QString err;
        QVERIFY2(createTemplate(env.src() + "/My Cool Plugin", "My Cool Plugin!", &err), qPrintable(err));
        const auto c = checkDirectory(env.src() + "/My Cool Plugin");
        QVERIFY2(c.ok, qPrintable(c.errors.isEmpty() ? "" : c.errors.first().message));
        QCOMPARE(c.manifest.id, QString("my-cool-plugin"));
        QVERIFY(readFile(env.src() + "/My Cool Plugin/main.lua").contains("hn.command"));
        QVERIFY(!createTemplate(env.src() + "/My Cool Plugin", "again", &err));  // refuses non-empty dir
        QVERIFY(!createTemplate(env.src() + "/x", "!!!", &err));
        QList<PluginError> errs;
        QVERIFY(packDirectory(env.src() + "/My Cool Plugin", arch("tpl.hnplugin"), &errs));
        QVERIFY(inst->install(arch("tpl.hnplugin")).ok);
    }
    void checkDirectoryReportsEverything() {
        const auto dir = env.src() + "/chk";
        writeFile(dir + "/plugin.json", manifestJson("Bad Id", {"network"}));
        writeFile(dir + "/main.lua", "x = = 1\n");
        writeFile(dir + "/lib.so", "x");
        const auto c = checkDirectory(dir);
        QVERIFY(!c.ok);
        QStringList m;
        for (const auto &e : c.errors) m << e.message;
        const auto all = m.join("\n");
        QVERIFY2(all.contains("id"), qPrintable(all));
        QVERIFY2(all.contains("net_hosts"), qPrintable(all));
        QVERIFY2(all.contains("native"), qPrintable(all));
        QVERIFY(!checkDirectory(env.src() + "/nope").ok);
        const auto ok = checkDirectory(fixture("hello"));
        QVERIFY2(ok.ok, qPrintable(ok.errors.isEmpty() ? "" : ok.errors.first().message));
        QVERIFY(ok.warnings.isEmpty());
        const auto probe = checkDirectory(fixture("api-probe"));
        QVERIFY(probe.ok);
        QVERIFY(probe.warnings.size() >= 4);  // network, notes.write, clipboard, theme
    }
    void installFixtures() {
        for (const char *f : {"hello", "api-probe", "escape-probe", "http-probe"}) {
            const auto r = inst->install(fixture(f));
            QVERIFY2(r.ok, qPrintable(QString(f) + ": " + allErrors(r)));
        }
        QVERIFY(QFileInfo(env.plugins() + "/hello/lib/util.lua").isFile());
    }
};
QTEST_APPLESS_MAIN(PackageTest)
#include "plugins_package_test.moc"
