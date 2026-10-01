#include "hn/platform/autostart.h"
#include <QDir>
#include <QFile>
#include <QSaveFile>

namespace hn::platform {

Autostart::Autostart(QString dir, QString exec) : m_dir(std::move(dir)), m_exec(std::move(exec)) {}

QString Autostart::defaultDir() {
  QString base = qEnvironmentVariable("XDG_CONFIG_HOME");
  if (base.isEmpty()) base = QDir::homePath() + QStringLiteral("/.config");
  return base + QStringLiteral("/autostart");
}

Autostart::State Autostart::state() const {
  QFile f(filePath());
  if (!f.exists()) return State::Off;
  if (!f.open(QIODevice::ReadOnly)) return State::ExternallyManaged;   // unreadable: do not touch
  for (const QByteArray& l : f.readAll().split('\n'))
    if (l.trimmed() == "X-Hyprnotes-Managed=true") return State::EnabledManaged;
  return State::ExternallyManaged;
}

Autostart::Result Autostart::enable() {
  switch (state()) {
  case State::EnabledManaged: return {true, State::EnabledManaged, {}};
  case State::ExternallyManaged:
    return {false, State::ExternallyManaged, QObject::tr("Startup is managed externally (%1). Hyprnotes left it unchanged.").arg(filePath())};
  case State::Off: break;
  }
  if (!QDir().mkpath(m_dir)) return {false, State::Off, QObject::tr("Cannot create %1").arg(m_dir)};
  QSaveFile f(filePath());
  if (!f.open(QIODevice::WriteOnly))
    return {false, State::Off, QObject::tr("Cannot write %1: %2").arg(filePath(), f.errorString())};
  f.write("[Desktop Entry]\nType=Application\nName=Hyprnotes\nComment=Start Hyprnotes in the background\nExec=" +
          m_exec.toUtf8() + "\nTerminal=false\nX-Hyprnotes-Managed=true\n");
  if (!f.commit()) return {false, State::Off, QObject::tr("Cannot write %1: %2").arg(filePath(), f.errorString())};
  return {true, State::EnabledManaged, {}};
}

Autostart::Result Autostart::disable() {
  switch (state()) {
  case State::Off: return {true, State::Off, {}};
  case State::ExternallyManaged:
    return {false, State::ExternallyManaged, QObject::tr("Startup is managed externally (%1). Hyprnotes left it unchanged.").arg(filePath())};
  case State::EnabledManaged: break;
  }
  if (!QFile::remove(filePath())) return {false, State::EnabledManaged, QObject::tr("Cannot remove %1").arg(filePath())};
  return {true, State::Off, {}};
}

} // namespace hn::platform
