#include "platform_fake_hyprland.h"
#include "hn/platform/window_placement.h"
#include <QTemporaryDir>
#include <QWidget>
#include <QtTest>
using namespace hn::platform;

static HyprClient cl(const QString& addr, qint64 pid, const QString& initTitle, const QString& title, const QString& cls = "hyprnotes") {
  HyprClient c; c.ref = {addr, 1}; c.pid = pid; c.initialTitle = initTitle; c.title = title; c.cls = cls; c.initialClass = cls; return c;
}

class WindowTest : public QObject {
  Q_OBJECT
private slots:
  void identityRoundTrip() {
    auto a = WindowIdentity::create(Role::Sticky), b = WindowIdentity::create(Role::Sticky);
    QVERIFY(a.token != b.token); QCOMPARE(a.token.size(), 16);
    QCOMPARE(a.initialTitle(), QString("hyprnotes-sticky:") + a.token);
    auto p = WindowIdentity::parseInitialTitle(a.initialTitle());
    QVERIFY(p); QCOMPARE(p->token, a.token); QVERIFY(p->role == Role::Sticky);
    QVERIFY(WindowIdentity::parseInitialTitle(WindowIdentity::create(Role::Organizer).initialTitle())->role == Role::Organizer);
    QVERIFY(!WindowIdentity::parseInitialTitle("hyprnotes-sticky:zz"));
    QVERIFY(!WindowIdentity::parseInitialTitle("My note"));
    QVERIFY(!WindowIdentity::parseInitialTitle("evil:0123456789abcdef"));
    {   // trailing suffix after the token is tolerated; a longer/different token is not
      auto s = WindowIdentity::parseInitialTitle(QString::fromUtf8("hyprnotes-sticky:0123456789abcdef \u2014 Hyprnotes"));
      QVERIFY(s && s->role == Role::Sticky && s->token == "0123456789abcdef");
      QVERIFY(WindowIdentity::parseInitialTitle("hyprnotes-organizer:0123456789abcdef - x"));
      QVERIFY(!WindowIdentity::parseInitialTitle("hyprnotes-sticky:0123456789abcdef0"));
      QVERIFY(!WindowIdentity::parseInitialTitle("hyprnotes-sticky:0123456789abcde \u2014 Hyprnotes"));
    }
  }
  void widgetCarriesTokenAndTitleMayChange() {
    QWidget w; auto id = WindowIdentity::create(Role::Sticky);
    applyIdentity(&w, id);
    QCOMPARE(w.objectName(), id.token); QCOMPARE(w.property("hn.role").toString(), QString("hyprnotes-sticky"));
    QCOMPARE(w.windowTitle(), id.initialTitle());
    w.setWindowTitle("Shopping"); QCOMPARE(w.property("hn.token").toString(), id.token);
  }
  void mappingSurvivesRenameDuplicatesAndPopOut() {
    const qint64 pid = 4242;
    auto s1 = WindowIdentity::create(Role::Sticky), s2 = WindowIdentity::create(Role::Sticky), org = WindowIdentity::create(Role::Organizer);
    WindowRegistry reg; reg.bind(s1.token, "notes/a.md"); reg.bind(s2.token, "notes/b.md");
    // duplicate titles "Todo", both renamed since map; other process's identical-looking window ignored
    QVector<HyprClient> all{cl("0x1", pid, s1.initialTitle(), "Todo"), cl("0x2", pid, s2.initialTitle(), "Todo"),
      cl("0x3", pid, org.initialTitle(), "Hyprnotes"), cl("0x4", 999, s1.initialTitle(), "Todo"), cl("0x5", pid, "kitty", "x", "kitty")};
    auto m = resolveClients(all, pid, "hyprnotes");
    QCOMPARE(m.size(), 3);
    QCOMPARE(m[s1.token].ref.address, QString("0x1")); QCOMPARE(m[s2.token].ref.address, QString("0x2"));
    QCOMPARE(reg.noteFor(s1.token), QString("notes/a.md"));
    // title changes: lookup is title independent
    all[0].title = "Renamed!"; all[1].title = "Renamed!";
    m = resolveClients(all, pid, "hyprnotes");
    QCOMPARE(m[s1.token].ref.address, QString("0x1")); QCOMPARE(m[s2.token].ref.address, QString("0x2"));
    // pop-out: note a moves to a new window with a new token; old window gone
    auto s3 = WindowIdentity::create(Role::Sticky);
    reg.rebind(s1.token, s3.token);
    all[0] = cl("0x9", pid, s3.initialTitle(), "Renamed!");
    m = resolveClients(all, pid, "hyprnotes");
    QCOMPARE(reg.noteFor(s3.token), QString("notes/a.md")); QCOMPARE(reg.noteFor(s1.token), QString());
    QCOMPARE(m[s3.token].ref.address, QString("0x9")); QVERIFY(!m.contains(s1.token));
    QCOMPARE(reg.tokensFor("notes/a.md"), QStringList{s3.token});
  }
  void clampRules() {
    const QRect work(0, 38, 1920, 1042);
    QCOMPARE(clampToWorkArea(QRect(1800, 1000, 360, 300), work), QRect(1560, 780, 360, 300));
    QCOMPARE(clampToWorkArea(QRect(-500, -500, 360, 300), work), QRect(0, 38, 360, 300));
    QCOMPARE(clampToWorkArea(QRect(10, 50, 5000, 5000), work), QRect(0, 38, 1920, 1042));
    QCOMPARE(clampToWorkArea(QRect(100, 100, 360, 300), work), QRect(100, 100, 360, 300));
  }
  void placementMonitorRemovalScaleAndWorkspaces() {
    HyprMonitor a; a.id = 0; a.name = "eDP-1"; a.rect = QRect(0, 0, 1920, 1080); a.reserved = QMargins(0, 38, 0, 0);
    HyprMonitor b; b.id = 1; b.name = "HDMI-A-1"; b.rect = QRect(1920, 0, 1920, 1080);
    WindowState s; s.monitor = "HDMI-A-1"; s.geometry = QRect(3000, 500, 360, 300); s.workspace = 7; s.mode = WorkspaceMode::AllWorkspaces;
    auto p = planPlacement(s, {a, b}, {{7, "7", 1, 1}});
    QVERIFY(!p.monitorMissing); QCOMPARE(p.rect, s.geometry); QCOMPARE(p.workspace, std::optional<int>(7)); QVERIFY(p.pinned);
    // monitor removed: falls back, clamped into remaining work area; workspace 7 gone -> not restored
    p = planPlacement(s, {a}, {{1, "1", 0, 0}});
    QVERIFY(p.monitorMissing); QVERIFY(a.workArea().contains(p.rect)); QVERIFY(!p.workspace);
    QCOMPARE(p.rect.size(), QSize(360, 300));
    // scale change: same monitor now 2x => logical work area half; window clamps
    HyprMonitor a2 = a; a2.scale = 2; a2.rect = QRect(0, 0, 960, 540);
    s.monitor = "eDP-1"; s.scale = 1; s.geometry = QRect(1500, 800, 360, 300);
    p = planPlacement(s, {a2}, {});
    QVERIFY(a2.workArea().contains(p.rect));
    // tiny monitor: size shrinks
    HyprMonitor t = a; t.rect = QRect(0, 0, 300, 200); t.reserved = {};
    p = planPlacement(s, {t}, {}); QCOMPARE(p.rect, QRect(0, 0, 300, 200));
    // no monitors known: pass through
    QCOMPARE(planPlacement(s, {}, {}).rect, s.geometry);
  }
  void sessionJsonAtomicAndTolerant() {
    QTemporaryDir d; const QString path = d.filePath("state/hyprnotes/session.json");
    SessionState s;
    WindowState w; w.noteKey = "notes/a.md"; w.geometry = QRect(1, 2, 360, 300); w.monitor = "eDP-1"; w.workspace = 3; w.scale = 1.5; w.mode = WorkspaceMode::AllWorkspaces;
    s.windows << w;
    QString err; QVERIFY2(saveSession(path, s, &err), qPrintable(err));
    QCOMPARE(QDir(QFileInfo(path).absolutePath()).entryList(QDir::Files).size(), 1);   // no temp leftovers
    bool ok = false; auto r = loadSession(path, &ok);
    QVERIFY(ok); QCOMPARE(r.windows.size(), 1);
    QCOMPARE(r.windows[0].geometry, w.geometry); QCOMPARE(r.windows[0].noteKey, w.noteKey);
    QCOMPARE(r.windows[0].scale, 1.5); QVERIFY(r.windows[0].mode == WorkspaceMode::AllWorkspaces);
    // overwrite keeps previous valid content if commit never happens: simulate corrupt file
    QFile f(path); QVERIFY(f.open(QIODevice::WriteOnly)); f.write("{ not json"); f.close();
    loadSession(path, &ok); QVERIFY(!ok);
    loadSession(d.filePath("missing.json"), &ok); QVERIFY(!ok);
    fromJson(R"({"version":1,"windows":[{"w":-5},{"x":"bad"},3]})", &ok); QVERIFY(ok);   // tolerant of junk entries
    qputenv("XDG_STATE_HOME", "/x/state");
    QCOMPARE(defaultSessionPath(), QString("/x/state/hyprnotes/session.json"));
  }
  void keepAboveNotClaimed() { static_assert(!supportsKeepAbove()); QVERIFY(!supportsKeepAbove()); }
  void applyPlacementIsSequentialIdempotentSets() {
    FakeHyprland fake; HyprlandIpc ipc(fake.paths()); ipc.start(); QTRY_VERIFY(ipc.connected());
    PlacementPlan p; p.rect = QRect(10, 20, 360, 300); p.workspace = 3; p.pinned = true;
    bool done = false, all = false;
    applyPlacement(ipc, {"0xabc", ipc.generation()}, p, [&](bool ok) { done = true; all = ok; });
    QTRY_VERIFY(done); QVERIFY(all);
    QCOMPARE(fake.received.size(), 5);
    const char* want[] = {"float(", "move({window='address:0xabc',workspace=3", "resize(", "move({window='address:0xabc',x=10", "pin("};
    for (int i = 0; i < 5; ++i) QVERIFY2(fake.received[i].contains(want[i]), qPrintable(fake.received[i]));
    // stale generation: plan fails without issuing anything
    done = false; fake.received.clear();
    applyPlacement(ipc, {"0xabc", ipc.generation() + 1}, p, [&](bool ok) { done = true; all = ok; });
    QTRY_VERIFY(done); QVERIFY(!all); QCOMPARE(fake.received.size(), 0);
  }
  void setWorkspaceModeUsesPin() {
    FakeHyprland fake; HyprlandIpc ipc(fake.paths()); ipc.start(); QTRY_VERIFY(ipc.connected());
    bool d = false;
    setWorkspaceMode(ipc, {"0xabc", ipc.generation()}, WorkspaceMode::AllWorkspaces, [&](const Reply&) { d = true; });
    QTRY_VERIFY(d); QVERIFY(fake.received[0].contains("pin(") && fake.received[0].contains("action='on'"));
  }
};
QTEST_MAIN(WindowTest)
#include "platform_test_window.moc"
