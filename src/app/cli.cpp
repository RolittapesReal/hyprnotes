#include "cli.h"
#include <QDir>
#include <QRegularExpression>
#include <QFile>
#include <QFileInfo>
#include "config.h"
#include "hn/core/paths.h"
#include "hn/plugins/store.h"
#include "hn/theme/theme_io.h"
#include <QCryptographicHash>

namespace hn::app {
using hn::platform::Action;

CliOptions parseCli(const QStringList &args) {
    CliOptions o;
    QString seen;
    bool apiSeen = false;
    for (int i = 0; i < args.size(); ++i) {
        const QString &a = args[i];
        Action act;
        if (a == "--import-theme") {
            if (i + 1 >= args.size()) { o.error = "--import-theme needs a FILE argument."; return o; }
            o.importTheme = args[++i];
            continue;
        }
        struct { const char *flag; QString CliOptions::*field; } plugOpts[] = {
            {"--install-plugin", &CliOptions::installPlugin}, {"--new-plugin", &CliOptions::newPlugin},
            {"--check-plugin", &CliOptions::checkPlugin}, {"--pack-plugin", &CliOptions::packPlugin}};
        bool plug = false;
        for (const auto &po : plugOpts) {
            if (a != po.flag) continue;
            if (i + 1 >= args.size() || args[i + 1].startsWith("--")) { o.error = QString("%1 needs an argument.").arg(a); return o; }
            if (o.pluginAction()) { o.error = "give only one of --install-plugin, --new-plugin, --check-plugin, --pack-plugin."; return o; }
            o.*(po.field) = args[++i];
            plug = true;
        }
        if (plug) continue;
        if (a == "--api") {
            bool ok = false;
            const int v = i + 1 < args.size() ? args[i + 1].toInt(&ok) : 0;
            if (!ok || v < 1 || v > hn::plugins::kApiVersionMax) { o.error = QString("--api needs 1 or %1.").arg(hn::plugins::kApiVersionMax); return o; }
            o.newApi = v;
            ++i;
            apiSeen = true;
            continue;
        }
        if (a == "--yes-i-trust-this-plugin") { o.trustPlugin = true; continue; }
        if (a == "--help" || a == "-h") { o.help = true; continue; }
        if (a == "--version") { o.version = true; continue; }
        if (a == "--print-hyprland-rules") { o.printRules = true; continue; }
        if (a == "--show-organizer") act = Action::ShowOrganizer;
        else if (a == "--new-note") act = Action::NewNote;
        else if (a == "--background") act = Action::Background;
        else {
            o.error = a.startsWith('-') ? QString("unknown option '%1'. Try 'hyprnotes --help'.").arg(a)
                                        : QString("unexpected argument '%1'. Try 'hyprnotes --help'.").arg(a);
            return o;
        }
        if (!seen.isEmpty() && seen != a) { o.error = QString("conflicting options %1 and %2; give only one action.").arg(seen, a); return o; }
        seen = a;
        o.action = act;
    }
    if (apiSeen && o.newPlugin.isEmpty()) { o.error = "--api only makes sense with --new-plugin NAME."; return o; }
    if (o.trustPlugin && o.installPlugin.isEmpty()) o.error = "--yes-i-trust-this-plugin only makes sense with --install-plugin PATH.";
    if (o.pluginAction() && !seen.isEmpty()) o.error = QString("%1 cannot be combined with %2.").arg(seen, o.installPlugin.isEmpty() ? "a plugin option" : "--install-plugin");
    return o;
}

int runImportTheme(const QString &file, QTextStream &out, QTextStream &err) {
    QString name, why;
    QStringList warnings;
    if (!hn::theme::importTheme(file, &name, &why, &warnings)) { err << "hyprnotes: theme not imported: " << why << "\n"; return 1; }
    for (const QString &w : std::as_const(warnings)) err << "hyprnotes: warning: " << w << "\n";
    const QString cfgPath = hn::core::paths::configDir() + "/config.json";
    if (!QFile::exists(cfgPath)) {   // writing config.json now would skip the first-run notes-folder choice
        out << "Imported theme \"" << name << "\". Hyprnotes has not been set up yet: pick it in Settings after first launch.\n";
        return 0;
    }
    hn::theme::Config cfg(cfgPath);
    if (!cfg.lastError().isEmpty()) {   // never overwrite a config we could not read
        err << "hyprnotes: theme \"" << name << "\" installed, but config.json is unreadable (" << cfg.lastError() << "); select it manually.\n";
        return 1;
    }
    hn::theme::Settings s = cfg.settings();
    s.theme = name;
    if (!cfg.save(s)) { err << "hyprnotes: theme \"" << name << "\" installed, but config.json could not be written.\n"; return 1; }
    out << "Imported theme \"" << name << "\" and selected it (a running Hyprnotes applies it live).\n";
    return 0;
}

namespace {
using namespace hn::plugins;
QString warningLine() {
    return QStringLiteral("Third-party plugins are not reviewed by the Hyprnotes project. A plugin can read or change your notes and, depending on its permissions, send data over the network. Only install plugins from sources you trust.");
}
void printCheck(const CheckResult &c, QTextStream &out) {
    for (const auto &w : c.warnings) out << "warning: " << w << "\n";
    out << "  " << c.fileCount << " files, " << c.totalBytes << " bytes, SHA-256 " << c.hash << "\n";
}
int failWith(const QList<PluginError> &errs, QTextStream &err) {
    for (const auto &e : errs) err << "error: " << e.message << "\n";
    return 1;
}
}  // namespace

int runPluginCli(const CliOptions &o, QTextStream &in, QTextStream &out, QTextStream &err) {
    if (!o.newPlugin.isEmpty()) {
        QString id = o.newPlugin.toLower();
        id.replace(QRegularExpression("[^a-z0-9]+"), "-");
        while (id.startsWith('-')) id.remove(0, 1);
        while (id.endsWith('-')) id.chop(1);
        const QString dir = QDir::current().filePath(id.isEmpty() ? QStringLiteral("plugin") : id);
        QString why;
        if (!createTemplate(dir, o.newPlugin, &why, o.newApi)) { err << "hyprnotes: " << why << "\n"; return 1; }
        out << "Created " << dir << "\n  edit main.lua, then:\n  hyprnotes --check-plugin " << dir << "\n  hyprnotes --pack-plugin " << dir
            << "\n  hyprnotes --install-plugin " << dir << "\n";
        return 0;
    }
    if (!o.checkPlugin.isEmpty()) {
        if (!QFileInfo(o.checkPlugin).isDir()) { err << "hyprnotes: not a folder: " << o.checkPlugin << "\n"; return 1; }
        const CheckResult c = checkDirectory(o.checkPlugin);
        if (!c.ok) return failWith(c.errors, err);
        out << "OK: " << c.manifest.name << " (" << c.manifest.id << ") " << c.manifest.version << ", "
            << (c.manifest.tier == Tier::Native ? "native" : "script") << " plugin, permissions: "
            << (c.manifest.permissions.isEmpty() ? QStringLiteral("none") : c.manifest.permissions.join(", ")) << "\n";
        printCheck(c, out);
        return 0;
    }
    if (!o.packPlugin.isEmpty()) {
        if (!QFileInfo(o.packPlugin).isDir()) { err << "hyprnotes: not a folder: " << o.packPlugin << "\n"; return 1; }
        const CheckResult c = checkDirectory(o.packPlugin);
        if (!c.ok) return failWith(c.errors, err);
        const QString outFile = QDir::current().filePath(c.manifest.id + "-" + c.manifest.version + ".hnplugin");
        QList<PluginError> errs;
        if (!packDirectory(o.packPlugin, outFile, &errs)) return failWith(errs, err);
        out << "Packed " << outFile << "\n";
        printCheck(c, out);
        return 0;
    }
    // --install-plugin: install disabled (nothing runs), show everything the consent dialog shows, ask, then record consent + enable.
    const QString src = QFileInfo(o.installPlugin).absoluteFilePath();
    TrustStore trust(hn::plugins::stateDir() + "/plugins.json");
    trust.load();
    AuditLog audit(hn::plugins::stateDir() + "/plugins-audit.jsonl");
    PackageInstaller inst(hn::plugins::pluginsDir(), &trust, &audit);
    const InstallResult r = inst.install(src, false);
    if (!r.ok) return failWith(r.errors, err);
    const QString dir = hn::plugins::pluginsDir() + "/" + r.id;
    const CheckResult c = checkDirectory(dir);
    out << "\nPlugin   " << c.manifest.name << "  (" << r.id << ") " << c.manifest.version << "\n"
        << "Author   " << c.manifest.author << "\n"
        << "Source   " << src << "\n"
        << "SHA-256  " << r.hash << "\n"
        << "Tier     " << (c.manifest.tier == Tier::Native ? "native" : "script") << "\n"
        << "Permissions:\n";
    if (c.manifest.permissions.isEmpty()) out << "  (none)\n";
    for (const auto &p : c.manifest.permissions)
        out << "  " << p.leftJustified(12) << (isDangerousPermission(p) ? "[DANGEROUS] " : "            ") << permissionDescription(p) << "\n";
    out << "\nWARNING: " << warningLine() << "\n";
    if (c.manifest.tier == Tier::Native) out << "WARNING: This plugin runs native code with full access to your user account. It is not sandboxed.\n";
    bool go = o.trustPlugin;
    if (!go) {
        out << "\nType the plugin id (" << r.id << ") to install and enable it, anything else cancels: " << Qt::flush;
        go = in.readLine().trimmed() == r.id;
    }
    if (!go) {
        QString why;
        inst.uninstall(r.id, &why);
        out << "Cancelled; the plugin was not installed.\n";
        return 1;
    }
    QString why;
    if (!trust.recordConsent(r.id, c.manifest.permissions, r.hash) || !trust.setEnabled(r.id, true, &why)) {
        err << "hyprnotes: could not record your approval: " << why << "\n";
        return 1;
    }
    audit.log("consent", r.id, QString("permissions [%1] sha256 %2 (command line)").arg(c.manifest.permissions.join(", "), r.hash));
    audit.log("enable", r.id);
    out << "Installed and enabled " << c.manifest.name << ". A running Hyprnotes picks it up after a restart.\n";
    return 0;
}

QString helpText() {
    return QStringLiteral(
        "Usage: hyprnotes [ACTION]\n\n"
        "Actions (only one; the running instance performs it, otherwise this process becomes the instance):\n"
        "  --show-organizer   open or focus the organizer (default)\n"
        "  --new-note         create a note and open it in a sticky window\n"
        "  --background       start without windows (tray only); no-op if already running\n\n"
        "Options:\n"
        "  --import-theme FILE  import a theme (Hyprnotes JSON, base16 YAML, VS Code JSON), select it, and exit\n"
        "  --install-plugin PATH  install a plugin (.hnplugin or folder); shows its permissions and asks you to type its id\n"
        "  --yes-i-trust-this-plugin  with --install-plugin: skip typing the id (you have reviewed the plugin)\n"
        "  --new-plugin NAME    scaffold a working script plugin in ./NAME and exit (add --api 2 for the note index / panel / completion API)\n"
        "  --check-plugin DIR   validate a plugin folder exactly as the installer would and exit\n"
        "  --pack-plugin DIR    pack a plugin folder into ID-VERSION.hnplugin and exit\n"
        "  --print-hyprland-rules  print the installed Hyprland rule snippets (path and contents) and exit\n"
        "  --version          print the version and exit\n"
        "  --help             print this help and exit\n\n"
        "Environment: HN_NOTES_DIR overrides the notes folder.\n");
}

QString hyprlandRulesText(QString *err) {
    QStringList roots;
    if (const QString e = qEnvironmentVariable("HN_SHARE_DIR"); !e.isEmpty()) roots << e;
    roots << QFileInfo(QFile::symLinkTarget("/proc/self/exe")).absolutePath() + "/../share";
    for (const QString &d : qEnvironmentVariable("XDG_DATA_DIRS", "/usr/local/share:/usr/share").split(':', Qt::SkipEmptyParts)) roots << d;
    for (const QString &r : std::as_const(roots)) {
        QString out;
        for (const char *f : {"hyprnotes.lua", "hyprnotes.conf"}) {
            QFile file(QDir::cleanPath(r + "/hyprnotes/hyprland/" + f));
            if (file.open(QIODevice::ReadOnly)) out += QStringLiteral("# ==== %1 ====\n%2\n").arg(QFileInfo(file).absoluteFilePath(), QString::fromUtf8(file.readAll()));
        }
        if (!out.isEmpty()) return out;
    }
    if (err) *err = QStringLiteral("no installed Hyprland snippets found (looked for share/hyprnotes/hyprland/hyprnotes.{lua,conf} under: %1)").arg(roots.join(", "));
    return {};
}

QString versionText() { return QStringLiteral("hyprnotes %1").arg(QStringLiteral(HN_VERSION)); }

} // namespace hn::app
