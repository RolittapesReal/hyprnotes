#include "hn/platform/hyprland_ipc.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QRegularExpression>
#include <memory>

namespace hn::platform {

namespace {
// ---- the ONE place with Hyprland-version-specific strings (0.56.2, Lua dispatchers) ----
struct HyprSyntax {
  static QByteArray win(const WindowRef& w) { return "window='address:" + w.address.toLatin1() + "'"; }
  static QByteArray float_(const WindowRef& w, bool on) {
    return "/dispatch hl.dsp.window.float({" + win(w) + ",action='" + (on ? "on" : "off") + "'})"; }
  static QByteArray pin(const WindowRef& w, bool on) {
    return "/dispatch hl.dsp.window.pin({" + win(w) + ",action='" + (on ? "on" : "off") + "'})"; }
  static QByteArray resize(const WindowRef& w, int x, int y) {
    return "/dispatch hl.dsp.window.resize({" + win(w) + ",x=" + QByteArray::number(x) + ",y=" + QByteArray::number(y) + ",exact=true})"; }
  static QByteArray move(const WindowRef& w, int x, int y) {
    return "/dispatch hl.dsp.window.move({" + win(w) + ",x=" + QByteArray::number(x) + ",y=" + QByteArray::number(y) + ",exact=true})"; }
  static QByteArray toWorkspace(const WindowRef& w, int ws) {
    return "/dispatch hl.dsp.window.move({" + win(w) + ",workspace=" + QByteArray::number(ws) + ",follow=false})"; }
  static constexpr const char* clients = "j/clients";
  static constexpr const char* activeWindow = "j/activewindow";
  static constexpr const char* monitors = "j/monitors";
  static constexpr const char* workspaces = "j/workspaces";
  // events that warrant a state re-query (names as in `event>>data`)
  static bool relevant(const QString& e) {
    static const QSet<QString> s{"openwindow", "closewindow", "movewindow", "movewindowv2", "windowtitle",
      "windowtitlev2", "changefloatingmode", "pin", "workspace", "workspacev2", "moveworkspace", "moveworkspacev2",
      "createworkspace", "createworkspacev2", "destroyworkspace", "destroyworkspacev2", "monitoradded",
      "monitoraddedv2", "monitorremoved", "monitorremovedv2", "focusedmon", "focusedmonv2", "configreloaded",
      "fullscreen", "activewindowv2"};
    return s.contains(e);
  }
};
} // namespace

HyprPaths HyprPaths::fromEnv() {
  const QString rt = qEnvironmentVariable("XDG_RUNTIME_DIR"), sig = qEnvironmentVariable("HYPRLAND_INSTANCE_SIGNATURE");
  if (rt.isEmpty() || sig.isEmpty()) return {};
  return inDir(rt + "/hypr/" + sig);
}

HyprlandIpc::HyprlandIpc(HyprPaths p, QObject* parent) : QObject(parent), m_paths(std::move(p)) {
  m_backoff.setSingleShot(true);
  connect(&m_backoff, &QTimer::timeout, this, &HyprlandIpc::openEventSocket);
  m_coalesce.setSingleShot(true);
  m_coalesce.setInterval(30);
  connect(&m_coalesce, &QTimer::timeout, this, [this] {
    QSet<QString> s; s.swap(m_pending);
    if (!s.isEmpty()) emit stateChanged(s);
  });
}
HyprlandIpc::~HyprlandIpc() { stop(); }

void HyprlandIpc::start() {
  if (m_running || !available()) return;
  m_running = true;
  openEventSocket();
}

void HyprlandIpc::stop() {
  m_running = false;
  m_backoff.stop();
  if (m_event) { m_event->disconnect(this); m_event->abort(); m_event->deleteLater(); m_event = nullptr; }
  if (m_connected) { m_connected = false; emit connectionChanged(false); }
}

void HyprlandIpc::openEventSocket() {
  if (!m_running) return;
  m_event = new QLocalSocket(this);
  connect(m_event, &QLocalSocket::connected, this, [this] {
    m_connected = true;
    m_backoffMs = 100;
    ++m_generation;
    const bool re = m_everConnected;
    m_everConnected = true;
    emit connectionChanged(true);
    if (re) emit reenumerateRequested(m_generation);
  });
  connect(m_event, &QLocalSocket::readyRead, this, &HyprlandIpc::onEventData);
  connect(m_event, &QLocalSocket::disconnected, this, &HyprlandIpc::onEventLost);
  connect(m_event, &QLocalSocket::errorOccurred, this, [this](QLocalSocket::LocalSocketError) { onEventLost(); });
  m_event->connectToServer(m_paths.event);
}

void HyprlandIpc::onEventLost() {
  if (!m_event) return;                      // errorOccurred + disconnected both fire; handle once
  m_event->disconnect(this);
  m_event->deleteLater();
  m_event = nullptr;
  m_evBuf.clear();
  if (m_connected) { m_connected = false; emit connectionChanged(false); }
  if (!m_running) return;
  m_backoff.start(m_backoffMs);              // bounded exponential backoff, 100ms .. 5s
  m_backoffMs = qMin(m_backoffMs * 2, 5000);
}

void HyprlandIpc::onEventData() {
  m_evBuf += m_event->readAll();
  if (m_evBuf.size() > (1 << 20)) m_evBuf.clear();   // bounded; garbage guard
  int nl;
  while ((nl = m_evBuf.indexOf('\n')) >= 0) {
    const QString line = QString::fromUtf8(m_evBuf.left(nl));
    m_evBuf.remove(0, nl + 1);
    const int sep = line.indexOf(QStringLiteral(">>"));
    if (sep <= 0) continue;
    const QString name = line.left(sep), data = line.mid(sep + 2);
    emit event(name, data);
    if (name == QLatin1String("configreloaded")) emit configReloaded();
    if (HyprSyntax::relevant(name)) { m_pending.insert(name); if (!m_coalesce.isActive()) m_coalesce.start(); }
  }
}

void HyprlandIpc::request(const QByteArray& cmd, ReplyFn cb, int deadlineMs) {
  auto done = [cb = std::move(cb)](Reply::Status st, QByteArray data, bool unknown) {
    if (cb) cb(Reply{st, std::move(data), unknown});
  };
  if (!available()) return QTimer::singleShot(0, this, [done] { done(Reply::Status::ConnectFailed, {}, false); });
  auto* s = new QLocalSocket(this);            // fresh per command, closed on completion
  auto* t = new QTimer(s);
  struct St { bool finished = false, wrote = false; QByteArray buf; };
  auto st = std::make_shared<St>();
  auto finish = [s, st, done](Reply::Status status, bool unknown) {
    if (st->finished) return;
    st->finished = true;
    if (s->isOpen()) st->buf += s->readAll();
    s->disconnect();
    s->abort();
    s->deleteLater();
    done(status, st->buf, unknown);
  };
  t->setSingleShot(true);
  connect(t, &QTimer::timeout, s, [finish, st] { finish(Reply::Status::Timeout, st->wrote); });
  connect(s, &QLocalSocket::connected, s, [s, cmd, st] { s->write(cmd); s->flush(); st->wrote = true; });
  connect(s, &QLocalSocket::readyRead, s, [s, st] { st->buf += s->readAll(); });
  connect(s, &QLocalSocket::disconnected, s, [finish] { finish(Reply::Status::Ok, false); });
  connect(s, &QLocalSocket::errorOccurred, s, [finish, st](QLocalSocket::LocalSocketError e) {
    if (e == QLocalSocket::PeerClosedError) return;    // handled by disconnected
    finish(st->wrote ? Reply::Status::Error : Reply::Status::ConnectFailed, st->wrote);
  });
  t->start(deadlineMs);
  s->connectToServer(m_paths.request);
}

// ---------------------------------------------------------------- parsing
static QRect rectOf(const QJsonObject& o, const char* at, const char* size) {
  const auto a = o[at].toArray(), b = o[size].toArray();
  return QRect(a.at(0).toInt(), a.at(1).toInt(), b.at(0).toInt(), b.at(1).toInt());
}
static HyprClient clientOf(const QJsonObject& o, quint64 gen) {
  HyprClient c;
  c.ref = {o["address"].toString(), gen};
  c.cls = o["class"].toString(); c.title = o["title"].toString();
  c.initialClass = o["initialClass"].toString(); c.initialTitle = o["initialTitle"].toString();
  c.rect = rectOf(o, "at", "size");
  c.workspaceId = o["workspace"].toObject()["id"].toInt();
  c.monitor = o["monitor"].toInt(-1);
  c.pid = o["pid"].toInteger();
  c.floating = o["floating"].toBool(); c.pinned = o["pinned"].toBool();
  c.mapped = o["mapped"].toBool(); c.xwayland = o["xwayland"].toBool();
  return c;
}
QVector<HyprClient> HyprlandIpc::parseClients(const QByteArray& j, quint64 gen, bool* ok) {
  QVector<HyprClient> out;
  const auto d = QJsonDocument::fromJson(j);
  if (ok) *ok = d.isArray();
  for (const auto& v : d.array()) out.push_back(clientOf(v.toObject(), gen));
  return out;
}
QVector<HyprMonitor> HyprlandIpc::parseMonitors(const QByteArray& j, bool* ok) {
  QVector<HyprMonitor> out;
  const auto d = QJsonDocument::fromJson(j);
  if (ok) *ok = d.isArray();
  for (const auto& v : d.array()) {
    const auto o = v.toObject();
    HyprMonitor m;
    m.id = o["id"].toInt(-1); m.name = o["name"].toString();
    m.scale = o["scale"].toDouble(1); if (m.scale <= 0) m.scale = 1;
    const auto r = o["reserved"].toArray();   // [left, top, right, bottom]
    m.reserved = QMargins(r.at(0).toInt(), r.at(1).toInt(), r.at(2).toInt(), r.at(3).toInt());
    m.rect = QRect(o["x"].toInt(), o["y"].toInt(), qRound(o["width"].toDouble() / m.scale), qRound(o["height"].toDouble() / m.scale));
    m.activeWorkspace = o["activeWorkspace"].toObject()["id"].toInt();
    out.push_back(m);
  }
  return out;
}
QVector<HyprWorkspace> HyprlandIpc::parseWorkspaces(const QByteArray& j, bool* ok) {
  QVector<HyprWorkspace> out;
  const auto d = QJsonDocument::fromJson(j);
  if (ok) *ok = d.isArray();
  for (const auto& v : d.array()) {
    const auto o = v.toObject();
    out.push_back({o["id"].toInt(), o["name"].toString(), o["monitorID"].toInt(-1), o["windows"].toInt()});
  }
  return out;
}

// ---------------------------------------------------------------- typed queries
void HyprlandIpc::queryClients(std::function<void(bool, QVector<HyprClient>, const Reply&)> cb) {
  const quint64 gen = m_generation;   // addresses are valid for the generation they were read in
  request(HyprSyntax::clients, [cb, gen](const Reply& r) {
    bool ok = false; QVector<HyprClient> v;
    if (r.ok()) v = parseClients(r.data, gen, &ok);
    cb(ok, v, r);
  });
}
void HyprlandIpc::queryActiveWindow(std::function<void(bool, HyprClient, const Reply&)> cb) {
  const quint64 gen = m_generation;
  request(HyprSyntax::activeWindow, [cb, gen](const Reply& r) {
    const auto d = QJsonDocument::fromJson(r.data);   // "{}" / empty when nothing focused
    const bool ok = r.ok() && d.isObject() && !d.object().isEmpty();
    cb(ok, ok ? clientOf(d.object(), gen) : HyprClient{}, r);
  });
}
void HyprlandIpc::queryMonitors(std::function<void(bool, QVector<HyprMonitor>, const Reply&)> cb) {
  request(HyprSyntax::monitors, [cb](const Reply& r) {
    bool ok = false; QVector<HyprMonitor> v;
    if (r.ok()) v = parseMonitors(r.data, &ok);
    cb(ok, v, r);
  });
}
void HyprlandIpc::queryWorkspaces(std::function<void(bool, QVector<HyprWorkspace>, const Reply&)> cb) {
  request(HyprSyntax::workspaces, [cb](const Reply& r) {
    bool ok = false; QVector<HyprWorkspace> v;
    if (r.ok()) v = parseWorkspaces(r.data, &ok);
    cb(ok, v, r);
  });
}

// ---------------------------------------------------------------- dispatch
void HyprlandIpc::dispatchWindow(const WindowRef& w, const QByteArray& cmd, ReplyFn cb) {
  // Address must be a bare hex token: it is interpolated into a Lua expression.
  static const QRegularExpression hex(QStringLiteral("^0x[0-9a-fA-F]{1,16}$"));
  if (!m_connected || w.generation != m_generation || !hex.match(w.address).hasMatch()) {
    QTimer::singleShot(0, this, [cb] { if (cb) cb(Reply{Reply::Status::Stale, {}, false}); });
    return;
  }
  request(cmd, std::move(cb));
}
void HyprlandIpc::setFloating(const WindowRef& w, bool on, ReplyFn cb) { dispatchWindow(w, HyprSyntax::float_(w, on), std::move(cb)); }
void HyprlandIpc::setPinned(const WindowRef& w, bool on, ReplyFn cb) { dispatchWindow(w, HyprSyntax::pin(w, on), std::move(cb)); }
void HyprlandIpc::resizeExact(const WindowRef& w, int x, int y, ReplyFn cb) { dispatchWindow(w, HyprSyntax::resize(w, x, y), std::move(cb)); }
void HyprlandIpc::moveExact(const WindowRef& w, int x, int y, ReplyFn cb) { dispatchWindow(w, HyprSyntax::move(w, x, y), std::move(cb)); }
void HyprlandIpc::moveToWorkspace(const WindowRef& w, int ws, ReplyFn cb) { dispatchWindow(w, HyprSyntax::toWorkspace(w, ws), std::move(cb)); }

} // namespace hn::platform
