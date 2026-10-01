#include <hn/theme/theme_io.h>
#include "config.h"
#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>

namespace hn::theme {
namespace {
const QRegularExpression kName("^[A-Za-z0-9][A-Za-z0-9._-]{0,47}$");

bool writeAtomic(const QString &path, const QByteArray &bytes, QString *err) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(bytes) != bytes.size() || !f.commit()) {
        if (err) *err = QString("cannot write %1: %2").arg(path, f.errorString());
        return false;
    }
    return true;
}

QString hex6(QString s) {   // #rgb / #rrggbb / #rrggbbaa (alpha dropped) -> #rrggbb, "" if not a color
    s = s.trimmed();
    if (s.startsWith('#')) s.remove(0, 1);
    if (!QRegularExpression("^[0-9A-Fa-f]+$").match(s).hasMatch()) return {};
    if (s.size() == 3) s = QString(s[0]) + s[0] + s[1] + s[1] + s[2] + s[2];
    if (s.size() != 6 && s.size() != 8) return {};
    return '#' + s.left(6).toUpper();
}

double lum(const QColor &c) { return (contrastRatio(c, QColor(Qt::black)) - 1) * 0.05; }   // WCAG relative luminance

QString onColor(const QString &accent) {
    const QColor a(accent);
    return contrastRatio(a, QColor("#111111")) >= contrastRatio(a, QColor(Qt::white)) ? "#111111" : "#FFFFFF";
}

QJsonObject wrap(const QJsonObject &tokens, const QString &name) {
    return {{"version", 1}, {"name", name}, {(lum(QColor(tokens["bg"].toString())) < 0.3 ? "dark" : "light"), tokens}};
}
} // namespace

QString themesDir() { return configHome() + "/hyprnotes/themes"; }
bool validThemeName(const QString &n) { return kName.match(n).hasMatch() && !n.contains(".."); }

QString sanitizeThemeName(const QString &raw) {
    QString s = raw.trimmed();
    s.replace(QRegularExpression("\\s+"), "-");
    s.remove(QRegularExpression("[^A-Za-z0-9._-]"));
    while (s.contains("..")) s.replace("..", ".");
    s.remove(QRegularExpression("^[._-]+"));
    return s.left(48);
}

bool convertBase16(const QByteArray &yaml, QJsonObject *out, QString *nameOut, QString *errorOut) {
    static const QRegularExpression kv("^\\s*(base0[0-9A-Fa-f])\\s*:\\s*[\"']?#?([0-9A-Fa-f]{6})[\"']?\\s*(#.*)?$");
    static const QRegularExpression nm("^\\s*(?:scheme|name)\\s*:\\s*[\"']?([^\"'#]*?)[\"']?\\s*$");
    QMap<QString, QString> b;
    QString name;
    for (const QByteArray &raw : yaml.split('\n')) {
        const QString line = QString::fromUtf8(raw).trimmed();
        if (auto m = kv.match(line); m.hasMatch()) b[m.captured(1).toLower()] = '#' + m.captured(2).toUpper();
        else if (auto n = nm.match(line); n.hasMatch() && name.isEmpty()) name = n.captured(1);
    }
    for (int i = 0; i < 16; ++i) {
        const QString k = QString("base%1").arg(i, 2, 16, QChar('0')).toLower();
        if (!b.contains(k)) { *errorOut = QString("base16: missing or invalid %1 (all 16 colors base00-base0F are required)").arg(k); return false; }
    }
    const QJsonArray na{b["base08"], b["base0d"], b["base0a"], b["base0b"], b["base03"], b["base07"]};
    const QJsonObject tok{{"bg", b["base00"]}, {"surface", b["base01"]}, {"selection", b["base02"]}, {"border", b["base03"]},
        {"muted", b["base04"]}, {"text", b["base05"]}, {"danger", b["base08"]}, {"success", b["base0b"]}, {"accent", b["base0d"]},
        {"accentText", onColor(b["base0d"])}, {"noteAccent", na}};
    *nameOut = name;
    *out = wrap(tok, name);
    return true;
}

bool convertVsCode(const QJsonObject &doc, QJsonObject *out, QString *nameOut, QString *errorOut) {
    const QJsonObject c = doc["colors"].toObject();
    auto get = [&](std::initializer_list<const char *> keys) {
        for (const char *k : keys) if (c.contains(k)) if (const QString h = hex6(c[k].toString()); !h.isEmpty()) return h;
        return QString();
    };
    const QString bg = get({"editor.background"}), fg = get({"editor.foreground"});
    if (bg.isEmpty() || fg.isEmpty()) { *errorOut = "VS Code theme: editor.background and editor.foreground are required"; return false; }
    QJsonObject tok{{"bg", bg}, {"text", fg}};
    const struct { const char *tok; std::initializer_list<const char *> keys; } m[] = {
        {"selection", {"editor.selectionBackground"}}, {"surface", {"sideBar.background", "editorWidget.background"}},
        {"border", {"panel.border", "editorGroup.border", "contrastBorder"}}, {"muted", {"editorLineNumber.foreground"}},
        {"danger", {"errorForeground"}}, {"accent", {"focusBorder", "button.background"}}};
    for (const auto &e : m) if (const QString h = get(e.keys); !h.isEmpty()) tok[e.tok] = h;
    if (tok.contains("accent")) tok["accentText"] = onColor(tok["accent"].toString());
    *nameOut = doc["name"].toString();
    *out = wrap(tok, *nameOut);
    return true;
}

