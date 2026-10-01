#include "hn/mods/mods.h"
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLibrary>
#include <QTemporaryDir>
#include <QtTest>

using namespace hn::mods;

struct FakeDoc : DocumentBridge {
    QString text = QStringLiteral("hello world"), sel = QStringLiteral("hello");
    QStringList log;  // recorded operations, used to check undo grouping
    QStringList undoSteps;
    bool open = false;
    QString selectionText() override { return sel; }
    void beginTransaction(const QString &n) override { log << "begin:" + n; open = true; }
    void replaceSelection(const QString &s) override { QVERIFY2(open, "mutation outside transaction"); log << "replace:" + s; text.replace(sel, s); sel = s; }
    void insertText(const QString &s) override { QVERIFY2(open, "mutation outside transaction"); log << "insert:" + s; }
    void endTransaction() override { log << "end"; open = false; undoSteps << "step"; }
};

static bool mapped(const QString &needle) {
    QFile f(QStringLiteral("/proc/self/maps"));
    (void)f.open(QIODevice::ReadOnly);
    return f.readAll().contains(needle.toUtf8());
}
static qint64 pssKb() {
    QFile f(QStringLiteral("/proc/self/smaps_rollup"));
    (void)f.open(QIODevice::ReadOnly);
    for (const auto &l : f.readAll().split('\n'))
        if (l.startsWith("Pss:")) return l.simplified().split(' ')[1].toLongLong();
    return -1;
}

class ModsTest : public QObject {
    Q_OBJECT
    QTemporaryDir tmp;
    QString modsDir() { return tmp.path() + "/mods"; }
    QString enabledPath() { return tmp.path() + "/config/mods-enabled.json"; }

    void install(const QString &id, const QString &libSrc, const QString &activation = "on-command:x", const QString &extra = {}) {
        const QString d = modsDir() + "/" + id;
        QDir().mkpath(d);
        const QString libName = QFileInfo(libSrc).fileName();
        QFile::remove(d + "/" + libName);
        QVERIFY(QFile::copy(libSrc, d + "/" + libName));
        QFile m(d + "/mod.json");
        (void)m.open(QIODevice::WriteOnly);
        m.write(QString(R"({"id":"%1","name":"%1","version":"1.0","host_api_version":1,"arch":"%2","library":"%3","activation":["%4"]%5})")
                    .arg(id, QSysInfo::buildCpuArchitecture(), libName, activation, extra).toUtf8());
    }
    void installExample() {
        const QString d = modsDir() + "/uppercase-selection";
        QDir().mkpath(d);
        QFile::remove(d + "/libuppercase_selection.so");
        QVERIFY(QFile::copy(QString(HN_EXAMPLE_BIN) + "/libuppercase_selection.so", d + "/libuppercase_selection.so"));
        QFile::remove(d + "/mod.json");
        QVERIFY(QFile::copy(QString(HN_EXAMPLE_DIR) + "/mod.json", d + "/mod.json"));
    }
    QString fix(const char *n) { return QString(HN_FIX_DIR) + "/lib" + n + ".so"; }

private slots:
    void truncated_library_is_rejected_not_loaded() {   // D12: used to SIGBUS inside dlopen
        const QString src = fix("mods_fixture_good");
        QFile f(src);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QByteArray full = f.readAll();
        for (int cut : {0, 10, 64, 400, int(full.size() / 2), int(full.size() - 1)}) {
            QDir(modsDir()).removeRecursively();
            install("trunc", src, "on-command:trunc-cmd", R"(,"commands":[{"id":"trunc-cmd"}])");
            QFile o(modsDir() + "/trunc/" + QFileInfo(src).fileName());
            QVERIFY(o.open(QIODevice::WriteOnly | QIODevice::Truncate));
            o.write(full.left(cut));
            o.close();
            saveEnabled(enabledPath(), {"trunc"});
            ModHost h; h.start(modsDir(), enabledPath());
            FakeDoc d;
            QVERIFY(!h.runCommand("trunc-cmd", &d));
            QVERIFY(!h.isLoaded("trunc"));
            QVERIFY(!h.errors().isEmpty());
        }
    }
    void init() { QDir(modsDir()).removeRecursively(); QDir(tmp.path() + "/config").removeRecursively(); }

