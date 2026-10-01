// Read-only checks against the real compositor. Gated: HN_LIVE_HYPR=1.
#include "hn/platform/hyprland_ipc.h"
#include "hn/platform/window_placement.h"
#include <QGuiApplication>
#include <QProcess>
#include <QWidget>
#include <QSignalSpy>
#include <QtTest>
using namespace hn::platform;

class LiveTest : public QObject {
  Q_OBJECT
private slots:
  void initTestCase() { if (qgetenv("HN_LIVE_HYPR") != "1") QSKIP("set HN_LIVE_HYPR=1 to run"); }
  void readOnlyQueries() {
    QVERIFY(HyprPaths::fromEnv().valid());
    HyprlandIpc ipc; ipc.start();
    QTRY_VERIFY_WITH_TIMEOUT(ipc.connected(), 2000);
    bool ok = false; QVector<HyprClient> cl; Reply rep;
    QElapsedTimer t; t.start();
    ipc.queryClients([&](bool o, auto v, const Reply& r) { ok = o; cl = v; rep = r; });
    QTRY_VERIFY(ok || rep.status != Reply::Status::Error);
    qInfo() << "clients" << cl.size() << "in" << t.elapsed() << "ms";
    QVERIFY(ok);
    // cross-check count against hyprctl
    QProcess p; p.start("hyprctl", {"-j", "clients"}); QVERIFY(p.waitForFinished(3000));
    QCOMPARE(HyprlandIpc::parseClients(p.readAllStandardOutput(), 0).size(), cl.size());
    for (const auto& c : cl) QVERIFY2(c.ref.address.startsWith("0x") && c.rect.width() > 0, qPrintable(c.ref.address));
    bool mok = false; QVector<HyprMonitor> mons;
    ipc.queryMonitors([&](bool o, auto v, const Reply&) { mok = o; mons = v; });
    QTRY_VERIFY(mok); QVERIFY(!mons.isEmpty()); QVERIFY(mons[0].workArea().width() > 0);
    qInfo() << "monitor" << mons[0].name << mons[0].rect << "work" << mons[0].workArea() << "scale" << mons[0].scale;
    bool wok = false; QVector<HyprWorkspace> ws;
    ipc.queryWorkspaces([&](bool o, auto v, const Reply&) { wok = o; ws = v; });
    QTRY_VERIFY(wok); QVERIFY(!ws.isEmpty());
    bool aok = false, adone = false; HyprClient aw;
    ipc.queryActiveWindow([&](bool o, HyprClient c, const Reply&) { aok = o; aw = c; adone = true; });
    QTRY_VERIFY(adone);
    if (aok) qInfo() << "active" << aw.cls << aw.rect;
    // a bogus query must not hang anything
    bool done = false; ipc.request("j/nonexistentcommand", [&](const Reply&) { done = true; });
    QTRY_VERIFY_WITH_TIMEOUT(done, 1000);
  }

  // Creates ONE window of our own (needs QT_QPA_PLATFORM=wayland and HN_LIVE_HYPR_WINDOW=1) and
  // drives only that window: identity lookup, float/resize/move/pin round trip.
  void ownWindowRoundTrip() {
    if (qgetenv("HN_LIVE_HYPR_WINDOW") != "1" || QGuiApplication::platformName() != "wayland")
      QSKIP("set HN_LIVE_HYPR_WINDOW=1 and QT_QPA_PLATFORM=wayland");
    QGuiApplication::setDesktopFileName("hyprnotes");
    HyprlandIpc ipc; ipc.start(); QTRY_VERIFY_WITH_TIMEOUT(ipc.connected(), 2000);
    QWidget w; const auto id = WindowIdentity::create(Role::Sticky);
    applyIdentity(&w, id); w.resize(500, 400); w.show();
    HyprClient me; bool found = false;
    auto look = [&] {
      bool done = false; QVector<HyprClient> cl;
      ipc.queryClients([&](bool, auto v, const Reply&) { cl = v; done = true; });
      QTRY_VERIFY_WITH_TIMEOUT(done, 1000);
      auto m = resolveClients(cl, QCoreApplication::applicationPid(), "hyprnotes");
      found = m.contains(id.token); if (found) me = m[id.token];
    };
    for (int i = 0; i < 20 && !found; ++i) { look(); if (!found) QTest::qWait(100); }
    QVERIFY(found);
    qInfo() << "own window" << me.ref.address << "floating" << me.floating << me.rect << "initialTitle" << me.initialTitle;
    w.setWindowTitle("renamed after map");       // lookup must still work
    QTest::qWait(150); look(); QVERIFY(found); QCOMPARE(me.title, QString("renamed after map"));
    auto step = [&](std::function<void(HyprlandIpc::ReplyFn)> f) {
      bool done = false; Reply r; f([&](const Reply& x) { r = x; done = true; });
      QTRY_VERIFY_WITH_TIMEOUT(done, 1500); qInfo() << "reply" << r.data; QVERIFY(r.dispatchOk()); };
    step([&](auto cb) { ipc.setFloating(me.ref, true, cb); });
    step([&](auto cb) { ipc.resizeExact(me.ref, 360, 300, cb); });
    step([&](auto cb) { ipc.moveExact(me.ref, 100, 100, cb); });
    step([&](auto cb) { ipc.setPinned(me.ref, true, cb); });
    QTest::qWait(300); look();
    qInfo() << "after" << me.floating << me.rect << "pinned" << me.pinned;
    QVERIFY(me.floating); QVERIFY(me.pinned); QCOMPARE(me.rect.size(), QSize(360, 300)); QCOMPARE(me.rect.topLeft(), QPoint(100, 100));
    step([&](auto cb) { ipc.setPinned(me.ref, false, cb); });
    QTest::qWait(200); look(); QVERIFY(!me.pinned);
  }
};
QTEST_MAIN(LiveTest)
#include "platform_test_live.moc"
