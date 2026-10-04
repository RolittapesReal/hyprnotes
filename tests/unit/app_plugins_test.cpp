// Plugin integration in the app: bridges, install -> consent -> enable, tamper, palette, triggers, toolbar/menu/keys,
// events, pre_save, network policy, CLI, consent dialog contents, Plugins page. Real fixtures from tests/unit/fixtures/plugins.
#include "app_test_util.h"
#include "cli.h"
#include "command_palette.h"
#include "consent_dialog.h"
#include "hn/plugins/store.h"
#include "plugins_page.h"
#include "settings_dialog.h"
#include <QCheckBox>
#include <QClipboard>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QScrollArea>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrl>

using namespace apptest;
using namespace hn::plugins;
using hn::editor::Mode;
using hn::platform::Action;

namespace {
QStringList g_logs;
QtMessageHandler g_prev = nullptr;
void capture(QtMsgType t, const QMessageLogContext &c, const QString &m) {
    if (m.startsWith("[plugin ")) { g_logs << m; return; }
    if (g_prev) g_prev(t, c, m);
}
bool logged(const QString &sub) { for (const auto &l : g_logs) if (l.contains(sub)) return true; return false; }

QString fx(const QString &n) { return QString(HN_PLUGIN_FIXTURES) + "/" + n; }

// Writes <root>/<id>/{plugin.json,main.lua}; returns the folder.
QString mk(const QString &root, const QString &id, const QStringList &perms, const QByteArray &lua, const QStringList &hosts = {}, const QString &name = {}) {
    const QString dir = root + "/" + id;
    QDir().mkpath(dir);
    QJsonObject o{{"id", id}, {"name", name.isEmpty() ? id : name}, {"version", "1.0.0"}, {"author", "tests"}, {"description", "test plugin"},
                  {"api", 1}, {"tier", "script"}, {"entry", "main.lua"}, {"permissions", QJsonArray::fromStringList(perms)}, {"min_app", "0.1.0"}};
    if (!hosts.isEmpty()) o["net_hosts"] = QJsonArray::fromStringList(hosts);
    QFile pj(dir + "/plugin.json"), ml(dir + "/main.lua");
    if (!pj.open(QIODevice::WriteOnly) || !ml.open(QIODevice::WriteOnly)) qFatal("cannot write plugin");
    pj.write(QJsonDocument(o).toJson());
    ml.write(lua);
    return dir;
}

PluginAction action(AppController &c, const QString &qid) {
    for (const auto &a : c.plugins().allActions()) if (a.qid == qid) return a;
    return {};
}
QString plain(NoteSession *s) { return s->editor()->visualEdit()->toPlainText(); }

ControllerOptions accepting(Lib &l, ConsentRequest *seen = nullptr) {
    auto o = l.opts();
    o.pluginHooks.consent = [seen](const ConsentRequest &r) { if (seen) *seen = r; return true; };
    return o;
}

// Plain-http loopback server standing in for a remote host: redirects, big bodies, silence, cookies.
class Server : public QTcpServer {
public:
    int hits = 0;
    QStringList requests;
    explicit Server() {
        QVERIFY(listen(QHostAddress::LocalHost));
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (auto *s = nextPendingConnection()) {
                connect(s, &QTcpSocket::readyRead, s, [this, s] {
                    if (!s->property("done").isNull() || !s->bytesAvailable()) return;
                    buf[s] += s->readAll();
                    if (!buf[s].contains("\r\n\r\n")) return;
                    s->setProperty("done", true);
                    handle(s, QString::fromLatin1(buf[s]));
                });
            }
        });
    }
    QString base() const { return QString("http://127.0.0.1:%1").arg(serverPort()); }
private:
    QHash<QTcpSocket *, QByteArray> buf;
    static void reply(QTcpSocket *s, const QByteArray &head, const QByteArray &body = {}) {
        s->write("HTTP/1.1 " + head + "\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
        s->disconnectFromHost();
    }
    void handle(QTcpSocket *s, const QString &req) {
        ++hits;
        requests << req;
        const QString path = req.section(' ', 1, 1);
        const QString me = QString("127.0.0.1:%1").arg(serverPort());
        if (path == "/ok") reply(s, "200 OK", "hello");
        else if (path == "/redir-same") s->write(("HTTP/1.1 302 Found\r\nLocation: http://" + me + "/ok\r\nContent-Length: 0\r\nConnection: close\r\n\r\n").toLatin1()), s->disconnectFromHost();
        else if (path == "/redir-other") s->write(("HTTP/1.1 302 Found\r\nLocation: http://localhost:" + QString::number(serverPort()) + "/ok\r\nContent-Length: 0\r\nConnection: close\r\n\r\n").toLatin1()), s->disconnectFromHost();
        else if (path == "/redir-https") s->write("HTTP/1.1 302 Found\r\nLocation: https://127.0.0.1/ok\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"), s->disconnectFromHost();
        else if (path == "/loop") s->write(("HTTP/1.1 302 Found\r\nLocation: http://" + me + "/loop\r\nContent-Length: 0\r\nConnection: close\r\n\r\n").toLatin1()), s->disconnectFromHost();
        else if (path == "/big") reply(s, "200 OK", QByteArray(2 * 1024 * 1024, 'x'));
        else if (path == "/cookie") s->write("HTTP/1.1 200 OK\r\nSet-Cookie: session=abc; Path=/\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok"), s->disconnectFromHost();
        else if (path == "/slow") { /* never answers */ }
        else reply(s, "404 Not Found", "no");
    }
};
}  // namespace

class PluginsAppTest : public QObject {
    Q_OBJECT
    static QProcessEnvironment cliEnv(const QString &root) {
        QProcessEnvironment e;
        e.insert("PATH", qEnvironmentVariable("PATH"));
        e.insert("HOME", root + "/home");
        e.insert("QT_QPA_PLATFORM", "no-such-platform");   // proves these actions need no display
        e.insert("HN_STATE_DIR", root + "/state");
        e.insert("HN_DATA_DIR", root + "/data");
        e.insert("HN_CONFIG_DIR", root + "/cfg");
        return e;
    }
    static int cli(const QString &root, const QStringList &args, QString *out = nullptr, QString *err = nullptr, const QByteArray &stdinData = {}, bool closeStdin = true) {
        QProcess p;
        p.setProcessEnvironment(cliEnv(root));
        p.setWorkingDirectory(root);
        p.start(HN_BIN, args);
        if (!p.waitForStarted(5000)) return -98;
        if (!stdinData.isEmpty()) p.write(stdinData);
        if (closeStdin) p.closeWriteChannel();
        if (!p.waitForFinished(20000)) { p.kill(); return -99; }
        if (out) *out = QString::fromLocal8Bit(p.readAllStandardOutput());
        if (err) *err = QString::fromLocal8Bit(p.readAllStandardError());
        return p.exitCode();
    }

private slots:
    void initTestCase() { g_prev = qInstallMessageHandler(capture); }
    void init() { g_logs.clear(); }

    // ---------------------------------------------------------------- lazy
    void nothing_is_created_until_a_plugin_is_enabled() {
        Lib l;
        l.write("a.md", "# T\n\nhello\n");
        const int nam0 = PluginNetBridge::managersCreated();
        {
            AppController c(l.opts());
            c.start(Action::Background);
            auto *s = c.openSticky("a.md");
            typeText(s, "abc");
            QTest::qWait(300);
            QVERIFY(!c.plugins().active());                          // no manager, hence no Lua host, no timer, no network objects
            QVERIFY(!c.plugins().networkManagerCreated());
            QVERIFY(!s->editor()->hasTriggerHandler());              // typing pays nothing
            QVERIFY(c.plugins().commands().isEmpty());
            QCOMPARE(PluginNetBridge::managersCreated(), nam0);
            // An installed-but-not-enabled plugin (the Plugins page creates the manager): still no Lua state, timer or network.
            QString msg;
            const auto r = c.plugins().install(fx("hello"), nullptr, &msg);   // consent hook declines
            QVERIFY2(r.ok, qPrintable(msg));
            auto *m = c.plugins().manager();
            QCOMPARE(m->info("hello").status, Status::NeedsConsent);
            QCOMPARE(m->loadedStates(), 0);
            QVERIFY(!m->timerActive());
            QCOMPARE(PluginNetBridge::managersCreated(), nam0);
            c.plugins().hooks().consent = [](const ConsentRequest &) { return true; };
            QVERIFY(c.plugins().enable("hello", nullptr));
            QCOMPARE(m->loadedStates(), 0);                          // registrations are cached; a state is created by the first hook
            QVERIFY(c.plugins().runQualified("hello:count", s));
            QCOMPARE(m->loadedStates(), 1);
            QCOMPARE(PluginNetBridge::managersCreated(), nam0);      // no plugin made a request
            QVERIFY(!c.plugins().networkManagerCreated());
        }
        {   // next launch: enabled plugin => manager exists straight away, app.started is emitted
            AppController c(l.opts());
            QVERIFY(!c.plugins().active());
            c.start(Action::Background);
            QVERIFY(c.plugins().active());
            QCOMPARE(c.plugins().manager()->info("hello").status, Status::Enabled);
            QVERIFY(!action(c, "hello:shout").qid.isEmpty());
        }
    }

