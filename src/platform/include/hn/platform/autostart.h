#pragma once
// Opt-in XDG autostart (desktop design section 5). Owns ONLY ~/.config/autostart/hyprnotes.desktop
// and only if it carries X-Hyprnotes-Managed=true. Default off = file absent.
#include <QString>

namespace hn::platform {

class Autostart {
public:
  enum class State { Off, EnabledManaged, ExternallyManaged };
  struct Result { bool ok = false; State state = State::Off; QString message; };
  explicit Autostart(QString autostartDir = defaultDir(), QString exec = QStringLiteral("hyprnotes --background"));
  static QString defaultDir();               // $XDG_CONFIG_HOME|~/.config + /autostart
  QString filePath() const { return m_dir + QStringLiteral("/hyprnotes.desktop"); }
  State state() const;
  Result enable();                           // refuses (ok=false) on external entry / unwritable dir
  Result disable();                          // removes only our managed entry
private:
  QString m_dir, m_exec;
};

} // namespace hn::platform
