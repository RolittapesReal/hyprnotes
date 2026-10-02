#pragma once
#include "hn/platform/instance_transport.h"
#include <QString>
#include <QStringList>
#include <QTextStream>

namespace hn::app {

struct CliOptions {
    hn::platform::Action action = hn::platform::Action::ShowOrganizer;
    bool version = false, help = false, printRules = false;
    QString importTheme;   // --import-theme FILE
    QString installPlugin, newPlugin, checkPlugin, packPlugin;   // --install-plugin PATH, --new-plugin NAME, --check-plugin DIR, --pack-plugin DIR
    int newApi = 1;                                              // --api N with --new-plugin (1 or 2)
    bool trustPlugin = false;                                    // --yes-i-trust-this-plugin (skips typing the id)
    bool pluginAction() const { return !installPlugin.isEmpty() || !newPlugin.isEmpty() || !checkPlugin.isEmpty() || !packPlugin.isEmpty(); }
    QString error;   // non-empty => print and exit 2
};
// args exclude argv[0]. No argument means --show-organizer.
CliOptions parseCli(const QStringList &args);
QString helpText();
// --import-theme: imports FILE (Hyprnotes JSON, base16 YAML, VS Code JSON) and, when config.json exists, selects it.
// A running instance applies it live through its config watcher. Returns the exit code; writes the result to out/err.
int runImportTheme(const QString &file, QTextStream &out, QTextStream &err);
// Plugin authoring / install actions. Headless: no display, no running instance needed. Return the exit code.
// --install-plugin shows name, author, source, SHA-256, permissions and the warning, and proceeds only when the plugin id is
// typed on `in` (or --yes-i-trust-this-plugin was given). Nothing is enabled before that; a declined install is rolled back.
int runPluginCli(const CliOptions &o, QTextStream &in, QTextStream &out, QTextStream &err);
QString versionText();
// Installed Hyprland rule snippets (path + contents). Searches $HN_SHARE_DIR, <exe>/../share, $XDG_DATA_DIRS.
// Returns empty and sets *err when none is installed.
QString hyprlandRulesText(QString *err);

} // namespace hn::app
