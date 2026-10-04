#include "hn/plugins/market.h"

namespace hn::plugins {

Preflight preflightPackage(const QString &file, const MarketEntry &e, const QString &appVersion) {
    Preflight p;
    auto fail = [&](const QString &m) { p.ok = false; p.error = m; return p; };
    const QString nativeMsg = QStringLiteral("Native plugins cannot be installed from Browse yet: they need signed releases.");
    if (e.native()) return fail(nativeMsg);   // first, so no file is read for a native entry
    p.check = checkPackage(file, appVersion);
    if (!p.check.ok) return fail(p.check.errors.isEmpty() ? QStringLiteral("The package is not a valid plugin.") : p.check.errors.first().message);
    const Manifest &m = p.check.manifest;
    auto sameSet = [](QStringList a, QStringList b) { a.sort(); b.sort(); return a == b; };
    if (m.id != e.id) return fail(QStringLiteral("The package contains plugin '%1', not '%2'.").arg(m.id, e.id));
    if (m.version != e.version) return fail(QStringLiteral("The package is version %1, the index says %2.").arg(m.version, e.version));
    if (m.tier != Tier::Script) return fail(nativeMsg);
    if (m.api != e.api) return fail(QStringLiteral("The package's API version differs from the index."));
    if (!sameSet(m.permissions, e.permissions)) return fail(QStringLiteral("The package asks for different permissions than the index lists."));
    if (!sameSet(m.netHosts, e.netHosts)) return fail(QStringLiteral("The package's network hosts differ from the index."));
    p.ok = true;
    return p;
}

}  // namespace hn::plugins
