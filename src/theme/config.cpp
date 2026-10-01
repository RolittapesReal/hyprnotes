#include "config.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <QSet>

namespace hn::theme {

QString configHome() {
    const QString x = qEnvironmentVariable("XDG_CONFIG_HOME");
    return x.isEmpty() ? QDir::homePath() + "/.config" : x;
}

QString Config::defaultPath() { return configHome() + "/hyprnotes/config.json"; }

Settings Config::defaults() {
    Settings s;
    const QString d = qEnvironmentVariable("XDG_DATA_HOME");
    s.notesFolder = (d.isEmpty() ? QDir::homePath() + "/.local/share" : d) + "/hyprnotes/notes";
    s.keybindings = {{"bold", QKeySequence("Ctrl+B")}, {"italic", QKeySequence("Ctrl+I")},
                     {"strike", QKeySequence("Ctrl+Shift+X")}, {"code", QKeySequence("Ctrl+E")},
                     {"link", QKeySequence("Ctrl+K")}, {"h1", QKeySequence("Ctrl+Alt+1")},
                     {"list-ul", QKeySequence("Ctrl+Shift+8")}, {"list-ol", QKeySequence("Ctrl+Shift+7")},
                     {"check", QKeySequence("Ctrl+Shift+9")}, {"quote", QKeySequence("Ctrl+Shift+.")},
                     {"new-note", QKeySequence("Ctrl+N")}, {"toggle-organizer", QKeySequence("Ctrl+Shift+O")},
                     {"search", QKeySequence("Ctrl+F")}, {"toggle-source", QKeySequence("Ctrl+/")}};
    s.toolbar = {"bold", "italic", "strike", "code", "h1", "list-ul", "list-ol", "check", "quote", "link"};
    return s;
}

namespace {
bool parseSize(const QJsonValue &v, QSize &out, int minW, int minH) {
    const QJsonArray a = v.toArray();
    if (a.size() != 2 || !a[0].isDouble() || !a[1].isDouble()) return false;
    const int w = a[0].toInt(), h = a[1].toInt();
    if (w < minW || h < minH || w > 10000 || h > 10000) return false;
    out = {w, h};
    return true;
}
} // namespace

// Fields that fail validation keep their previous value; missing fields take defaults.
static Settings parse(const QJsonObject &o, const Settings &prev, QStringList &errs) {
    Settings s = Config::defaults();
    auto bad = [&](const char *f, const QString &why) { errs << QString("%1: %2").arg(f, why); };
    if (auto v = o["theme"]; !v.isUndefined()) {
        const QString t = v.toString();
        if (v.isString() && !t.isEmpty() && !t.contains('/') && !t.contains("..")) s.theme = t;
        else { s.theme = prev.theme; bad("theme", "must be a plain name"); }
    }
    if (auto v = o["colorScheme"]; !v.isUndefined()) {
        const QString t = v.toString();
        if (t == "system" || t == "light" || t == "dark") s.colorScheme = t;
        else { s.colorScheme = prev.colorScheme; bad("colorScheme", "must be system|light|dark"); }
    }
    if (auto v = o["notesFolder"]; !v.isUndefined()) {
        const QString t = v.toString();
        if (v.isString() && QDir::isAbsolutePath(t)) s.notesFolder = QDir::cleanPath(t);
        else { s.notesFolder = prev.notesFolder.isEmpty() ? s.notesFolder : prev.notesFolder; bad("notesFolder", "must be an absolute path"); }
    }
    if (auto v = o["trayEnabled"]; !v.isUndefined()) {
        if (v.isBool()) s.trayEnabled = v.toBool(); else { s.trayEnabled = prev.trayEnabled; bad("trayEnabled", "must be boolean"); }
    }
    if (auto v = o["fontSize"]; !v.isUndefined()) {
        const int n = v.toInt(-1);
        if (v.isDouble() && n >= 0 && n <= 32) s.fontSize = n; else { s.fontSize = prev.fontSize; bad("fontSize", "must be 0..32 (0 = theme default)"); }
    }
    if (auto v = o["reduceMotion"]; !v.isUndefined()) {
        if (v.isBool()) s.reduceMotion = v.toBool(); else { s.reduceMotion = prev.reduceMotion; bad("reduceMotion", "must be boolean"); }
    }
    if (auto v = o["keybindings"]; !v.isUndefined()) {
        if (!v.isObject()) bad("keybindings", "must be an object");
        else for (const QJsonObject kb = v.toObject(); const auto &key : kb.keys()) {
            const QString str = kb[key].toString();
            const QKeySequence ks = QKeySequence::fromString(str);
            if (kb[key].isString() && (str.isEmpty() || !ks.isEmpty())) s.keybindings[key] = ks; // "" unbinds
            else bad("keybindings", QString("invalid sequence for %1").arg(key));
        }
    }
    if (auto v = o["toolbar"]; !v.isUndefined()) {
        QStringList l; QSet<QString> seen; bool ok = v.isArray();
        if (ok) for (const auto &e : v.toArray()) {
            const QString id = e.toString();
            if (!e.isString() || id.isEmpty() || seen.contains(id)) { ok = false; break; }
            seen.insert(id); l << id;
        }
        if (ok) s.toolbar = l; else { s.toolbar = prev.toolbar.isEmpty() ? s.toolbar : prev.toolbar; bad("toolbar", "must be unique non-empty strings"); }
    }
    if (auto w = o["windows"]; !w.isUndefined()) {
        const QJsonObject wo = w.toObject();
        if (!w.isObject()) bad("windows", "must be an object");
        else {
            if (wo.contains("stickySize") && !parseSize(wo["stickySize"], s.stickySize, 100, 100)) { s.stickySize = prev.stickySize; bad("windows.stickySize", "[w,h] >= 100"); }
            if (wo.contains("stickyMin") && !parseSize(wo["stickyMin"], s.stickyMin, 100, 100)) { s.stickyMin = prev.stickyMin; bad("windows.stickyMin", "[w,h] >= 100"); }
            if (wo.contains("organizerSize") && !parseSize(wo["organizerSize"], s.organizerSize, 200, 200)) { s.organizerSize = prev.organizerSize; bad("windows.organizerSize", "[w,h] >= 200"); }
            if (auto f = wo["organizerFloating"]; !f.isUndefined()) {
                if (f.isBool()) s.organizerFloating = f.toBool(); else { s.organizerFloating = prev.organizerFloating; bad("windows.organizerFloating", "must be boolean"); }
            }
        }
    }
    return s;
}

Config::Config(const QString &path, QObject *parent) : QObject(parent), m_path(path), m_s(defaults()) {
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(50);
    connect(&m_debounce, &QTimer::timeout, this, &Config::onFs);
    reload();
}

bool Config::reload() {
    QFile f(m_path);
    if (!f.exists()) { m_error.clear(); m_lastBytes.clear(); return true; } // absent file = defaults, nothing written
    if (!f.open(QIODevice::ReadOnly)) { m_error = "cannot read " + m_path; return false; }
    m_lastBytes = f.readAll();
    QJsonParseError pe;
    const QJsonDocument d = QJsonDocument::fromJson(m_lastBytes, &pe);
    if (d.isNull() || !d.isObject()) { m_error = "malformed JSON: " + pe.errorString(); return false; }
    if (d.object()["version"].toInt(-1) != 1) { m_error = "unsupported or missing \"version\" (expected 1)"; return false; }
    QStringList errs;
    m_s = parse(d.object(), m_s, errs);
    m_error = errs.join("; ");
    return errs.isEmpty();
}

bool Config::save(const Settings &s) {
    QJsonObject kb;
    for (auto it = s.keybindings.begin(); it != s.keybindings.end(); ++it) kb[it.key()] = it.value().toString(QKeySequence::PortableText);
    auto sz = [](QSize z) { return QJsonArray{z.width(), z.height()}; };
    const QJsonObject o{{"version", 1}, {"theme", s.theme}, {"colorScheme", s.colorScheme}, {"notesFolder", s.notesFolder},
                        {"trayEnabled", s.trayEnabled}, {"fontSize", s.fontSize}, {"reduceMotion", s.reduceMotion}, {"keybindings", kb},
                        {"toolbar", QJsonArray::fromStringList(s.toolbar)},
                        {"windows", QJsonObject{{"stickySize", sz(s.stickySize)}, {"stickyMin", sz(s.stickyMin)}, {"organizerSize", sz(s.organizerSize)}, {"organizerFloating", s.organizerFloating}}}};
    QDir().mkpath(QFileInfo(m_path).absolutePath());
    QSaveFile f(m_path);
    const QByteArray bytes = QJsonDocument(o).toJson(QJsonDocument::Indented);
    if (!f.open(QIODevice::WriteOnly) || f.write(bytes) != bytes.size() || !f.commit()) { m_error = "cannot write " + m_path; return false; }
    m_s = s; m_lastBytes = bytes; m_error.clear();
    return true;
}

void Config::watch() {
    if (m_w) return;
    m_w = new QFileSystemWatcher(this);
    connect(m_w, &QFileSystemWatcher::directoryChanged, &m_debounce, qOverload<>(&QTimer::start));
    connect(m_w, &QFileSystemWatcher::fileChanged, &m_debounce, qOverload<>(&QTimer::start));
    rearm();
}

// Atomic replaces drop the file watch, so re-add after every event. If the config dir does not exist yet, watch the
// nearest existing ancestor so its creation is noticed.
void Config::rearm() {
    QDir d(QFileInfo(m_path).absolutePath());
    while (!d.exists() && d.cdUp()) {}
    const QString dir = d.absolutePath();
    if (!m_w->directories().contains(dir)) { if (!m_w->directories().isEmpty()) m_w->removePaths(m_w->directories()); m_w->addPath(dir); }
    if (QFile::exists(m_path) && !m_w->files().contains(m_path)) m_w->addPath(m_path);
}

void Config::onFs() {
    rearm();
    QFile f(m_path);
    const QByteArray now = f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    if (now == m_lastBytes) return;
    const Settings before = m_s; const QString errBefore = m_error;
    reload();
    if (m_s != before || m_error != errBefore) emit changed();
}

} // namespace hn::theme
