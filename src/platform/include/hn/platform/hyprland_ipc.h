#pragma once
// Hyprland IPC (spec 7.2): ONE persistent event-socket reader + a fresh, async, 250ms-deadline
// request socket per command. Nothing here blocks the GUI thread.
//
// Verified against Hyprland 0.56.2 (Lua config): `dispatch` takes a Lua expression
// (hl.dsp.window.float({window='address:0x..',action='on'})), replies "ok" or "error: ...".
// All version-specific strings live in HyprSyntax (hyprland_ipc.cpp, one table).
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QRect>
#include <QSet>
#include <QTimer>
#include <QLocalSocket>
#include <QVector>
#include <functional>

namespace hn::platform {

constexpr int kRequestDeadlineMs = 250;

struct HyprPaths {
  QString request, event;
  bool valid() const { return !request.isEmpty(); }
  // $XDG_RUNTIME_DIR/hypr/$HYPRLAND_INSTANCE_SIGNATURE/.socket{,2}.sock; empty if env missing.
  static HyprPaths fromEnv();
  static HyprPaths inDir(const QString& dir) { return {dir + "/.socket.sock", dir + "/.socket2.sock"}; }
};

struct Reply {
  enum class Status { Ok, ConnectFailed, Timeout, Stale, Error } status = Status::Error;
  QByteArray data;
  // Timeout after the request was written: compositor may or may not have applied it.
  // Callers must reconcile (re-query) before retrying; prefer idempotent set operations.
  bool unknownState = false;
  bool ok() const { return status == Status::Ok; }
  // dispatch succeeded iff transport ok and the reply is literally "ok"
  bool dispatchOk() const { return ok() && data.trimmed() == "ok"; }
};

// A window address is only valid for the event-connection generation it was read in.
struct WindowRef {
  QString address;
  quint64 generation = 0;
};

struct HyprClient {
  WindowRef ref;
  QString cls, title, initialClass, initialTitle;
  QRect rect;              // at/size (layout px)
  int workspaceId = 0;
  int monitor = -1;
  qint64 pid = 0;
  bool floating = false, pinned = false, mapped = false, xwayland = false;
};
struct HyprMonitor {
  int id = -1;
  QString name;
  QRect rect;              // logical (width/scale)
  QMargins reserved;       // left, top, right, bottom
  double scale = 1;
  int activeWorkspace = 0;
  QRect workArea() const { return rect.marginsRemoved(reserved); }
};
struct HyprWorkspace { int id = 0; QString name; int monitorId = -1; int windows = 0; };

class HyprlandIpc : public QObject {
  Q_OBJECT
public:
  explicit HyprlandIpc(HyprPaths paths = HyprPaths::fromEnv(), QObject* parent = nullptr);
  ~HyprlandIpc() override;
  bool available() const { return m_paths.valid(); }
  bool connected() const { return m_connected; }
  quint64 generation() const { return m_generation; }
  void start();            // open event socket (idempotent); reconnects with bounded backoff
  void stop();

  using ReplyFn = std::function<void(const Reply&)>;
  // Raw request; `cmd` like "j/clients" or "/dispatch <lua>". cb is always called once.
  void request(const QByteArray& cmd, ReplyFn cb, int deadlineMs = kRequestDeadlineMs);

  // typed queries (cb gets ok=false on any failure/timeout; reply carries the detail)
  void queryClients(std::function<void(bool, QVector<HyprClient>, const Reply&)> cb);
  void queryActiveWindow(std::function<void(bool, HyprClient, const Reply&)> cb);
  void queryMonitors(std::function<void(bool, QVector<HyprMonitor>, const Reply&)> cb);
  void queryWorkspaces(std::function<void(bool, QVector<HyprWorkspace>, const Reply&)> cb);

  // Idempotent set operations on a window. Refused (Stale) if disconnected or if `w` was read
  // in an older connection generation.
  void setFloating(const WindowRef& w, bool on, ReplyFn cb = {});
  void setPinned(const WindowRef& w, bool on, ReplyFn cb = {});
  void resizeExact(const WindowRef& w, int width, int height, ReplyFn cb = {});
  void moveExact(const WindowRef& w, int x, int y, ReplyFn cb = {});
  void moveToWorkspace(const WindowRef& w, int workspaceId, ReplyFn cb = {});

  // parsing helpers (public for tests)
  static QVector<HyprClient> parseClients(const QByteArray& json, quint64 gen, bool* ok = nullptr);
  static QVector<HyprMonitor> parseMonitors(const QByteArray& json, bool* ok = nullptr);
  static QVector<HyprWorkspace> parseWorkspaces(const QByteArray& json, bool* ok = nullptr);

signals:
  void connectionChanged(bool connected);
  // After a *re*connect: window addresses are stale; re-enumerate app-owned windows.
  void reenumerateRequested(quint64 generation);
  void event(const QString& name, const QString& data);   // every raw event
  // Coalesced (~30ms) set of relevant event names since last emission; do ONE follow-up query.
  void stateChanged(const QSet<QString>& events);
  void configReloaded();                                   // reconcile, never override rules

private:
  void openEventSocket();
  void onEventData();
  void onEventLost();
  void dispatchWindow(const WindowRef& w, const QByteArray& lua, ReplyFn cb);

  HyprPaths m_paths;
  QLocalSocket* m_event = nullptr;
  QByteArray m_evBuf;
  QTimer m_backoff, m_coalesce;
  int m_backoffMs = 100;
  bool m_running = false, m_connected = false, m_everConnected = false;
  quint64 m_generation = 0;
  QSet<QString> m_pending;
};

} // namespace hn::platform
