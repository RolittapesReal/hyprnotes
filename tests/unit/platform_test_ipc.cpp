#include "platform_fake_hyprland.h"
#include <QSignalSpy>
#include <QtTest>
using namespace hn::platform;
#define QTRY_VERIFY_WITH_TIMEOUT_STATIC(c, t) do { QElapsedTimer tt; tt.start(); while (!(c) && tt.elapsed() < (t)) QTest::qWait(5); } while (0)

class IpcTest : public QObject {
  Q_OBJECT
  static Reply sync(const std::function<void(HyprlandIpc::ReplyFn)>& f) {
    Reply out; bool done = false;
    f([&](const Reply& r) { out = r; done = true; });
    QTRY_VERIFY_WITH_TIMEOUT_STATIC(done, 3000);
    return out;
  }
private slots:
  void parsing() {
    bool ok = false;
    auto cl = HyprlandIpc::parseClients(kClientsJson, 7, &ok);
    QVERIFY(ok); QCOMPARE(cl.size(), 2);
    QCOMPARE(cl[0].ref.address, QString("0x55738da49dd0")); QCOMPARE(cl[0].ref.generation, quint64(7));
    QCOMPARE(cl[0].rect, QRect(336, -782, 1248, 702)); QCOMPARE(cl[0].workspaceId, 2);
    QCOMPARE(cl[0].initialTitle, QString("hyprnotes-sticky:00000000000000aa")); QVERIFY(cl[1].pinned);
    auto m = HyprlandIpc::parseMonitors(kMonitorsJson, &ok);
    QVERIFY(ok); QCOMPARE(m[0].rect, QRect(0, 0, 1920, 1080));   // 3840/scale 2
    QCOMPARE(m[0].workArea(), QRect(0, 38, 1920, 1042));
    HyprlandIpc::parseClients("garbage", 1, &ok); QVERIFY(!ok);
  }
  void queriesAndDispatchAgainstFake() {
    FakeHyprland fake;
    fake.handler = [](const QByteArray& c) -> QByteArray {
      if (c == "j/clients") return kClientsJson;
      if (c == "j/monitors") return kMonitorsJson;
      if (c == "j/activewindow") return "{}";
      if (c == "j/workspaces") return R"([{"id":2,"name":"2","monitorID":0,"windows":1}])";
      if (c.contains("fail")) return "error: nope";
      return "ok";
    };
    HyprlandIpc ipc(fake.paths());
    ipc.start();
    QSignalSpy conn(&ipc, &HyprlandIpc::connectionChanged);
    QTRY_VERIFY(ipc.connected());
    bool ok = false; QVector<HyprClient> cl;
    ipc.queryClients([&](bool o, QVector<HyprClient> c, const Reply&) { ok = o; cl = c; });
    QTRY_VERIFY(ok); QCOMPARE(cl.size(), 2); QCOMPARE(cl[0].ref.generation, ipc.generation());
    bool mon = false; ipc.queryMonitors([&](bool o, auto v, const Reply&) { mon = o && v.size() == 1; }); QTRY_VERIFY(mon);
    bool ws = false; ipc.queryWorkspaces([&](bool o, auto v, const Reply&) { ws = o && v.size() == 1; }); QTRY_VERIFY(ws);
    bool aw = true; ipc.queryActiveWindow([&](bool o, auto, const Reply&) { aw = o; }); QTRY_VERIFY(!aw);   // "{}" => none

    const WindowRef w = cl[0].ref;
    QVERIFY(sync([&](auto cb) { ipc.setFloating(w, true, cb); }).dispatchOk());
    QVERIFY(sync([&](auto cb) { ipc.setPinned(w, false, cb); }).dispatchOk());
    QVERIFY(sync([&](auto cb) { ipc.resizeExact(w, 360, 300, cb); }).dispatchOk());
    QVERIFY(sync([&](auto cb) { ipc.moveExact(w, 10, 20, cb); }).dispatchOk());
    QVERIFY(sync([&](auto cb) { ipc.moveToWorkspace(w, 3, cb); }).dispatchOk());
    QCOMPARE(fake.received.value(fake.received.size() - 5),
      QString("/dispatch hl.dsp.window.float({window='address:0x55738da49dd0',action='on'})"));
    QCOMPARE(fake.received.value(fake.received.size() - 3),
      QString("/dispatch hl.dsp.window.resize({window='address:0x55738da49dd0',x=360,y=300,exact=true})"));
    // error reply is surfaced, not treated as success
    fake.handler = [](const QByteArray&) -> QByteArray { return "error: bad"; };
    Reply r = sync([&](auto cb) { ipc.setFloating(w, true, cb); });
    QVERIFY(r.ok()); QVERIFY(!r.dispatchOk());
    // injection through the address is refused before any socket is opened
    const int n = fake.received.size();
    r = sync([&](auto cb) { ipc.setFloating({"0x1'});os.execute('x')--", ipc.generation()}, true, cb); });
    QCOMPARE(r.status, Reply::Status::Stale); QCOMPARE(fake.received.size(), n);
  }
  void timeoutIsUnknownStateAndSocketClosed() {
    FakeHyprland fake; fake.hang = true;
    HyprlandIpc ipc(fake.paths());
    int ticks = 0; QTimer ticker; ticker.setInterval(10); connect(&ticker, &QTimer::timeout, [&] { ++ticks; }); ticker.start();
    QElapsedTimer t; t.start();
    bool done = false; Reply r;
    ipc.request("/dispatch x", [&](const Reply& x) { r = x; done = true; });
    QVERIFY(t.elapsed() < 50);                       // returned immediately: async
    QTRY_VERIFY_WITH_TIMEOUT(done, 1000);
    QCOMPARE(r.status, Reply::Status::Timeout); QVERIFY(r.unknownState);
    QVERIFY(t.elapsed() >= 240 && t.elapsed() < 600);
    QVERIFY(ticks >= 10);                            // GUI thread kept running during the wait
    QTRY_COMPARE_WITH_TIMEOUT(fake.closedRequests, 1, 1000);   // closed immediately after deadline
  }
  void connectFailureIsNotUnknownState() {
    QTemporaryDir d;
    HyprlandIpc ipc(HyprPaths::inDir(d.path()));
    const Reply r = sync([&](auto cb) { ipc.request("j/clients", cb); });
    QCOMPARE(r.status, Reply::Status::ConnectFailed); QVERIFY(!r.unknownState);
  }
  void noEnvMeansUnavailable() {
    HyprlandIpc ipc(HyprPaths{});
    QVERIFY(!ipc.available());
    ipc.start(); QVERIFY(!ipc.connected());
    qunsetenv("HYPRLAND_INSTANCE_SIGNATURE");
    QVERIFY(!HyprPaths::fromEnv().valid());
    qputenv("XDG_RUNTIME_DIR", "/run/user/9"); qputenv("HYPRLAND_INSTANCE_SIGNATURE", "abc");
    QCOMPARE(HyprPaths::fromEnv().request, QString("/run/user/9/hypr/abc/.socket.sock"));
    QCOMPARE(HyprPaths::fromEnv().event, QString("/run/user/9/hypr/abc/.socket2.sock"));
  }
  void eventsParsedAndCoalesced() {
    FakeHyprland fake;
    HyprlandIpc ipc(fake.paths()); ipc.start();
    QTRY_VERIFY(ipc.connected()); QTRY_COMPARE(fake.evClients.size(), 1);
    QSignalSpy raw(&ipc, &HyprlandIpc::event), st(&ipc, &HyprlandIpc::stateChanged), rel(&ipc, &HyprlandIpc::configReloaded);
    // burst, plus a line split across writes, plus junk and an irrelevant event
    fake.emitEvent("openwindow>>0xabc,1,kitty,t\nwindowtitle>>0xabc\nnot an event\nsubmap>>x\nmovewin");
    QTest::qWait(5);
    fake.emitEvent("dowv2>>0xabc,2,x\nconfigreloaded>>\n");
    QTRY_COMPARE(st.size(), 1);
    QCOMPARE(raw.size(), 5);
    QCOMPARE(raw[0][0].toString(), QString("openwindow")); QCOMPARE(raw[0][1].toString(), QString("0xabc,1,kitty,t"));
    const auto set = st[0][0].value<QSet<QString>>();
    QVERIFY(set.contains("openwindow") && set.contains("windowtitle") && set.contains("movewindowv2") && !set.contains("submap"));
    QCOMPARE(rel.size(), 1);
    QTest::qWait(100); QCOMPARE(st.size(), 1);       // nothing else fires while idle
  }
  void reconnectInvalidatesAddressesAndReenumerates() {
    FakeHyprland fake;
    HyprlandIpc ipc(fake.paths());
    QSignalSpy re(&ipc, &HyprlandIpc::reenumerateRequested), cc(&ipc, &HyprlandIpc::connectionChanged);
    ipc.start();
    QTRY_VERIFY(ipc.connected());
    QCOMPARE(re.size(), 0);                          // first connect is not a *re*connect
    const WindowRef old{"0xabc", ipc.generation()};
    QVERIFY(sync([&](auto cb) { ipc.setFloating(old, true, cb); }).dispatchOk());
    fake.dropEvents();
    QTRY_VERIFY(!ipc.connected());
    const int before = fake.received.size();
    QCOMPARE(sync([&](auto cb) { ipc.setFloating(old, true, cb); }).status, Reply::Status::Stale);
    QCOMPARE(fake.received.size(), before);          // no command issued on stale address
    QTest::qWait(350);                               // a few backoff rounds fail while the stream is down
    QVERIFY(!ipc.connected());
    fake.startEvents();
    QTRY_VERIFY_WITH_TIMEOUT(ipc.connected(), 6000);
    QCOMPARE(re.size(), 1); QCOMPARE(re[0][0].toULongLong(), ipc.generation());
    QCOMPARE(sync([&](auto cb) { ipc.setFloating(old, true, cb); }).status, Reply::Status::Stale);   // old address still stale
    const WindowRef fresh{"0xabc", ipc.generation()};
    QVERIFY(sync([&](auto cb) { ipc.setFloating(fresh, true, cb); }).dispatchOk());
  }
};
QTEST_MAIN(IpcTest)
#include "platform_test_ipc.moc"
