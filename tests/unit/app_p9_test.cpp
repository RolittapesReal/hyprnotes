// P9: signal-driven quit, config-owned tray/font defaults + app.json migration, snippet cache bound,
// Hyprland placement / session save against the fake compositor, --background laziness, no keep-above UI.
#include "app_test_util.h"
#include "platform_fake_hyprland.h"
#include "signal_bridge.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QMenu>
#include <QPointer>
#include "ui_common.h"
#include <QProcess>
#include <csignal>
#include <unistd.h>

using namespace apptest;
using hn::platform::Action;

class P9Test : public QObject {
    Q_OBJECT
    static QProcessEnvironment env(const QString &root) {
        QProcessEnvironment e;
        e.insert("PATH", qEnvironmentVariable("PATH"));
        e.insert("HOME", root + "/home");
        e.insert("QT_QPA_PLATFORM", "offscreen");
        e.insert("XDG_RUNTIME_DIR", root + "/run");
        e.insert("WAYLAND_DISPLAY", "wayland-hn-p9");
        e.insert("HN_NOTES_DIR", root + "/notes");
        e.insert("HN_CONFIG_DIR", root + "/cfg");
        e.insert("HN_STATE_DIR", root + "/state");
        e.insert("HN_CACHE_DIR", root + "/cache");
        e.insert("HN_DATA_DIR", root + "/data");
        return e;
    }
    static QByteArray readFile(const QString &p) { QFile f(p); return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray(); }

private slots:
    // ---- (a) signals ----
    void signal_bridge_delivers_each_signal() {
        hn::app::SignalBridge b;
        QVERIFY(b.ok());
        QSignalSpy spy(&b, &hn::app::SignalBridge::quitSignal);
        for (int s : {SIGTERM, SIGINT, SIGHUP}) ::raise(s);
        QTRY_COMPARE(spy.size(), 3);
        QCOMPARE(spy[0][0].toInt(), int(SIGTERM));
        QCOMPARE(spy[1][0].toInt(), int(SIGINT));
        QCOMPARE(spy[2][0].toInt(), int(SIGHUP));
    }

    void signal_quit_saves_first_and_exits() {
        Lib l; l.write("a.md", "# A\n\nbase\n");
        AppController c(l.opts());
        hn::app::SignalBridge b;
        connect(&b, &hn::app::SignalBridge::quitSignal, &c, [&] { c.requestQuit(); });
        auto *a = c.openSticky("a.md");
        typeText(a, "NEW");
        QSignalSpy ex(&c, &AppController::exitRequested);
        ::raise(SIGTERM);
        QTRY_COMPARE_WITH_TIMEOUT(ex.size(), 1, 8000);
        QVERIFY(l.read("a.md").contains("NEW"));   // saved before the exit was requested
    }

