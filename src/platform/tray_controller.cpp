#include "hn/platform/tray_controller.h"
#include <QGuiApplication>

namespace hn::platform {

TrayController::TrayController(const QIcon& icon, QObject* parent) : QObject(parent), m_icon(icon) {
  m_new = m_menu.addAction(tr("New Note"));
  m_show = m_menu.addAction(tr("Show Organizer"));
  m_menu.addSeparator();
  m_quit = m_menu.addAction(tr("Quit"));
  connect(m_new, &QAction::triggered, this, &TrayController::newNoteRequested);
  connect(m_show, &QAction::triggered, this, &TrayController::showOrganizerRequested);
  connect(m_quit, &QAction::triggered, this, &TrayController::quitRequested);
  m_icon.setContextMenu(&m_menu);
  m_icon.setToolTip(QStringLiteral("Hyprnotes"));
  connect(&m_icon, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason r) {
    refreshAvailability();
    if (r == QSystemTrayIcon::Trigger || r == QSystemTrayIcon::DoubleClick) emit showOrganizerRequested();
  });
  // Passive, event-driven re-check (no timer).
  connect(qGuiApp, &QGuiApplication::applicationStateChanged, this, [this] { refreshAvailability(); });
}

void TrayController::setEnabled(bool on, bool organizerHidden) {
  if (on == m_enabled) return;
  if (!on && organizerHidden) emit showOrganizerRequested();
  m_enabled = on;
  if (on) m_icon.show(); else m_icon.hide();
  refreshAvailability();
}

void TrayController::refreshAvailability() {
  const bool a = m_enabled && QSystemTrayIcon::isSystemTrayAvailable();
  if (a == m_available) return;
  m_available = a;
  emit availabilityChanged(a);
}

} // namespace hn::platform