    void app_started_and_note_events_reach_subscribers() {
        Lib l;
        const QString src = l.dir.filePath("src");
        mk(src, "ev", {}, R"(
hn.on("app.started", function() hn.log("EV started") end)
hn.on("note.opened", function(p) hn.log("EV opened " .. p) end)
hn.on("note.changed", function(p) hn.log("EV changed " .. p) end)
hn.on("note.saved", function(p) hn.log("EV saved " .. p) end)
hn.on("note.closed", function(p) hn.log("EV closed " .. p) end)
hn.on("selection.changed", function(p) hn.log("EV selection " .. p) end)
)");
        l.write("a.md", "# T\n\nhello world\n");
        {
            AppController c(accepting(l));
            QString msg;
            QVERIFY(c.plugins().install(src + "/ev", nullptr, &msg).ok);
            QCOMPARE(c.plugins().manager()->info("ev").status, Status::Enabled);
        }
        AppController c(l.opts());
        c.start(Action::Background);
        QTRY_VERIFY(logged("EV started"));
        auto *s = c.openSticky("a.md");
        s->setTiming({200, 500});
        QTRY_VERIFY(logged("EV opened a.md"));
        typeText(s, "xyz");
        QTRY_VERIFY_WITH_TIMEOUT(logged("EV changed a.md"), 2000);     // coalesced (>= 250 ms), but delivered
        QTRY_VERIFY_WITH_TIMEOUT(logged("EV saved a.md"), 3000);
        QVERIFY(logged("EV selection a.md"));
        c.closeNote("a.md");
        QTRY_VERIFY_WITH_TIMEOUT(logged("EV closed a.md"), 3000);
    }

    // ---------------------------------------------------------------- consent flow
    void install_then_consent_then_enable_through_the_dialog_logic() {
        Lib l;
        l.write("a.md", "# T\n\nhello world\n");
        ConsentRequest seen;
        int asked = 0;
        auto o = l.opts();
        o.pluginHooks.consent = [&](const ConsentRequest &r) { seen = r; ++asked; return asked >= 2; };   // first look: Cancel
        AppController c(o);
        c.start(Action::Background);
        auto *s = c.openSticky("a.md");
        QString msg;
        auto r = c.plugins().install(fx("hello"), nullptr, &msg);
        QVERIFY2(r.ok, qPrintable(msg));
        QCOMPARE(asked, 1);
        auto *m = c.plugins().manager();
        QCOMPARE(m->info("hello").status, Status::NeedsConsent);   // installed, NOT enabled
        QVERIFY(msg.contains("not enabled"));
        QVERIFY(c.plugins().commands().isEmpty());                // nothing registered, nothing runs before consent
        QString err;
        QVERIFY(!m->runCommand("hello:count", nullptr, &err));
        QVERIFY(!c.plugins().runQualified("hello:shout", s));
        QCOMPARE(m->loadedStates(), 0);
        QVERIFY(!QFile::exists(l.state + "/plugin-data/hello.json"));
        // the request carries everything the dialog shows
        QCOMPARE(seen.name, QString("Hello"));
        QCOMPARE(seen.author, QString("Hyprnotes tests"));
        QCOMPARE(seen.version, QString("1.0.0"));
        QCOMPARE(seen.permissions, QStringList({"note.read", "note.edit", "ui", "storage"}));
        QVERIFY(!seen.native);
        QVERIFY(seen.source.contains("hello"));
        QCOMPARE(seen.sha256, hn::plugins::hashDirectory(l.dir.filePath("plugins/hello")));
        QCOMPARE(seen.sha256.size(), 64);
        QVERIFY(seen.reconsentReason.isEmpty());
        // second time: "I trust this plugin - Enable"
        QVERIFY(c.plugins().enable("hello", nullptr, &msg));
        QCOMPARE(asked, 2);
        QCOMPARE(m->info("hello").status, Status::Enabled);
        QVERIFY(m->trust()->record("hello").hasConsent);
        QVERIFY(m->trust()->record("hello").enabled);
        QVERIFY(!action(c, "hello:shout").qid.isEmpty());
        // an already consented plugin enables without asking again
        QVERIFY(m->disable("hello"));
        QVERIFY(c.plugins().enable("hello", nullptr, &msg));
        QCOMPARE(asked, 2);
        QVERIFY(c.plugins().auditText().contains("consent"));
    }

    void tamper_disables_and_asks_again() {
        Lib l;
        ConsentRequest seen;
        {
            AppController c(accepting(l));
            c.start(Action::Background);
            QString msg;
            QVERIFY(c.plugins().install(fx("hello"), nullptr, &msg).ok);
            QCOMPARE(c.plugins().manager()->info("hello").status, Status::Enabled);
        }
        QFile f(l.dir.filePath("plugins/hello/main.lua"));
        QVERIFY(f.open(QIODevice::Append));
        f.write("\nhn.command{ id = 'evil', title = 'Evil', run = function() end }\n");
        f.close();
        AppController c(l.opts());
        c.start(Action::Background);
        auto *m = c.plugins().manager();
        QCOMPARE(m->info("hello").status, Status::NeedsConsent);
        QVERIFY(m->info("hello").needsReconsent);
        QVERIFY(c.plugins().commands().isEmpty());                 // the modified plugin never ran
        QVERIFY(c.plugins().auditText().contains("tamper"));
        QVERIFY(!c.plugins().enable("hello", nullptr));            // dialog declines (test default)
        c.plugins().hooks().consent = [&](const ConsentRequest &r) { seen = r; return true; };
        QVERIFY(c.plugins().enable("hello", nullptr));
        QVERIFY(seen.reconsentReason.contains("changed"));
        QCOMPARE(m->info("hello").status, Status::Enabled);
        QVERIFY(!action(c, "hello:evil").qid.isEmpty());
    }

    void permission_growth_on_upgrade_asks_again_and_names_the_new_permission() {
        Lib l;
        const QString src = l.dir.filePath("src");
        mk(src, "grow", {"note.read"}, "hn.command{ id='c', title='C', run=function() end }");
        ConsentRequest seen;
        AppController c(accepting(l, &seen));
        QString msg;
        QVERIFY(c.plugins().install(src + "/grow", nullptr, &msg).ok);
        QCOMPARE(c.plugins().manager()->info("grow").status, Status::Enabled);
        mk(src, "grow", {"note.read", "network"}, "hn.command{ id='c', title='C', run=function() end }", {"api.example.com"});
        c.plugins().hooks().confirm = [](const QString &, const QString &) { return true; };   // replace the installed version
        int asked = 0;
        c.plugins().hooks().consent = [&](const ConsentRequest &r) { seen = r; ++asked; return false; };
        const auto r = c.plugins().install(src + "/grow", nullptr, &msg);
        QVERIFY2(r.ok && r.upgraded, qPrintable(msg));
        QCOMPARE(asked, 1);
        QVERIFY(seen.newPermissions.contains("network"));
        QCOMPARE(c.plugins().manager()->info("grow").status, Status::NeedsConsent);   // declined: disabled, not silently kept
    }

