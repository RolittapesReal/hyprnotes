#include "hn/mods/mods.h"
#include "hyprnotes/mod_api.h"
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSysInfo>

namespace hn::mods {

static const QRegularExpression &idRe() {
    static const QRegularExpression r(QStringLiteral("^[a-z0-9][a-z0-9._-]{0,63}$"));
    return r;
}

bool parseManifest(const QByteArray &json, const QString &dir, Manifest *out, QList<ModError> *errs) {
    QString id = QFileInfo(dir).fileName();
    auto fail = [&](const QString &m) { if (errs) errs->append({id, m}); return false; };
    QJsonParseError pe;
    const auto doc = QJsonDocument::fromJson(json, &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) return fail(QStringLiteral("mod.json: not a valid JSON object"));
    const auto o = doc.object();
    Manifest m;
    m.dir = dir;
    auto str = [&](const char *k, QString *dst, bool required = true) {
        const auto v = o.value(QLatin1String(k));
        if (v.isString() && !v.toString().isEmpty()) { *dst = v.toString(); return true; }
        if (!required && v.isUndefined()) return true;
        return fail(QStringLiteral("mod.json: field '%1' missing or not a non-empty string").arg(QLatin1String(k)));
    };
    if (!str("id", &m.id) || !str("name", &m.name) || !str("version", &m.version) || !str("arch", &m.arch)
        || !str("library", &m.library) || !str("entry", &m.entry, false)) return false;
    if (!idRe().match(m.id).hasMatch()) return fail(QStringLiteral("mod.json: invalid id '%1'").arg(m.id));
    if (m.id != id) return fail(QStringLiteral("mod.json: id '%1' does not match directory name").arg(m.id));
    const auto hv = o.value(QLatin1String("host_api_version"));
    if (!hv.isDouble() || hv.toDouble() != int(hv.toDouble())) return fail(QStringLiteral("mod.json: host_api_version must be an integer"));
    m.hostApiVersion = hv.toInt();
    if (m.hostApiVersion != int(HN_MOD_API_VERSION))
        return fail(QStringLiteral("incompatible host_api_version %1 (host supports %2)").arg(m.hostApiVersion).arg(HN_MOD_API_VERSION));
    if (m.arch != QSysInfo::buildCpuArchitecture())
        return fail(QStringLiteral("unsupported arch '%1' (host is %2)").arg(m.arch, QSysInfo::buildCpuArchitecture()));
    if (m.library.startsWith(QLatin1Char('/')) || m.library.contains(QStringLiteral("..")))
        return fail(QStringLiteral("mod.json: library must be a relative path inside the mod directory"));
    static const QRegularExpression ident(QStringLiteral("^[A-Za-z_][A-Za-z0-9_]*$"));
    if (!ident.match(m.entry).hasMatch()) return fail(QStringLiteral("mod.json: invalid entry symbol name"));
    const auto act = o.value(QLatin1String("activation"));
    if (!act.isArray() || act.toArray().isEmpty()) return fail(QStringLiteral("mod.json: activation must be a non-empty array"));
    for (const auto &a : act.toArray()) {
        const QString s = a.toString();
        const bool okCmd = s.startsWith(QStringLiteral("on-command:")) && idRe().match(s.mid(11)).hasMatch();
        if (!(s == QStringLiteral("on-startup") || s == QStringLiteral("on-note-open") || okCmd))
            return fail(QStringLiteral("mod.json: unknown activation condition '%1'").arg(s));
        m.activation << s;
    }
    const auto cmds = o.value(QLatin1String("commands"));
    if (!cmds.isUndefined()) {
        if (!cmds.isArray()) return fail(QStringLiteral("mod.json: commands must be an array"));
        for (const auto &c : cmds.toArray()) {
            const auto co = c.toObject();
            const QString cid = co.value(QLatin1String("id")).toString();
            if (!idRe().match(cid).hasMatch()) return fail(QStringLiteral("mod.json: invalid command id"));
            m.commands.append({cid, co.value(QLatin1String("title")).toString(cid), m.id});
        }
    }
    *out = m;
    return true;
}

QList<Manifest> scanManifests(const QString &modsDir, QList<ModError> *errs) {
    QList<Manifest> res;
    const QDir root(modsDir);
    for (const auto &fi : root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        QFile f(fi.absoluteFilePath() + QStringLiteral("/mod.json"));
        if (!f.open(QIODevice::ReadOnly)) {
            if (errs) errs->append({fi.fileName(), QStringLiteral("mod.json not readable")});
            continue;
        }
        Manifest m;
        if (f.size() > (1 << 20)) { if (errs) errs->append({fi.fileName(), QStringLiteral("mod.json too large")}); continue; }
        if (parseManifest(f.readAll(), fi.absoluteFilePath(), &m, errs)) res.append(m);
    }
    return res;
}

QStringList loadEnabled(const QString &path, QList<ModError> *errs) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    const auto doc = QJsonDocument::fromJson(f.readAll());
    const auto arr = doc.object().value(QLatin1String("enabled"));
    if (!doc.isObject() || !arr.isArray()) {
        if (errs) errs->append({QString(), QStringLiteral("enabled list malformed, treating all mods as disabled")});
        return {};
    }
    QStringList l;
    for (const auto &v : arr.toArray()) if (v.isString()) l << v.toString();
    return l;
}

bool saveEnabled(const QString &path, const QStringList &ids) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(QJsonDocument(QJsonObject{{QStringLiteral("version"), 1}, {QStringLiteral("enabled"), QJsonArray::fromStringList(ids)}}).toJson());
    return f.commit();
}

} // namespace hn::mods
