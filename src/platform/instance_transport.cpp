#include "hn/platform/instance_transport.h"
#include <QDeadlineTimer>
#include <QEventLoop>
#include <QFile>
#include <QLocalSocket>
#include <QLockFile>
#include <QTimer>
#include <QVariant>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

namespace hn::platform {

QString actionName(Action a) {
  switch (a) {
  case Action::ShowOrganizer: return QStringLiteral("show-organizer");
  case Action::NewNote: return QStringLiteral("new-note");
  case Action::Background: return QStringLiteral("background");
  }
  return {};
}

static bool parseAction(const QByteArray& s, Action* out) {
  for (Action a : {Action::ShowOrganizer, Action::NewNote, Action::Background})
    if (s == actionName(a).toLatin1()) { *out = a; return true; }
  return false;
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
  if (!lock.tryLock(3000)) return fail(QStringLiteral("could not lock instance endpoint"));

  if (QFile::exists(m_path)) {
    QString why;
    if (!endpointIsSafe(m_path, &why)) return fail(QStringLiteral("refusing endpoint: ") + why);
    QLocalSocket probe;
    probe.connectToServer(m_path);
    if (probe.waitForConnected(500)) { probe.abort(); return Result::AlreadyRunning; }
    // Nothing answered: stale (crashed instance). Safe to remove while we hold the lock.
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
      const QList<QByteArray> parts = buf->left(nl).trimmed().split(' ');
      if (parts.size() != 3 || parts[0] != "HNIPC") return finish({}); // garbage: drop silently
      bool ok = false;
      const int ver = parts[1].toInt(&ok);
      if (!ok) return finish({});
      if (ver != kProtocolVersion) return finish("HNIPC 1 err incompatible-version\n");
      Action a;
      if (!parseAction(parts[2], &a)) return finish("HNIPC 1 err unknown-command\n");
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