    // ---------------------------------------------------------------- bridges
    void plugin_callback_is_one_undo_step_and_goes_through_autosave() {
        Lib l;
        l.write("a.md", "# T\n\nhello world\n");
        AppController c(accepting(l));
        c.start(Action::Background);
        QString msg;
        QVERIFY(c.plugins().install(fx("hello"), nullptr, &msg).ok);
        auto *s = c.openSticky("a.md");
        s->setTiming({200, 500});
        auto *ed = s->editor();
        QTextEdit *v = ed->visualEdit();
        QTextCursor cur = v->document()->find("hello");
        QVERIFY(!cur.isNull());
        v->setTextCursor(cur);
        const int before = ed->history().position();
        QVERIFY(c.plugins().runQualified("hello:shout", s));       // replace_selection + insert in ONE callback
        QVERIFY(plain(s).contains("HELLO! world"));
        QCOMPARE(ed->history().position(), before + 1);
        QTRY_VERIFY_WITH_TIMEOUT(l.read("a.md").contains("HELLO!"), 3000);
        QVERIFY(ed->undo());
        QVERIFY(plain(s).contains("hello world") && !plain(s).contains("HELLO"));
        QVERIFY(ed->redo());
        QVERIFY(plain(s).contains("HELLO! world"));
        // a second callback is a second step, not merged
        v->setTextCursor(v->document()->find("world"));
        QVERIFY(c.plugins().runQualified("hello:shout", s));
        QCOMPARE(ed->history().position(), before + 2);
        QVERIFY(ed->undo());
        QVERIFY(plain(s).contains("HELLO! world"));
    }

    void note_bridge_reads_and_set_text_is_one_undo_step() {
        Lib l;
        l.write("a.md", "# Title\n\nbody #tag\n");
        AppController c(l.opts());
        auto *s = c.openSticky("a.md");
        PluginNoteBridge b(s);
        QCOMPARE(b.path(), QString("a.md"));
        QCOMPARE(b.title(), QString("Title"));
        QVERIFY(b.tags().contains("tag"));
        QVERIFY(b.text().contains("body #tag"));
        const int before = s->editor()->history().position();
        b.beginTransaction("t");
        b.setText("# New\n\nreplaced\n");
        b.insert(" more");
        b.endTransaction();
        QCOMPARE(s->editor()->history().position(), before + 1);
        QVERIFY(b.text().contains("replaced"));
        QVERIFY(s->editor()->undo());
        QVERIFY(b.text().contains("body #tag"));
        // a dying session turns every call into a no-op (detachNote before windows die)
        b.detach();
        QCOMPARE(b.text(), QString());
        b.beginTransaction("x");
        b.insert("zz");
        b.endTransaction();
        QVERIFY(!plain(s).contains("zz"));
    }

    void permission_denial_is_audited_and_reaches_the_ui() {
        Lib l;
        const QString src = l.dir.filePath("src");
        mk(src, "nosy", {"note.read", "ui"}, R"(
hn.command{ id = "edit", title = "Edit", run = function() hn.note.insert("x") end }
hn.command{ id = "soft", title = "Soft", run = function()
  local ok, err = pcall(hn.clipboard.get)
  hn.ui.notify("clip " .. tostring(ok) .. " " .. tostring(err))
end }
)", {}, "Nosy");
        l.write("a.md", "# T\n\nhello\n");
        AppController c(accepting(l));
        c.start(Action::Background);
        QVERIFY(c.plugins().install(src + "/nosy", nullptr, nullptr).ok);
        auto *s = c.openSticky("a.md");
        QString err;
        QVERIFY(!c.plugins().runQualified("nosy:edit", s, &err));
        QVERIFY2(err.contains("permission denied"), qPrintable(err));
        QVERIFY(c.lastNotice().contains("Nosy"));                      // surfaced in the status strip
        QVERIFY(c.lastNotice().contains("permission denied"));
        QVERIFY(!plain(s).contains("hellox"));
        QVERIFY(c.plugins().runQualified("nosy:soft", s));            // pcall'd denial: no failure, still audited
        QVERIFY(c.lastNotice().contains("clip false"));
        const QString audit = c.plugins().auditText();
        QVERIFY2(audit.contains("denied"), qPrintable(audit));
        QVERIFY(audit.contains("note.edit"));
        QVERIFY(audit.contains("clipboard"));
    }

    void ui_bridge_prompt_confirm_pick_and_notify() {
        Lib l;
        const QString src = l.dir.filePath("src");
        mk(src, "talk", {"ui"}, R"(
hn.command{ id = "ask", title = "Ask", run = function()
  local a = hn.ui.prompt("Title", "Label", "dflt")
  local b = hn.ui.confirm("Sure?")
  local c = hn.ui.pick("Pick", {"one", "two", "three"})
  hn.ui.notify(tostring(a) .. "|" .. tostring(b) .. "|" .. tostring(c))
end }
)", {}, "Talk");
        auto o = accepting(l);
        QStringList seen;
        o.pluginHooks.prompt = [&](const QString &p, const QString &t, const QString &lab, const QString &d) { seen << p + "/" + t + "/" + lab + "/" + d; return std::optional<QString>("typed"); };
        o.pluginHooks.confirm = [&](const QString &p, const QString &m) { seen << p + "/" + m; return true; };
        o.pluginHooks.pick = [&](const QString &p, const QString &t, const QStringList &items) { seen << p + "/" + t + "/" + items.join(","); return 2; };
        AppController c(o);
        c.start(Action::Background);
        QVERIFY(c.plugins().install(src + "/talk", nullptr, nullptr).ok);
        QVERIFY(c.plugins().runQualified("talk:ask", nullptr));
        QCOMPARE(c.lastNotice(), QString("Talk: typed|true|3"));   // pick is 1-based for Lua, the bridge returned index 2
        QCOMPARE(seen.size(), 3);
        QCOMPARE(seen[0], QString("Talk/Title/Label/dflt"));
        QCOMPARE(seen[2], QString("Talk/Pick/one,two,three"));
    }

    void library_bridge_has_the_save_safety_of_the_app() {
        Lib l;
        l.write("closed.md", "# Closed\n\nold\n");
        l.write("open.md", "# Open\n\ntext\n");
        AppController c(l.opts());
        PluginLibraryBridge b(&c);
        auto notes = b.list({});
        QStringList rels;
        for (const auto &n : notes) rels << n.path;
        QVERIFY(rels.contains("closed.md") && rels.contains("open.md"));
        QString t;
        QVERIFY(b.read("closed.md", &t));
        QVERIFY(t.contains("old"));
        QVERIFY(!b.read("../etc/passwd", &t));                       // escapes the library
        QVERIFY(!b.read("/etc/passwd", &t));
        QVERIFY(!b.read("closed.txt", &t));
        QVERIFY(!b.write("../x.md", "boom"));
        QVERIFY(!b.write("nonexistent.md", "boom"));                 // create() makes notes
        const QString made = b.create("Made", "# Made\n\nhere\n");
        QVERIFY(!made.isEmpty());
        QVERIFY(l.read(made).contains("here"));
        QVERIFY(b.write("closed.md", "# Closed\n\nnew text\n"));    // atomic save through the repository, waits for the result
        QVERIFY(l.read("closed.md").contains("new text"));
        // open note: written through the editor (undoable, autosave), never behind its back
        auto *s = c.openSticky("open.md");
        const int before = s->editor()->history().position();
        QVERIFY(b.write("open.md", "# Open\n\nfrom plugin\n"));
        QVERIFY(plain(s).contains("from plugin"));
        QCOMPARE(s->editor()->history().position(), before + 1);
        QVERIFY(b.read("open.md", &t) && t.contains("from plugin"));   // unsaved edits are what a plugin reads
        QVERIFY(l.read("open.md").contains("text"));                   // disk still has the old text until autosave
        QVERIFY(b.remove("closed.md"));
        QVERIFY(!QFile::exists(l.notes + "/closed.md"));
        QVERIFY(!b.remove("../x.md"));
    }

