#pragma once
// Plugin runtime public types: manifest, host bridges (implemented by the app), registrations.
#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <functional>
#include <optional>

namespace hn::plugins {

inline constexpr int kApiVersion = 1;     // the original API; plugins declaring "api": 1 see exactly this surface
inline constexpr int kApiVersionMax = 2;  // newest API this app supports ("api": 2 adds notes.index, panels, completion, link handlers)
inline constexpr const char *kAppVersion = "0.2.0";

// Caps from the design spec.
inline constexpr qint64 kMaxArchiveBytes = 5 * 1024 * 1024;
inline constexpr int kMaxFiles = 200;
inline constexpr qint64 kMaxScriptFileBytes = 1024 * 1024;
inline constexpr qint64 kMaxNativeFileBytes = 5 * 1024 * 1024;
inline constexpr qint64 kMaxTotalBytes = 20 * 1024 * 1024; // zip-bomb guard (uncompressed)
inline constexpr qint64 kMaxStorageBytes = 1024 * 1024;

struct PluginError { QString pluginId; QString message; };

enum class Tier { Script, Native };

struct Manifest {
    QString id, name, version, author, description, homepage;
    int api = 0;
    Tier tier = Tier::Script;
    QString entry = QStringLiteral("main.lua");
    QStringList permissions, netHosts;
    QString minApp = QStringLiteral("0.1.0");
    QString dir;  // absolute directory (set by caller)
    bool has(const QString &perm) const { return permissions.contains(perm); }
};

// Warning texts shown (verbatim, always visible) by the consent dialog and the install CLI.
inline constexpr const char *kThirdPartyWarning =
    "Third-party plugins are not reviewed by the Hyprnotes project. A plugin can read or change your notes and, depending on its "
    "permissions, send data over the network. Only install plugins from sources you trust.";
inline constexpr const char *kNativeWarning = "This plugin runs native code with full access to your user account. It is not sandboxed.";

// Permission names: note.read note.edit notes.read notes.write ui storage clipboard network theme native
//   API 2 only: notes.index ui.panel editor.complete editor.links
int permissionMinApi(const QString &p);             // 1 for the original permissions, 2 for the API-2 ones
QStringList knownPermissions();
bool isDangerousPermission(const QString &p);       // network, notes.write, clipboard, theme, native
QString permissionDescription(const QString &p);    // plain language for the consent dialog
// One row of the consent dialog / docs table. risk: "low" | "medium" | "high" | "critical".
struct PermissionInfo { QString name, description, risk; bool dangerous = false; };
QList<PermissionInfo> permissionTable();            // every known permission, in display order
QString describePermission(const QString &p);       // same text as permissionDescription()
bool isDangerous(const QString &p);                 // same as isDangerousPermission(): highlight in the consent dialog
// Host-side redirect/URL policy for NetBridge implementations: true only for https, no credentials, default port and an
// exact (case-insensitive) host match in `hosts`. Resolve relative Location headers before calling.
bool isHostAllowed(const QString &url, const QStringList &hosts);
bool validPluginId(const QString &id);
bool validHostName(const QString &host);
int compareVersions(const QString &a, const QString &b);

// Validates plugin.json. Collects every problem. Returns true iff errs gained nothing. Never throws.
bool parseManifest(const QByteArray &json, const QString &dir, Manifest *out, QList<PluginError> *errs,
                   const QString &appVersion = QString::fromLatin1(kAppVersion));

// $HN_DATA_DIR|$XDG_DATA_HOME/hyprnotes/plugins and $HN_STATE_DIR|$XDG_STATE_HOME/hyprnotes (same rules as hn::core::paths).
QString pluginsDir();
QString stateDir();

// ---- Host bridges (implemented by the app; the runtime enforces permissions/limits before calling them) ----
class NoteBridge {  // the active note of one window
public:
    virtual ~NoteBridge() = default;
    virtual QString text() = 0;
    virtual QString selection() = 0;
    virtual QString path() = 0;
    virtual QString title() = 0;
    virtual QStringList tags() = 0;
    virtual void beginTransaction(const QString &name) = 0;  // one undo step per plugin callback
    virtual void replaceSelection(const QString &s) = 0;
    virtual void insert(const QString &s) = 0;
    virtual void setText(const QString &s) = 0;
    virtual void endTransaction() = 0;
};
struct NoteInfo { QString path, title; };

// ---- API 2 bridge data ----
// Result of an API-2 bridge call. The index side enforces its own 100 ms budget and reports it as Timeout.
enum class BridgeStatus { Ok, Unsupported, Timeout, Invalid, NotFound };
struct LinkRow {  // one [[link]]: outgoing (links) or incoming (backlinks, src = the note containing it)
    QString kind, target, alias, anchor, resolved, src, context;  // kind: "link" | "embed"; resolved = note path or empty
    int line = 0;
};
struct ResolveResult { QString status; QString path; QStringList candidates; };  // status: "resolved" | "ambiguous" | "unresolved"

class LibraryBridge {
public:
    virtual ~LibraryBridge() = default;
    virtual QList<NoteInfo> list(const QString &query) = 0;
    virtual bool read(const QString &path, QString *text) = 0;
    virtual QString create(const QString &title, const QString &text) = 0;  // returns path, empty on failure
    virtual bool write(const QString &path, const QString &text) = 0;
    virtual bool remove(const QString &path) = 0;
    // ---- API 2 (additive; the defaults say "unsupported" so older implementations and fakes keep compiling) ----
    // The runtime has already checked permissions, argument shape and the per-plugin rate limit. Every call here should finish
    // within 100 ms; the index reports an overrun as Timeout (the runtime also discards a result that arrived too late).
    virtual BridgeStatus links(const QString &path, QList<LinkRow> *out) { Q_UNUSED(path) Q_UNUSED(out) return BridgeStatus::Unsupported; }
    virtual BridgeStatus backlinks(const QString &path, int limit, int offset, QList<LinkRow> *out) { Q_UNUSED(path) Q_UNUSED(limit) Q_UNUSED(offset) Q_UNUSED(out) return BridgeStatus::Unsupported; }
    virtual BridgeStatus resolve(const QString &name, ResolveResult *out) { Q_UNUSED(name) Q_UNUSED(out) return BridgeStatus::Unsupported; }
    virtual BridgeStatus frontmatter(const QString &path, QJsonObject *out) { Q_UNUSED(path) Q_UNUSED(out) return BridgeStatus::Unsupported; }
    // spec: already validated and normalised {from, where[], order[], select[], limit<=500, offset}; never SQL.
    virtual BridgeStatus query(const QJsonObject &spec, QJsonArray *rows) { Q_UNUSED(spec) Q_UNUSED(rows) return BridgeStatus::Unsupported; }
    virtual BridgeStatus open(const QString &path, const QString &where) { Q_UNUSED(path) Q_UNUSED(where) return BridgeStatus::Unsupported; }  // where: "organizer" | "sticky"
    // newName is a bare file name (no directories). Applying link rewrites is up to the app (it confirms with the user).
    virtual BridgeStatus rename(const QString &path, const QString &newName, bool updateLinks, QString *newPath) { Q_UNUSED(path) Q_UNUSED(newName) Q_UNUSED(updateLinks) Q_UNUSED(newPath) return BridgeStatus::Unsupported; }
};
class UiBridge {  // may block (nested event loop); the runtime excludes blocked time from the callback budget
public:
    virtual ~UiBridge() = default;
    virtual void notify(const QString &pluginId, const QString &msg) = 0;
    virtual std::optional<QString> prompt(const QString &pluginId, const QString &title, const QString &label, const QString &def) = 0;
    virtual bool confirm(const QString &pluginId, const QString &msg) = 0;
    virtual int pick(const QString &pluginId, const QString &title, const QStringList &items) = 0;  // index or -1
};
struct HttpRequest {
    QString method, url;  // already validated: https, host in allowedHosts
    QByteArray body;
    QList<QPair<QString, QString>> headers;
    int timeoutMs = 10000;
    qint64 maxResponseBytes = 1024 * 1024;
    QStringList allowedHosts;  // the bridge MUST NOT follow a redirect to a host outside this list (or to http)
};
struct HttpResponse { int status = 0; QByteArray body; QString error; };
class NetBridge {
public:
    virtual ~NetBridge() = default;
    virtual HttpResponse perform(const HttpRequest &req) = 0;  // only performs the request
};
class ClipboardBridge {
public:
    virtual ~ClipboardBridge() = default;
    virtual QString get() = 0;
    virtual void set(const QString &s) = 0;
};
class ThemeBridge {
public:
    virtual ~ThemeBridge() = default;
    virtual bool setToken(const QString &pluginId, const QString &token, const QString &value) = 0;
};
// ---- API 2: panels and editor hooks (host-drawn, declarative) ----
struct PanelBlock {
    QString type;                // heading | text | list | item | markdown | button | empty
    QString text;                // heading/text/markdown/empty content, button label
    QString title, subtitle, path;  // item
    int line = 0;                // item: 1-based line in path, 0 = none
    int level = 2;               // heading: 1-3
    int click = 0;               // item/button: token for PluginManager::panelClick (0 = not clickable)
    QList<PanelBlock> items;     // list: item blocks
};
// Implemented by the app's panel dock. All methods default to no-ops.
class PanelBridge {
public:
    virtual ~PanelBridge() = default;
    virtual void showPanel(const QString &qualifiedId, const QString &title, const QString &icon) { Q_UNUSED(qualifiedId) Q_UNUSED(title) Q_UNUSED(icon) }  // a panel became available
    virtual void updatePanel(const QString &qualifiedId, const QList<PanelBlock> &blocks, const QString &error = QString()) { Q_UNUSED(qualifiedId) Q_UNUSED(blocks) Q_UNUSED(error) }
    virtual void removePanel(const QString &qualifiedId) { Q_UNUSED(qualifiedId) }
};
struct CompletionItem { QString pluginId, label, detail, insert; int cursorOffset = -1; bool markdown = false; };  // cursorOffset: characters into insert, -1 = end; markdown: insert through the Markdown importer
struct LinkActivation { QString kind, target, alias, anchor, resolved; };                   // resolved = note path or empty
// Implemented by the editor. Replies to PluginManager::requestCompletion / requestLinkActivation.
class EditorHooksBridge {
public:
    virtual ~EditorHooksBridge() = default;
    virtual void completionReply(quint64 token, const QList<CompletionItem> &items) { Q_UNUSED(token) Q_UNUSED(items) }
    virtual void linkActivationReply(quint64 token, bool handled) { Q_UNUSED(token) Q_UNUSED(handled) }
};
struct HostBridges {
    LibraryBridge *library = nullptr;
    UiBridge *ui = nullptr;
    NetBridge *net = nullptr;
    ClipboardBridge *clipboard = nullptr;
    ThemeBridge *theme = nullptr;
    PanelBridge *panel = nullptr;          // API 2
    EditorHooksBridge *editor = nullptr;   // API 2
};

// Runtime URL policy for hn.http (exposed for tests): https only, no userinfo, default port, host in allowlist.
bool checkHttpUrl(const QString &url, const QStringList &allowedHosts, QString *host, QString *err);

// ---- Registrations collected from Lua (queried by the UI) ----
struct CommandReg { QString pluginId, id, title, key; QString qualifiedId() const { return pluginId + QLatin1Char(':') + id; } };
struct ToolbarReg { QString pluginId, id, title, icon; };
struct MenuReg { QString pluginId, id, title, where; };
struct TriggerReg { QString pluginId, pattern; };
struct SettingReg { QString pluginId, id, type, title; QVariant def; };
struct PanelReg {
    QString pluginId, id, title, icon;
    QStringList refreshOn;  // events that ask the host to re-render the panel
    bool onEvent = false;   // the plugin also wants those events delivered (on_event)
    QString qualifiedId() const { return pluginId + QLatin1Char(':') + id; }
};
struct CompleteReg { QString pluginId, id, trigger; };
struct LinkHandlerReg { QString pluginId; bool hasPattern = false; };

struct PluginRegs {
    QList<CommandReg> commands;
    QList<ToolbarReg> toolbars;
    QList<MenuReg> menus;
    QList<TriggerReg> triggers;
    QList<SettingReg> settings;
    QList<PanelReg> panels;               // API 2
    QList<CompleteReg> completions;       // API 2
    QList<LinkHandlerReg> linkHandlers;   // API 2
    QStringList events;  // subscribed events
    QJsonObject toJson() const;
    static PluginRegs fromJson(const QString &pluginId, const QJsonObject &o);
    bool operator==(const PluginRegs &o) const { return toJson() == o.toJson(); }
};

class PluginRegistry : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    void set(const QString &pluginId, const PluginRegs &r);
    void remove(const QString &pluginId);
    void clear();
    const PluginRegs *of(const QString &pluginId) const;
    QList<CommandReg> commands() const;
    QList<ToolbarReg> toolbarButtons() const;
    QList<MenuReg> menuItems(const QString &where = QString()) const;
    QList<TriggerReg> triggers() const;
    QList<SettingReg> settings(const QString &pluginId = QString()) const;
    QStringList subscribers(const QString &event) const;
    QList<PanelReg> panels(const QString &pluginId = QString()) const;      // API 2
    QList<PanelReg> panelsRefreshingOn(const QString &event) const;
    QList<CompleteReg> completions(const QString &trigger = QString()) const;
    QList<LinkHandlerReg> linkHandlers() const;
signals:
    void changed();
private:
    QMap<QString, PluginRegs> m_;
};

}  // namespace hn::plugins
