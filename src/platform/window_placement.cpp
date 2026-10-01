#include "hn/platform/window_placement.h"
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <QPointer>
#include <memory>
#include <vector>

namespace hn::platform {

QString defaultSessionPath() {
  QString base = qEnvironmentVariable("XDG_STATE_HOME");
  if (base.isEmpty()) base = QDir::homePath() + QStringLiteral("/.local/state");
  return base + QStringLiteral("/hyprnotes/session.json");
}

QByteArray toJson(const SessionState& s) {
  QJsonArray arr;
  for (const auto& w : s.windows) {
    arr.append(QJsonObject{{"note", w.noteKey}, {"role", roleName(w.role)},
      {"x", w.geometry.x()}, {"y", w.geometry.y()}, {"w", w.geometry.width()}, {"h", w.geometry.height()},
      {"monitor", w.monitor}, {"workspace", w.workspace}, {"scale", w.scale},
      {"allWorkspaces", w.mode == WorkspaceMode::AllWorkspaces}});
  }
  return QJsonDocument(QJsonObject{{"version", s.version}, {"windows", arr}}).toJson(QJsonDocument::Indented);
}

SessionState fromJson(const QByteArray& j, bool* ok) {
  SessionState s;
  QJsonParseError pe;
  const auto d = QJsonDocument::fromJson(j, &pe);
  if (ok) *ok = pe.error == QJsonParseError::NoError && d.isObject();
  if (!d.isObject()) return s;
  s.version = d.object()["version"].toInt(1);
  for (const auto& v : d.object()["windows"].toArray()) {
    const auto o = v.toObject();
    WindowState w;
    w.noteKey = o["note"].toString();
    w.role = o["role"].toString() == roleName(Role::Organizer) ? Role::Organizer : Role::Sticky;
    w.geometry = QRect(o["x"].toInt(), o["y"].toInt(), qMax(0, o["w"].toInt()), qMax(0, o["h"].toInt()));
    w.monitor = o["monitor"].toString();
    w.workspace = o["workspace"].toInt();
    w.scale = o["scale"].toDouble(1);
    w.mode = o["allWorkspaces"].toBool() ? WorkspaceMode::AllWorkspaces : WorkspaceMode::ThisWorkspace;
    s.windows.push_back(w);
  }
  return s;
}

bool saveSession(const QString& path, const SessionState& s, QString* err) {
  if (!QDir().mkpath(QFileInfo(path).absolutePath())) { if (err) *err = QStringLiteral("cannot create state dir"); return false; }
  QSaveFile f(path);   // write-temp + rename: atomic
  if (!f.open(QIODevice::WriteOnly)) { if (err) *err = f.errorString(); return false; }
  f.write(toJson(s));
  if (!f.commit()) { if (err) *err = f.errorString(); return false; }
  return true;
}

SessionState loadSession(const QString& path, bool* ok) {
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly)) { if (ok) *ok = false; return {}; }
  return fromJson(f.read(4 << 20), ok);
}

WindowState stateFromClient(const HyprClient& c, const QString& noteKey, Role role, const QVector<HyprMonitor>& mons) {
  WindowState w;
  w.noteKey = noteKey; w.role = role; w.geometry = c.rect; w.workspace = c.workspaceId;
  w.mode = c.pinned ? WorkspaceMode::AllWorkspaces : WorkspaceMode::ThisWorkspace;
  for (const auto& m : mons) if (m.id == c.monitor) { w.monitor = m.name; w.scale = m.scale; }
  return w;
}

QRect clampToWorkArea(QRect r, const QRect& work) {
  r.setWidth(qMin(qMax(r.width(), 1), work.width()));
  r.setHeight(qMin(qMax(r.height(), 1), work.height()));
  r.moveLeft(qBound(work.left(), r.left(), work.right() + 1 - r.width()));
  r.moveTop(qBound(work.top(), r.top(), work.bottom() + 1 - r.height()));
  return r;
}

PlacementPlan planPlacement(const WindowState& s, const QVector<HyprMonitor>& mons, const QVector<HyprWorkspace>& wss) {
  PlacementPlan p;
  p.pinned = s.mode == WorkspaceMode::AllWorkspaces;
  if (mons.isEmpty()) { p.rect = s.geometry; return p; }
  const HyprMonitor* best = nullptr;
  for (const auto& m : mons) if (m.name == s.monitor) best = &m;
  if (!best) {
    p.monitorMissing = true;
    qint64 bestArea = -1;   // most overlap with the saved rect, else first monitor
    for (const auto& m : mons) {
      const QRect i = m.rect.intersected(s.geometry);
      const qint64 a = i.isEmpty() ? 0 : qint64(i.width()) * i.height();
      if (a > bestArea) { bestArea = a; best = &m; }
    }
  }
  p.rect = clampToWorkArea(s.geometry, best->workArea());
  for (const auto& w : wss) if (w.id == s.workspace && s.workspace > 0) p.workspace = w.id;
  return p;
}

namespace {
struct Runner : std::enable_shared_from_this<Runner> {
  using Step = std::function<void(HyprlandIpc::ReplyFn)>;
  std::vector<Step> steps;
  std::function<void(bool)> done;
  QPointer<HyprlandIpc> ip;
  void next(size_t i, bool ok) {
    if (i == steps.size() || !ip) { if (done) done(ok && ip); return; }
    auto self = shared_from_this();
    steps[i]([self, i, ok](const Reply& r) { self->next(i + 1, ok && r.dispatchOk()); });
  }
};
} // namespace

void applyPlacement(HyprlandIpc& ipc, const WindowRef& w, const PlacementPlan& p, std::function<void(bool)> done) {
  // Sequential (each step after the previous reply) so float lands before size/move.
  auto r = std::make_shared<Runner>();
  r->ip = &ipc;
  r->done = std::move(done);
  QPointer<HyprlandIpc> ip(&ipc);
  r->steps.push_back([=](auto cb) { ip->setFloating(w, true, cb); });
  if (p.workspace) r->steps.push_back([=](auto cb) { ip->moveToWorkspace(w, *p.workspace, cb); });
  r->steps.push_back([=](auto cb) { ip->resizeExact(w, p.rect.width(), p.rect.height(), cb); });
  r->steps.push_back([=](auto cb) { ip->moveExact(w, p.rect.x(), p.rect.y(), cb); });
  r->steps.push_back([=](auto cb) { ip->setPinned(w, p.pinned, cb); });
  r->next(0, true);
}

void setWorkspaceMode(HyprlandIpc& ipc, const WindowRef& w, WorkspaceMode m, HyprlandIpc::ReplyFn cb) {
  ipc.setPinned(w, m == WorkspaceMode::AllWorkspaces, std::move(cb));
}

} // namespace hn::platform