    void clipboard_and_theme_bridges() {
        Lib l;
        AppController c(l.opts());
        PluginClipboardBridge cb;
        cb.set("from plugin");
        QCOMPARE(QGuiApplication::clipboard()->text(), QString("from plugin"));
        QCOMPARE(cb.get(), QString("from plugin"));
        PluginThemeBridge tb(&c);
        QVERIFY(tb.setToken("p", "accent", "#112233"));
        QCOMPARE(c.theme().accent.name(), QString("#112233"));
        QVERIFY(!tb.setToken("p", "nonsense", "#112233"));
        QVERIFY(!tb.setToken("p", "accent", "red; evil"));
        QVERIFY(!tb.setToken("p", "text", c.theme().bg.name()));        // unreadable result is refused
        QCOMPARE(c.theme().accent.name(), QString("#112233"));
    }

    // ---------------------------------------------------------------- network
    void net_policy_every_hop_is_checked_size_time_cookies() {
        Server srv;
        PluginNetBridge net;
        QCOMPARE(PluginNetBridge::managersCreated(), PluginNetBridge::managersCreated());
        const int created0 = PluginNetBridge::managersCreated();
        QVERIFY(!net.managerCreated());                               // lazy: nothing until the first request
        // test validator: plain-http loopback stands in for https (the production rule is tested below)
        net.setValidator([](const QUrl &u, const QStringList &hosts, QString *why) {
            if (hosts.contains(u.host())) return true;
            if (why) *why = "host " + u.host() + " is not allowed";
            return false;
        });
        HttpRequest req;
        req.method = "GET";
        req.allowedHosts = {"127.0.0.1"};
        req.timeoutMs = 3000;
        req.url = srv.base() + "/ok";
        auto r = net.perform(req);
        QCOMPARE(r.status, 200);
        QCOMPARE(r.body, QByteArray("hello"));
        QVERIFY(net.managerCreated());
        QCOMPARE(PluginNetBridge::managersCreated(), created0 + 1);
        req.url = srv.base() + "/redir-same";                         // redirect inside the allowlist is followed
        r = net.perform(req);
        QCOMPARE(r.status, 200);
        QCOMPARE(r.body, QByteArray("hello"));
        const int hitsBefore = srv.hits;
        req.url = srv.base() + "/redir-other";                        // different host: refused, the target is never contacted
        r = net.perform(req);
        QCOMPARE(r.status, 0);
        QVERIFY2(r.error.contains("redirect refused"), qPrintable(r.error));
        QVERIFY(r.body.isEmpty());
        QCOMPARE(srv.hits, hitsBefore + 1);
        req.url = srv.base() + "/loop";
        r = net.perform(req);
        QVERIFY2(r.error.contains("too many redirects"), qPrintable(r.error));
        req.url = srv.base() + "/big";
        req.maxResponseBytes = 4096;
        r = net.perform(req);
        QVERIFY2(r.error.contains("larger than"), qPrintable(r.error));
        QVERIFY(r.body.isEmpty());
        req.maxResponseBytes = 1024 * 1024;
        req.url = srv.base() + "/slow";
        req.timeoutMs = 400;
        QElapsedTimer t;
        t.start();
        r = net.perform(req);
        QVERIFY2(r.error.contains("timed out"), qPrintable(r.error));
        QVERIFY(t.elapsed() < 3000);
        // no cookie persistence, no cache
        req.timeoutMs = 3000;
        req.url = srv.base() + "/cookie";
        QCOMPARE(net.perform(req).status, 200);
        srv.requests.clear();
        req.url = srv.base() + "/ok";
        QCOMPARE(net.perform(req).status, 200);
        QCOMPARE(net.perform(req).status, 200);
        QCOMPARE(srv.requests.size(), 2);                             // not served from a cache
        for (const auto &rq : std::as_const(srv.requests)) QVERIFY(!rq.contains("Cookie:", Qt::CaseInsensitive));
        // production validator: https only, listed hosts only, no userinfo / other ports
        const QStringList hosts{"api.example.com"};
        QString why;
        QVERIFY(PluginNetBridge::defaultValidator(QUrl("https://api.example.com/x"), hosts, &why));
        for (const char *bad : {"http://api.example.com/x", "https://evil.example.com/", "https://api.example.com:8443/", "https://u:p@api.example.com/",
                                "https://api.example.com.evil.net/", "ftp://api.example.com/", "https://127.0.0.1/"})
            QVERIFY2(!PluginNetBridge::defaultValidator(QUrl(bad), hosts, &why), bad);
        // and the default (production) bridge refuses a plain-http URL before any socket is opened
        PluginNetBridge prod;
        req.allowedHosts = {"127.0.0.1"};
        req.url = srv.base() + "/ok";
        const int hits = srv.hits;
        r = prod.perform(req);
        QVERIFY(!r.error.isEmpty());
        QCOMPARE(srv.hits, hits);
        QVERIFY(!prod.managerCreated());                              // refused before the manager was even needed
        req.allowedHosts = {"api.example.com"};
        req.url = "https://api.example.com/x";
        // (no outbound request is made in tests; a refusal path is enough here)
    }

    // ---------------------------------------------------------------- events / pre_save
    void pre_save_transforms_the_saved_bytes_and_a_failure_never_blocks_saving() {
        Lib l;
        const QString src = l.dir.filePath("src");
        mk(src, "fixer", {}, R"(hn.on("note.pre_save", function(text) return (text:gsub("TODO", "DONE")) end))", {}, "Fixer");
        mk(src, "broken", {}, R"(hn.on("note.pre_save", function(text) error("boom") end))", {}, "Broken");
        l.write("a.md", "# T\n\nfirst\n");
        AppController c(accepting(l));
        c.start(Action::Background);
        QVERIFY(c.plugins().install(src + "/fixer", nullptr, nullptr).ok);
        QVERIFY(c.plugins().install(src + "/broken", nullptr, nullptr).ok);
        auto *s = c.openSticky("a.md");
        s->setTiming({150, 400});
        s->editor()->visualEdit()->moveCursor(QTextCursor::End);
        typeText(s, " TODO");
        QTRY_VERIFY_WITH_TIMEOUT(l.read("a.md").contains("DONE"), 4000);    // fixer applied; the broken plugin did not block the save
        QVERIFY(!l.read("a.md").contains("TODO"));
        QVERIFY(plain(s).contains("TODO"));                                  // the editor keeps what the user typed
        QTRY_COMPARE_WITH_TIMEOUT(int(s->state()), int(NoteSession::State::Clean), 3000);
        QVERIFY(logged("boom") || c.plugins().auditText().contains("failure"));
    }

    // ---------------------------------------------------------------- triggers
    void typing_trigger_applies_as_one_undoable_step_and_restores_the_literal() {
        Lib l;
        l.write("a.md", "# T\n\nhello world\n");
        AppController c(accepting(l));
        c.start(Action::Background);
        QVERIFY(c.plugins().install(fx("hello"), nullptr, nullptr).ok);
        auto *s = c.openSticky("a.md");
        auto *ed = s->editor();
        QTextEdit *v = ed->visualEdit();
        QVERIFY(ed->hasTriggerHandler());
        v->moveCursor(QTextCursor::End);
        typeText(s, " ::date");
        QVERIFY2(plain(s).contains("hello world 2026-10-01"), qPrintable(plain(s)));
        QVERIFY(!plain(s).contains("::date"));
        QVERIFY(ed->undo());                                                   // one step: back to the literal the user typed
        QVERIFY2(plain(s).contains("hello world ::date"), qPrintable(plain(s)));
        QVERIFY(!plain(s).contains("2026-10-01"));
        QVERIFY(ed->redo());
        QVERIFY(plain(s).contains("hello world 2026-10-01"));
        // a setting-driven trigger runs the plugin's Lua for the right token only
        v->moveCursor(QTextCursor::End);
        typeText(s, " ::hello");
        QVERIFY2(plain(s).contains("2026-10-01 Hello"), qPrintable(plain(s)));
        // token boundary: not at the end of a longer word
        v->moveCursor(QTextCursor::End);
        typeText(s, " x::date");
        QVERIFY(plain(s).endsWith("x::date"));
        // not in the middle of a token that merely contains it, not while a selection exists
        typeText(s, "s");
        QVERIFY(plain(s).endsWith("x::dates"));
    }

