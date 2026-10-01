#include "hn/core/paths.h"
#include <QtTest>
using namespace hn::core;

class PathsTest : public QObject {
    Q_OBJECT
    void clear() {
        for (auto v : {"HN_CONFIG_DIR", "HN_STATE_DIR", "HN_CACHE_DIR", "HN_DATA_DIR", "HN_NOTES_DIR", "XDG_CONFIG_HOME",
                       "XDG_STATE_HOME", "XDG_CACHE_HOME", "XDG_DATA_HOME"})
            qunsetenv(v);
    }
private slots:
    void defaults() {
        clear();
        QString h = QDir::homePath();
        QCOMPARE(paths::configDir(), h + "/.config/hyprnotes");
        QCOMPARE(paths::stateDir(), h + "/.local/state/hyprnotes");
        QCOMPARE(paths::cacheDir(), h + "/.cache/hyprnotes");
        QCOMPARE(paths::dataDir(), h + "/.local/share/hyprnotes");
        QCOMPARE(paths::defaultNotesDir(), h + "/Notes/Hyprnotes");
    }
    void xdg() {
        clear();
        qputenv("XDG_CONFIG_HOME", "/x/cfg");
        qputenv("XDG_STATE_HOME", "/x/state/");
        qputenv("XDG_CACHE_HOME", "relative/ignored");
        QCOMPARE(paths::configDir(), QString("/x/cfg/hyprnotes"));
        QCOMPARE(paths::stateDir(), QString("/x/state/hyprnotes"));
        QCOMPARE(paths::cacheDir(), QDir::homePath() + "/.cache/hyprnotes");
        clear();
    }
    void overrides() {
        clear();
        qputenv("XDG_CONFIG_HOME", "/x/cfg");
        qputenv("HN_CONFIG_DIR", "/t/c");
        qputenv("HN_STATE_DIR", "/t/s");
        qputenv("HN_CACHE_DIR", "/t/ca");
        qputenv("HN_DATA_DIR", "/t/d");
        qputenv("HN_NOTES_DIR", "/t/n");
        QCOMPARE(paths::configDir(), QString("/t/c"));
        QCOMPARE(paths::stateDir(), QString("/t/s"));
        QCOMPARE(paths::cacheDir(), QString("/t/ca"));
        QCOMPARE(paths::dataDir(), QString("/t/d"));
        QCOMPARE(paths::defaultNotesDir(), QString("/t/n"));
        clear();
    }
    void nothingCreated() {
        clear();
        QTemporaryDir t;
        qputenv("HN_STATE_DIR", (t.path() + "/s").toUtf8());
        paths::stateDir();
        QVERIFY(!QFileInfo::exists(t.path() + "/s"));
        clear();
    }
};
QTEST_APPLESS_MAIN(PathsTest)
#include "core_paths_test.moc"
