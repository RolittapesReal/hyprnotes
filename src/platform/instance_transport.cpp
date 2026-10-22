#include "hn/platform/instance_transport.h"
#include <QDeadlineTimer>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QLocalSocket>
#include <QLockFile>
#include <QTimer>
#include <QVariant>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <sys/un.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

namespace hn::platform {

QString actionName(Action a) {
  switch (a) {
  case Action::ShowOrganizer: return QStringLiteral("show-organizer");
  case Action::NewNote: return QStringLiteral("new-note");
  case Action::Background: return QStringLiteral("background");
  case Action::RefreshPlugins: return QStringLiteral("refresh-plugins");
  }
  return {};
}

static bool parseAction(const QByteArray& s, Action* out) {
  for (Action a : {Action::ShowOrganizer, Action::NewNote, Action::Background, Action::RefreshPlugins})
    if (s == actionName(a).toLatin1()) { *out = a; return true; }
  return false;
}

bool decodeInstanceRequest(const QByteArray& line, Action* action, QString* error) {
  const auto parts = line.trimmed().split(' ');
  auto fail = [&](const char* why) { if (error) *error = QString::fromLatin1(why); return false; };
  if (parts.size() != 3 || parts[0] != "HNIPC") return fail("malformed request");
  bool ok = false;
  const int version = parts[1].toInt(&ok);
  if (!ok) return fail("malformed version");
  if (version != kProtocolVersion) return fail("incompatible version");
  if (!parseAction(parts[2], action)) return fail("unknown command");
  if (error) error->clear();
  return true;
}

QString defaultEndpointPath() {
  const QString dir = qEnvironmentVariable("XDG_RUNTIME_DIR");
  QString disp = qEnvironmentVariable("WAYLAND_DISPLAY");
  if (dir.isEmpty() || disp.isEmpty()) return {};
  disp.replace(QLatin1Char('/'), QLatin1Char('_'));
  return QStringLiteral("%1/hyprnotes-%2-%3.sock").arg(dir, disp).arg(getuid());
}

bool endpointIsSafe(const QString& path, QString* why) {
  struct stat st{};
  auto fail = [&](const char* m) { if (why) *why = QString::fromLatin1(m); return false; };
  if (::lstat(QFile::encodeName(path).constData(), &st) != 0) return fail("endpoint missing");
  if (!S_ISSOCK(st.st_mode)) return fail("endpoint is not a socket");
  if (st.st_uid != getuid()) return fail("endpoint owned by another user");
  if (st.st_mode & 077) return fail("endpoint accessible by group/other");
  return true;
}

enum class EndpointProbe { NoListener, Unconfirmed, Hyprnotes };

static bool endpointHasNoListener(const QString& path) {
  const QByteArray encoded = QFile::encodeName(path);
  sockaddr_un address{};
  if (encoded.isEmpty() || size_t(encoded.size()) >= sizeof(address.sun_path)) return false;
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, encoded.constData(), size_t(encoded.size()));
  address.sun_path[encoded.size()] = '\0';

  const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (fd < 0) return false;
  const socklen_t length = socklen_t(offsetof(sockaddr_un, sun_path) + size_t(encoded.size()) + 1);
  const int result = ::connect(fd, reinterpret_cast<sockaddr*>(&address), length);
  const int connectError = errno;
  ::close(fd);
  return result != 0 && (connectError == ECONNREFUSED || connectError == ENOENT);
}