    void typing_trigger_never_fires_in_code_or_during_ime_composition_or_in_source_mode_fences() {
        Lib l;
        l.write("code.md", "# T\n\nplain\n\n```\ncode\n```\n\nand `inl` end\n");
        AppController c(accepting(l));
        c.start(Action::Background);
        QVERIFY(c.plugins().install(fx("hello"), nullptr, nullptr).ok);
        auto *s = c.openSticky("code.md");
        auto *ed = s->editor();
        QTextEdit *v = ed->visualEdit();
        // fenced code block
        QTextCursor cur = v->document()->find("code");
        QVERIFY(!cur.isNull());
        cur.movePosition(QTextCursor::EndOfBlock);
        v->setTextCursor(cur);
        typeText(s, " ::date");
        QVERIFY2(plain(s).contains("code ::date"), qPrintable(plain(s)));
        // inline code span
        cur = v->document()->find("inl");
        v->setTextCursor(cur);
        typeText(s, " ::date");
        QVERIFY2(plain(s).contains("::date end") || plain(s).contains("inl ::date"), qPrintable(plain(s)));
        QCOMPARE(plain(s).count("::date"), 2);
        QVERIFY(!plain(s).contains("2026-10-01"));
        // IME composition in progress: the literal stays
        v->moveCursor(QTextCursor::End);
        QInputMethodEvent ime("z", {});
        QApplication::sendEvent(v, &ime);
        typeText(s, " ::date");
        QVERIFY(!plain(s).contains("2026-10-01"));
        QInputMethodEvent done;
        done.setCommitString("");
        QApplication::sendEvent(v, &done);
        // source mode: works outside fences, not inside
        QVERIFY(ed->setMode(Mode::Source));
        QPlainTextEdit *src = ed->sourceEdit();
        src->moveCursor(QTextCursor::End);
        QTest::keyClick(src, Qt::Key_Return);
        typeText(s, "after ::date");
        QVERIFY2(src->toPlainText().contains("after 2026-10-01"), qPrintable(src->toPlainText()));
        QTextCursor sc = src->textCursor();
        sc.setPosition(src->toPlainText().indexOf("code") + 4);
        src->setTextCursor(sc);
        typeText(s, " ::date");
        QVERIFY2(src->toPlainText().contains("code ::date"), qPrintable(src->toPlainText()));
    }

    // ---------------------------------------------------------------- registry: menus, toolbar, palette, keys
    void toolbar_menu_and_tools_registration_without_reflow() {
        Lib l;
        const QString src = l.dir.filePath("src");
        mk(src, "tools", {}, R"(hn.menu_item{ id = "t", title = "Tool item", where = "tools", run = function() hn.log("TOOLRUN") end })", {}, "Tools");
        l.write("a.md", "# T\n\nhello world\n");
        AppController c(accepting(l));
        c.start(Action::Background);
        auto *s = c.openSticky("a.md");
        auto *tb = s->toolbar();
        const QSize hint = tb->sizeHint();
        const int h = tb->height(), nMore = tb->moreMenu()->actions().size();
        const auto kids = tb->findChildren<QWidget *>().size();
        QVERIFY(c.plugins().install(fx("hello"), nullptr, nullptr).ok);
        QVERIFY(c.plugins().install(src + "/tools", nullptr, nullptr).ok);
        QCOMPARE(tb->sizeHint(), hint);                                        // the strip itself never changes
        QCOMPARE(tb->height(), h);
        QCOMPARE(tb->findChildren<QWidget *>().size(), kids);
        const auto acts = tb->moreMenu()->actions();
        QVERIFY(acts.size() > nMore);
        QAction *plug = nullptr;
        bool sectionSeen = false;
        for (auto *a : acts) {
            if (a->isSeparator() && a->text() == "PLUGINS") sectionSeen = true;
            if (a->property("hnPlugin").toString() == "hello:tb") plug = a;
        }
        QVERIFY(sectionSeen);
        QVERIFY(plug);
        QVERIFY(plug->text().contains("Toolbar"));
        QVERIFY(plug->isEnabled());
        plug->trigger();
        QVERIFY(logged("toolbar"));
        s->editor()->setMode(Mode::Source);                                    // plugin actions stay usable in source mode
        QVERIFY(plug->isEnabled());
        s->editor()->setMode(Mode::Visual);
        // note right-click menu: 'Plugins' submenu with commands and note menu items
        QMenu menu;
        c.populateNoteMenu(&menu, "a.md", false);
        QMenu *pm = nullptr;
        for (auto *a : menu.actions()) if (a->menu() && a->text() == "Plugins") pm = a->menu();
        QVERIFY(pm);
        QStringList texts;
        for (auto *a : pm->actions()) texts << a->text();
        QVERIFY2(texts.join("|").contains("Hello: Shout selection"), qPrintable(texts.join("|")));
        QVERIFY(texts.join("|").contains("Hello: Menu"));
        for (auto *a : pm->actions()) if (a->text().contains("Hello: Menu")) a->trigger();
        QVERIFY(logged("menu"));
        // tools items
        QMenu tools;
        c.populateToolsMenu(&tools, s);
        QVERIFY(!tools.actions().isEmpty());
        QMenu *tm = nullptr;
        for (auto *a : tools.actions()) if (a->menu()) tm = a->menu();
        QVERIFY(tm && tm->actions().first()->text() == "Tools: Tool item");
        tm->actions().first()->trigger();
        QVERIFY(logged("TOOLRUN"));
        // disabling removes it all again
        QVERIFY(c.plugins().manager()->disable("hello"));
        for (auto *a : tb->moreMenu()->actions()) QVERIFY(a->property("hnPlugin").toString() != "hello:tb");
    }

    void palette_fuzzy_filter_keyboard_and_plugin_names() {
        QVERIFY(CommandPalette::fuzzyScore("shout", "Shout selection") > CommandPalette::fuzzyScore("shn", "Shout selection"));
        QVERIFY(CommandPalette::fuzzyScore("xyz", "Shout selection") < 0);
        QVERIFY(CommandPalette::fuzzyScore("ss", "Shout selection") >= 0);          // subsequence
        QVERIFY(CommandPalette::fuzzyScore("SEL", "Shout selection") > 0);          // case-insensitive
        QVERIFY(CommandPalette::fuzzyScore("sel", "shout selection") > CommandPalette::fuzzyScore("sel", "preselected stuff"));   // word start wins
        Lib l;
        l.write("a.md", "# T\n\nhello world\n");
        AppController c(accepting(l));
        c.start(Action::Background);
        QVERIFY(c.plugins().install(fx("hello"), nullptr, nullptr).ok);
        auto *s = c.openSticky("a.md");
        auto *w = c.stickyOf("a.md");
        w->show();
        QVERIFY(QTest::qWaitForWindowExposed(w));
        s->editor()->visualEdit()->setTextCursor(s->editor()->visualEdit()->document()->find("hello"));
        w->activateWindow();
        QTest::keyClick(w, Qt::Key_P, Qt::ControlModifier | Qt::ShiftModifier);   // Ctrl+Shift+P
        auto *pal = CommandPalette::openOn(w);
        QVERIFY(pal);
        QVERIFY(pal->visibleIds().size() >= 6);                                      // hello registers 4 commands + toolbar + menu
        pal->setFilter("shout");
        QCOMPARE(pal->visibleIds().size(), 1);
        pal->setFilter("");
        QVERIFY(pal->visibleIds().size() >= 6);
        pal->setFilter("cnt");
        QVERIFY(pal->visibleIds().size() >= 1);
        pal->setFilter("zzzz");
        QVERIFY(pal->visibleIds().isEmpty());
        pal->setFilter("sho");
        QVERIFY(!pal->visibleIds().isEmpty());
        QTest::keyClick(pal->input(), Qt::Key_Down);                                  // keyboard only: down, up, enter
        QTest::keyClick(pal->input(), Qt::Key_Up);
        QTest::keyClick(pal->input(), Qt::Key_Return);
        QVERIFY(!CommandPalette::openOn(w) || !CommandPalette::openOn(w)->isVisible());
        QTRY_VERIFY(plain(s).contains("HELLO!"));                                     // the chosen command ran
        // Esc closes without running anything, a second Ctrl+Shift+P toggles
        QTest::keyClick(w, Qt::Key_P, Qt::ControlModifier | Qt::ShiftModifier);
        pal = CommandPalette::openOn(w);
        QVERIFY(pal);
        const QString after = plain(s);
        QTest::keyClick(pal->input(), Qt::Key_Escape);
        QTest::qWait(50);
        QVERIFY(!CommandPalette::openOn(w));
        QCOMPARE(plain(s), after);
        // without any plugin command the palette says so
        QVERIFY(c.plugins().manager()->disable("hello"));
        QTest::keyClick(w, Qt::Key_P, Qt::ControlModifier | Qt::ShiftModifier);
        pal = CommandPalette::openOn(w);
        QVERIFY(pal);
        QVERIFY(pal->visibleIds().isEmpty());
        pal->dismiss();
    }

