#include "hn/platform/instance_transport.h"
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QProcess>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QtTest>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <chrono>
#include <atomic>
#include <future>
#include <thread>
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
  void requestDecoderValidatesProtocolAndActions() {
    Action action{}; QString error;
    QVERIFY(decodeInstanceRequest("HNIPC 1 new-note", &action, &error));
    QCOMPARE(action, Action::NewNote); QVERIFY(error.isEmpty());
    QVERIFY(!decodeInstanceRequest("HNIPC 9 new-note", &action, &error));
    QCOMPARE(error, QString("incompatible version"));
    QVERIFY(!decodeInstanceRequest("HNIPC 1 launch", &action, &error));
    QCOMPARE(error, QString("unknown command"));
    QVERIFY(!decodeInstanceRequest("garbage", &action, &error));
    QCOMPARE(error, QString("malformed request"));
  }
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
    const QString helper = QCoreApplication::applicationDirPath() + "/platform_race_helper";
    QProcess owner;
    owner.start(helper, {p, "1"});
    QVERIFY(owner.waitForStarted(2000));
    QVERIFY(owner.waitForReadyRead(5000));
    QVERIFY(owner.readAllStandardOutput().contains("SERVER"));
    struct stat before{}; ::stat(qPrintable(p), &before);
    InstanceServer b(p);
    QCOMPARE(b.listen(), InstanceServer::Result::AlreadyRunning);
    struct stat after{}; ::stat(qPrintable(p), &after);
    QCOMPARE(after.st_ino, before.st_ino);
    QCOMPARE(sendAction(Action::ShowOrganizer, p).status, S::Acked);
    QVERIFY(owner.waitForFinished(3000));
    QVERIFY(owner.readAllStandardOutput().contains("HANDLED 1")); // probe did not dispatch; explicit request did
  }
  void lockContentionWithLiveEndpointReturnsAlreadyRunning() {
    const QString p = dir.filePath("contended.sock");
    const QString helper = QCoreApplication::applicationDirPath() + "/platform_race_helper";
    QProcess owner;
    owner.start(helper, {p, "1"});
    QVERIFY(owner.waitForStarted(2000));
    QVERIFY(owner.waitForReadyRead(5000));
    QVERIFY(owner.readAllStandardOutput().contains("SERVER"));
    QLockFile contender(p + QStringLiteral(".lock"));
    QVERIFY(contender.tryLock(0));
    InstanceServer b(p);
    QCOMPARE(b.listen(), InstanceServer::Result::AlreadyRunning);
    contender.unlock();
    QCOMPARE(sendAction(Action::ShowOrganizer, p).status, S::Acked);
    QVERIFY(owner.waitForFinished(3000));
    QVERIFY(owner.readAllStandardOutput().contains("HANDLED 1"));
  }
  void genericListenerDoesNotMasqueradeAsInstance_data() {
    QTest::addColumn<QByteArray>("response");
    QTest::newRow("silent") << QByteArray();
    QTest::newRow("wrong-protocol") << QByteArray("HTTP/1.0 200 OK\r\n\r\n");
    QTest::newRow("valid-prefix-with-oversized-tail")
        << QByteArray("HNIPC 1 err unknown-command\n") + QByteArray(1024 * 1024, 'x');
  }
  void genericListenerDoesNotMasqueradeAsInstance() {
    QFETCH(QByteArray, response);
    const QString p = dir.filePath(QStringLiteral("generic-%1.sock").arg(QString::fromLatin1(QTest::currentDataTag())));
    const QByteArray nativePath = QFile::encodeName(p);
    const int listener = ::socket(AF_UNIX, SOCK_STREAM, 0);
    QVERIFY(listener >= 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    qstrncpy(addr.sun_path, nativePath.constData(), sizeof(addr.sun_path));
    QVERIFY(::bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    QVERIFY(::chmod(nativePath.constData(), 0600) == 0);
    QVERIFY(::listen(listener, 4) == 0);
    struct stat before{};
    QCOMPARE(::stat(nativePath.constData(), &before), 0);
    QVERIFY(endpointIsSafe(p));

    QByteArray received;
    std::atomic<qint64> sentBytes{0};
    std::thread peer([&] {
      const int client = ::accept(listener, nullptr, nullptr);
      if (client < 0) return;
      int sendBuffer = 4096;
      ::setsockopt(client, SOL_SOCKET, SO_SNDBUF, &sendBuffer, sizeof(sendBuffer));
      timeval sendTimeout{2, 0};
      ::setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &sendTimeout, sizeof(sendTimeout));
      char bytes[128]{};
      const ssize_t count = ::recv(client, bytes, sizeof(bytes), 0);
      if (count > 0) received = QByteArray(bytes, int(count));
      qsizetype offset = 0;
      while (offset < response.size()) {
        const ssize_t written = ::send(client, response.constData() + offset,
                                       size_t(response.size() - offset), MSG_NOSIGNAL);
        if (written <= 0) break;
        offset += written;
        sentBytes.store(offset);
      }
      ::close(client);
    });

    std::promise<bool> lockedPromise;
    auto lockedFuture = lockedPromise.get_future();
    std::thread lockHolder([&] {
      QLockFile lock(p + QStringLiteral(".lock"));
      const bool locked = lock.tryLock(0);
      lockedPromise.set_value(locked);
      if (locked) std::this_thread::sleep_for(std::chrono::milliseconds(3500));
    });
    const bool lockHeld = lockedFuture.wait_for(std::chrono::seconds(1)) == std::future_status::ready && lockedFuture.get();
    InstanceServer candidate(p);
    QString error;
    QElapsedTimer listenTime;
    listenTime.start();
    const auto result = candidate.listen(&error);
    const qint64 elapsedMs = listenTime.elapsed();
    lockHolder.join();
    peer.join();
    ::close(listener);

    QVERIFY(lockHeld);
    QVERIFY(elapsedMs >= 2800);  // proves the contended-lock path was exercised
    QCOMPARE(result, InstanceServer::Result::Failed);
    QVERIFY(error.contains("did not confirm"));
    QCOMPARE(received, QByteArray("HNIPC 1 __hn_probe__\n"));
    if (response.size() > 1024)
      QVERIFY2(sentBytes.load() <= 64 * 1024, qPrintable(QStringLiteral("probe consumed too much flood: %1 bytes").arg(sentBytes.load())));
    struct stat after{};
    QCOMPARE(::stat(nativePath.constData(), &after), 0);
    QCOMPARE(after.st_ino, before.st_ino);  // unverified live endpoint is never removed
  }
  void startupInProgressEventuallyReleasesLock() {
    const QString p = dir.filePath("starting.sock");
    std::promise<bool> lockedPromise;
    auto lockedFuture = lockedPromise.get_future();
    std::thread starter([&] {
      QLockFile lock(p + QStringLiteral(".lock"));
      const bool locked = lock.tryLock(0);
      lockedPromise.set_value(locked);
      if (locked) std::this_thread::sleep_for(std::chrono::milliseconds(150));
    });
    const bool lockHeld = lockedFuture.wait_for(std::chrono::seconds(1)) == std::future_status::ready && lockedFuture.get();
    InstanceServer server(p);
    const auto result = server.listen();
    starter.join();
    QVERIFY(lockHeld);
    QCOMPARE(result, InstanceServer::Result::Listening);
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
      pr->start(helper, {p, QString::number(N - 1)});
      ps << pr;
    }
    int servers = 0, acked = 0, other = 0, serverHandled = -1;
    QStringList outputs;
    for (auto* pr : ps) {
      QVERIFY(pr->waitForFinished(90000));   // generous: a slow machine only makes the test slower, never wrong
      const QString out = pr->readAllStandardOutput();
      outputs << QStringLiteral("exit=%1: %2").arg(pr->exitCode()).arg(out.trimmed());
      if (out.contains("SERVER")) {
        ++servers;
        const QRegularExpression handled(QStringLiteral("HANDLED (\\d+)"));
        const auto match = handled.match(out);
        if (match.hasMatch()) serverHandled = match.captured(1).toInt();
      }
      else if (out.contains("CLIENT 0")) ++acked;
      else ++other;
    }
    const QString details = outputs.join(QLatin1Char('\n'));
    QVERIFY2(servers == 1, qPrintable(details));
    QVERIFY2(acked == N - 1, qPrintable(details));
    QVERIFY2(other == 0, qPrintable(details));
    QVERIFY2(serverHandled == N - 1, qPrintable(details));
  }
};
QTEST_MAIN(TransportTest)
#include "platform_test_transport.moc"
