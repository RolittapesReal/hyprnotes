#pragma once
// Lua script-plugin runtime (LuaPluginHost) and the PluginManager facade the app drives.
#include "hn/plugins/store.h"
#include "hn/plugins/types.h"
#include <QTimer>
#include <memory>

namespace hn::plugins {

using Logger = std::function<void(int level, const QString &pluginId, const QString &msg)>;

enum class Budget { Event, Command, PreSave, Trigger, Load, Render, Complete };

struct HostEnv {
    QString stateDir;                    // plugin storage lives in <stateDir>/plugin-data/<id>.json
    TrustStore *trust = nullptr;
    AuditLog *audit = nullptr;
    PluginRegistry *registry = nullptr;
    HostBridges bridges;
    Logger logger;
    QString appVersion = QString::fromLatin1(kAppVersion);
    // Wall-clock budgets (spec): events 50 ms, commands 2 s, pre_save 20 ms. Blocking host calls (ui, http) do not count.
    int eventMs = 50, commandMs = 2000, preSaveMs = 20, triggerMs = 50, loadMs = 250;
    quint64 memoryCap = 16u * 1024 * 1024;  // Lua allocator cap per plugin
    int breakerThreshold = 3;               // consecutive failures => auto-disable
    std::function<qint64()> clock;          // epoch seconds for hn.time (null = system clock; tests inject a fixed time)
    // API 2: panel render 50 ms, completion 20 ms. Time inside index calls is excluded from those (like ui/http), but each call is
    // limited to indexCallMs, all index calls of one callback to indexCallbackMs, and a plugin to indexPerSecond calls per second.
    int renderMs = 50, completeMs = 20;
    int indexCallMs = 100, indexCallbackMs = 500, indexPerSecond = 200;
};

// One lua_State per plugin, created lazily on first hook delivery, destroyed on disable/unload.
class LuaPluginHost : public QObject {
    Q_OBJECT
public:
    explicit LuaPluginHost(HostEnv env, QObject *parent = nullptr);
    ~LuaPluginHost() override;
    HostEnv &env();

    // Declares an enabled script plugin. Creates NO Lua state. cached = registrations from a previous run (same package hash).
    void setPlugin(const Manifest &m, const QStringList &consented, const PluginRegs *cached);
    void unload(const QString &id);   // closes the state, keeps the declaration
    void forget(const QString &id);   // unload + drop declaration
    bool isLoaded(const QString &id) const;
    bool isBusy(const QString &id) const;
    bool usesNote(NoteBridge *note) const;  // includes disabled callbacks still unwinding
    int loadedCount() const;
    quint64 memoryUsed(const QString &id) const;
    bool ensureLoaded(const QString &id, QString *err = nullptr);

    enum class Kind { Command, Toolbar, Menu };
    bool run(const QString &id, Kind kind, const QString &localId, NoteBridge *note, QString *err = nullptr);
    void deliver(const QString &id, const QString &event, const QString &arg, NoteBridge *note);
    QString preSave(const QString &id, const QString &text, NoteBridge *note);
    bool runTrigger(const QString &id, const QString &pattern, NoteBridge *note, QString *replacement);

    // ---- API 2 ----
    // Each of these creates the plugin's Lua state on demand. The manager has already checked that the plugin is enabled.
    bool renderPanel(const QString &id, const QString &panelId, NoteBridge *note, QList<PanelBlock> *out, QString *err = nullptr);
    bool panelClick(const QString &id, const QString &panelId, int token, NoteBridge *note, QString *err = nullptr);
    void deliverPanelEvent(const QString &id, const QString &panelId, const QString &event, const QString &arg, NoteBridge *note);
    QList<CompletionItem> complete(const QString &id, const QString &completeId, const QString &query, NoteBridge *note, int *dropped = nullptr, QString *err = nullptr);
    bool activateLink(const QString &id, const LinkActivation &ref, NoteBridge *note);  // true = a handler consumed the activation

    bool setSetting(const QString &id, const QString &settingId, const QVariant &v);
    QVariant setting(const QString &id, const QString &settingId);
    void deleteStorage(const QString &id);

