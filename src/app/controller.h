#pragma once
// Application controller (spec 4.2/4.3, desktop design 3/4): owns repository, index, config, mods,
// Hyprland IPC, tray, the note-session / window registry, session restore and save-aware shutdown.
#include "config.h"
#include "hn/core/library_index.h"
#include "hn/core/note_repository.h"
#include "hn/platform/hyprland_ipc.h"
#include "hn/platform/instance_transport.h"
#include "hn/platform/tray_controller.h"
#include "hn/platform/window_identity.h"
#include "hn/platform/window_placement.h"
#include "hn/plugins/market.h"
#include "link_rename.h"
#include "note_session.h"
#include "note_state.h"
#include "organizer_window.h"
#include "plugin_service.h"
#include "sticky_window.h"
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <functional>
#include <memory>
#include <optional>

class QMenu;

namespace hn::app {

class OrganizerWindow;
class StickyWindow;

enum class RecoveryChoice { Restore, Discard, Later };

struct ControllerOptions {
    QString notesDir;                      // explicit override (HN_NOTES_DIR); empty => config / first run
    QString configPath, stateDir, cacheDir, modsDir, modsEnabledPath;   // empty => hn::core::paths
    QString pluginsDir;                    // empty => $XDG_DATA_HOME/hyprnotes/plugins (beside modsDir when that is overridden)
    QString marketIndexUrl;                // empty => the production registry URL (tests inject a loopback server)
    hn::plugins::UrlPolicy marketUrlPolicy;   // empty => https only (tests allow loopback http)
    PluginHooks pluginHooks;               // test seams for the plugin dialogs (consent, prompt, confirm, pick)
    bool background = false;               // --background: nothing is opened or loaded eagerly
    bool useTray = true;
    std::optional<hn::platform::HyprPaths> hyprPaths;   // tests point this at a fake compositor; default: environment
    bool useHyprland = true;               // effective only when the Hyprland environment is present
    std::function<QString(const QString &suggested)> chooseNotesFolder;              // first run; default dialog
    std::function<RecoveryChoice(const QList<hn::core::RecoveryEntry> &)> recoveryPrompt;   // default dialog
    std::function<bool(const QString &title, const QString &text)> confirm;          // default QMessageBox
    std::function<LinkChoice(const QString &summary, const QStringList &lines)> linkChoice;   // rename with backlinks; default dialog
};

// Remembered across runs in <state>/ui-state.json.
struct DockPrefs { bool visible = true; int width = 320; };

// Kept for source compatibility: the text size now lives in hn::theme::Settings::fontSize.
struct AppPrefs { int fontSize = 0; };   // 0 = theme default

class AppController : public QObject {
    Q_OBJECT
public:
    explicit AppController(ControllerOptions opt, QObject *parent = nullptr);
    ~AppController() override;

    // ---- launch ----
    void start(hn::platform::Action initial);
    bool handleAction(hn::platform::Action a);     // from InstanceServer / tray; false => nack

    // ---- services ----
    hn::theme::Config &config() { return *m_config; }
    hn::theme::Settings settings() const { return m_settings; }
    AppPrefs prefs() const { return {m_settings.fontSize}; }
    const hn::theme::Theme &theme() const { return m_theme; }
    hn::core::NoteRepository &repo() { return *m_repo; }
    hn::core::LibraryIndex *index();               // lazy: created (and synced) on first use
    bool hasIndex() const { return bool(m_index); }
    PluginService &plugins() { return *m_plugins; }
    hn::plugins::MarketClient &market();           // created on first call; nothing touches the network before the first request
    bool marketCreated() const { return bool(m_market); }
    bool marketNoticeAcknowledged() const;         // <stateDir>/market-notice-ack exists
    bool acknowledgeMarketNotice();                // false when the marker could not be written
    QString cacheDir() const { return m_cacheDir; }
    QString marketIndexUrl() const { return m_opt.marketIndexUrl.isEmpty() ? hn::plugins::MarketClient::defaultIndexUrl() : m_opt.marketIndexUrl; }
    hn::platform::TrayController *tray() { return m_tray.get(); }
    hn::platform::HyprlandIpc *ipc() { return m_ipc.get(); }
    hn::platform::WindowRegistry &registry() { return m_registry; }
    NoteStateStore &noteState() { return m_noteState; }
    QString notesRoot() const { return m_repo->root(); }
    QString modsEnabledPath() const { return m_modsEnabled; }   // legacy mods list (migration source; never rewritten)
    QString legacyModsDir() const { return m_modsDir; }
    QString stateDir() const { return m_stateDir; }
    QString statePath(const QString &name) const { return m_stateDir + "/" + name; }

