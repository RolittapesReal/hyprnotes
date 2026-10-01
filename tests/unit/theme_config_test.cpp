#include <QtTest>
#include <QCoreApplication>
#include <QDir>
#include <QTemporaryDir>
#include <hn/theme/animation.h>
#include "config.h"
using namespace hn::theme;

class ConfigTest : public QObject {
    Q_OBJECT
    QTemporaryDir dir;
    QString path() { return dir.path() + "/hyprnotes/config.json"; }
    void write(const QByteArray &b) {
        QDir().mkpath(dir.path() + "/hyprnotes");
        QSaveFile f(path()); QVERIFY(f.open(QIODevice::WriteOnly)); f.write(b); QVERIFY(f.commit());
    }
private slots:
    void init() { QDir(dir.path()).removeRecursively(); QDir().mkpath(dir.path()); }
    void defaultsAndNoWriteAtLoad() {
        Config c(path());
        QVERIFY(c.lastError().isEmpty());
        QVERIFY(!QFile::exists(path()));
        const Settings &s = c.settings();
        QCOMPARE(s.theme, QString("modernist")); QCOMPARE(s.colorScheme, QString("system"));
        QCOMPARE(s.stickySize, QSize(360, 300)); QCOMPARE(s.stickyMin, QSize(260, 180)); QCOMPARE(s.organizerSize, QSize(900, 640));
        QVERIFY(!s.reduceMotion); QVERIFY(s.trayEnabled); QCOMPARE(s.fontSize, 0);
        QCOMPARE(s.keybindings["bold"], QKeySequence("Ctrl+B"));
        QVERIFY(s.toolbar.startsWith("bold"));
    }
    void partialFile() {
        write(R"({"version":1,"theme":"mine","colorScheme":"dark","windows":{"stickySize":[400,320]},"keybindings":{"bold":"Ctrl+Alt+B"},"toolbar":["link","bold"]})");
        Config c(path());
        QVERIFY2(c.lastError().isEmpty(), qPrintable(c.lastError()));
        const Settings &s = c.settings();
        QCOMPARE(s.theme, QString("mine")); QCOMPARE(s.colorScheme, QString("dark"));
        QCOMPARE(s.stickySize, QSize(400, 320)); QCOMPARE(s.organizerSize, QSize(900, 640));
        QCOMPARE(s.keybindings["bold"], QKeySequence("Ctrl+Alt+B")); QCOMPARE(s.keybindings["italic"], QKeySequence("Ctrl+I"));
        QCOMPARE(s.toolbar, (QStringList{"link", "bold"}));
    }
    void malformedKeepsLastValid() {
        write(R"({"version":1,"theme":"good"})");
        Config c(path());
        QCOMPARE(c.settings().theme, QString("good"));
        write("{ broken");
        QVERIFY(!c.reload()); QVERIFY(!c.lastError().isEmpty());
        QCOMPARE(c.settings().theme, QString("good"));
        write(R"({"version":9,"theme":"x"})");
        QVERIFY(!c.reload()); QCOMPARE(c.settings().theme, QString("good"));
        write(R"({"version":1,"theme":"a","colorScheme":"purple","windows":{"stickySize":[1,1]},"toolbar":["a","a"],"trayEnabled":"yes","keybindings":{"bold":"Ctrl+Bogus+"}})");
        QVERIFY(!c.reload());
        QCOMPARE(c.settings().theme, QString("a"));                    // valid field applied
        QCOMPARE(c.settings().colorScheme, QString("system"));         // invalid fields: prior value kept
        QCOMPARE(c.settings().stickySize, QSize(360, 300));
        QVERIFY(c.lastError().contains("toolbar") && c.lastError().contains("colorScheme"));
    }
    void fontSizeRoundTripAndValidation() {
        write(R"({"version":1,"fontSize":18})");
        Config c(path());
        QCOMPARE(c.settings().fontSize, 18);
        write(R"({"version":1,"fontSize":99})");
        QVERIFY(!c.reload()); QVERIFY(c.lastError().contains("fontSize"));
        QCOMPARE(c.settings().fontSize, 18);                            // invalid: previous value kept
        Settings s = Config::defaults(); s.fontSize = 21; s.trayEnabled = false;
        QVERIFY(c.save(s));
        Config d(path());
        QCOMPARE(d.settings().fontSize, 21); QVERIFY(!d.settings().trayEnabled);
    }
    void saveAtomicRoundTrip() {
        Config c(path());
        Settings s = Config::defaults(); s.theme = "z"; s.reduceMotion = true; s.toolbar = {"code"};
        QVERIFY(c.save(s));
        QVERIFY(QFile::exists(path()));
        Config d(path());
        QVERIFY2(d.lastError().isEmpty(), qPrintable(d.lastError()));
        QVERIFY(d.settings() == s);
        QCOMPARE(QDir(dir.path() + "/hyprnotes").entryList(QDir::Files).size(), 1);   // no temp leftovers
    }
    void reloadEventCoalesced() {
        write(R"({"version":1,"theme":"one"})");
        Config c(path()); c.watch();
        QSignalSpy spy(&c, &Config::changed);
        for (int i = 0; i < 3; ++i) write(QByteArray(R"({"version":1,"theme":"two"})"));
        QVERIFY(spy.wait(3000));
        QTest::qWait(300);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(c.settings().theme, QString("two"));
        write(R"({"version":1,"theme":"three"})");                     // watcher re-armed after atomic replace
        QVERIFY(spy.wait(3000));
        QCOMPARE(c.settings().theme, QString("three"));
        write("garbage");                                               // malformed: event, last valid kept
        QVERIFY(spy.wait(3000));
        QVERIFY(!c.lastError().isEmpty()); QCOMPARE(c.settings().theme, QString("three"));
    }
    void reloadWhenDirCreatedLater() {
        Config c(path()); c.watch();
        QSignalSpy spy(&c, &Config::changed);
        write(R"({"version":1,"theme":"late"})");
        QVERIFY(spy.wait(3000));
        QCOMPARE(c.settings().theme, QString("late"));
    }
};
QTEST_MAIN(ConfigTest)
#include "theme_config_test.moc"