    void signal_quit_with_failing_save_keeps_app_alive() {
        if (geteuid() == 0) QSKIP("root ignores directory permissions");
        Lib l; l.write("a.md", "# A\n\nbase\n");
        AppController c(l.opts());
        hn::app::SignalBridge b;
        connect(&b, &hn::app::SignalBridge::quitSignal, &c, [&] { c.requestQuit(); });
        auto *a = c.openSticky("a.md");
        QFile::setPermissions(l.notes, QFileDevice::ReadOwner | QFileDevice::ExeOwner);
        typeText(a, "NO");
        QSignalSpy ex(&c, &AppController::exitRequested), ab(&c, &AppController::quitAborted);
        ::raise(SIGHUP);
        QTRY_COMPARE_WITH_TIMEOUT(ab.size(), 1, 8000);
        QCOMPARE(ex.size(), 0);
        QVERIFY(c.stickyOf("a.md") && c.stickyOf("a.md")->isVisible());
        QFile::setPermissions(l.notes, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        c.closeNote("a.md", true);
    }

    // ---- (e) + (a) real process: --background creates nothing, SIGTERM exits 0 through the quit path ----
    void background_process_is_lazy_and_exits_cleanly_on_sigterm() {
        QTemporaryDir d;
        QDir().mkpath(d.path() + "/run");
        QFile::setPermissions(d.path() + "/run", QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        QDir().mkpath(d.path() + "/notes");
        QFile n(d.path() + "/notes/Body.md");
        QVERIFY(n.open(QIODevice::WriteOnly)); n.write("# Body\n\nsecret body text\n"); n.close();
        QProcess p;
        p.setProcessEnvironment(env(d.path()));
        p.start(HN_BIN, {"--background"});
        QVERIFY(p.waitForStarted());
        const QString sock = d.path() + "/run/hyprnotes-wayland-hn-p9-" + QString::number(getuid()) + ".sock";
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(sock), 10000);
        QTest::qWait(500);
        // nothing was indexed (the index is what reads note bodies) and no state was created
        QVERIFY(!QFileInfo::exists(d.path() + "/cache") || QDir(d.path() + "/cache").entryList(QDir::Files | QDir::NoDotAndDotDot | QDir::AllDirs).isEmpty());
        QVERIFY(!QFileInfo::exists(d.path() + "/cfg/config.json"));
        QCOMPARE(readFile(d.path() + "/notes/Body.md"), QByteArray("# Body\n\nsecret body text\n"));
        QCOMPARE(p.state(), QProcess::Running);
        ::kill(pid_t(p.processId()), SIGTERM);
        QVERIFY(p.waitForFinished(8000));
        QCOMPARE(p.exitStatus(), QProcess::NormalExit);   // not killed by the signal: handled by the save-aware path
        QCOMPARE(p.exitCode(), 0);
    }

    void background_controller_has_no_windows_sessions_or_index() {
        Lib l; l.write("a.md", "# A\n");
        auto o = l.opts();
        o.background = true;
        AppController c(o);
        c.start(Action::Background);
        QTest::qWait(200);
        QCOMPARE(c.windowCount(), 0);
        QVERIFY(c.openNotes().isEmpty());
        QVERIFY(!c.hasIndex());
        QVERIFY(!c.organizer());
        for (auto *w : QApplication::topLevelWidgets()) QVERIFY2(!w->isVisible(), qPrintable(w->metaObject()->className()));
        QVERIFY(!QFileInfo::exists(l.cache) || QDir(l.cache).isEmpty());
    }

    // ---- (b) tray default + fontSize in config, app.json migration ----
    void config_defaults_tray_on_and_font_in_config() {
        Lib l; QDir().mkpath(l.dir.filePath("cfg"));
        QFile f(l.cfg); QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(R"({"version":1,"colorScheme":"light"})"); f.close();   // an older file without trayEnabled
        AppController c(l.opts());
        QVERIFY(c.settings().trayEnabled);
        QCOMPARE(c.settings().fontSize, 0);
        auto s = c.settings(); s.fontSize = 18;
        c.applySettings(s, c.prefs());
        QCOMPARE(c.theme().baseSize, 18);
        hn::theme::Config cfg(l.cfg);
        QCOMPARE(cfg.settings().fontSize, 18);
        QVERIFY(!QFileInfo::exists(l.dir.filePath("cfg/app.json")));   // no app-owned side file any more
    }

    void legacy_app_json_is_migrated_once() {
        Lib l; QDir().mkpath(l.dir.filePath("cfg"));
        QFile f(l.cfg); QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(R"({"version":1,"colorScheme":"dark"})"); f.close();
        QFile a(l.dir.filePath("cfg/app.json")); QVERIFY(a.open(QIODevice::WriteOnly));
        a.write(R"({"version":1,"fontSize":19})"); a.close();
        {
            AppController c(l.opts());
            QCOMPARE(c.settings().fontSize, 19);
            QCOMPARE(c.theme().baseSize, 19);
            QCOMPARE(c.settings().colorScheme, QString("dark"));
        }
        QVERIFY(!QFileInfo::exists(l.dir.filePath("cfg/app.json")));   // folded into config.json, then dropped
        hn::theme::Config cfg(l.cfg);
        QCOMPARE(cfg.settings().fontSize, 19);
        QCOMPARE(cfg.settings().colorScheme, QString("dark"));
        {
            AppController c(l.opts());
            QCOMPARE(c.settings().fontSize, 19);
        }
    }

    void legacy_app_json_without_config_waits_for_first_save() {
        Lib l; QDir().mkpath(l.dir.filePath("cfg"));
        QFile a(l.dir.filePath("cfg/app.json")); QVERIFY(a.open(QIODevice::WriteOnly));
        a.write(R"({"version":1,"fontSize":17})"); a.close();
        AppController c(l.opts());
        QCOMPARE(c.settings().fontSize, 17);
        QVERIFY(!QFileInfo::exists(l.cfg));                              // never written at load
        QVERIFY(QFileInfo::exists(l.dir.filePath("cfg/app.json")));
        c.applySettings(c.settings(), c.prefs());                        // first real save
        QVERIFY(QFileInfo::exists(l.cfg));
        QVERIFY(!QFileInfo::exists(l.dir.filePath("cfg/app.json")));
        hn::theme::Config cfg(l.cfg);
        QCOMPARE(cfg.settings().fontSize, 17);
    }

    // ---- (c) snippets: bounded cache, no file reads while painting ----
    void organizer_list_snippets_are_cached_and_bounded() {
        Lib l;
        for (int i = 0; i < 40; ++i) l.write(QString("n%1.md").arg(i, 2, 10, QChar('0')), QByteArray("# T\n\nsnippet body ") + QByteArray::number(i) + "\n");
        AppController c(l.opts());
        c.showOrganizer();
        auto *o = c.organizer();
        QTRY_COMPARE_WITH_TIMEOUT(o->model()->rowCount(), 40, 8000);
        QStringList before;
        for (int i = 0; i < 40; ++i) before << o->model()->index(i, 0).data(NoteListModel::SnippetRole).toString();
        QVERIFY(before[3].startsWith("snippet body"));
        // remove the files: painting (grab) must not need them, rows keep their snippet
        QDir(l.notes).removeRecursively();
        o->resize(900, 640);
        o->list()->viewport()->repaint();
        const QImage img = o->grab().toImage();
        QVERIFY(!img.isNull());
        for (int i = 0; i < 40; ++i) QCOMPARE(o->model()->index(i, 0).data(NoteListModel::SnippetRole).toString(), before[i]);
        QVERIFY(NoteListModel::kMaxRows == 500);
    }

    // ---- polish: cheat sheet, colour popover ----
    void question_mark_opens_cheat_sheet_and_escape_closes_it() {
        Lib l; l.write("a.md", "# A\n");
        AppController c(l.opts());
        c.showOrganizer();
        auto *o = c.organizer();
        o->list()->setFocus();
        QVERIFY(!hn::app::ui::ShortcutSheet::isOpen(o));
        QTest::keyClick(o->list(), Qt::Key_Question, Qt::ShiftModifier);
        QVERIFY(hn::app::ui::ShortcutSheet::isOpen(o));
        auto *sheet = o->findChild<QWidget *>("hnSheet", Qt::FindDirectChildrenOnly);
        QVERIFY(sheet && sheet->size() == o->size());
        QVERIFY(!sheet->grab().toImage().isNull());
        QTest::keyClick(sheet, Qt::Key_Escape);
        QTRY_VERIFY(!hn::app::ui::ShortcutSheet::isOpen(o));
        // typed "?" in the search field is text, never a shortcut
        o->searchEdit()->setFocus();
        QTest::keyClick(o->searchEdit(), Qt::Key_Question, Qt::ShiftModifier);
        QVERIFY(!hn::app::ui::ShortcutSheet::isOpen(o));
        QCOMPARE(o->searchEdit()->text(), QString("?"));
    }

    void swatch_popover_is_keyboard_operable() {
        auto *pop = new hn::app::ui::SwatchPopover(2);
        QSignalSpy spy(pop, &hn::app::ui::SwatchPopover::picked);
        QPointer<hn::app::ui::SwatchPopover> guard(pop);
        pop->popup(QPoint(10, 10));
        QTest::keyClick(pop, Qt::Key_Right);
        QTest::keyClick(pop, Qt::Key_Return);
        QCOMPARE(spy.size(), 1);
        QCOMPARE(spy[0][0].toInt(), 3);
        QTRY_VERIFY(!guard);                    // closes (and deletes itself) after a pick
        auto *p2 = new hn::app::ui::SwatchPopover(0);
        QSignalSpy spy2(p2, &hn::app::ui::SwatchPopover::picked);
        p2->popup(QPoint(10, 10));
        QTest::keyClick(p2, Qt::Key_5);
        QCOMPARE(spy2[0][0].toInt(), 4);
    }

    // ---- (f) keep-above is unsupported and never offered ----
    void no_keep_above_in_any_menu_or_header() {
        QVERIFY(!hn::platform::supportsKeepAbove());
        Lib l; l.write("a.md", "# A\n");
        AppController c(l.opts());
        c.openSticky("a.md");
        QMenu m;
        c.populateNoteMenu(&m, "a.md", true);
        std::function<void(QMenu *)> scan = [&](QMenu *menu) {
            for (auto *a : menu->actions()) {
                QVERIFY2(!a->text().contains("above", Qt::CaseInsensitive) && !a->text().contains("on top", Qt::CaseInsensitive), qPrintable(a->text()));
                if (a->menu()) scan(a->menu());
            }
        };
        scan(&m);
        auto *w = c.stickyOf("a.md");
        for (auto *b : w->findChildren<QWidget *>()) {
            QVERIFY(!b->toolTip().contains("above", Qt::CaseInsensitive));
            QVERIFY(!b->toolTip().contains("keep on top", Qt::CaseInsensitive));
        }
    }

    // ---- (d) Hyprland placement + session save through the fake compositor ----
    void hyprland_restores_placement_and_saves_session() {
        Lib l; l.write("a.md", "# A\n");
        FakeHyprland fake;
        QString token;                                  // learned once the sticky exists
        QRect liveRect(0, 0, 0, 0);                     // what the compositor currently reports
        bool livePinned = false;
        fake.handler = [&](const QByteArray &cmd) -> QByteArray {
            if (cmd.contains("clients")) {
                if (token.isEmpty()) return "[]";
                return QJsonDocument(QJsonArray{QJsonObject{
                    {"address", "0x55738da49dd0"}, {"mapped", true}, {"hidden", false},
                    {"at", QJsonArray{liveRect.x(), liveRect.y()}}, {"size", QJsonArray{liveRect.width(), liveRect.height()}},
                    {"workspace", QJsonObject{{"id", 2}, {"name", "2"}}}, {"floating", true}, {"monitor", 0},
                    {"class", "hyprnotes"}, {"initialClass", "hyprnotes"}, {"title", "A"},
                    {"initialTitle", "hyprnotes-sticky:" + token}, {"pid", double(QCoreApplication::applicationPid())},
                    {"xwayland", false}, {"pinned", livePinned}}}).toJson(QJsonDocument::Compact);
            }
            if (cmd.contains("monitors")) return kMonitorsJson;
            if (cmd.contains("workspaces")) return R"([{"id":2,"name":"2","monitorID":0,"windows":1}])";
            return "ok";
        };
        // a saved session: sticky at (120,140) 420x320, pinned
        hn::platform::SessionState st;
        hn::platform::WindowState ws;
        ws.noteKey = "a.md"; ws.role = hn::platform::Role::Sticky; ws.geometry = QRect(120, 140, 420, 320);
        ws.monitor = "eDP-1"; ws.workspace = 2; ws.scale = 2; ws.mode = hn::platform::WorkspaceMode::AllWorkspaces;
        st.windows << ws;
        QString err;
        QVERIFY(hn::platform::saveSession(l.state + "/session.json", st, &err));

        auto o = l.opts();
        o.useHyprland = true;
        o.hyprPaths = fake.paths();
        AppController c(o);
        QVERIFY(c.ipc());
        QTRY_VERIFY_WITH_TIMEOUT(c.ipc()->connected(), 5000);
        QVERIFY(c.handleAction(Action::ShowOrganizer));     // restores the saved sticky
        auto *w = c.stickyOf("a.md");
        QVERIFY(w);
        QCOMPARE(w->workspaceMode(), hn::platform::WorkspaceMode::AllWorkspaces);
        token = w->token();
        liveRect = QRect(10, 10, 360, 300);
        fake.emitEvent("openwindow>>55738da49dd0,2,hyprnotes,A\n");
        // the placement sequence: float, workspace, exact size, exact move, pin
        QTRY_VERIFY_WITH_TIMEOUT(fake.received.filter("420").size() >= 1 && fake.received.filter("pin").size() >= 1, 8000);
        const QString all = fake.received.join("\n");
        QVERIFY2(all.contains("420") && all.contains("320"), qPrintable(all));
        QVERIFY2(all.contains("120") && all.contains("140"), qPrintable(all));
        QVERIFY2(all.contains("0x55738da49dd0"), qPrintable(all));
        const int pinAt = all.lastIndexOf("pin");
        QVERIFY(pinAt > all.indexOf("420"));                // pin comes after the exact size step (sequential, not racing)

        // now the user moves/resizes it and unpins: the next coalesced refresh must land in session.json
        liveRect = QRect(300, 200, 500, 400);
        livePinned = false;
        fake.emitEvent("movewindowv2>>55738da49dd0,2,2\n");
        QTRY_VERIFY_WITH_TIMEOUT(c.sessionState().windows.size() == 1 && c.sessionState().windows[0].geometry == liveRect, 8000);
        QCOMPARE(c.sessionState().windows[0].mode, hn::platform::WorkspaceMode::ThisWorkspace);
        c.flushSessionFile();
        bool ok = false;
        const auto disk = hn::platform::loadSession(l.state + "/session.json", &ok);
        QVERIFY(ok);
        QCOMPARE(disk.windows.size(), 1);
        QCOMPARE(disk.windows[0].noteKey, QString("a.md"));
        QCOMPARE(disk.windows[0].geometry, liveRect);
        QCOMPARE(disk.windows[0].mode, hn::platform::WorkspaceMode::ThisWorkspace);

        // toggling the workspace chip sends a pin dispatch for the right window
        fake.received.clear();
        c.setWorkspaceMode("a.md", hn::platform::WorkspaceMode::AllWorkspaces);
        QTRY_VERIFY_WITH_TIMEOUT(fake.received.join("\n").contains("pin"), 3000);
        QVERIFY(fake.received.join("\n").contains("0x55738da49dd0"));
        c.closeNote("a.md");
        QTRY_VERIFY(!c.session("a.md"));
    }
};

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    P9Test t;
    return QTest::qExec(&t, argc, argv);
}
#include "app_p9_test.moc"