    // ---- notes and windows ----
    NoteSession *session(const QString &rel) const { return m_sessions.value(rel); }
    QStringList openNotes() const { return m_sessions.keys(); }
    StickyWindow *stickyOf(const QString &rel) const;
    QList<StickyWindow *> stickies() const;
    OrganizerWindow *organizer() const { return m_organizer; }
    QString organizerNote() const;
    int windowCount() const;

    QString newNote(const QString &folder = {}, bool intoOrganizer = false);
    NoteSession *openSticky(const QString &rel, bool focus = true, const hn::platform::WindowState *restore = nullptr);
    NoteSession *openInOrganizer(const QString &rel);
    bool popOut(const QString &rel);
    bool popIn(const QString &rel);
    void closeNote(const QString &rel, bool discard = false);   // save-aware; refused closes keep the editor visible
    void discardNote(const QString &rel);                       // confirmed explicit discard
    bool renameNote(const QString &rel, const QString &newTitle, QString *err = nullptr);   // plain rename, links in other notes are not touched
    // Rename; with LinkMode::Ask and notes linking here, ONE dialog offers Update links / Rename only / Cancel (see link_rename.cpp).
    // Returns false on failure (err set) or cancel (err empty). *newRel = the new path.
    bool renameNoteWithLinks(const QString &rel, const QString &newTitle, LinkMode mode, QString *err = nullptr, QString *newRel = nullptr);
    const LinkUpdateReport &lastLinkReport() const { return m_linkReport; }
    DockPrefs dockPrefs() const;
    void setDockPrefs(const DockPrefs &p);
    bool deleteNote(const QString &rel, QString *err = nullptr);
    int noteColor(const QString &rel) { return m_noteState.color(rel); }
    void setNoteColor(const QString &rel, int idx);
    bool editTags(const QString &rel, const QStringList &desired);
    bool runModCommand(const QString &rel, const QString &commandId);   // a (migrated) native plugin command by its local id
    QList<hn::core::DirtyDoc> dirtyDocs() const;
    void populateNoteMenu(QMenu *menu, const QString &rel, bool withDelete);
    // Full right-click menu: open, rename, tags, color, duplicate, copy path, show in folder, delete (+ pop in / close in a sticky).
    void showNoteContextMenu(const QString &rel, const QPoint &globalPos, QWidget *parent, bool inSticky = false);
    QString duplicateNote(const QString &rel, QString *err = nullptr);
    void promptRename(const QString &rel, QWidget *parent);
    void promptEditTags(const QString &rel, QWidget *parent);

    void showOrganizer(bool focus = true);
    bool organizerCloseRequested();                // window close button; true only when the window may be destroyed by Qt right now
    void toggleOrganizer();
    void showSettings(QWidget *parent = nullptr, int tab = -1);

    // ---- plugins ----
    void announce(const QString &message);          // status strip of the active window (6 s); also emitted as notice()
    QString lastNotice() const { return m_lastNotice; }
    bool setThemeOverride(const QString &pluginId, const QString &token, const QString &value);   // session-only, validated
    QKeySequence paletteKey() const;                // user binding, else Ctrl+Shift+P
    QMap<QString, QKeySequence> effectiveBindings() const;   // settings + palette default (no plugin keys)
    void showShortcuts(QWidget *window);            // cheat sheet incl. plugin keys
    void showPalette(QWidget *host, NoteSession *s);
    void populateToolsMenu(QMenu *menu, NoteSession *s);

    // ---- settings / theme ----
    void applySettings(const hn::theme::Settings &s, const AppPrefs &p);
    // Import a theme file (Hyprnotes JSON, base16 YAML, VS Code JSON), select and apply it. *message: result / errors / warnings.
    bool importThemeFile(const QString &path, QString *message);
    static QString pluginPathFromMime(const class QMimeData *m);   // the one local .hnplugin file or plugin folder in a drop, else empty
    static QString themeFileFromMime(const class QMimeData *m);   // the one local .json/.yaml/.yml url in a drop, else empty
    bool setNotesFolder(const QString &path, QString *why = nullptr);
    void setWorkspaceMode(const QString &rel, hn::platform::WorkspaceMode m);

