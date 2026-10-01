#pragma once
// QSystemTrayIcon wrapper (desktop design section 4). No polling timer anywhere.
// Qt registers a visible icon by itself when a StatusNotifier host (Waybar tray) appears later,
// but QSystemTrayIcon exposes no "host appeared" signal and QtDBus is out of scope, so
// availability is re-evaluated on events (enable/disable, tray activation/menu use, app state
// change) and on explicit refreshAvailability(). Documented limitation.
#include <QIcon>
#include <QMenu>
#include <QObject>
#include <QSystemTrayIcon>

namespace hn::platform {

class TrayController : public QObject {
  Q_OBJECT
public:
  enum class ClosePolicy { HideToTray, CloseNormally };
  explicit TrayController(const QIcon& icon, QObject* parent = nullptr);
  bool enabled() const { return m_enabled; }
  bool available() const { return m_available; }
  // Organizer close: hide only when the tray is enabled AND usable, else close via save path.
  ClosePolicy closeOrganizerPolicy() const {
    return m_enabled && m_available ? ClosePolicy::HideToTray : ClosePolicy::CloseNormally; }
  // Disabling while the organizer is hidden emits showOrganizerRequested() BEFORE removing the
  // icon so the running app stays reachable.
  void setEnabled(bool on, bool organizerHidden = false);
  void refreshAvailability();
  QMenu* menu() { return &m_menu; }
  QAction* newNoteAction() { return m_new; }
  QAction* showOrganizerAction() { return m_show; }
  QAction* quitAction() { return m_quit; }

signals:
  void newNoteRequested();
  void showOrganizerRequested();
  void quitRequested();
  void availabilityChanged(bool available);

private:
  QSystemTrayIcon m_icon;
  QMenu m_menu;
  QAction *m_new, *m_show, *m_quit;
  bool m_enabled = false, m_available = false;
};

} // namespace hn::platform
