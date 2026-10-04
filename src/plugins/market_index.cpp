#include "hn/plugins/market.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <cmath>

namespace hn::plugins {

bool httpsOnly(const QUrl &u) {
    return u.isValid() && u.scheme() == QLatin1String("https") && !u.host().isEmpty() && u.userInfo().isEmpty()
           && (u.port() == -1 || u.port() == 443);
}

namespace {

bool plainText(const QString &s, bool allowNewline = false) {
    for (const QChar c : s)
        if ((c.unicode() < 0x20 && !(allowNewline && c == QLatin1Char('\n'))) || c.unicode() == 0x7f) return false;
    return true;
}

bool str(const QJsonObject &o, const char *k, int maxLen, QString *dst, bool allowNewline = false) {
    const auto v = o.value(QLatin1String(k));
    if (v.isUndefined() || v.isNull()) return false;
    if (!v.isString()) return false;
    *dst = v.toString();
    return !dst->trimmed().isEmpty() && dst->size() <= maxLen && plainText(*dst, allowNewline);
}

bool strList(const QJsonObject &o, const char *k, int maxItems, int maxLen, QStringList *dst) {
    const auto v = o.value(QLatin1String(k));
    if (v.isUndefined()) return true;
    if (!v.isArray()) return false;
    const auto a = v.toArray();
    if (a.size() > maxItems) return false;
    for (const auto &x : a) {
        if (!x.isString() || x.toString().size() > maxLen || x.toString().isEmpty() || !plainText(x.toString())) return false;
        dst->append(x.toString());
    }
    return true;
}

bool knownPermission(const QString &p) {
    static const QSet<QString> known = [] { QSet<QString> s; for (const auto &i : permissionTable()) s << i.name; return s; }();
    return known.contains(p);
}

bool parseEntry(const QJsonObject &o, const UrlPolicy &policy, MarketEntry *e) {
    static const QRegularExpression shaRe(QRegularExpression::anchoredPattern(QStringLiteral("[0-9a-fA-F]{64}")));
    static const QRegularExpression verRe(QRegularExpression::anchoredPattern(QStringLiteral("[0-9]{1,6}\\.[0-9]{1,6}\\.[0-9]{1,6}(-[0-9A-Za-z.-]{1,32})?")));
    if (!str(o, "id", 64, &e->id) || !validPluginId(e->id)) return false;
    if (!str(o, "name", 80, &e->name) || !str(o, "version", 40, &e->version) || !verRe.match(e->version).hasMatch()) return false;
    if (!str(o, "author", 80, &e->author) || !str(o, "description", 500, &e->description, true)) return false;
    if (!str(o, "tier", 16, &e->tier) || (e->tier != QLatin1String("script") && e->tier != QLatin1String("native"))) return false;
    if (!str(o, "min_app", 40, &e->minApp) || !verRe.match(e->minApp).hasMatch()) return false;
    const int api = o.value(QLatin1String("api")).toInt(0);
    if (api != 1 && api != 2) return false;
    e->api = api;
    if (!str(o, "url", 512, &e->url) || !policy(QUrl(e->url, QUrl::StrictMode))) return false;
    if (!str(o, "sha256", 64, &e->sha256) || !shaRe.match(e->sha256).hasMatch()) return false;
    e->sha256 = e->sha256.toLower();
    const double size = o.value(QLatin1String("size")).toDouble(0);
    if (size < 1 || size != std::floor(size) || size > double(kMaxMarketPackageBytes)) return false;
    e->size = qint64(size);
    if (!strList(o, "tags", 8, 24, &e->tags) || !strList(o, "permissions", 32, 40, &e->permissions) || !strList(o, "net_hosts", 16, 253, &e->netHosts)) return false;
    if (!e->native()) for (const auto &p : e->permissions) if (!knownPermission(p)) return false;
    for (const auto &h : e->netHosts) if (!validHostName(h)) return false;
    return true;
}

}  // namespace

bool parseMarketIndex(const QByteArray &json, MarketIndex *out, QString *err, const UrlPolicy &policy) {
    auto fail = [&](const QString &m) { if (err) *err = m; return false; };
    if (json.size() > kMaxIndexBytes) return fail(QStringLiteral("the plugin index is larger than %1 MiB").arg(kMaxIndexBytes >> 20));
    QJsonParseError pe;
    const auto doc = QJsonDocument::fromJson(json, &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) return fail(QStringLiteral("the plugin index is not valid JSON"));
    const auto root = doc.object();
    if (root.value(QLatin1String("schema")).toInt(0) != 1) return fail(QStringLiteral("unsupported plugin index version"));
    const auto plugins = root.value(QLatin1String("plugins"));
    if (!plugins.isArray()) return fail(QStringLiteral("the plugin index has no plugin list"));
    MarketIndex ix;
    ix.updated = root.value(QLatin1String("updated")).toString().left(40);
    QSet<QString> seen;
    const UrlPolicy effectivePolicy = policy ? policy : UrlPolicy(httpsOnly);
    for (const auto &v : plugins.toArray()) {
        MarketEntry e;
        if (!v.isObject() || !parseEntry(v.toObject(), effectivePolicy, &e) || seen.contains(e.id)) { ++ix.skipped; continue; }
        if (ix.entries.size() >= kMaxIndexEntries) { ++ix.skipped; continue; }
        seen << e.id;
        ix.entries << e;
    }
    *out = std::move(ix);
    return true;
}

MarketState marketState(const MarketEntry &e, const QString &installedVersion) {
    if (installedVersion.isEmpty()) return MarketState::NotInstalled;
    const int c = compareVersions(e.version, installedVersion);
    return c == 0 ? MarketState::Installed : c > 0 ? MarketState::UpdateAvailable : MarketState::InstalledNewer;
}

}  // namespace hn::plugins