    // ---- lifecycle ----
    void requestQuit();
    bool quitting() const { return m_quitting; }
    bool exiting() const { return m_exiting; }
    void setTiming(NoteSession::Timing t) { m_timing = t; }
    void setTrayUsableOverride(std::optional<bool> v) { m_trayOverride = v; }
    bool trayUsable() const;
    void flushSessionFile();
    const hn::platform::SessionState &sessionState() const { return m_session; }
    void stickyGeometryChanged(const QString &rel);
    void bindShortcuts(QWidget *host, std::function<NoteSession *()> current);
    void beginInteractive();                       // first-run choice, session restore, recovery offer (once)

signals:
    void exitRequested();
    void quitAborted(const QString &reason);
    void themeChanged();
    void notesChanged();                           // library content changed (create/rename/delete/save/copy)
    void noteOpened(const QString &rel);
    void noteClosed(const QString &rel);
    void noteRenamed(const QString &oldRel, const QString &newRel);
    void noteColorChanged(const QString &rel);
    void errorOccurred(const QString &message);
    void notice(const QString &message);

private:
    friend class StickyWindow;
    friend class OrganizerWindow;
    NoteSession *createSession(const QString &rel);
    StickyWindow *createSticky(NoteSession *s, bool focus, const hn::platform::WindowState *restore);
    void release(const QString &rel);
    void onCloseRefused(const QString &rel);
    void maybeExit();
    void finishOrganizerClose();
    void checkQuit();
    void ensureOrganizer();
    void retireOrganizerNote(NoteSession *prev);
    void applyThemeNow();
    static void applyThemeToken(hn::theme::Theme &t, const QString &token, const QColor &c);
    void themeSlice();
    QList<QPointer<NoteSession>> m_themeQueue;
    void onConfigChanged();
    void restoreSessionOnce();
    void restoreBatch(QList<hn::platform::WindowState> todo);
    static constexpr int kRestoreMax = 30, kRestoreBatch = 10;
    QStringList m_pendingRestoreDrop;
    void offerRecovery();
    void rebindShortcuts();
    void runAction(const QString &id, NoteSession *s, QWidget *host);
    void updateSessionEntry(const QString &rel);
    void removeSessionEntry(const QString &rel);
    void scheduleSessionSave() { m_sessionTimer.start(); }
    void refreshClients();
    void ensureFloating(const QString &tok, const hn::platform::HyprClient &c, const QSize &size, const QVector<hn::platform::HyprMonitor> &mons, bool center);
    void applyClients(const QVector<hn::platform::HyprClient> &clients, const QVector<hn::platform::HyprMonitor> &mons,
                      const QVector<hn::platform::HyprWorkspace> &wss);
    void onLink(const QString &fromRel, const QUrl &url);
    bool ask(const QString &title, const QString &text);
    void migrateLegacyPrefs(bool haveConfig);
    void rootChanged();
    QString renamedRelFor(const QString &rel, const QString &newTitle, QString *err) const;

    ControllerOptions m_opt;
    LinkUpdateReport m_linkReport;
    mutable std::optional<DockPrefs> m_dockPrefs;
    QString m_savedFolder;
    QString m_configPath, m_stateDir, m_cacheDir, m_modsEnabled;
    std::unique_ptr<hn::theme::Config> m_config;
    hn::theme::Settings m_settings;
    bool m_legacyPending = false;
    hn::theme::Theme m_theme;
    std::unique_ptr<hn::core::NoteRepository> m_repo;
    std::unique_ptr<hn::core::LibraryIndex> m_index;
    std::unique_ptr<hn::plugins::MarketClient> m_market;
    PluginService *m_plugins = nullptr;   // QObject child; deleted explicitly after the sessions
    QString m_modsDir, m_lastNotice;
    QMap<QString, QMap<QString, QString>> m_themeOverrides;   // plugin -> token -> #rrggbb
    std::unique_ptr<hn::platform::HyprlandIpc> m_ipc;
    std::unique_ptr<hn::platform::TrayController> m_tray;
    hn::platform::WindowRegistry m_registry;
    NoteStateStore m_noteState;
    QHash<QString, NoteSession *> m_sessions;
    QHash<QString, QPointer<StickyWindow>> m_stickies;
    QPointer<OrganizerWindow> m_organizer;
    NoteSession::Timing m_timing;
    hn::platform::SessionState m_session;
    QTimer m_sessionTimer;
    QHash<QString, hn::platform::WindowRef> m_refs;                 // token -> live client ref
    QHash<QString, hn::platform::WindowState> m_pendingPlacement;   // token -> state to apply once mapped
    QHash<QString, int> m_floatTries;   // token -> 1 fallback sent (verify next pass), 2 done/gave up
    bool m_refreshing = false, m_refreshAgain = false;
    std::optional<bool> m_trayOverride;
    bool m_firstRun = false, m_sessionRestored = false, m_recoveryOffered = false;
    bool m_quitting = false, m_exiting = false, m_organizerClosing = false;
    struct ShortcutHost { QPointer<QWidget> w; std::function<NoteSession *()> cur; };
    QList<ShortcutHost> m_shortcutHosts;
};

} // namespace hn::app