    void manifestValidation() {
        Manifest m; QList<ModError> e;
        const QString arch = QSysInfo::buildCpuArchitecture();
        auto good = QString(R"({"id":"a","name":"A","version":"1","host_api_version":1,"arch":"%1","library":"liba.so","activation":["on-startup"]})").arg(arch).toUtf8();
        QVERIFY(parseManifest(good, "/x/a", &m, &e));
        QCOMPARE(m.entry, QString("hn_mod_entry"));
        const QList<QByteArray> bad = {
            "not json", "[]", "{}",
            QString(R"({"id":"a","name":"A","version":"1","host_api_version":2,"arch":"%1","library":"l.so","activation":["on-startup"]})").arg(arch).toUtf8(),
            R"({"id":"a","name":"A","version":"1","host_api_version":1,"arch":"sparc","library":"l.so","activation":["on-startup"]})",
            QString(R"({"id":"a","name":"A","version":"1","host_api_version":1,"arch":"%1","library":"../l.so","activation":["on-startup"]})").arg(arch).toUtf8(),
            QString(R"({"id":"a","name":"A","version":"1","host_api_version":1,"arch":"%1","library":"/etc/l.so","activation":["on-startup"]})").arg(arch).toUtf8(),
            QString(R"({"id":"a","name":"A","version":"1","host_api_version":1,"arch":"%1","library":"l.so","activation":["on-whenever"]})").arg(arch).toUtf8(),
            QString(R"({"id":"a","name":"A","version":"1","host_api_version":1,"arch":"%1","library":"l.so","activation":[]})").arg(arch).toUtf8(),
            QString(R"({"id":"B!","name":"A","version":"1","host_api_version":1,"arch":"%1","library":"l.so","activation":["on-startup"]})").arg(arch).toUtf8(),
            QString(R"({"id":"other","name":"A","version":"1","host_api_version":1,"arch":"%1","library":"l.so","activation":["on-startup"]})").arg(arch).toUtf8(),
        };
        for (const auto &b : bad) { e.clear(); QVERIFY2(!parseManifest(b, "/x/a", &m, &e), b.constData()); QCOMPARE(e.size(), 1); }
    }

    void malformedManifestIsReportedNotFatal() {
        QDir().mkpath(modsDir() + "/broken");
        QFile f(modsDir() + "/broken/mod.json"); (void)f.open(QIODevice::WriteOnly); f.write("{oops"); f.close();
        installExample();
        saveEnabled(enabledPath(), {"uppercase-selection", "broken", "ghost"});
        ModHost h; h.start(modsDir(), enabledPath());
        QCOMPARE(h.installed().size(), 1);
        QCOMPARE(h.errors().size(), 3);  // broken manifest + enabled-but-not-valid "broken" and "ghost"
    }

    void enabledListRoundTripAndMalformed() {
        QVERIFY(saveEnabled(enabledPath(), {"a", "b"}));
        QCOMPARE(loadEnabled(enabledPath()), QStringList({"a", "b"}));
        QFile f(enabledPath()); (void)f.open(QIODevice::WriteOnly | QIODevice::Truncate); f.write("garbage"); f.close();
        QList<ModError> e;
        QVERIFY(loadEnabled(enabledPath(), &e).isEmpty());
        QCOMPARE(e.size(), 1);
    }

    void lazyActivationAndUndoGrouping() {
        installExample();
        saveEnabled(enabledPath(), {"uppercase-selection"});
        ModHost h; h.start(modsDir(), enabledPath());
        QVERIFY(h.errors().isEmpty());
        QCOMPARE(h.commands().size(), 1);                       // declared without loading
        QVERIFY(!h.isLoaded("uppercase-selection"));
        QVERIFY(!mapped("libuppercase_selection.so"));
        FakeDoc d;
        QVERIFY(h.runCommand("uppercase-selection", &d));
        QVERIFY(h.isLoaded("uppercase-selection"));
        QVERIFY(mapped("libuppercase_selection.so"));
        QCOMPARE(d.log, QStringList({"begin:Uppercase selection", "replace:HELLO", "end"}));
        QCOMPARE(d.text, QString("HELLO world"));
        QCOMPARE(d.undoSteps.size(), 1);                        // a single undo step
        QVERIFY(!h.runCommand("nope", &d));
    }

    void repeatedActivationAndTenInstances() {
        installExample();
        saveEnabled(enabledPath(), {"uppercase-selection"});
        for (int round = 0; round < 5; ++round) {               // repeated start/activate/shutdown cycles
            ModHost h; h.start(modsDir(), enabledPath());
            FakeDoc d;
            QVERIFY(h.runCommand("uppercase-selection", &d));
            QVERIFY(h.runCommand("uppercase-selection", &d));   // second run: no re-activation
            QCOMPARE(d.undoSteps.size(), 2);
            QCOMPARE(h.stats().slowCallbacks, 0);
        }
        QVERIFY(!mapped("libuppercase_selection.so"));          // nothing left after hosts destroyed
        ModHost h; h.start(modsDir(), enabledPath());
        FakeDoc docs[10];
        for (auto &d : docs) d.sel = "win";
        for (auto &d : docs) QVERIFY(h.runCommand("uppercase-selection", &d));
        for (auto &d : docs) { QCOMPARE(d.sel, QString("WIN")); QCOMPARE(d.undoSteps.size(), 1); }
    }

    void disabledModNeverLoaded() {
        installExample();
        saveEnabled(enabledPath(), {});
        ModHost h; h.start(modsDir(), enabledPath());
        FakeDoc d;
        QVERIFY(h.commands().isEmpty());
        QVERIFY(!h.runCommand("uppercase-selection", &d));
        QVERIFY(!h.isLoaded("uppercase-selection"));
        QVERIFY(!mapped("libuppercase_selection.so"));
        QVERIFY(d.log.isEmpty());
    }