    void plugin_keys_use_the_keybinding_system_with_conflict_rules() {
        Lib l;
        const QString src = l.dir.filePath("src");
        mk(src, "keys", {"note.edit"}, R"(
hn.command{ id = "go", title = "Go", key = "Ctrl+Alt+U", run = function() hn.note.insert("[go]") end }
hn.command{ id = "plain", title = "Plain", key = "U", run = function() end }
hn.command{ id = "steal", title = "Steal", key = "Ctrl+B", run = function() end }
)", {}, "Keys");
        l.write("a.md", "# T\n\nhello\n");
        AppController c(accepting(l));
        c.start(Action::Background);
        QVERIFY(c.plugins().install(src + "/keys", nullptr, nullptr).ok);
        const auto keys = c.plugins().pluginKeys();
        QCOMPARE(keys.value("plugin:keys:go"), QKeySequence("Ctrl+Alt+U"));
        QVERIFY(!keys.contains("plugin:keys:plain"));                                // a plugin cannot take plain typing keys
        QVERIFY(!keys.contains("plugin:keys:steal"));                                // built-in bindings win
        auto *s = c.openSticky("a.md");
        auto *w = c.stickyOf("a.md");
        w->show();
        QVERIFY(QTest::qWaitForWindowExposed(w));
        w->activateWindow();
        s->editor()->visualEdit()->moveCursor(QTextCursor::End);
        QTest::keyClick(w, Qt::Key_U, Qt::ControlModifier | Qt::AltModifier);
        QTRY_VERIFY(plain(s).contains("[go]"));
        // cheat sheet shows it
        c.showShortcuts(w);
        auto *sheet = ui::ShortcutSheet::openOn(w);
        QVERIFY(sheet);
        bool found = false, palette = false;
        for (const auto &r : sheet->rows()) {
            if (r.label == "Keys: Go") { found = true; QCOMPARE(r.group, QString("PLUGINS")); }
            if (r.label == "Command palette") palette = true;
        }
        QVERIFY(found);
        QVERIFY(palette);
        c.showShortcuts(w);
        // a user override in config wins over the plugin default
        auto st = c.settings();
        st.keybindings["plugin:keys:go"] = QKeySequence("Ctrl+Alt+J");
        c.applySettings(st, c.prefs());
        QCOMPARE(c.plugins().pluginKeys().value("plugin:keys:go"), QKeySequence("Ctrl+Alt+J"));
    }

    // ---------------------------------------------------------------- legacy mods
    void legacy_mods_are_listed_as_native_plugins_and_wait_for_approval() {
        Lib l;
        const QString so = QString(HN_EXAMPLE_MOD_DIR) + "/libuppercase_selection.so";
        if (!QFile::exists(so)) QSKIP("example mod not built");
        const QString modDir = l.dir.filePath("mods/uppercase-selection");
        QDir().mkpath(modDir);
        QVERIFY(QFile::copy(so, modDir + "/libuppercase_selection.so"));
        QVERIFY(QFile::copy(QString(HN_EXAMPLE_MOD_SRC) + "/mod.json", modDir + "/mod.json"));
        QDir().mkpath(l.dir.filePath("cfg"));
        QFile en(l.dir.filePath("cfg/mods-enabled.json"));
        QVERIFY(en.open(QIODevice::WriteOnly));
        en.write(R"({"version":1,"enabled":["uppercase-selection"]})");
        en.close();
        AppController c(l.opts());
        c.start(Action::Background);
        QVERIFY(!c.plugins().active());                                              // nothing enabled in the new model: no manager yet
        QCOMPARE(c.plugins().legacyModsAwaitingApproval(), 1);
        auto *m = c.plugins().manager();
        const auto info = m->info("uppercase-selection");
        QCOMPARE(int(info.manifest.tier), int(Tier::Native));
        QCOMPARE(info.manifest.permissions, QStringList({"native"}));
        QCOMPARE(info.status, Status::NeedsConsent);                                 // the old list is not consent
        QVERIFY(QFile::exists(l.dir.filePath("plugins/uppercase-selection/plugin.json")));
        QVERIFY(QFile::exists(modDir + "/mod.json"));                                // the original mod is left alone
        QVERIFY(c.plugins().native()->activeCount() == 0);                           // no library loaded before approval
        ConsentRequest seen;
        c.plugins().hooks().consent = [&](const ConsentRequest &r) { seen = r; return true; };
        QVERIFY(c.plugins().enable("uppercase-selection", nullptr));
        QVERIFY(seen.native);
        QCOMPARE(c.plugins().legacyModsAwaitingApproval(), 0);
        // removing a migrated plugin does not bring it back on the next scan
        QVERIFY(m->remove("uppercase-selection"));
        m->scan();
        QVERIFY(!QFile::exists(l.dir.filePath("plugins/uppercase-selection")));
    }

    // ---------------------------------------------------------------- command line
    void cli_new_check_pack_install_headless() {
        QTemporaryDir d;
        QDir().mkpath(d.path() + "/home");
        QString out, err;
        QCOMPARE(cli(d.path(), {"--new-plugin", "Demo Notes"}, &out, &err), 0);
        QVERIFY2(out.contains("demo-notes"), qPrintable(out + err));
        QVERIFY(QFile::exists(d.path() + "/demo-notes/plugin.json"));
        QVERIFY(QFile::exists(d.path() + "/demo-notes/main.lua"));
        QVERIFY(cli(d.path(), {"--new-plugin", "Demo Notes"}, &out, &err) != 0);     // refuses to overwrite
        QCOMPARE(cli(d.path(), {"--check-plugin", "demo-notes"}, &out, &err), 0);
        QVERIFY2(out.startsWith("OK:") && out.contains("SHA-256"), qPrintable(out));
        QCOMPARE(cli(d.path(), {"--pack-plugin", "demo-notes"}, &out, &err), 0);
        QVERIFY(QFile::exists(d.path() + "/demo-notes-0.1.0.hnplugin"));
        // a broken plugin fails check and pack with a readable error
        QFile pj(d.path() + "/demo-notes/plugin.json");
        QVERIFY(pj.open(QIODevice::ReadWrite));
        QByteArray j = pj.readAll();
        j.replace("\"note.read\"", "\"bogus.perm\"");
        pj.seek(0); pj.resize(0); pj.write(j); pj.close();
        QVERIFY(cli(d.path(), {"--check-plugin", "demo-notes"}, &out, &err) != 0);
        QVERIFY2(err.contains("bogus.perm"), qPrintable(err));
        QVERIFY(cli(d.path(), {"--pack-plugin", "demo-notes"}, &out, &err) != 0);
        QVERIFY(cli(d.path(), {"--check-plugin", d.path() + "/nowhere"}, &out, &err) != 0);
        // --install-plugin: warning + hash + permissions are printed; wrong confirmation installs nothing
        QCOMPARE(cli(d.path(), {"--new-plugin", "Second"}, &out, &err), 0);
        QVERIFY(cli(d.path(), {"--install-plugin", "second"}, &out, &err, "nope\n") != 0);
        QVERIFY2(out.contains("Third-party plugins are not reviewed by the Hyprnotes project. A plugin can read or change your notes and, depending on its permissions, send data over the network. Only install plugins from sources you trust."), qPrintable(out));
        QVERIFY(out.contains("SHA-256") && out.contains("note.read") && out.contains("Cancelled"));
        QVERIFY(!QFile::exists(d.path() + "/data/plugins/second"));
        QVERIFY(cli(d.path(), {"--install-plugin", "second"}, &out, &err, QByteArray()) != 0);   // EOF = cancel
        QVERIFY(!QFile::exists(d.path() + "/data/plugins/second"));
        QCOMPARE(cli(d.path(), {"--install-plugin", "second"}, &out, &err, "second\n"), 0);       // typed the id
        QVERIFY2(QFile::exists(d.path() + "/data/plugins/second/plugin.json"), qPrintable(out + err));
        {
            hn::plugins::TrustStore ts(d.path() + "/state/plugins.json");
            ts.load();
            QVERIFY(ts.record("second").hasConsent);
            QVERIFY(ts.record("second").enabled);
        }
        QFile audit(d.path() + "/state/plugins-audit.jsonl");
        QVERIFY(audit.open(QIODevice::ReadOnly));
        const QByteArray log = audit.readAll();
        QVERIFY(log.contains("\"install\"") && log.contains("\"consent\"") && log.contains("\"enable\""));
        QVERIFY(cli(d.path(), {"--install-plugin", "second"}, &out, &err, "second\n") != 0);       // already installed
        // archive + --yes-i-trust-this-plugin skips the prompt (nothing is read from stdin)
        QCOMPARE(cli(d.path(), {"--new-plugin", "Third"}, &out, &err), 0);
        QCOMPARE(cli(d.path(), {"--pack-plugin", "third"}, &out, &err), 0);
        QCOMPARE(cli(d.path(), {"--install-plugin", "third-0.1.0.hnplugin", "--yes-i-trust-this-plugin"}, &out, &err), 0);
        QVERIFY(QFile::exists(d.path() + "/data/plugins/third/plugin.json"));
        QVERIFY(cli(d.path(), {"--install-plugin", "missing.hnplugin", "--yes-i-trust-this-plugin"}, &out, &err) != 0);
        // option parsing
        QVERIFY(!parseCli({"--yes-i-trust-this-plugin"}).error.isEmpty());
        QVERIFY(!parseCli({"--install-plugin"}).error.isEmpty());
        QVERIFY(!parseCli({"--install-plugin", "a", "--check-plugin", "b"}).error.isEmpty());
        QVERIFY(parseCli({"--install-plugin", "a", "--yes-i-trust-this-plugin"}).trustPlugin);
        QVERIFY(helpText().contains("--install-plugin") && helpText().contains("--pack-plugin"));
    }