bool importTheme(const QString &src, QString *nameOut, QString *errorOut, QStringList *warningsOut) {
    QString dummy;
    QString &err = errorOut ? *errorOut : dummy;
    QStringList warnings;
    auto fail = [&](const QString &m) { err = m; return false; };
    const QFileInfo fi(src);
    if (!fi.isFile()) return fail(QString("%1 is not a readable file").arg(src));
    if (fi.size() > kMaxThemeBytes) return fail(QString("theme file is larger than %1 KiB").arg(kMaxThemeBytes / 1024));
    QFile f(src);
    if (!f.open(QIODevice::ReadOnly)) return fail(QString("cannot read %1: %2").arg(src, f.errorString()));
    const QByteArray bytes = f.read(kMaxThemeBytes + 1);
    if (bytes.size() > kMaxThemeBytes) return fail(QString("theme file is larger than %1 KiB").arg(kMaxThemeBytes / 1024));

    QJsonObject o;
    QString suggested;
    if (bytes.trimmed().startsWith('{')) {
        QJsonParseError pe;
        const QJsonDocument d = QJsonDocument::fromJson(bytes, &pe);
        if (!d.isObject()) return fail("malformed JSON: " + pe.errorString() + " (comments and trailing commas are not supported)");
        o = d.object();
        if (o.contains("version")) suggested = o["name"].toString();
        else if (o["colors"].isObject()) { if (!convertVsCode(o, &o, &suggested, &err)) return false; }
        else return fail("unrecognized theme format: JSON without \"version\" or VS Code \"colors\"");
    } else if (bytes.contains("base00")) {
        if (!convertBase16(bytes, &o, &suggested, &err)) return false;
    } else {
        return fail("unrecognized theme format (expected a Hyprnotes theme JSON, a base16 YAML or a VS Code color theme JSON)");
    }
    if (suggested.contains('/') || suggested.contains('\\') || suggested.contains("..") || suggested.contains(QChar(0)))
        return fail("unsafe theme name (path separators and \"..\" are not allowed)");
    QString base = sanitizeThemeName(suggested);
    if (base.isEmpty()) base = sanitizeThemeName(fi.completeBaseName());
    if (base.isEmpty()) return fail("no usable theme name");
    if (base.compare("modernist", Qt::CaseInsensitive) == 0) return fail("\"modernist\" is the built-in theme name; rename the theme");
    if (const QString why = validateThemeObject(o, &warnings); !why.isEmpty()) return fail(why);

    // ponytail: exists-check then atomic rename is not race-free against a concurrent importer; fine for a human-driven action.
    for (int i = 1; i < 100; ++i) {
        const QString name = i == 1 ? base : QString("%1-%2").arg(base).arg(i);
        o["name"] = name;
        const QByteArray out = QJsonDocument(o).toJson(QJsonDocument::Indented);
        const QString dest = themesDir() + "/" + name + ".json";
        QFile ex(dest);
        if (ex.exists()) {
            if (ex.open(QIODevice::ReadOnly) && ex.readAll() == out) {
                warnings << QString("theme \"%1\" is already installed").arg(name);
            } else continue;
        } else {
            if (!writeAtomic(dest, out, &err)) return false;
            if (i > 1) warnings << QString("a different theme named \"%1\" exists; installed as \"%2\"").arg(base, name);
        }
        if (nameOut) *nameOut = name;
        if (warningsOut) *warningsOut = warnings;
        return true;
    }
    return fail("too many themes with the same name");
}

bool exportTheme(const QString &name, const QString &dest, QString *errorOut) {
    QString dummy;
    QString &err = errorOut ? *errorOut : dummy;
    if (dest.isEmpty()) { err = "no destination"; return false; }
    QByteArray bytes;
    if (name == "modernist" || name.isEmpty()) bytes = QJsonDocument(builtinThemeObject()).toJson(QJsonDocument::Indented);
    else {
        QFile f(themesDir() + "/" + name + ".json");
        if (!validThemeName(name) || !f.open(QIODevice::ReadOnly)) { err = QString("theme \"%1\" not found").arg(name); return false; }
        bytes = f.readAll();
    }
    return writeAtomic(dest, bytes, &err);
}

QStringList listThemes() {
    QStringList r{"modernist"};
    QStringList u;
    for (const QFileInfo &fi : QDir(themesDir()).entryInfoList({"*.json"}, QDir::Files, QDir::Name))
        if (validThemeName(fi.completeBaseName()) && fi.completeBaseName() != "modernist") u << fi.completeBaseName();
    return r + u;
}

bool removeTheme(const QString &name, QString *errorOut) {
    QString dummy;
    QString &err = errorOut ? *errorOut : dummy;
    if (name == "modernist") { err = "the built-in theme cannot be removed"; return false; }
    if (!validThemeName(name) || !QFile::exists(themesDir() + "/" + name + ".json")) { err = QString("theme \"%1\" not found").arg(name); return false; }
    if (!QFile::remove(themesDir() + "/" + name + ".json")) { err = "could not delete the theme file"; return false; }
    return true;
}

} // namespace hn::theme
