#include "cli.h"
#include "policy.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>
#include <unistd.h>

using namespace hn::app;
using hn::platform::Action;

class CliTest : public QObject {
    Q_OBJECT
    static QProcessEnvironment env(const QString &root) {
        QProcessEnvironment e;
        e.insert("PATH", qEnvironmentVariable("PATH"));
        e.insert("HOME", root + "/home");
        e.insert("QT_QPA_PLATFORM", "offscreen");
        e.insert("XDG_RUNTIME_DIR", root + "/run");
        e.insert("WAYLAND_DISPLAY", "wayland-hn-test");
        e.insert("HN_NOTES_DIR", root + "/notes");
        e.insert("HN_CONFIG_DIR", root + "/cfg");
        e.insert("HN_STATE_DIR", root + "/state");
        e.insert("HN_CACHE_DIR", root + "/cache");
        e.insert("HN_DATA_DIR", root + "/data");
        e.insert("HN_SHARE_DIR", root + "/share");
        return e;
    }
    static int run(const QString &root, const QStringList &args, QString *out = nullptr, QString *err = nullptr, int ms = 15000) {
        QProcess p;
        p.setProcessEnvironment(env(root));
        p.start(HN_BIN, args);
        if (!p.waitForFinished(ms)) { p.kill(); return -99; }
        if (out) *out = QString::fromLocal8Bit(p.readAllStandardOutput());
        if (err) *err = QString::fromLocal8Bit(p.readAllStandardError());
        return p.exitCode();
    }
private slots:
    void parse_options() {
        QCOMPARE(parseCli({}).action, Action::ShowOrganizer);
        QCOMPARE(parseCli({"--new-note"}).action, Action::NewNote);
        QCOMPARE(parseCli({"--background"}).action, Action::Background);
        QCOMPARE(parseCli({"--show-organizer"}).action, Action::ShowOrganizer);
        QVERIFY(parseCli({"--version"}).version);
        QVERIFY(parseCli({"--help"}).help);
        QVERIFY(!parseCli({"--bogus"}).error.isEmpty());
        QVERIFY(!parseCli({"file.md"}).error.isEmpty());
        QVERIFY(parseCli({"--new-note", "--background"}).error.contains("conflicting"));
        QVERIFY(helpText().contains("--background"));
    }

    void version_help_and_errors_exit_without_a_session() {
        QTemporaryDir d;
        QString out, err;
        QCOMPARE(run(d.path(), {"--version"}, &out), 0);
        QVERIFY(out.startsWith("hyprnotes 0.1.0"));
        QCOMPARE(run(d.path(), {"--help"}, &out), 0);
        QVERIFY(out.contains("--show-organizer"));
        QCOMPARE(run(d.path(), {"--nope"}, &out, &err), 2);
        QVERIFY(err.contains("unknown option"));
    }

    void print_hyprland_rules_prints_installed_path_and_contents() {
        QVERIFY(parseCli({"--print-hyprland-rules"}).printRules);
        QTemporaryDir d;
        const QString dir = d.path() + "/share/hyprnotes/hyprland";
        QDir().mkpath(dir);
        for (const char *f : {"hyprnotes.lua", "hyprnotes.conf"}) {
            QFile in(QString(HN_SOURCE_DIR) + "/packaging/share/hyprland/" + f), out(dir + "/" + f);
            QVERIFY(in.open(QIODevice::ReadOnly) && out.open(QIODevice::WriteOnly));
            out.write(in.readAll());
        }
        QString o;
        QCOMPARE(run(d.path(), {"--print-hyprland-rules"}, &o), 0);
        QVERIFY2(o.contains(dir + "/hyprnotes.lua") && o.contains(dir + "/hyprnotes.conf"), qPrintable(o));
        QVERIFY(o.contains("initial_title") && o.contains("^hyprnotes-sticky:"));
    }

    void two_process_forwarding_has_one_persistent_instance() {
        QTemporaryDir d;
        QDir().mkpath(d.path() + "/run");
        QFile::setPermissions(d.path() + "/run", QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        QDir().mkpath(d.path() + "/notes");
        QProcess server;
        server.setProcessEnvironment(env(d.path()));
        server.start(HN_BIN, {"--background"});
        QVERIFY(server.waitForStarted());
        const QString sock = d.path() + "/run/hyprnotes-wayland-hn-test-" + QString::number(getuid()) + ".sock";
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(sock), 10000);
        QTest::qWait(300);

        QString err;
        QCOMPARE(run(d.path(), {"--new-note"}, nullptr, &err), 0);          // forwarded + acknowledged, client exits
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(d.path() + "/notes/Untitled.md"), 5000);
        QCOMPARE(run(d.path(), {"--new-note"}, nullptr, &err), 0);
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(d.path() + "/notes/Untitled 2.md"), 5000);
        QCOMPARE(run(d.path(), {"--show-organizer"}, nullptr, &err), 0);
        QCOMPARE(run(d.path(), {"--background"}, nullptr, &err), 0);        // no-op on a running instance
        QCOMPARE(server.state(), QProcess::Running);                          // still the one persistent process
        QCOMPARE(QDir(d.path() + "/notes").entryList({"*.md"}).size(), 2);    // exactly one note per request, no replay

        server.terminate();
        QVERIFY(server.waitForFinished(8000));
    }

    void unreachable_session_is_an_actionable_error() {
        QTemporaryDir d;
        QProcess p;
        auto e = env(d.path());
        e.remove("WAYLAND_DISPLAY");
        p.setProcessEnvironment(e);
        p.start(HN_BIN, {"--show-organizer"});
        QVERIFY(p.waitForFinished(10000));
        QCOMPARE(p.exitCode(), 1);
        QVERIFY(QString::fromLocal8Bit(p.readAllStandardError()).contains("Wayland"));
    }
};

QTEST_MAIN(CliTest)
#include "app_cli_test.moc"
