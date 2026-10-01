#include "hn/platform/instance_transport.h"
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
using namespace hn::platform;
using S = ClientResult::Status;

class TransportTest : public QObject {
  Q_OBJECT
  QTemporaryDir dir;
  QString path() const { return dir.filePath("ep.sock"); }
private slots:
  void roundTripAndPermissions() {
    InstanceServer s(path());
    QList<Action> got;
    s.setHandler([&](Action a) { got << a; return true; });
    QCOMPARE(s.listen(), InstanceServer::Result::Listening);
    struct stat st{}; QCOMPARE(::stat(qPrintable(path()), &st), 0);
    QCOMPARE(st.st_mode & 0777, mode_t(0600));
    QVERIFY(endpointIsSafe(path()));
    for (Action a : {Action::ShowOrganizer, Action::NewNote, Action::Background})
      QCOMPARE(sendAction(a, path()).status, S::Acked);
    QCOMPARE(got, (QList<Action>{Action::ShowOrganizer, Action::NewNote, Action::Background}));
  }
  void handlerFailureIsRejected() {
    InstanceServer s(dir.filePath("rej.sock"));
    s.setHandler([](Action) { return false; });
    QCOMPARE(s.listen(), InstanceServer::Result::Listening);
    QCOMPARE(sendAction(Action::NewNote, s.endpointPath()).status, S::Rejected);
  }
  void incompatibleVersionIsActionable() {
    InstanceServer s(dir.filePath("ver.sock"));
    QCOMPARE(s.listen(), InstanceServer::Result::Listening);
    const auto r = sendAction(Action::ShowOrganizer, s.endpointPath(), 1000, 99);
    QCOMPARE(r.status, S::Incompatible);
    QVERIFY(r.message.contains("Quit"));
  }
  void garbageAndOversizeRejectedServerSurvives() {
    InstanceServer s(dir.filePath("g.sock"));
    int handled = 0;
    s.setHandler([&](Action) { ++handled; return true; });
    QCOMPARE(s.listen(), InstanceServer::Result::Listening);
    auto poke = [&](const QByteArray& bytes) -> QByteArray {
      QLocalSocket c; c.connectToServer(s.endpointPath());
      if (!c.waitForConnected(500)) return "CONNECT-FAIL";
      c.write(bytes); c.flush();
      QElapsedTimer tt; tt.start();
      while (c.state() != QLocalSocket::UnconnectedState && c.bytesAvailable() == 0 && tt.elapsed() < 1500) QTest::qWait(5);
      return c.readAll();
    };
    QCOMPARE(poke("GET / HTTP/1.1\r\n\r\n"), QByteArray());
    QCOMPARE(poke(QByteArray("\x00\xff\x01\x02\n", 5)), QByteArray());
    QCOMPARE(poke("HNIPC 1 ; rm -rf ~\n"), QByteArray());
    QCOMPARE(poke("HNIPC 1 rm\n"), QByteArray("HNIPC 1 err unknown-command\n"));
    QCOMPARE(poke("HNIPC x show-organizer\n"), QByteArray());
    QVERIFY(poke(QByteArray(5000, 'A')).startsWith("HNIPC 1 err too-large"));
    QCOMPARE(handled, 0);
    QCOMPARE(sendAction(Action::ShowOrganizer, s.endpointPath()).status, S::Acked);
    QCOMPARE(handled, 1);
  }
  void silentServerGivesUnknownCompletionNoReplay() {
    QLocalServer mute; mute.setSocketOptions(QLocalServer::UserAccessOption);
    QVERIFY(mute.listen(dir.filePath("mute.sock")));
    int connections = 0;
    QObject::connect(&mute, &QLocalServer::newConnection, [&] { ++connections; mute.nextPendingConnection(); });
    QElapsedTimer t; t.start();
    const auto r = sendAction(Action::NewNote, mute.fullServerName(), 300);
    QCOMPARE(r.status, S::UnknownCompletion);
    QVERIFY(r.message.contains("not retried"));
    QVERIFY(t.elapsed() < 900);
    QCOMPARE(connections, 1);   // exactly one attempt, never replayed
  }
  void defaultDeadlineIsOneSecond() { QCOMPARE(kAckDeadlineMs, 1000); }
  void noInstanceAndStaleEndpoint() {
    const QString p = dir.filePath("stale.sock");
    QCOMPARE(sendAction(Action::ShowOrganizer, p).status, S::NoInstance);
    // leave a dead socket file behind (crashed instance)
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un a{}; a.sun_family = AF_UNIX; qstrncpy(a.sun_path, qPrintable(p), sizeof a.sun_path - 1);
    QCOMPARE(::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a), 0);
    ::close(fd);
    ::chmod(qPrintable(p), 0600);
    QVERIFY(QFile::exists(p));
    QCOMPARE(sendAction(Action::ShowOrganizer, p).status, S::NoInstance);
    InstanceServer s(p);
    QCOMPARE(s.listen(), InstanceServer::Result::Listening);   // stale removed, re-bound
    QCOMPARE(sendAction(Action::ShowOrganizer, p).status, S::Acked);
  }
  void liveEndpointNeverDeleted() {
    const QString p = dir.filePath("live.sock");
    InstanceServer a(p);
    QCOMPARE(a.listen(), InstanceServer::Result::Listening);
    struct stat before{}; ::stat(qPrintable(p), &before);
    InstanceServer b(p);
    QCOMPARE(b.listen(), InstanceServer::Result::AlreadyRunning);
    struct stat after{}; ::stat(qPrintable(p), &after);
    QCOMPARE(after.st_ino, before.st_ino);
    QCOMPARE(sendAction(Action::ShowOrganizer, p).status, S::Acked);
  }
  void unsafeEndpointRefused() {
    InstanceServer a(dir.filePath("open.sock"));
    QCOMPARE(a.listen(), InstanceServer::Result::Listening);
    ::chmod(qPrintable(a.endpointPath()), 0666);
    QString why;
    QVERIFY(!endpointIsSafe(a.endpointPath(), &why));
    QCOMPARE(sendAction(Action::ShowOrganizer, a.endpointPath()).status, S::UnsafeEndpoint);
    InstanceServer b(a.endpointPath());
    QCOMPARE(b.listen(), InstanceServer::Result::Failed);
    QFile f(dir.filePath("regular")); QVERIFY(f.open(QIODevice::WriteOnly)); f.close();
    QVERIFY(!endpointIsSafe(f.fileName(), &why)); QVERIFY(why.contains("socket"));
  }
  void endpointScopedBySessionAndUid() {
    qputenv("XDG_RUNTIME_DIR", "/run/user/1000");
    qputenv("WAYLAND_DISPLAY", "wayland-7");
    QCOMPARE(defaultEndpointPath(), QString("/run/user/1000/hyprnotes-wayland-7-%1.sock").arg(getuid()));
    qputenv("WAYLAND_DISPLAY", "wayland-8");
    QVERIFY(!defaultEndpointPath().contains("wayland-7"));
    qunsetenv("WAYLAND_DISPLAY");
    QVERIFY(defaultEndpointPath().isEmpty());
  }
  void startupRaceExactlyOneServer() {
    const QString helper = QCoreApplication::applicationDirPath() + "/platform_race_helper";
    QVERIFY(QFile::exists(helper));
    const QString p = dir.filePath("race.sock");
    constexpr int N = 12;
    QList<QProcess*> ps;
    for (int i = 0; i < N; ++i) {
      auto* pr = new QProcess(this);
      pr->start(helper, {p});
      ps << pr;
    }
    int servers = 0, acked = 0, other = 0;
    for (auto* pr : ps) {
      QVERIFY(pr->waitForFinished(15000));
      const QString out = pr->readAllStandardOutput();
      if (out.contains("SERVER")) { ++servers; QVERIFY2(out.contains("HANDLED 11"), qPrintable(out)); }
      else if (out.contains("CLIENT 0")) ++acked;
      else { ++other; qWarning() << out; }
    }
    QCOMPARE(servers, 1);
    QCOMPARE(acked, N - 1);
    QCOMPARE(other, 0);
  }
};
QTEST_MAIN(TransportTest)
#include "platform_test_transport.moc"