static EndpointProbe probeEndpoint(const QString& path) {
  QLocalSocket socket;
  QElapsedTimer connectTimer;
  connectTimer.start();
  socket.connectToServer(path);
  if (!socket.waitForConnected(500)) {
    socket.abort();
    if (connectTimer.elapsed() >= 500) return EndpointProbe::Unconfirmed;
    // Qt maps both refusal and timeout to ConnectionRefusedError. Confirm
    // staleness with a nonblocking AF_UNIX connect; EAGAIN/EINPROGRESS remain
    // unconfirmed, so a busy live listener is never unlinked.
    return endpointHasNoListener(path) ? EndpointProbe::NoListener : EndpointProbe::Unconfirmed;
  }

  // Unknown action is rejected before dispatch, so this confirms HNIPC without
  // opening a window, creating a note, or changing application state.
  const QByteArray request("HNIPC 1 __hn_probe__\n");
  const QByteArray expected("HNIPC 1 err unknown-command\n");
  socket.setReadBufferSize(expected.size() + 1);
  if (socket.write(request) != request.size() || !socket.flush() || !socket.waitForReadyRead(500)) {
    socket.abort();
    return EndpointProbe::Unconfirmed;
  }
  // Read at most one byte beyond the only valid reply. The Qt-side receive
  // buffer has same bound, so a peer flooding the socket cannot make readAll()
  // allocate/drain an unbounded response. Require EOF within a bounded window
  // and reject any delayed tail bytes already queued at close.
  QByteArray response = socket.read(expected.size() + 1);
  const bool closed = response.size() == expected.size() && socket.waitForDisconnected(500);
  const bool noTail = socket.bytesAvailable() == 0 && response.size() == expected.size();
  socket.abort();
  return response == expected && closed && noTail
      ? EndpointProbe::Hyprnotes : EndpointProbe::Unconfirmed;
}

// ---------------------------------------------------------------- server
InstanceServer::InstanceServer(QString p, QObject* parent) : QObject(parent), m_path(std::move(p)) {
  m_server.setSocketOptions(QLocalServer::UserAccessOption);
  connect(&m_server, &QLocalServer::newConnection, this, &InstanceServer::onConnection);
}