    void rejectsIncompatibleModules() {
        install("badver", fix("mods_fixture_bad_version"), "on-command:badver-cmd");
        install("small", fix("mods_fixture_small"), "on-command:small-cmd");
        install("failing", fix("mods_fixture_fail"), "on-command:ghost");
        saveEnabled(enabledPath(), {"badver", "small", "failing"});
        ModHost h; h.setLogger([](int, const QString &, const QString &) {});
        h.start(modsDir(), enabledPath());
        FakeDoc d;
        QVERIFY(!h.runCommand("badver-cmd", &d));
        QVERIFY(!h.runCommand("small-cmd", &d));
        QVERIFY(!h.runCommand("ghost", &d));
        QCOMPARE(h.errors().size(), 3);
        for (const auto &e : h.errors()) qInfo() << "error:" << e.modId << e.message;
        QVERIFY(!h.isLoaded("badver") && !h.isLoaded("small") && !h.isLoaded("failing"));
        QVERIFY(!mapped("libmods_fixture_"));                   // rejected libraries were unloaded
        QVERIFY(h.commands().size() == 3);                      // declared stubs only, none callable
        QVERIFY(!h.runCommand("ghost", &d));                    // no retry storm
        QCOMPARE(h.errors().size(), 3);
    }

    void manifestVersionMismatchNeverLoadsLibrary() {
        install("old", fix("mods_fixture_good"));
        QFile m(modsDir() + "/old/mod.json"); (void)m.open(QIODevice::ReadWrite);
        auto s = m.readAll(); s.replace("\"host_api_version\":1", "\"host_api_version\":7"); m.seek(0); m.resize(0); m.write(s); m.close();
        saveEnabled(enabledPath(), {"old"});
        ModHost h; h.start(modsDir(), enabledPath());
        QVERIFY(h.installed().isEmpty());
        QVERIFY(!mapped("libmods_fixture_good"));
    }

    void slowCallbackAndOpenTransaction() {
        install("fx", fix("mods_fixture_good"), "on-startup", R"(,"commands":[{"id":"slow"},{"id":"leak"}])");
        saveEnabled(enabledPath(), {"fx"});
        QStringList logs;
        ModHost h; h.setLogger([&](int, const QString &, const QString &m) { logs << m; });
        h.start(modsDir(), enabledPath());
        QVERIFY(h.isLoaded("fx"));                              // on-startup
        FakeDoc d;
        QVERIFY(h.runCommand("slow", &d));
        QCOMPARE(h.stats().slowCallbacks, 1);
        QVERIFY(logs.last().contains("took"));
        QVERIFY(h.runCommand("leak", &d));
        QVERIFY(!d.open);                                       // host closed the dangling transaction
        QCOMPARE(d.undoSteps.size(), 1);
    }

    void eventsCoalesceAndSchedule() {
        install("fx", fix("mods_fixture_good"), "on-note-open", R"(,"commands":[{"id":"sched"}])");
        saveEnabled(enabledPath(), {"fx"});
        ModHost h; h.start(modsDir(), enabledPath());
        QVERIFY(!h.isLoaded("fx"));
        h.post(EventType::SelectionChanged, "n1");              // nobody loaded: dropped, no timer
        QVERIFY(!h.isLoaded("fx"));
        h.post(EventType::NoteOpened, "n1");                    // activates on-note-open mod
        QVERIFY(h.isLoaded("fx"));
        for (int i = 0; i < 100; ++i) h.post(EventType::SelectionChanged, "n1");
        h.post(EventType::SelectionChanged, "n2");
        h.post(EventType::NoteSaved, "n1"); h.post(EventType::NoteSaved, "n1");
        h.flushEvents();
        QLibrary lib(modsDir() + "/fx/libmods_fixture_good.so");
        auto events = reinterpret_cast<int (*)()>(lib.resolve("hn_fixture_events"));
        auto work = reinterpret_cast<int (*)()>(lib.resolve("hn_fixture_work"));
        QVERIFY(events);
        QCOMPARE(events(), 1 + 2 + 1);                          // opened + sel(n1,n2) + saved(n1)
        FakeDoc d;
        QVERIFY(h.runCommand("sched", &d));
        QCOMPARE(work(), 0);
        QTRY_COMPARE(work(), 1);                                // host-scheduled
        h.shutdown();
        QVERIFY(h.commands().size() == 1);
    }

    void overheadMeasurement() {
        installExample();
        saveEnabled(enabledPath(), {"uppercase-selection"});
        ModHost h; h.start(modsDir(), enabledPath());
        const qint64 before = pssKb();
        FakeDoc d;
        QVERIFY(h.runCommand("uppercase-selection", &d));
        const qint64 after = pssKb();
        qInfo().noquote() << QString("PSS before activation: %1 kB, after: %2 kB, delta: %3 kB (process incl. test binary)").arg(before).arg(after).arg(after - before);
        QVERIFY(after - before < 2048);                         // generous: example must stay well below 2 MiB
    }
};

QTEST_GUILESS_MAIN(ModsTest)
#include "mods_test.moc"