    // ---------------------------------------------------------------- consent dialog
    void consent_dialog_contents_warning_delay_and_focus() {
        ConsentRequest r;
        r.id = "demo"; r.name = "Demo plugin"; r.author = "Someone"; r.version = "2.1.0"; r.description = "Does things.";
        r.source = "/home/me/Downloads/demo.hnplugin";
        r.sha256 = QString(64, 'a');
        r.permissions = {"note.read", "network", "notes.write"};
        ConsentDialog d(r);
        d.show();
        QVERIFY(QTest::qWaitForWindowExposed(&d));
        auto *warn = d.findChild<QLabel *>("hnConsentWarningText");
        QVERIFY(warn);
        QCOMPARE(warn->text(), QString("Third-party plugins are not reviewed by the Hyprnotes project. A plugin can read or change your notes and, depending on its permissions, send data over the network. Only install plugins from sources you trust."));
        QVERIFY(warn->isVisible());
        QVERIFY(d.warningPanel()->isVisible());
        for (QWidget *p = d.warningPanel()->parentWidget(); p; p = p->parentWidget()) QVERIFY(!qobject_cast<QScrollArea *>(p));   // never scrolled/collapsed away
        QVERIFY(!d.nativePanel());
        QVERIFY(!d.findChild<QLabel *>("hnConsentNativeText"));
        QCOMPARE(d.hashLabel()->text(), r.sha256);
        QVERIFY(d.hashLabel()->textInteractionFlags() & Qt::TextSelectableByMouse);
        QCOMPARE(d.findChild<QLabel *>("hnConsentSource")->text(), r.source);
        QCOMPARE(d.permissionLabels().size(), 3);
        for (auto *lab : d.permissionLabels()) {
            const QString p = lab->property("permission").toString();
            QVERIFY(lab->text().startsWith(p));
            QVERIFY(lab->text().contains(hn::plugins::permissionDescription(p)));
            QTRY_COMPARE(lab->palette().color(QPalette::WindowText),
                         hn::plugins::isDangerousPermission(p) ? ui::theme().danger : ui::theme().text);
            QVERIFY(!lab->accessibleName().isEmpty());
        }
        QCOMPARE(d.approveButton()->text(), QString("I trust this plugin - Enable"));
        QCOMPARE(d.cancelButton()->text(), QString("Cancel"));
        QVERIFY(!d.approveButton()->accessibleName().isEmpty());
        QVERIFY(!d.cancelButton()->accessibleName().isEmpty());
        QVERIFY(d.cancelButton()->isDefault());
        QVERIFY(!d.approveButton()->isDefault());
        QTRY_VERIFY(d.cancelButton()->hasFocus());                                   // Cancel is the default focus
        // reflex clicking: disabled for two seconds
        QVERIFY(!d.approveButton()->isEnabled());
        QSignalSpy accepted(&d, &QDialog::accepted);
        QTest::mouseClick(d.approveButton(), Qt::LeftButton);
        QTest::keyClick(&d, Qt::Key_Space);
        QCOMPARE(accepted.count(), 0);
        QTest::qWait(1200);
        QVERIFY(!d.approveButton()->isEnabled());                                    // still within the two seconds
        QTRY_VERIFY_WITH_TIMEOUT(d.approveButton()->isEnabled(), 2500);
        QTest::mouseClick(d.approveButton(), Qt::LeftButton);
        QCOMPARE(accepted.count(), 1);
        // Enter and Escape never approve
        ConsentDialog d2(r, nullptr, 0);
        d2.show();
        QVERIFY(QTest::qWaitForWindowExposed(&d2));
        QSignalSpy a2(&d2, &QDialog::accepted), r2(&d2, &QDialog::rejected);
        QTest::keyClick(&d2, Qt::Key_Return);
        QCOMPARE(a2.count(), 0);
        QCOMPARE(r2.count(), 1);
    }

    void consent_dialog_native_plugins_get_the_extra_warning_and_reconsent_notice() {
        ConsentRequest r;
        r.id = "nat"; r.name = "Native thing"; r.author = "A"; r.version = "1.0.0"; r.source = "/x"; r.sha256 = QString(64, 'b');
        r.permissions = {"native"};
        r.native = true;
        r.reconsentReason = "The plugin's files changed since you approved it and it now asks for more permissions.";
        r.newPermissions = {"native"};
        ConsentDialog d(r, nullptr, 0);
        d.show();
        QVERIFY(QTest::qWaitForWindowExposed(&d));
        QVERIFY(d.nativePanel() && d.nativePanel()->isVisible());
        QCOMPARE(d.findChild<QLabel *>("hnConsentNativeText")->text(), QString("This plugin runs native code with full access to your user account. It is not sandboxed."));
        QVERIFY(d.findChild<QLabel *>("hnConsentWarningText")->isVisible());     // the generic warning stays
        auto *reason = d.findChild<QLabel *>("hnConsentReason");
        QVERIFY(reason && reason->text().contains("changed") && reason->text().contains("native"));
    }

