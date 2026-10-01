#pragma once
// Geometry save/restore + workspace modes (spec 7.1). Pure planning (clamp) is separated from
// applying (async IPC) so the logic is testable without a compositor.
#include "hn/platform/hyprland_ipc.h"
#include "hn/platform/window_identity.h"
#include <QString>
#include <optional>

namespace hn::platform {

enum class WorkspaceMode { ThisWorkspace, AllWorkspaces /* Hyprland pin */ };

// Keep-above is a distinct stacking capability; a one-time raise is not persistent. Not proven
// for plain floating toplevels on Hyprland, so report false (UI must hide the option).
constexpr bool supportsKeepAbove() { return false; }

struct WindowState {
  QString noteKey;                    // app-defined note identity (path/id), never the title
  Role role = Role::Sticky;
  QRect geometry;                     // layout px
  QString monitor;                    // name
  int workspace = 0;                  // 0 = unknown
  double scale = 1;
  WorkspaceMode mode = WorkspaceMode::ThisWorkspace;
};

struct SessionState {
  int version = 1;
  QVector<WindowState> windows;
};

QString defaultSessionPath();                        // $XDG_STATE_HOME/hyprnotes/session.json
QByteArray toJson(const SessionState& s);
SessionState fromJson(const QByteArray& j, bool* ok = nullptr);   // tolerant; ok=false on corrupt
bool saveSession(const QString& path, const SessionState& s, QString* err = nullptr);   // atomic
SessionState loadSession(const QString& path, bool* ok = nullptr);

WindowState stateFromClient(const HyprClient& c, const QString& noteKey, Role role,
                            const QVector<HyprMonitor>& monitors);

struct PlacementPlan {
  QRect rect;
  std::optional<int> workspace;       // only if it still exists
  bool pinned = false;
  bool monitorMissing = false;        // fell back to another monitor
};
// Clamp into the work area of the saved monitor (or the best fallback). Size is shrunk if it
// no longer fits. Logical size is preserved across scale changes (Hyprland and Qt both use
// logical px), then clamped.
PlacementPlan planPlacement(const WindowState& s, const QVector<HyprMonitor>& monitors,
                            const QVector<HyprWorkspace>& workspaces);
QRect clampToWorkArea(QRect r, const QRect& work);

// Applies a plan to a live window: float on, exact size/position, workspace, pin.
// All operations are idempotent sets; replies with unknownState should be reconciled by caller.
void applyPlacement(HyprlandIpc& ipc, const WindowRef& w, const PlacementPlan& p,
                    std::function<void(bool allOk)> done = {});
void setWorkspaceMode(HyprlandIpc& ipc, const WindowRef& w, WorkspaceMode m, HyprlandIpc::ReplyFn cb = {});

} // namespace hn::platform
