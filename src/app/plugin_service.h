#pragma once
// Everything the app needs around hn_plugins: the lazily created PluginManager, the real bridges, native adapter, legacy-mod
// migration, per-session hooks (events, pre_save, triggers, toolbar), the install/consent/enable flows and the command model
// the menus and palette read. Nothing here exists until a plugin that needs it is enabled (or the Plugins page is opened).
#include "consent_dialog.h"
#include "hn/plugins/runtime.h"
#include "native_adapter.h"
#include "plugin_bridges.h"
#include <QKeySequence>
#include <QMap>
#include <QObject>
#include <functional>
#include <memory>
#include <optional>

namespace hn::app {

class AppController;

// Test seams. Null => the real (modal) dialog.
struct PluginHooks {
    std::function<bool(const ConsentRequest &)> consent;
    std::function<std::optional<QString>(const QString &plugin, const QString &title, const QString &label, const QString &def)> prompt;
    std::function<bool(const QString &plugin, const QString &msg)> confirm;
    std::function<int(const QString &plugin, const QString &title, const QStringList &items)> pick;
};

struct PluginAction {
    enum Kind { Command, Toolbar, Menu } kind = Command;
    QString qid, title, pluginId, pluginName, key, icon, where;
};

class PluginService : public QObject {
    Q_OBJECT
public:
    PluginService(AppController *c, QString pluginsDir, PluginHooks hooks);
    ~PluginService() override;

    AppController *controller() const { return m_c; }
    QString pluginsDir() const { return m_dir; }
    PluginHooks &hooks() { return m_hooks; }

    // ---- lifecycle ----
    bool active() const { return bool(m_mgr); }
    hn::plugins::PluginManager *manager();                 // creates (scan + legacy migration) on first use
    hn::plugins::PluginManager *managerIfActive() const { return m_mgr.get(); }
    bool somethingEnabledOnDisk() const;                   // cheap: reads plugins.json only
    void startIfNeeded();                                  // launch: creates the manager only when a plugin is enabled; emits app.started
    int legacyModsAwaitingApproval() const;                // legacy mods that were enabled and now need consent
    bool networkManagerCreated() const { return m_net && m_net->managerCreated(); }
    PluginNetBridge *net() const { return m_net.get(); }
    ModNativeAdapter *native() const { return m_native.get(); }
    QString nameOf(const QString &pluginId) const;

    // ---- sessions ----
    void attach(NoteSession *s);                           // events, pre_save, triggers, toolbar (no-op while inactive)
    void release(NoteSession *s);                          // before the session dies: flush + detach its bridge
    PluginNoteBridge *bridgeFor(NoteSession *s);
    void post(const QString &event, NoteSession *s);       // cheap no-op while inactive

    // ---- command model ----
    QList<PluginAction> commands() const;                  // script registry + native commands
    QList<PluginAction> toolbarButtons() const;
    QList<PluginAction> menuItems(const QString &where) const;
    QList<PluginAction> allActions() const;                // commands, menu items, toolbar buttons (palette)
    QMap<QString, QKeySequence> pluginKeys() const;        // "plugin:<qid>" -> validated, conflict-free sequence
    bool run(const PluginAction &a, NoteSession *s, QString *err = nullptr);
    bool runQualified(const QString &qid, NoteSession *s, QString *err = nullptr);   // command, else menu item, else toolbar button

    // ---- flows (UI) ----
    ConsentRequest requestFor(const QString &id) const;
    bool review(const QString &id, QWidget *parent);       // consent dialog -> consent + enable
    hn::plugins::InstallResult install(const QString &source, QWidget *parent, QString *message);   // install, then review
    bool enable(const QString &id, QWidget *parent, QString *message = nullptr);   // consent dialog first when consent is missing/invalid
    QString auditText(int maxLines = 300) const;
    QString sourceOf(const QString &id) const;
    void rememberSource(const QString &id, const QString &source);

signals:
    void changed();                                        // plugin list or registrations changed

private:
    void create();
    void migrateLegacyMods();
    void refreshSessions();
    void wireSession(NoteSession *s);
    QString legacyEnabledPath() const;
    AppController *m_c;
    QString m_dir;
    PluginHooks m_hooks;
    std::unique_ptr<ModNativeAdapter> m_native;
    std::unique_ptr<PluginNetBridge> m_net;
    std::unique_ptr<PluginLibraryBridge> m_lib;
    std::unique_ptr<PluginUiBridge> m_ui;
    std::unique_ptr<PluginClipboardBridge> m_clip;
    std::unique_ptr<PluginThemeBridge> m_theme;
    std::unique_ptr<hn::plugins::PluginManager> m_mgr;
    QHash<NoteSession *, PluginNoteBridge *> m_bridges;
    QHash<NoteSession *, QList<QMetaObject::Connection>> m_conns;
    QMap<QString, QString> m_names;
    QStringList m_legacyPending;
    bool m_started = false;
};

} // namespace hn::app