InstanceServer::Result InstanceServer::listen(QString* error) {
  auto fail = [&](const QString& m) { if (error) *error = m; return Result::Failed; };
  if (m_path.isEmpty()) return fail(QStringLiteral("no XDG_RUNTIME_DIR/WAYLAND_DISPLAY: not in a desktop session"));
  QLockFile lock(m_path + QStringLiteral(".lock"));
  lock.setStaleLockTime(5000);
  if (!lock.tryLock(3000)) {
    bool unconfirmedPeer = false;
    // Another starter may still own the lock after binding. Confirm its protocol
    // before declaring a lost race; never remove or replace an endpoint without the lock.
    if (lock.error() == QLockFile::LockFailedError && QFile::exists(m_path) && endpointIsSafe(m_path)) {
      const auto probe = probeEndpoint(m_path);
      if (probe == EndpointProbe::Hyprnotes) return Result::AlreadyRunning;
      unconfirmedPeer = probe == EndpointProbe::Unconfirmed;
    }
    // Lock ownership can span a slow first-start path before its socket is
    // connectable. Give that same in-progress startup one final bounded window.
    if (lock.error() != QLockFile::LockFailedError || !lock.tryLock(3000))
      return fail(unconfirmedPeer
          ? QStringLiteral("endpoint is busy but did not confirm the Hyprnotes protocol")
          : QStringLiteral("could not lock instance endpoint"));
  }

  if (QFile::exists(m_path)) {
    QString why;
    if (!endpointIsSafe(m_path, &why)) return fail(QStringLiteral("refusing endpoint: ") + why);
    const auto probe = probeEndpoint(m_path);
    if (probe == EndpointProbe::Hyprnotes) return Result::AlreadyRunning;
    if (probe == EndpointProbe::Unconfirmed)
      return fail(QStringLiteral("endpoint is active but did not confirm the Hyprnotes protocol"));
    // A refused connection means stale (crashed instance). Remove only while
    // holding the endpoint lock; a connected but unrecognized peer is preserved.
    QLocalServer::removeServer(m_path);
  }
  if (!m_server.listen(m_path)) return fail(m_server.errorString());
  QFile::setPermissions(m_path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
  return Result::Listening;
}

void InstanceServer::onConnection() {
  while (QLocalSocket* s = m_server.nextPendingConnection()) {
    // Peer must be the same uid (defence in depth on top of 0600).
    struct ucred cred{};
    socklen_t len = sizeof cred;
    if (::getsockopt(s->socketDescriptor(), SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0 || cred.uid != getuid()) {
      s->abort(); s->deleteLater(); continue;
    }
    auto* buf = new QByteArray;
    auto finish = [s](const QByteArray& reply) {
      if (!s->isOpen()) return;
      if (!reply.isEmpty()) s->write(reply);
      s->disconnectFromServer();
    };
    auto* timer = new QTimer(s);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, s, [s] { s->abort(); });
    timer->start(kAckDeadlineMs);
    connect(s, &QLocalSocket::disconnected, s, [s, buf] { delete buf; s->deleteLater(); });
    connect(s, &QLocalSocket::readyRead, s, [this, s, buf, finish] {
      if (s->property("done").toBool()) return;
      buf->append(s->readAll());
      const int nl = buf->indexOf('\n');
      if (nl < 0) {
        if (buf->size() > kMaxLine) { s->setProperty("done", true); finish("HNIPC 1 err too-large\n"); }
        return;
      }
      s->setProperty("done", true);
      if (nl > kMaxLine) return finish("HNIPC 1 err too-large\n");
      Action a;
      QString decodeError;
      if (!decodeInstanceRequest(buf->left(nl), &a, &decodeError)) {
        if (decodeError == QLatin1String("malformed request") || decodeError == QLatin1String("malformed version")) return finish({});
        if (decodeError == QLatin1String("incompatible version")) return finish("HNIPC 1 err incompatible-version\n");
        return finish("HNIPC 1 err unknown-command\n");
      }
      const bool handled = !m_handler || m_handler(a);
      finish(handled ? "HNIPC 1 ok\n" : "HNIPC 1 err failed\n");
    });
  }
}

// ---------------------------------------------------------------- client
ClientResult sendAction(Action a, const QString& path, int deadlineMs, int ver) {
  using S = ClientResult::Status;
  auto res = [](S s, QString m) { return ClientResult{s, std::move(m)}; };
  if (path.isEmpty()) return res(S::Unavailable, QStringLiteral("Not in a Wayland desktop session (XDG_RUNTIME_DIR/WAYLAND_DISPLAY unset)."));
  if (!QFile::exists(path)) return res(S::NoInstance, QStringLiteral("No running Hyprnotes instance."));
  QString why;
  if (!endpointIsSafe(path, &why))
    return res(S::UnsafeEndpoint, QStringLiteral("Refusing to talk to %1 (%2). Remove it if it is stale.").arg(path, why));

  QLocalSocket sock;
  QEventLoop loop;
  QTimer deadline;
  deadline.setSingleShot(true);
  bool sent = false, timedOut = false;
  QByteArray reply;
  QObject::connect(&deadline, &QTimer::timeout, &loop, [&] { timedOut = true; loop.quit(); });
  QObject::connect(&sock, &QLocalSocket::connected, &loop, [&] {
    sock.write(QByteArray("HNIPC ") + QByteArray::number(ver) + ' ' + actionName(a).toLatin1() + '\n');
    sent = true;
  });
  QObject::connect(&sock, &QLocalSocket::readyRead, &loop, [&] {
    reply += sock.readAll();
    if (reply.contains('\n') || reply.size() > kMaxLine) loop.quit();
  });
  QObject::connect(&sock, &QLocalSocket::disconnected, &loop, &QEventLoop::quit);
  QObject::connect(&sock, &QLocalSocket::errorOccurred, &loop, &QEventLoop::quit);
  deadline.start(deadlineMs);
  sock.connectToServer(path);
  if (sock.state() != QLocalSocket::UnconnectedState) loop.exec();   // immediate failure: error already fired
  if (sock.isOpen()) reply += sock.readAll();
  sock.abort();

  const QString line = QString::fromLatin1(reply.left(kMaxLine)).trimmed();
  if (line == QLatin1String("HNIPC 1 ok")) return res(S::Acked, {});
  if (line.startsWith(QLatin1String("HNIPC 1 err incompatible")))
    return res(S::Incompatible, QStringLiteral("The running Hyprnotes instance uses a different protocol version. Quit it (tray > Quit) and start the new version."));
  if (line.startsWith(QLatin1String("HNIPC 1 err")))
    return res(S::Rejected, QStringLiteral("The running instance refused the request: ") + line.mid(11));
  if (!sent) {
    if (timedOut) return res(S::Unavailable, QStringLiteral("Timed out connecting to the running instance; nothing was sent."));
    return res(S::NoInstance, QStringLiteral("No running Hyprnotes instance (connection refused)."));
  }
  if (a == Action::NewNote)
    return res(S::UnknownCompletion, QStringLiteral("No acknowledgement: the note may or may not have been created. Check the organizer; the request was not retried."));
  return res(S::UnknownCompletion, QStringLiteral("No acknowledgement from the running instance within the deadline."));
}

} // namespace hn::platform