    // ---------------------------------------------------------------- Plugins page
    void plugins_page_lists_shows_permissions_settings_and_actions() {
        Lib l;
        l.write("a.md", "# T\n\nhello\n");
        auto o = l.opts();
        bool accept = false;
        o.pluginHooks.consent = [&](const ConsentRequest &) { return accept; };
        o.pluginHooks.confirm = [](const QString &, const QString &) { return true; };
        AppController c(o);
        c.start(Action::Background);
        SettingsDialog dlg(&c);
        dlg.showTab(SettingsDialog::kPluginsTab);
        dlg.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dlg));
        auto *page = dlg.pluginsPage();
        QTRY_VERIFY(c.plugins().active());                                        // the page creates the manager on first show
        QCOMPARE(page->warningLabel()->text(), ConsentDialog::warningText());     // the same warning sits at the top of the page
        QVERIFY(page->warningLabel()->isVisible());
        QCOMPARE(page->list()->count(), 0);
        QVERIFY(page->button("install"));
        QVERIFY(page->installPath(fx("api-probe")));                              // consent declined: installed, needs consent
        QVERIFY(page->installPath(fx("hello")));
        QCOMPARE(page->list()->count(), 2);
        page->select("api-probe");
        QCOMPARE(page->statusText("api-probe"), QString("NEEDS CONSENT"));
        QVERIFY(page->button("enable") && !page->button("disable"));
        // permissions in plain language, dangerous ones flagged
        QCOMPARE(page->permissionLabels().size(), 9);
        QStringList flagged;
        for (auto *lab : page->permissionLabels()) {
            const QString p = lab->property("permission").toString();
            QVERIFY(lab->text().contains(hn::plugins::permissionDescription(p)));
            if (lab->property("danger").toBool()) flagged << p;
            QCOMPARE(lab->property("danger").toBool(), hn::plugins::isDangerousPermission(p));
        }
        flagged.sort();
        QCOMPARE(flagged, QStringList({"clipboard", "network", "notes.write", "theme"}));
        // enabling goes through the consent dialog logic
        accept = true;
        QTest::mouseClick(page->button("enable"), Qt::LeftButton);
        QCOMPARE(page->statusText("api-probe"), QString("ENABLED"));
        QVERIFY(page->button("disable") && !page->button("enable"));
        // settings from hn.setting, only while enabled
        page->select("hello");
        QCOMPARE(page->statusText("hello"), QString("NEEDS CONSENT"));   // installed while the dialog was declined
        QTest::mouseClick(page->button("enable"), Qt::LeftButton);
        QCOMPARE(page->statusText("hello"), QString("ENABLED"));
        QCOMPARE(page->settingEditors().size(), 2);
        auto *loud = qobject_cast<QCheckBox *>(page->settingEditors()[1]);
        QVERIFY(loud);
        QVERIFY(!loud->isChecked());
        loud->setChecked(true);
        QCOMPARE(c.plugins().manager()->host()->setting("hello", "loud").toBool(), true);
        auto *greet = qobject_cast<QLineEdit *>(page->settingEditors()[0]);
        QVERIFY(greet);
        QCOMPARE(greet->text(), QString("Hello"));
        greet->setText("Howdy");
        greet->editingFinished();
        QCOMPARE(c.plugins().manager()->host()->setting("hello", "greeting").toString(), QString("Howdy"));
        // reload (developer), disable, audit log, remove
        QTest::mouseClick(page->button("reload"), Qt::LeftButton);
        QCOMPARE(page->statusText("hello"), QString("ENABLED"));
        QTest::mouseClick(page->button("disable"), Qt::LeftButton);
        QCOMPARE(page->statusText("hello"), QString("DISABLED"));
        QCOMPARE(page->settingEditors().size(), 0);
        page->showAudit("hello");
        auto *audit = dlg.findChild<QDialog *>("hnAuditDialog");
        QVERIFY(audit);
        const QString text = audit->findChild<QPlainTextEdit *>("hnAuditText")->toPlainText();
        QVERIFY2(text.contains("install") && text.contains("consent") && text.contains("disable"), qPrintable(text));
        audit->close();
        QVERIFY(QFile::exists(l.dir.filePath("plugins/hello/plugin.json")));
        QTest::mouseClick(page->button("remove"), Qt::LeftButton);
        QVERIFY(!QFile::exists(l.dir.filePath("plugins/hello")));
        QCOMPARE(page->list()->count(), 1);
        // drag and drop recognises plugin packages and folders only
        QMimeData okFolder, okFile, bad;
        okFolder.setUrls({QUrl::fromLocalFile(fx("hello"))});
        QCOMPARE(AppController::pluginPathFromMime(&okFolder), fx("hello"));
        QTemporaryDir td;
        QFile pk(td.path() + "/x.hnplugin");
        QVERIFY(pk.open(QIODevice::WriteOnly));
        pk.close();
        okFile.setUrls({QUrl::fromLocalFile(pk.fileName())});
        QCOMPARE(AppController::pluginPathFromMime(&okFile), pk.fileName());
        bad.setUrls({QUrl::fromLocalFile(td.path() + "/readme.txt")});
        QVERIFY(AppController::pluginPathFromMime(&bad).isEmpty());
        QMimeData none;
        QVERIFY(AppController::pluginPathFromMime(&none).isEmpty());
    }

    // ---------------------------------------------------------------- screenshots (opt-in)
    void screenshots() {
        const QString out = qEnvironmentVariable("HN_SCREENSHOT_DIR");
        if (out.isEmpty()) QSKIP("HN_SCREENSHOT_DIR not set");
        QDir().mkpath(out);
        for (const char *scheme : {"light", "dark"}) {
            Lib l;
            l.write("a.md", "# Notes\n\nhello\n");
            auto o = l.opts();
            bool accept = false;
            o.pluginHooks.consent = [&](const ConsentRequest &) { return accept; };
            AppController c(o);
            auto st = c.settings(); st.colorScheme = scheme; c.applySettings(st, c.prefs());
            c.start(Action::Background);
            const QString src = l.dir.filePath("src");
            // consent dialogs: a script plugin with dangerous permissions, then a native one
            ConsentRequest r;
            r.id = "api-probe"; r.name = "API probe"; r.author = "Hyprnotes tests"; r.version = "1.0.0";
            r.description = "Calls every hn.* API inside pcall and logs name|ok|error for each.";
            r.source = "/home/me/Downloads/api-probe.hnplugin";
            r.sha256 = "9f2c41a7d0b35e6c8a1f4b27e90d3c5a6b8e1f0472c9d3a5e8b61f7042ac93de";
            r.permissions = {"note.read", "note.edit", "notes.read", "notes.write", "ui", "storage", "clipboard", "network", "theme"};
            ConsentDialog d(r, nullptr, 2000);
            d.show();
            QVERIFY(QTest::qWaitForWindowExposed(&d));
            d.grab();
            QVERIFY(d.grab().save(QString("%1/plugin-consent-%2.png").arg(out, scheme)));
            ConsentRequest n = r;
            n.id = "fullmd"; n.name = "Full markdown"; n.author = "Hyprnotes"; n.version = "0.3.0"; n.description = "Native markdown engine.";
            n.native = true; n.permissions = {"native"}; n.source = "/usr/share/hyprnotes/plugins/fullmd";
            n.reconsentReason = "The plugin's files changed since you approved it. Review it again before it runs.";
            ConsentDialog dn(n, nullptr, 0);
            dn.show();
            QVERIFY(QTest::qWaitForWindowExposed(&dn));
            QVERIFY(dn.grab().save(QString("%1/plugin-consent-native-%2.png").arg(out, scheme)));
            // Plugins page with a mix of statuses
            mk(src, "broken", {}, "this is not lua (", {}, "Broken plugin");
            accept = true;
            QVERIFY(c.plugins().install(fx("hello"), nullptr, nullptr).ok);
            accept = false;
            c.plugins().install(fx("api-probe"), nullptr, nullptr);
            c.plugins().install(src + "/broken", nullptr, nullptr);
            SettingsDialog dlg(&c);
            dlg.showTab(SettingsDialog::kPluginsTab);
            dlg.resize(840, 720);
            dlg.show();
            QVERIFY(QTest::qWaitForWindowExposed(&dlg));
            dlg.pluginsPage()->select("hello");
            QTest::qWait(300);
            QVERIFY(dlg.grab().save(QString("%1/plugins-page-%2.png").arg(out, scheme)));
            dlg.pluginsPage()->select("api-probe");
            QTest::qWait(300);
            QVERIFY(dlg.grab().save(QString("%1/plugins-page-consent-%2.png").arg(out, scheme)));
            dlg.close();
            // command palette over a sticky
            auto *s = c.openSticky("a.md");
            auto *w = c.stickyOf("a.md");
            w->resize(560, 340);
            w->show();
            QVERIFY(QTest::qWaitForWindowExposed(w));
            c.showPalette(w, s);
            auto *pal = CommandPalette::openOn(w);
            QVERIFY(pal);
            pal->setFilter("");
            QTest::qWait(200);
            QVERIFY(w->grab().save(QString("%1/plugin-palette-%2.png").arg(out, scheme)));
            pal->dismiss();
        }
    }
};

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    PluginsAppTest t;
    return QTest::qExec(&t, argc, argv);
}

#include "app_plugins_test.moc"
