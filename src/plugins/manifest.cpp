#include "hn/plugins/types.h"
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QUrl>

namespace hn::plugins {

QStringList knownPermissions() {
    return {"note.read", "note.edit", "notes.read", "notes.write", "ui", "storage", "clipboard", "network", "theme", "native",
            "notes.index", "ui.panel", "editor.complete", "editor.links"};
}
int permissionMinApi(const QString &p) {
    static const QSet<QString> v2{"notes.index", "ui.panel", "editor.complete", "editor.links"};
    return v2.contains(p) ? 2 : 1;
}
bool isDangerousPermission(const QString &p) {
    static const QSet<QString> d{"network", "notes.write", "clipboard", "theme", "native"};
    return d.contains(p);
}
QString permissionDescription(const QString &p) {
    static const QHash<QString, QString> d{
        {"note.read", "Read the text, selection, title and tags of the open note"},
        {"note.edit", "Change the open note (replace selection, insert, replace all text)"},
        {"notes.read", "List and read every note in your library"},
        {"notes.write", "Create, overwrite and DELETE notes in your library"},
        {"ui", "Show notifications and ask you questions"},
        {"storage", "Keep up to 1 MiB of its own data"},
        {"clipboard", "Read and replace your clipboard contents"},
        {"network", "Send and receive data over HTTPS with the hosts it lists"},
        {"theme", "Change theme colours"},
        {"native", "Run native code with full access to your account (not sandboxed)"},
        {"notes.index", "Read link, tag and task data about all notes"},
        {"ui.panel", "Show its own panel next to your notes"},
        {"editor.complete", "Offer completion suggestions while you type"},
        {"editor.links", "Handle clicks on [[links]] in your notes"}};
    return d.value(p, QStringLiteral("Unknown permission"));
}

QList<PermissionInfo> permissionTable() {
    static const QHash<QString, QString> risk{{"note.read", "medium"}, {"note.edit", "medium"}, {"notes.read", "medium"}, {"notes.write", "high"},
                                              {"ui", "low"}, {"storage", "low"}, {"clipboard", "high"}, {"network", "high"}, {"theme", "medium"}, {"native", "critical"},
                                              {"notes.index", "medium"}, {"ui.panel", "low"}, {"editor.complete", "low"}, {"editor.links", "medium"}};
    QList<PermissionInfo> t;
    for (const auto &p : knownPermissions()) t.append({p, permissionDescription(p), risk.value(p, QStringLiteral("high")), isDangerousPermission(p)});
    return t;
}
QString describePermission(const QString &p) { return permissionDescription(p); }
bool isDangerous(const QString &p) { return isDangerousPermission(p); }
bool isHostAllowed(const QString &url, const QStringList &hosts) { return checkHttpUrl(url, hosts, nullptr, nullptr); }

bool validPluginId(const QString &id) {
    static const QRegularExpression re(QStringLiteral("^[a-z0-9][a-z0-9._-]{0,63}$"));
    return re.match(id).hasMatch() && !id.contains(QStringLiteral(".."));
}

bool validHostName(const QString &h) {
    if (h.isEmpty() || h.size() > 253 || h != h.toLower()) return false;
    static const QRegularExpression label(QStringLiteral("^[a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?$"));
    const auto parts = h.split(QLatin1Char('.'));
    if (parts.size() < 2) return false;  // also rejects "localhost"
    for (const auto &p : parts)
        if (!label.match(p).hasMatch()) return false;
    static const QRegularExpression digits(QStringLiteral("^[0-9]+$"));
    if (digits.match(parts.last()).hasMatch()) return false;  // IPv4 literal
    for (const char *bad : {"local", "localhost", "internal", "lan", "home", "corp"})
        if (parts.last() == QLatin1String(bad)) return false;
    return true;
}

static QList<int> verParts(const QString &v) {
    QList<int> r;
    for (const auto &p : v.section(QLatin1Char('-'), 0, 0).split(QLatin1Char('.'))) r << p.toInt();
    while (r.size() < 3) r << 0;
    return r;
}
int compareVersions(const QString &a, const QString &b) {
    const auto x = verParts(a), y = verParts(b);
    for (int i = 0; i < 3; ++i)
        if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    return 0;
}

static bool hasCtl(const QString &s, bool allowNl = false) {
    for (QChar c : s)
        if (c.unicode() < 0x20 && !(allowNl && c == QLatin1Char('\n'))) return true;
        else if (c.unicode() == 0x7f) return true;
    return false;
}

static bool relPathOk(const QString &p) {
    if (p.isEmpty() || p.size() > 128 || p.startsWith(QLatin1Char('/')) || p.contains(QLatin1Char('\\')) || hasCtl(p)) return false;
    if (p.size() > 1 && p[1] == QLatin1Char(':')) return false;
    for (const auto &c : p.split(QLatin1Char('/')))
        if (c.isEmpty() || c == QLatin1String(".") || c == QLatin1String("..")) return false;
    return true;
}

bool parseManifest(const QByteArray &json, const QString &dir, Manifest *out, QList<PluginError> *errs, const QString &appVersion) {
    QList<PluginError> local;
    QString who = QFileInfo(dir).fileName();
    auto err = [&](const QString &m) { local.append({who, m}); };
    QJsonParseError pe;
    const auto doc = QJsonDocument::fromJson(json, &pe);
    if (json.size() > 64 * 1024) err(QStringLiteral("plugin.json is larger than 64 KiB"));
    else if (pe.error != QJsonParseError::NoError || !doc.isObject())
        err(QStringLiteral("plugin.json is not valid JSON (an object is required)"));
    if (!local.isEmpty()) { if (errs) *errs += local; return false; }
    const auto o = doc.object();
    Manifest m;
    m.dir = dir;
    auto reqStr = [&](const char *k, QString &dst, int maxLen, bool nl = false) {
        const auto v = o.value(QLatin1String(k));
        if (!v.isString() || v.toString().trimmed().isEmpty()) {
            err(QStringLiteral("field '%1' is required and must be a non-empty string").arg(QLatin1String(k)));
            return;
        }
        dst = v.toString();
        if (dst.size() > maxLen) err(QStringLiteral("field '%1' is longer than %2 characters").arg(QLatin1String(k)).arg(maxLen));
        if (hasCtl(dst, nl)) err(QStringLiteral("field '%1' contains control characters").arg(QLatin1String(k)));
    };
    reqStr("id", m.id, 64);
    if (!m.id.isEmpty() && !validPluginId(m.id))
        err(QStringLiteral("id '%1' is invalid: use 1-64 characters from a-z 0-9 . _ - starting with a letter or digit").arg(m.id));
    else if (!m.id.isEmpty()) who = m.id;
    reqStr("name", m.name, 80);
    reqStr("version", m.version, 40);
    static const QRegularExpression verRe(QStringLiteral("^[0-9]{1,6}\\.[0-9]{1,6}\\.[0-9]{1,6}(-[0-9A-Za-z.-]{1,32})?$"));
    if (!m.version.isEmpty() && !verRe.match(m.version).hasMatch())
        err(QStringLiteral("version '%1' must look like 1.2.3").arg(m.version));
    reqStr("author", m.author, 80);
    reqStr("description", m.description, 500, true);
    if (const auto v = o.value(QLatin1String("homepage")); !v.isUndefined()) {
        const QUrl u(v.toString());
        if (!v.isString() || !u.isValid() || u.host().isEmpty() || (u.scheme() != QLatin1String("https") && u.scheme() != QLatin1String("http")) || hasCtl(v.toString()))
            err(QStringLiteral("homepage must be an http(s) URL"));
        else m.homepage = v.toString();
    }
    // api
    const auto av = o.value(QLatin1String("api"));
    if (!av.isDouble() || av.toDouble() != double(int(av.toDouble()))) err(QStringLiteral("field 'api' must be the integer %1 or %2").arg(kApiVersion).arg(kApiVersionMax));
    else {
        m.api = av.toInt();
        if (m.api > kApiVersionMax) err(QStringLiteral("plugin needs plugin API %1 but this app supports API %2").arg(m.api).arg(kApiVersionMax));
        else if (m.api < kApiVersion) err(QStringLiteral("unsupported plugin API %1 (this app supports API %2 to %3)").arg(m.api).arg(kApiVersion).arg(kApiVersionMax));
    }
    // tier
    const auto tv = o.value(QLatin1String("tier"));
    if (tv.toString() == QLatin1String("script")) m.tier = Tier::Script;
    else if (tv.toString() == QLatin1String("native")) m.tier = Tier::Native;
    else err(QStringLiteral("field 'tier' must be \"script\" or \"native\""));
    // entry
    const auto ev = o.value(QLatin1String("entry"));
    if (ev.isUndefined() && m.tier == Tier::Script) m.entry = QStringLiteral("main.lua");
    else if (!ev.isString() || !relPathOk(ev.toString()))
        err(QStringLiteral("field 'entry' must be a relative path inside the plugin folder (no '..', no leading '/')"));
    else {
        m.entry = ev.toString();
        if (m.tier == Tier::Script && !m.entry.endsWith(QLatin1String(".lua"))) err(QStringLiteral("script plugin entry must be a .lua file"));
    }
    // permissions
    const auto pv = o.value(QLatin1String("permissions"));
    if (!pv.isArray()) err(QStringLiteral("field 'permissions' must be an array (use [] for none)"));
    else {
        const auto known = knownPermissions();
        for (const auto &x : pv.toArray()) {
            const QString s = x.toString();
            if (!x.isString() || !known.contains(s)) err(QStringLiteral("unknown permission '%1' (known: %2)").arg(x.isString() ? s : QStringLiteral("<non-string>"), known.join(QStringLiteral(", "))));
            else if (m.permissions.contains(s)) err(QStringLiteral("permission '%1' is listed twice").arg(s));
            else if (m.api >= kApiVersion && permissionMinApi(s) > m.api) err(QStringLiteral("permission '%1' needs \"api\": %2 in plugin.json").arg(s).arg(permissionMinApi(s)));
            else m.permissions << s;
        }
        if (m.tier == Tier::Script && m.permissions.contains(QStringLiteral("native"))) err(QStringLiteral("script plugins cannot request the 'native' permission"));
        if (m.tier == Tier::Native && !m.permissions.contains(QStringLiteral("native"))) err(QStringLiteral("native plugins must list the 'native' permission"));
    }
    // net_hosts
    const auto nv = o.value(QLatin1String("net_hosts"));
    if (!nv.isUndefined()) {
        if (!nv.isArray()) err(QStringLiteral("field 'net_hosts' must be an array of host names"));
        else {
            if (nv.toArray().size() > 16) err(QStringLiteral("net_hosts lists more than 16 hosts"));
            for (const auto &x : nv.toArray()) {
                const QString h = x.toString();
                if (!x.isString() || !validHostName(h))
                    err(QStringLiteral("net_hosts entry '%1' is not a valid public lowercase host name (no scheme, port, path, wildcard, IP address or local name)").arg(x.isString() ? h : QStringLiteral("<non-string>")));
                else if (m.netHosts.contains(h)) err(QStringLiteral("net_hosts lists '%1' twice").arg(h));
                else m.netHosts << h;
            }
        }
    }
    const bool net = m.permissions.contains(QStringLiteral("network"));
    if (net && m.netHosts.isEmpty()) err(QStringLiteral("permission 'network' requires at least one host in net_hosts"));
    if (!net && !m.netHosts.isEmpty()) err(QStringLiteral("net_hosts is set but permission 'network' is not requested"));
    // min_app
    if (const auto v = o.value(QLatin1String("min_app")); !v.isUndefined()) {
        if (!v.isString() || !verRe.match(v.toString()).hasMatch()) err(QStringLiteral("min_app must look like 0.1.0"));
        else {
            m.minApp = v.toString();
            if (compareVersions(m.minApp, appVersion) > 0)
                err(QStringLiteral("plugin requires Hyprnotes %1 or newer (this is %2)").arg(m.minApp, appVersion));
        }
    }
    if (!local.isEmpty()) { if (errs) *errs += local; return false; }
    if (out) *out = m;
    return true;
}

static QString resolve(const char *override_, const char *xdg, const char *homeRel) {
    const QString o = qEnvironmentVariable(override_);
    if (!o.isEmpty()) return QDir::cleanPath(o);
    const QString x = qEnvironmentVariable(xdg);
    if (!x.isEmpty() && QDir::isAbsolutePath(x)) return QDir::cleanPath(x) + QStringLiteral("/hyprnotes");
    return QDir::homePath() + QLatin1Char('/') + QLatin1String(homeRel) + QStringLiteral("/hyprnotes");
}
QString pluginsDir() { return resolve("HN_DATA_DIR", "XDG_DATA_HOME", ".local/share") + QStringLiteral("/plugins"); }
QString stateDir() { return resolve("HN_STATE_DIR", "XDG_STATE_HOME", ".local/state"); }

bool checkHttpUrl(const QString &url, const QStringList &allowed, QString *host, QString *err) {
    auto fail = [&](const QString &m) { if (err) *err = m; return false; };
    if (url.size() > 2048) return fail(QStringLiteral("URL too long"));
    if (hasCtl(url) || url.contains(QLatin1Char(' '))) return fail(QStringLiteral("URL contains control characters or spaces"));
    const QUrl u(url, QUrl::StrictMode);
    if (!u.isValid()) return fail(QStringLiteral("invalid URL"));
    if (url.left(8).toLower() != QLatin1String("https://") || u.scheme() != QLatin1String("https")) return fail(QStringLiteral("only https:// URLs are allowed"));
    if (!u.userInfo().isEmpty()) return fail(QStringLiteral("URLs with credentials are not allowed"));
    if (u.port() != -1 && u.port() != 443) return fail(QStringLiteral("only the default https port is allowed"));
    const QString h = u.host(QUrl::EncodeUnicode);
    if (h.isEmpty()) return fail(QStringLiteral("URL has no host"));
    if (!allowed.contains(h)) return fail(QStringLiteral("host '%1' is not listed in net_hosts").arg(h));
    if (host) *host = h;
    return true;
}

}  // namespace hn::plugins
