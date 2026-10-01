#pragma once
// Window identity (spec 7.1).
// Qt cannot set a per-window Wayland app-id (it is per process: desktopFileName, class
// "hyprnotes"), and xdg-toplevel-tag is unsupported. So the role + immutable instance token are
// encoded in the FIRST title set before show():   hyprnotes-<role>:<token>
// Hyprland records it as `initialTitle`, which never changes when the app later calls
// setWindowTitle(noteTitle). Rules match `initial_title` (role) before the first frame, and the
// live client is found with (pid == ours, class == app-id, initialTitle token) -- title-independent.
// Token <-> note mapping is owned by WindowRegistry, so renames, duplicate titles and
// pop-in/pop-out (new window = new token rebound to the same note) are all unambiguous.
#include "hn/platform/hyprland_ipc.h"
#include <QHash>
#include <QMultiHash>
#include <optional>

class QWidget;

namespace hn::platform {

enum class Role { Organizer, Sticky };
QString roleName(Role r);          // "hyprnotes-organizer" / "hyprnotes-sticky"

struct WindowIdentity {
  Role role = Role::Sticky;
  QString token;                      // 16 hex chars, unique per window instance
  static WindowIdentity create(Role r);
  QString initialTitle() const;       // "hyprnotes-sticky:<token>"
  static std::optional<WindowIdentity> parseInitialTitle(const QString& t);
};

// Call BEFORE show(): sets objectName(token), properties hn.role/hn.token, and the initial title.
// Afterwards the app may freely setWindowTitle().
void applyIdentity(QWidget* w, const WindowIdentity& id);

// token -> live Hyprland client for our own process. Titles are never consulted.
QHash<QString, HyprClient> resolveClients(const QVector<HyprClient>& all, qint64 pid, const QString& appClass);

class WindowRegistry {
public:
  void bind(const QString& token, const QString& noteKey) { m_note.insert(token, noteKey); }
  void unbind(const QString& token) { m_note.remove(token); }
  QString noteFor(const QString& token) const { return m_note.value(token); }
  QStringList tokensFor(const QString& noteKey) const { return m_note.keys(noteKey); }
  // pop-out / pop-in: the old window is gone, a new window (new token) now shows the note.
  void rebind(const QString& oldToken, const QString& newToken) {
    const QString n = m_note.take(oldToken); if (!n.isEmpty()) m_note.insert(newToken, n); }
private:
  QHash<QString, QString> m_note;
};

} // namespace hn::platform