    struct Impl;  // internal

signals:
    void callbackFinished(const QString &id);  // after stack/transaction cleanup, unload and failure accounting
    void autoDisabled(const QString &id, const QString &reason);  // circuit breaker or integrity failure
    void panelRefreshRequested(const QString &pluginId, const QString &panelId, NoteBridge *note);  // hn.panel_refresh (never re-enters Lua)

private:
    std::unique_ptr<Impl> d;
};

// Native tier: implemented later on top of hn::mods::ModHost. Declared here so the manager does not depend on it.
class NativePluginAdapter {
public:
    virtual ~NativePluginAdapter() = default;
    virtual bool activate(const Manifest &m, QString *err) = 0;
    virtual void deactivate(const QString &pluginId) = 0;
    virtual bool runCommand(const QString &pluginId, const QString &commandId, NoteBridge *note, QString *err) = 0;
    virtual void postEvent(const QString &pluginId, const QString &event, const QString &arg) = 0;
};
class StubNativeAdapter : public NativePluginAdapter {
public:
    bool activate(const Manifest &, QString *err) override { if (err) *err = QStringLiteral("native plugin support is not wired in this build"); return false; }
    void deactivate(const QString &) override {}
    bool runCommand(const QString &, const QString &, NoteBridge *, QString *err) override { if (err) *err = QStringLiteral("native plugin support is not wired in this build"); return false; }
    void postEvent(const QString &, const QString &, const QString &) override {}
};

enum class Status { Disabled, NeedsConsent, Enabled, Failed };

struct PluginInfo {
    Manifest manifest;
    Status status = Status::Disabled;
    QString detail;            // human message for NeedsConsent/Failed
    QString hash;              // current package hash
    bool consented = false;
    bool needsReconsent = false;
    QStringList consentedPermissions;
    bool loaded = false;       // a Lua state currently exists
};

struct ManagerConfig {
    QString pluginsDir = hn::plugins::pluginsDir();
    QString stateDir = hn::plugins::stateDir();
    QString appVersion = QString::fromLatin1(kAppVersion);
    HostBridges bridges;
    Logger logger;
    NativePluginAdapter *native = nullptr;  // not owned; null => StubNativeAdapter
    int coalesceMs = 250;
    std::function<qint64()> clock;          // optional fixed clock for hn.time (tests)
};

struct TriggerMatch { QString pluginId, pattern, replacement; };

class PluginManager : public QObject {
    Q_OBJECT
public:
    explicit PluginManager(ManagerConfig cfg, QObject *parent = nullptr);
    ~PluginManager() override;

    void scan();  // reads manifests + verifies hashes; creates no Lua state unless an enabled plugin has no registration cache
    QList<PluginInfo> plugins() const;
    PluginInfo info(const QString &id) const;
    QList<PluginError> errors() const { return errors_; }

    InstallResult install(const QString &source, bool allowUpgrade = false);
    bool consent(const QString &id, const QStringList &permissions, QString *err = nullptr);
    bool enable(const QString &id, QString *err = nullptr);
    bool disable(const QString &id);
    bool remove(const QString &id, QString *err = nullptr);
    // Hot reload. If files changed since consent: disabled + needs re-consent, unless trustChanges (explicit developer action)
    // and the requested permissions did not grow.
    bool reload(const QString &id, bool trustChanges = false, QString *err = nullptr);

    bool runCommand(const QString &qualifiedId, NoteBridge *note, QString *err = nullptr);
    bool runToolbarButton(const QString &qualifiedId, NoteBridge *note, QString *err = nullptr);
    bool runMenuItem(const QString &qualifiedId, NoteBridge *note, QString *err = nullptr);
    // note.changed / selection.changed are coalesced; others are delivered immediately. Only subscribed plugins are touched.
    void post(const QString &event, const QString &arg, NoteBridge *note);
    void flushEvents();
    void appStarted() { post(QStringLiteral("app.started"), cfg_.appVersion, nullptr); }  // call once after scan() when the app is up (arg = app version)
    void detachNote(NoteBridge *note);  // call before a NoteBridge is destroyed
    QString preSave(const QString &text, NoteBridge *note);
    std::optional<TriggerMatch> matchTrigger(const QString &textBeforeCursor, NoteBridge *note);

