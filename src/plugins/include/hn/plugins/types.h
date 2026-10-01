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

inline constexpr int kApiVersion = 1;
inline constexpr const char *kAppVersion = "0.1.0";

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
class LibraryBridge {
public:
    virtual ~LibraryBridge() = default;
    virtual QList<NoteInfo> list(const QString &query) = 0;
    virtual bool read(const QString &path, QString *text) = 0;
    virtual QString create(const QString &title, const QString &text) = 0;  // returns path, empty on failure
    virtual bool write(const QString &path, const QString &text) = 0;
    virtual bool remove(const QString &path) = 0;
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
struct HostBridges {
    LibraryBridge *library = nullptr;
    UiBridge *ui = nullptr;
    NetBridge *net = nullptr;
    ClipboardBridge *clipboard = nullptr;
    ThemeBridge *theme = nullptr;
};

// Runtime URL policy for hn.http (exposed for tests): https only, no userinfo, default port, host in allowlist.
bool checkHttpUrl(const QString &url, const QStringList &allowedHosts, QString *host, QString *err);

// ---- Registrations collected from Lua (queried by the UI) ----
struct CommandReg { QString pluginId, id, title, key; QString qualifiedId() const { return pluginId + QLatin1Char(':') + id; } };
struct ToolbarReg { QString pluginId, id, title, icon; };
struct MenuReg { QString pluginId, id, title, where; };
struct TriggerReg { QString pluginId, pattern; };
struct SettingReg { QString pluginId, id, type, title; QVariant def; };

struct PluginRegs {
    QList<CommandReg> commands;
    QList<ToolbarReg> toolbars;
    QList<MenuReg> menus;
    QList<TriggerReg> triggers;
    QList<SettingReg> settings;
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
signals:
    void changed();
private:
    QMap<QString, PluginRegs> m_;
};

}  // namespace hn::plugins
