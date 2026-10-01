#pragma once
// Session-scoped single-instance transport (spec 4.2, desktop design section 3).
// Wire protocol v1, one request per connection, newline-terminated, <= kMaxLine bytes:
//   client -> "HNIPC <ver> <show-organizer|new-note|background>\n"
//   server -> "HNIPC 1 ok\n" | "HNIPC 1 err <reason>\n"
// Anything else (garbage, oversize, slow) is dropped. Commands are an enum, never shell/code.
#include <QObject>
#include <QString>
#include <QLocalServer>
#include <functional>

namespace hn::platform {

enum class Action { ShowOrganizer, NewNote, Background };
constexpr int kProtocolVersion = 1;
constexpr int kMaxLine = 128;
constexpr int kAckDeadlineMs = 1000;

QString actionName(Action a);

// $XDG_RUNTIME_DIR/hyprnotes-<WAYLAND_DISPLAY>-<uid>.sock ; empty if the env is unusable.
QString defaultEndpointPath();

class InstanceServer : public QObject {
  Q_OBJECT
public:
  enum class Result { Listening, AlreadyRunning, Failed };
  // handler runs on the GUI thread before the ack is sent; return false to nack.
  using Handler = std::function<bool(Action)>;
  explicit InstanceServer(QString endpointPath = defaultEndpointPath(), QObject* parent = nullptr);
  void setHandler(Handler h) { m_handler = std::move(h); }
  // Race-safe: serialized by a lock file, connect-probes an existing endpoint first and only
  // removes it when nothing answers (stale). A live instance's endpoint is never deleted.
  Result listen(QString* error = nullptr);
  QString endpointPath() const { return m_path; }
private:
  void onConnection();
  QString m_path;
  QLocalServer m_server;
  Handler m_handler;
};

struct ClientResult {
  enum class Status {
    Acked,             // action performed; client may exit 0
    NoInstance,        // nothing listening (safe to start as first instance)
    UnsafeEndpoint,    // endpoint not ours / not 0600 / not a socket: refused
    UnknownCompletion, // request sent, no ack in time. NEVER replay NewNote.
    Incompatible,      // peer speaks another protocol version
    Rejected,          // peer refused / failed the action
    Unavailable        // connect/handshake failed before the request was sent
  } status = Status::Unavailable;
  QString message;   // actionable, user-facing
};

// Blocking (local event loop) send used by the transient launcher process.
ClientResult sendAction(Action a, const QString& endpointPath = defaultEndpointPath(),
                        int deadlineMs = kAckDeadlineMs, int protocolVersion = kProtocolVersion);
// True if the endpoint path is a socket owned by us with no group/other access.
bool endpointIsSafe(const QString& path, QString* why = nullptr);

} // namespace hn::platform
