#include "hn/platform/window_identity.h"
#include <QRandomGenerator>
#include <QWidget>

namespace hn::platform {

QString roleName(Role r) { return r == Role::Organizer ? QStringLiteral("hyprnotes-organizer") : QStringLiteral("hyprnotes-sticky"); }

WindowIdentity WindowIdentity::create(Role r) {
  const quint64 v = QRandomGenerator::system()->generate64();
  return {r, QStringLiteral("%1").arg(v, 16, 16, QLatin1Char('0'))};
}
QString WindowIdentity::initialTitle() const { return roleName(role) + QLatin1Char(':') + token; }

std::optional<WindowIdentity> WindowIdentity::parseInitialTitle(const QString& t) {
  const int c = t.indexOf(QLatin1Char(':'));
  if (c < 0) return std::nullopt;
  // Tolerate a trailing suffix after the token (e.g. " - Hyprnotes" appended by a display name).
  static const QRegularExpression re(QStringLiteral("^([0-9a-f]{16})(?![0-9A-Za-z])"));
  const auto m = re.match(t.mid(c + 1));
  if (!m.hasMatch()) return std::nullopt;
  const QString r = t.left(c), tok = m.captured(1);
  if (r == roleName(Role::Organizer)) return WindowIdentity{Role::Organizer, tok};
  if (r == roleName(Role::Sticky)) return WindowIdentity{Role::Sticky, tok};
  return std::nullopt;
}

void applyIdentity(QWidget* w, const WindowIdentity& id) {
  w->setObjectName(id.token);
  w->setProperty("hn.role", roleName(id.role));
  w->setProperty("hn.token", id.token);
  w->setWindowTitle(id.initialTitle());
}

QHash<QString, HyprClient> resolveClients(const QVector<HyprClient>& all, qint64 pid, const QString& appClass) {
  QHash<QString, HyprClient> out;
  for (const auto& c : all) {
    if (c.pid != pid || c.initialClass != appClass) continue;
    if (auto id = WindowIdentity::parseInitialTitle(c.initialTitle)) out.insert(id->token, c);
  }
  return out;
}

} // namespace hn::platform