    // ---- API 2: panels, completion, link activation (UI-driven; none of these run anything unless a plugin registered for it) ----
    // Renders one panel (creating the Lua state if needed) into host-drawn blocks. Blocks carry click tokens for panelClick().
    bool renderPanel(const QString &pluginId, const QString &panelId, NoteBridge *note, QList<PanelBlock> *out, QString *err = nullptr);
    // renderPanel + PanelBridge::updatePanel (the error text, if any, goes along with empty blocks).
    bool refreshPanel(const QString &pluginId, const QString &panelId, NoteBridge *note, QString *err = nullptr);
    // Host scheduling: runs now if safe, otherwise coalesces until the callback returns.
    void requestPanelRefresh(const QString &pluginId, const QString &panelId, NoteBridge *note);
    // Runs an on_click callback with the panel's plugin identity and normal budgets. Tokens are only valid until the next render.
    bool panelClick(const QString &pluginId, const QString &panelId, int token, NoteBridge *note, QString *err = nullptr);
    // Only panels the UI marked active are re-rendered by events / hn.panel_refresh; inactive panels cost nothing.
    void setPanelActive(const QString &pluginId, const QString &panelId, bool active);
    void flushPanels();  // runs queued hn.panel_refresh requests now
    // Items from every plugin that registered `trigger` (at most 50 in total, 20 ms per plugin).
    QList<CompletionItem> complete(const QString &trigger, const QString &query, NoteBridge *note);
    void requestCompletion(quint64 token, const QString &trigger, const QString &query, NoteBridge *note);   // -> EditorHooksBridge::completionReply
    bool activateLink(const LinkActivation &ref, NoteBridge *note);                                          // true = handled by a plugin
    void requestLinkActivation(quint64 token, const LinkActivation &ref, NoteBridge *note);                  // -> EditorHooksBridge::linkActivationReply

    PluginRegistry *registry() { return &registry_; }
    LuaPluginHost *host() { return host_.get(); }
    TrustStore *trust() { return &trust_; }
    AuditLog *audit() { return &audit_; }
    int loadedStates() const { return host_->loadedCount(); }
    bool timerActive() const { return timer_.isActive() || panelTimer_.isActive(); }

signals:
    void pluginsChanged();
    void pluginAutoDisabled(const QString &id, const QString &reason);

private:
    struct Entry { Manifest m; QString hash; QString lastError; bool invalid = false; };
    bool run(const QString &q, LuaPluginHost::Kind k, NoteBridge *note, QString *err);
    void activateEntry(const QString &id, Entry &e);
    void loadEntry(const QString &dir, const QString &dirName);
    void saveCache();
    void deliverNow(const QString &event, const QString &arg, NoteBridge *note);
    void syncPanels();
    void publishRegistry();
    void invalidatePanels(const QString &id);
    struct NoteRef {
        NoteBridge *note = nullptr;
        std::shared_ptr<bool> alive;
        bool valid() const { return !alive || *alive; }
    };
    NoteRef noteRef(NoteBridge *note);
    struct PanelRequest {
        NoteRef note;
        quint64 generation = 0;
        bool explicitContext = false;
        bool allowLoad = false; // a fresh request may load cached registrations once
    };
    // Empty panelId identifies a regular hn.on subscriber.
    struct DeferredEvent { QString pluginId, panelId, event, arg; PanelRequest request; };
    bool validPanel(const QString &qid, const PanelRequest &request) const;
    void queuePanelRefresh(const QString &qid, NoteBridge *note, bool explicitContext);
    void schedulePanelDrain();
    QList<PanelReg> activePanelsFor(const QString &event) const;

    ManagerConfig cfg_;
    TrustStore trust_;
    AuditLog audit_;
    PluginRegistry registry_;
    std::unique_ptr<LuaPluginHost> host_;
    StubNativeAdapter stub_;
    NativePluginAdapter *native_;
    QMap<QString, Entry> entries_;
    QList<PluginError> errors_;
    QJsonObject cache_;  // id -> {hash, regs}: lets the UI list commands without creating Lua states
    bool cacheSuspended_ = false;
    int discoveryDepth_ = 0;
    bool registryPending_ = false, drainScheduled_ = false, drainingPanels_ = false;
    int drainCascade_ = 0;  // callbacks run since the deferred queue was last empty
    quint64 nextGeneration_ = 0;
    QHash<QString, quint64> generations_;
    QHash<NoteBridge *, std::shared_ptr<bool>> noteLives_;
    QList<DeferredEvent> deferredEvents_;
    struct Pending { QString arg; NoteRef note; };
    QMap<QString, Pending> pending_;
    QTimer timer_, panelTimer_;
    QMap<QString, QString> shownPanels_;                 // qualified id -> title/icon last announced to the PanelBridge
    QSet<QString> activePanels_;                         // qualified ids the UI currently displays
    QMap<QString, PanelRequest> pendingPanels_;          // latest host context per panel
};

}  // namespace hn::plugins
