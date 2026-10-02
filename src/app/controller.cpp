#include <hn/theme/theme_io.h>
#include <QMimeData>
#include "controller.h"
#include "editor_bridge.h"
#include "hn/core/fs_util.h"
#include "hn/core/paths.h"
#include "command_palette.h"
#include "organizer_window.h"
#include "policy.h"
#include "settings_dialog.h"
#include "sticky_window.h"
#include "tag_edit.h"
#include "ui_common.h"
#include <QClipboard>
#include <QElapsedTimer>
#include <QInputDialog>
#include <QAction>
#include <QApplication>
#include <QCursor>
#include <QActionGroup>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QMessageBox>
#include <QPixmap>
#include <QPainter>
#include <QRegularExpression>
#include <QShortcut>
#include <QStyleHints>
#include <QUrl>

using namespace hn::core;
using namespace hn::platform;
using hn::theme::Settings;

namespace hn::app {

// ---------------------------------------------------------------- construction
AppController::AppController(ControllerOptions opt, QObject *parent)
    : QObject(parent), m_opt(std::move(opt)),
      m_configPath(m_opt.configPath.isEmpty() ? paths::configDir() + "/config.json" : m_opt.configPath),
      m_stateDir(m_opt.stateDir.isEmpty() ? paths::stateDir() : m_opt.stateDir),
      m_cacheDir(m_opt.cacheDir.isEmpty() ? paths::cacheDir() : m_opt.cacheDir),
      m_noteState((m_opt.stateDir.isEmpty() ? paths::stateDir() : m_opt.stateDir) + "/notes-state.json") {
    m_config = std::make_unique<hn::theme::Config>(m_configPath);
    m_settings = m_config->settings();
    const bool haveConfig = QFile::exists(m_configPath);
    if (!haveConfig) {   // first-run folder choice (Settings defaults already carry trayEnabled = true)
        m_settings.notesFolder = paths::defaultNotesDir();
        m_firstRun = m_opt.notesDir.isEmpty();
    }
    migrateLegacyPrefs(haveConfig);
    m_savedFolder = m_settings.notesFolder;
    const QString root = !m_opt.notesDir.isEmpty() ? m_opt.notesDir : m_settings.notesFolder;
    if (!m_opt.notesDir.isEmpty()) m_settings.notesFolder = QDir::cleanPath(QFileInfo(m_opt.notesDir).absoluteFilePath());   // env override (HN_NOTES_DIR) is session-only
    m_repo = std::make_unique<NoteRepository>(root, m_stateDir + "/recovery");
    ui::animation().reduceMotion = m_settings.reduceMotion;
    applyThemeNow();
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, [this] {
        if (m_settings.colorScheme == "system") applyThemeNow();
    });

    m_modsEnabled = m_opt.modsEnabledPath.isEmpty() ? paths::configDir() + "/mods-enabled.json" : m_opt.modsEnabledPath;
    m_modsDir = m_opt.modsDir.isEmpty() ? paths::dataDir() + "/mods" : m_opt.modsDir;
    // Plugins: nothing is created here beyond this thin service. The manager (and any Lua / network objects) appear only when a
    // plugin is enabled (start()) or the Plugins page is used. Legacy mods no longer load on their own: they are migrated to
    // native plugins and need approval like any other plugin.
    const QString pluginsDir = !m_opt.pluginsDir.isEmpty() ? m_opt.pluginsDir
                             : !m_opt.modsDir.isEmpty() ? QFileInfo(m_opt.modsDir).absolutePath() + "/plugins" : paths::dataDir() + "/plugins";
    m_plugins = new PluginService(this, pluginsDir, m_opt.pluginHooks);
    connect(m_plugins, &PluginService::changed, this, [this] { rebindShortcuts(); });

    if (m_opt.useTray) {
        m_tray = std::make_unique<TrayController>(ui::appIcon());
        auto *settingsAct = m_tray->menu()->addAction(tr("Settings…"));
        m_tray->menu()->removeAction(settingsAct);
        m_tray->menu()->insertAction(m_tray->quitAction(), settingsAct);
        connect(settingsAct, &QAction::triggered, this, [this] { showOrganizer(true); showSettings(m_organizer); });
        connect(m_tray.get(), &TrayController::newNoteRequested, this, [this] { handleAction(Action::NewNote); });
        connect(m_tray.get(), &TrayController::showOrganizerRequested, this, [this] { handleAction(Action::ShowOrganizer); });
        connect(m_tray.get(), &TrayController::quitRequested, this, &AppController::requestQuit);
        connect(m_tray.get(), &TrayController::availabilityChanged, this, [this](bool a) {
            if (!a && m_organizer && !m_organizer->isVisible()) showOrganizer(false);   // never strand a hidden organizer
        });
        m_tray->setEnabled(m_settings.trayEnabled);
    }

    const HyprPaths hyprPaths = m_opt.hyprPaths ? *m_opt.hyprPaths : HyprPaths::fromEnv();
    if (m_opt.useHyprland && hyprPaths.valid()) {
        m_ipc = std::make_unique<HyprlandIpc>(hyprPaths);
        connect(m_ipc.get(), &HyprlandIpc::stateChanged, this, [this] { refreshClients(); });
        connect(m_ipc.get(), &HyprlandIpc::reenumerateRequested, this, [this] { m_refs.clear(); refreshClients(); });
        connect(m_ipc.get(), &HyprlandIpc::connectionChanged, this, [this](bool c) { if (!c) m_refs.clear(); else refreshClients(); });
        m_ipc->start();
    }

    m_sessionTimer.setSingleShot(true);
    m_sessionTimer.setInterval(400);
    connect(&m_sessionTimer, &QTimer::timeout, this, &AppController::flushSessionFile);
    bool ok = false;
    m_session = loadSession(m_stateDir + "/session.json", &ok);
    if (!ok) m_session = {};

    m_config->watch();
    connect(m_config.get(), &hn::theme::Config::changed, this, &AppController::onConfigChanged);
}

AppController::~AppController() {
    m_exiting = true;
    if (m_ipc) { m_ipc->disconnect(this); m_ipc.reset(); }   // its stop() emits connectionChanged: must not reach members already destroyed
    for (auto w : std::as_const(m_stickies)) if (w) w->detach();
    if (m_organizer) m_organizer->detachSession();
    qDeleteAll(m_sessions);
    m_sessions.clear();
    for (auto w : std::as_const(m_stickies)) delete w.data();
    delete m_organizer.data();
    delete m_plugins;
    m_plugins = nullptr;
}

// ---------------------------------------------------------------- launch
void AppController::start(Action initial) {
    m_plugins->startIfNeeded();   // creates the manager only when a plugin is enabled; emits app.started
    if (initial != Action::Background) handleAction(initial);
}

bool AppController::handleAction(Action a) {
    if (m_quitting) return false;
    switch (a) {
    case Action::Background: return true;   // never hides windows the user is using
    case Action::ShowOrganizer: beginInteractive(); showOrganizer(true); return true;
    case Action::NewNote: beginInteractive(); return !newNote().isEmpty();
    }
    return false;
}

void AppController::beginInteractive() {
    if (m_firstRun) {
        m_firstRun = false;
        const QString suggested = paths::defaultNotesDir();
        const QString chosen = m_opt.chooseNotesFolder ? m_opt.chooseNotesFolder(suggested) : runFirstRunDialog(suggested);
        Settings s = m_settings;
        s.notesFolder = chosen.isEmpty() ? suggested : chosen;
        applySettings(s, prefs());   // persists config.json: first run is over
    }
    restoreSessionOnce();
    offerRecovery();
    if (const int n = m_plugins->legacyModsAwaitingApproval(); n > 0)
        QTimer::singleShot(0, this, [this, n] { announce(tr("%n mod(s) from your old mods list now need approval under Settings > Plugins.", "", n)); });
}

// ---------------------------------------------------------------- theme / settings
void AppController::applyThemeNow() {
    const QString &cs = m_settings.colorScheme;
    const bool dark = cs == "dark" || (cs == "system" && QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark);
    m_theme = hn::theme::loadTheme(m_settings.theme, dark);
    if (m_settings.fontSize > 0) m_theme.baseSize = m_settings.fontSize;
    for (const auto &tokens : std::as_const(m_themeOverrides))
        for (auto it = tokens.begin(); it != tokens.end(); ++it) applyThemeToken(m_theme, it.key(), QColor(it.value()));
    ui::setTheme(m_theme);
    ui::animation().reduceMotion = m_settings.reduceMotion;
    hn::theme::applyTheme(m_theme);
    // D10: re-theming a session re-formats its whole document; do it in time-boxed slices, shown notes first.
    m_themeQueue.clear();
    QList<QPointer<NoteSession>> hidden;
    for (auto *s : std::as_const(m_sessions)) (s->editor()->isVisible() ? m_themeQueue : hidden) << QPointer<NoteSession>(s);
    m_themeQueue += hidden;
    themeSlice();
    emit themeChanged();
}

void AppController::themeSlice() {
    QElapsedTimer t; t.start();
    while (!m_themeQueue.isEmpty() && t.elapsed() < 25) {
        if (auto s = m_themeQueue.takeFirst()) s->applyTheme(m_theme);
    }
    if (!m_themeQueue.isEmpty()) QTimer::singleShot(0, this, &AppController::themeSlice);
}

// Earlier builds kept the text size in app.json next to config.json. It now lives in config.json ("fontSize"):
// import it once (only when config.json has no value yet), persist, then drop the old file.
void AppController::migrateLegacyPrefs(bool haveConfig) {
    const QString legacy = QFileInfo(m_configPath).absolutePath() + "/app.json";
    QFile f(legacy);
    if (!f.open(QIODevice::ReadOnly)) return;
    const int fs = qBound(0, QJsonDocument::fromJson(f.readAll()).object()["fontSize"].toInt(), 32);
    f.close();
    if (m_settings.fontSize == 0) m_settings.fontSize = fs;
    if (haveConfig) { if (m_config->save(m_settings)) QFile::remove(legacy); }   // else removed after the first real save
    else m_legacyPending = true;
}

void AppController::applySettings(const Settings &s0, const AppPrefs &p) {
    Settings s = s0;
    if (p.fontSize != m_settings.fontSize && s0.fontSize == m_settings.fontSize) s.fontSize = p.fontSize;   // legacy AppPrefs path
    const bool folderChanged = QDir::cleanPath(s.notesFolder) != m_repo->root();
    if (folderChanged) m_opt.notesDir.clear();   // an explicit choice replaces the session override
    const bool organizerHidden = m_organizer && !m_organizer->isVisible();
    const Settings old = m_settings;
    m_settings = s;
    const bool prefsChanged = s.fontSize != old.fontSize;
    Settings toSave = s;
    if (!m_opt.notesDir.isEmpty()) toSave.notesFolder = m_savedFolder;   // never persist the HN_NOTES_DIR override
    else m_savedFolder = s.notesFolder;
    if (!m_config->save(toSave)) emit errorOccurred(tr("Could not write %1: settings apply for this session only.").arg(m_config->path()));
    else if (m_legacyPending) { m_legacyPending = false; QFile::remove(QFileInfo(m_configPath).absolutePath() + "/app.json"); }
    if (m_tray && old.trayEnabled != s.trayEnabled) m_tray->setEnabled(s.trayEnabled, organizerHidden);
    if (old.colorScheme != s.colorScheme || old.theme != s.theme || old.reduceMotion != s.reduceMotion || prefsChanged) applyThemeNow();
    if (old.keybindings != s.keybindings) rebindShortcuts();
    if (folderChanged) {
        QString why;
        if (!setNotesFolder(s.notesFolder, &why)) emit errorOccurred(why);
    }
}

bool AppController::importThemeFile(const QString &path, QString *message) {
    QString name, err;
    QStringList warnings;
    if (!hn::theme::importTheme(path, &name, &err, &warnings)) { if (message) *message = tr("Theme not imported: %1").arg(err); return false; }
    Settings s = m_settings;
    s.theme = name;
    applySettings(s, prefs());
    if (message) *message = (tr("Imported theme \"%1\".").arg(name) + (warnings.isEmpty() ? QString() : "\n" + warnings.join("\n")));
    return true;
}

QString AppController::themeFileFromMime(const QMimeData *m) {
    if (!m || !m->hasUrls() || m->urls().size() != 1 || !m->urls().first().isLocalFile()) return {};
    const QString p = m->urls().first().toLocalFile();
    const QString ext = QFileInfo(p).suffix().toLower();
    return ext == "json" || ext == "yaml" || ext == "yml" ? p : QString();
}

QString AppController::pluginPathFromMime(const QMimeData *m) {
    if (!m || !m->hasUrls() || m->urls().size() != 1 || !m->urls().first().isLocalFile()) return {};
    const QString p = m->urls().first().toLocalFile();
    const QFileInfo fi(p);
    if (fi.isDir()) return QFileInfo::exists(p + "/plugin.json") ? p : QString();
    const QString n = fi.fileName().toLower();
    return n.endsWith(".hnplugin") || n.endsWith(".zip") || n.endsWith(".tar") || n.endsWith(".tar.gz") ? p : QString();
}

void AppController::onConfigChanged() {
    const Settings s = m_config->settings();
    if (s == m_settings) return;
    const Settings old = m_settings;
    m_settings = s;
    if (m_tray && old.trayEnabled != s.trayEnabled) m_tray->setEnabled(s.trayEnabled, m_organizer && !m_organizer->isVisible());
    applyThemeNow();
    rebindShortcuts();
}

bool AppController::setNotesFolder(const QString &path, QString *why) {
    const QString clean = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    if (clean == m_repo->root()) return true;
    if (!m_sessions.isEmpty()) {
        if (why) *why = tr("Notes folder saved. Close open notes and restart Hyprnotes to switch libraries.");
        return false;
    }
    m_plugins->quiesce();   // an index call that overran its 100 ms must not outlive the index
    m_index.reset();
    m_repo = std::make_unique<NoteRepository>(clean, m_stateDir + "/recovery");
    rootChanged();
    return true;
}

void AppController::rootChanged() {
    m_session = {};
    if (m_organizer) { m_organizer->detachSession(); m_organizer->refresh(); }
    emit notesChanged();
}

LibraryIndex *AppController::index() {
    if (!m_index) {
        m_index = std::make_unique<LibraryIndex>(m_repo->root(), m_cacheDir);
        m_index->sync();
        m_repo->watchLibrary(true);
        connect(m_repo.get(), &NoteRepository::libraryChanged, m_index.get(), [this] { if (m_index) m_index->sync(); });
    }
    return m_index.get();
}

// ---------------------------------------------------------------- sessions
NoteSession *AppController::createSession(const QString &rel) {
    auto *s = new NoteSession(m_repo.get(), rel, this);
    s->setTiming(m_timing);
    s->applyTheme(m_theme);   // BEFORE load: NoteEditor::setTheme() after a load would wipe undo and mark the note modified
    QString err;
    if (!s->load(&err)) {
        delete s;
        emit errorOccurred(tr("Cannot open %1: %2").arg(rel, err));
        qWarning("cannot open %s: %s", qPrintable(rel), qPrintable(err));
        return nullptr;
    }
    const NoteView v = m_noteState.get(rel);
    if (v.cursor || v.scroll) s->restoreView(v.cursor, v.scroll);
    connect(s, &NoteSession::savedOk, this, [this](const QString &r) {
        if (m_index) m_index->updatePath(r);
        emit notesChanged();
    });
    connect(s, &NoteSession::copyCreated, this, [this](const QString &r) { if (m_index) m_index->updatePath(r); emit notesChanged(); });
    connect(s, &NoteSession::removedWhileClean, this, [this](const QString &r) { closeNote(r, true); emit notesChanged(); });
    connect(s->editor(), &hn::editor::NoteEditor::linkActivated, this, [this, s](const QUrl &u) { onLink(s->rel(), u); });
    m_sessions.insert(rel, s);
    m_plugins->attach(s);   // events, pre_save, triggers, toolbar buttons; no-op while no plugin is enabled
    emit noteOpened(rel);
    return s;
}

void AppController::onLink(const QString &fromRel, const QUrl &url) {
    if (url.scheme() == "http" || url.scheme() == "https" || url.scheme() == "mailto") { QDesktopServices::openUrl(url); return; }
    if (url.scheme().isEmpty() || url.isRelative()) {   // relative link to another note in the library
        QString target = QDir::cleanPath((QFileInfo(fromRel).path() == "." ? QString() : QFileInfo(fromRel).path() + "/") + url.path());
        if (!target.endsWith(".md", Qt::CaseInsensitive)) target += ".md";
        if (QFile::exists(m_repo->absolutePath(target))) openSticky(target, true);
    }
}

StickyWindow *AppController::stickyOf(const QString &rel) const { return m_stickies.value(rel).data(); }

QList<StickyWindow *> AppController::stickies() const {
    QList<StickyWindow *> out;
    for (auto &w : m_stickies) if (w) out << w.data();
    return out;
}

QString AppController::organizerNote() const {
    return m_organizer && m_organizer->currentSession() ? m_organizer->currentSession()->rel() : QString();
}

int AppController::windowCount() const {
    int n = 0;
    for (auto &w : m_stickies) if (w) ++n;
    if (m_organizer) ++n;
    return n;
}

StickyWindow *AppController::createSticky(NoteSession *s, bool focus, const WindowState *restore) {
    auto *w = new StickyWindow(this, s);
    m_stickies.insert(s->rel(), w);
    m_registry.bind(w->token(), s->rel());
    if (restore) {
        if (restore->geometry.isValid() && restore->geometry.width() >= 100) w->resize(restore->geometry.size());
        w->setWorkspaceMode(restore->mode);
        if (m_ipc) m_pendingPlacement.insert(w->token(), *restore);
    }
    w->present(focus);
    updateSessionEntry(s->rel());
    scheduleSessionSave();
    if (m_ipc) QTimer::singleShot(0, this, &AppController::refreshClients);
    return w;
}

NoteSession *AppController::openSticky(const QString &rel, bool focus, const WindowState *restore) {
    if (NoteSession *s = session(rel)) {
        if (auto *w = stickyOf(rel)) { w->present(focus); return s; }
        if (m_organizer && m_organizer->currentSession() == s) { popOut(rel); return s; }
        // orphaned (closing) session: give it a window so nothing is hidden
        createSticky(s, focus, restore);
        return s;
    }
    NoteSession *s = createSession(rel);
    if (!s) return nullptr;
    createSticky(s, focus, restore);
    return s;
}

NoteSession *AppController::openInOrganizer(const QString &rel) {
    if (NoteSession *s = session(rel)) {
        if (auto *w = stickyOf(rel)) {   // already open elsewhere: focus that editor, never duplicate it
            w->present(true);
            ensureOrganizer();
            // D15: showElsewhere() drops the organizer's current note; it must be retired, not left without a window.
            if (NoteSession *prev = m_organizer->currentSession(); prev && prev != s) { m_organizer->detachSession(); retireOrganizerNote(prev); }
            m_organizer->showElsewhere(rel);
            return s;
        }
        if (m_organizer && m_organizer->currentSession() == s) return s;
    }
    ensureOrganizer();
    NoteSession *prev = m_organizer->currentSession();
    NoteSession *s = session(rel);
    if (!s) s = createSession(rel);
    if (!s) return nullptr;
    if (prev && prev != s) { m_organizer->detachSession(); retireOrganizerNote(prev); }
    m_organizer->attachSession(s);
    return s;
}

// The organizer moved on from `prev`: finish its save, release it, or keep it visible as a sticky if it cannot close.
void AppController::retireOrganizerNote(NoteSession *prev) {
    const QString rel = prev->rel();
    closeNote(rel);
}

bool AppController::popOut(const QString &rel) {
    NoteSession *s = session(rel);
    if (!s || stickyOf(rel)) return false;
    if (m_organizer && m_organizer->currentSession() == s) {
        m_organizer->detachSession();
        m_organizer->showElsewhere(rel);
    }
    createSticky(s, true, nullptr);
    return true;
}

bool AppController::popIn(const QString &rel) {
    NoteSession *s = session(rel);
    StickyWindow *w = stickyOf(rel);
    if (!s || !w) {   // sticky already closed (note released) or never a sticky: open it in the organizer
        openInOrganizer(rel);
        if (m_organizer) m_organizer->present(true);
        return true;
    }
    ensureOrganizer();
    NoteSession *prev = m_organizer->currentSession();
    if (prev && prev != s) { m_organizer->detachSession(); retireOrganizerNote(prev); }
    m_stickies.remove(rel);
    m_registry.unbind(w->token());
    m_refs.remove(w->token());
    m_pendingPlacement.remove(w->token());
    m_floatTries.remove(w->token());
    w->detach();
    w->markTransferred();
    w->close();
    w->deleteLater();
    removeSessionEntry(rel);
    m_organizer->attachSession(s);
    m_organizer->present(true);
    return true;
}

void AppController::closeNote(const QString &rel, bool discard) {
    NoteSession *s = session(rel);
    if (!s) return;
    if (discard) { s->discard(); release(rel); return; }
    connect(s, &NoteSession::closeReady, this, [this, rel] { release(rel); }, Qt::SingleShotConnection);
    connect(s, &NoteSession::closeRefused, this, [this, rel] { onCloseRefused(rel); }, Qt::SingleShotConnection);
    s->requestClose();
}

void AppController::discardNote(const QString &rel) {
    if (ask(tr("Discard changes?"), tr("Your unsaved edits to this note will be permanently discarded. The file on disk is kept as it is."))) closeNote(rel, true);
}

void AppController::onCloseRefused(const QString &rel) {
    NoteSession *s = session(rel);
    if (!s) return;
    if (auto *w = stickyOf(rel)) w->present(false);
    else if (m_organizer && m_organizer->currentSession() == s) { m_organizer->present(false); }
    else createSticky(s, true, nullptr);   // orphan: keep the editor and its error visible
    m_organizerClosing = false;
    if (m_quitting) { m_quitting = false; emit quitAborted(s->detail()); }
}

void AppController::release(const QString &rel) {
    NoteSession *s = m_sessions.take(rel);
    if (!s) return;
    m_noteState.setView(rel, s->cursorPosition(), s->scrollValue());
    m_plugins->release(s);   // note.closed, then the bridge is detached before the session dies
    m_repo->unwatch(rel);
    if (auto w = m_stickies.take(rel)) {
        m_registry.unbind(w->token());
        m_refs.remove(w->token());
        m_pendingPlacement.remove(w->token());
        m_floatTries.remove(w->token());
        w->detach();
        w->markTransferred();
        w->close();
        w->deleteLater();
        if (!m_exiting) { removeSessionEntry(rel); scheduleSessionSave(); }
    }
    if (m_organizer && m_organizer->currentSession() == s) m_organizer->detachSession();
    if (s->editor()) s->editor()->releaseResources();
    s->deleteLater();
    if (m_organizer) m_organizer->noteReleased(rel);
    emit noteClosed(rel);
    if (m_quitting) checkQuit();
    else if (m_organizerClosing && m_organizer && !m_organizer->currentSession()) finishOrganizerClose();
    else maybeExit();
}

// ---------------------------------------------------------------- organizer
void AppController::ensureOrganizer() {
    if (m_organizer) return;
    index();
    m_organizer = new OrganizerWindow(this);
}

void AppController::showOrganizer(bool focus) {
    ensureOrganizer();
    m_organizer->present(focus);
}

void AppController::toggleOrganizer() {
    if (m_organizer && m_organizer->isVisible()) organizerCloseRequested();
    else showOrganizer(true);
}

bool AppController::trayUsable() const {
    if (m_trayOverride) return *m_trayOverride;
    return m_tray && m_tray->enabled() && m_tray->available();
}

bool AppController::organizerCloseRequested() {
    if (!m_organizer || m_exiting) return true;
    const bool enabled = m_tray ? m_tray->enabled() : false;
    if (organizerCloseAction(m_trayOverride ? *m_trayOverride : enabled, trayUsable()) == OrganizerClose::HideToTray) {
        m_organizer->hide();   // keeps the open editor, selection, cursor and history
        return false;
    }
    m_organizerClosing = true;   // normal close: save path first, then the lazily created window goes away
    if (NoteSession *s = m_organizer->currentSession()) closeNote(s->rel());
    else finishOrganizerClose();
    return false;
}

void AppController::finishOrganizerClose() {
    m_organizerClosing = false;
    OrganizerWindow *org = m_organizer;
    if (!org) return;
    m_organizer = nullptr;
    org->hide();
    org->deleteLater();
    maybeExit();
}

void AppController::maybeExit() {
    if (m_exiting || m_quitting) return;
    if (exitAfterWindowClosed(trayUsable(), windowCount())) {
        m_exiting = true;
        flushSessionFile();
        emit exitRequested();
    }
}

void AppController::showSettings(QWidget *parent, int tab) {
    static QPointer<SettingsDialog> open;
    if (open) { if (tab >= 0) open->showTab(tab); open->raise(); open->activateWindow(); return; }
    open = new SettingsDialog(this, parent);
    if (tab >= 0) open->showTab(tab);
    open->setAttribute(Qt::WA_DeleteOnClose);
    open->show();
}

// ---------------------------------------------------------------- note operations
QString AppController::newNote(const QString &folder, bool intoOrganizer) {
    QString err;
    const QString rel = m_repo->create(folder, tr("Untitled"), {}, &err);
    if (rel.isEmpty()) {
        emit errorOccurred(tr("Could not create a note: %1").arg(err));
        qWarning("new note failed: %s", qPrintable(err));
        return {};
    }
    if (m_index) m_index->updatePath(rel);
    emit notesChanged();
    if (intoOrganizer && m_organizer && m_organizer->isVisible()) {
        openInOrganizer(rel);
        if (auto *s = session(rel)) s->editor()->focusEditor();
    } else openSticky(rel, true);
    return rel;
}

void AppController::setNoteColor(const QString &rel, int idx) {
    m_noteState.setColor(rel, idx);
    emit noteColorChanged(rel);
}

QString AppController::renamedRelFor(const QString &rel, const QString &newTitle, QString *err) const {
    QString base = newTitle;
    base.replace(QRegularExpression("[/\\\\\\x00-\\x1f]"), " ");
    base = base.trimmed();
    while (base.startsWith('.')) base.remove(0, 1);
    base = base.trimmed().left(120);
    if (base.isEmpty()) { if (err) *err = tr("The name cannot be empty."); return {}; }
    const QString dir = QFileInfo(rel).path() == "." ? QString() : QFileInfo(rel).path() + "/";
    return dir + base + ".md";
}

bool AppController::renameNote(const QString &rel, const QString &newTitle, QString *err) {
    auto fail = [&](const QString &m) { if (err) *err = m; return false; };
    QString why;
    const QString newRel = renamedRelFor(rel, newTitle, &why);
    if (newRel.isEmpty()) return fail(why);
    if (newRel == rel) return true;
    NoteSession *s = session(rel);
    if (s && !s->settled()) { s->flush(); return fail(tr("The note is still saving. Try again in a moment.")); }
    QString e;
    if (!m_repo->rename(rel, newRel, &e)) return fail(e);
    if (s) {
        m_sessions.remove(rel);
        m_sessions.insert(newRel, s);
        s->setRel(newRel);
        if (auto w = m_stickies.take(rel)) { m_stickies.insert(newRel, w); m_registry.bind(w->token(), newRel); }
    }
    m_noteState.rename(rel, newRel);
    if (m_index) m_index->renamePath(rel, newRel);
    for (auto &w : m_session.windows) if (w.noteKey == rel) w.noteKey = newRel;
    scheduleSessionSave();
    emit noteRenamed(rel, newRel);
    emit notesChanged();
    return true;
}

bool AppController::deleteNote(const QString &rel, QString *err) {
    if (session(rel)) closeNote(rel, true);
    QString e;
    if (!m_repo->remove(rel, &e)) { if (err) *err = e; return false; }
    if (m_index) m_index->removePath(rel);
    m_noteState.remove(rel);
    removeSessionEntry(rel);
    scheduleSessionSave();
    emit notesChanged();
    return true;
}

bool AppController::editTags(const QString &rel, const QStringList &desired) {
    NoteSession *s = session(rel);
    if (!s) return false;
    const QStringList current = extractMeta(QFileInfo(rel).completeBaseName(), s->editor()->toMarkdownBytes()).tags;
    QStringList add, remove;
    for (const auto &t : desired) if (!current.contains(t)) add << t;
    for (const auto &t : current) if (!desired.contains(t)) remove << t;
    applyTagEdit(s->editor(), add, remove);
    return true;
}

bool AppController::runModCommand(const QString &rel, const QString &commandId) {
    NoteSession *s = session(rel);
    if (!s || !m_plugins->active()) return false;
    for (const auto &a : m_plugins->commands())
        if (a.qid.endsWith(QLatin1Char(':') + commandId)) return m_plugins->run(a, s);
    return false;
}

DockPrefs AppController::dockPrefs() const {
    if (!m_dockPrefs) {
        DockPrefs p;
        QFile f(m_stateDir + "/ui-state.json");
        if (f.open(QIODevice::ReadOnly)) {
            const QJsonObject d = QJsonDocument::fromJson(f.readAll()).object().value("dock").toObject();
            if (d.contains("visible")) p.visible = d.value("visible").toBool(true);
            p.width = qBound(240, d.value("width").toInt(p.width), 640);
        }
        m_dockPrefs = p;
    }
    return *m_dockPrefs;
}

void AppController::setDockPrefs(const DockPrefs &p0) {
    DockPrefs p = p0;
    p.width = qBound(240, p.width, 640);
    const DockPrefs cur = dockPrefs();
    if (cur.visible == p.visible && cur.width == p.width) return;
    m_dockPrefs = p;
    QDir().mkpath(m_stateDir);
    hn::core::atomicWrite(m_stateDir + "/ui-state.json",
                          QJsonDocument(QJsonObject{{"version", 1}, {"dock", QJsonObject{{"visible", p.visible}, {"width", p.width}}}}).toJson(QJsonDocument::Compact), nullptr);
}

QList<DirtyDoc> AppController::dirtyDocs() const {
    QList<DirtyDoc> out;
    for (auto *s : m_sessions) if (s->editor()->isModified()) out.append({s->rel(), QString::fromUtf8(s->editor()->toMarkdownBytes())});
    return out;
}

void AppController::populateNoteMenu(QMenu *menu, const QString &rel, bool withDelete) {
    NoteSession *s = session(rel);
    auto *colorAct = menu->addAction(tr("Color…"));
    connect(colorAct, &QAction::triggered, this, [this, rel] {
        const QPoint at = QCursor::pos();
        QTimer::singleShot(0, this, [this, rel, at] {   // after the menu has closed, so the popover keeps focus
            auto *pop = new ui::SwatchPopover(noteColor(rel));
            connect(pop, &ui::SwatchPopover::picked, this, [this, rel](int i) { setNoteColor(rel, i); });
            pop->popup(at);
        });
    });
    if (s) {
        const bool src = s->editor()->mode() == hn::editor::Mode::Source;
        auto *a = menu->addAction(ui::icon(src ? "visual" : "source"), src ? tr("Switch to visual mode") : tr("Switch to source mode"));
        connect(a, &QAction::triggered, this, [s, src] { s->editor()->setMode(src ? hn::editor::Mode::Visual : hn::editor::Mode::Source); });
        auto items = m_plugins->commands() + m_plugins->menuItems(QStringLiteral("note"));
        if (!items.isEmpty()) {
            auto *pm = menu->addMenu(tr("Plugins"));
            pm->setObjectName(QStringLiteral("hnPluginsMenu"));
            const auto keys = m_plugins->pluginKeys();
            for (const auto &a : items) {
                const QKeySequence ks = keys.value(QStringLiteral("plugin:") + a.qid);
                auto *pa = pm->addAction(QStringLiteral("%1: %2").arg(a.pluginName, a.title) + (ks.isEmpty() ? QString() : QStringLiteral("\t") + ks.toString(QKeySequence::NativeText)));
                connect(pa, &QAction::triggered, this, [this, rel, a] { if (auto *ss = session(rel)) m_plugins->run(a, ss); });
            }
        }
    }
    auto *help = menu->addAction(tr("Keyboard shortcuts\tF1"));
    connect(help, &QAction::triggered, this, [this] {
        if (QWidget *w = QApplication::activeWindow()) showShortcuts(w);
    });
    if (withDelete) {
        menu->addSeparator();
        auto *d = menu->addAction(ui::icon("close", m_theme.danger), tr("Delete note…"));
        connect(d, &QAction::triggered, this, [this, rel] {
            if (!ask(tr("Delete note?"), tr("“%1” will be permanently deleted from disk.").arg(QFileInfo(rel).completeBaseName()))) return;
            QString err;
            if (!deleteNote(rel, &err)) emit errorOccurred(tr("Could not delete: %1").arg(err));
        });
    }
}

QString AppController::duplicateNote(const QString &rel, QString *err) {
    QByteArray bytes;
    if (NoteSession *s = session(rel)) bytes = s->editor()->toMarkdownBytes();
    else {
        QFile f(m_repo->absolutePath(rel));
        if (!f.open(QIODevice::ReadOnly)) { if (err) *err = f.errorString(); return {}; }
        bytes = f.readAll();
    }
    const QString dir = QFileInfo(rel).path() == "." ? QString() : QFileInfo(rel).path();
    const QString copy = m_repo->create(dir, tr("%1 copy").arg(QFileInfo(rel).completeBaseName()), bytes, err);
    if (copy.isEmpty()) return {};
    if (m_index) m_index->updatePath(copy);
    emit notesChanged();
    return copy;
}

void AppController::promptRename(const QString &rel, QWidget *parent) {
    bool ok = false;
    const QString name = QInputDialog::getText(parent, tr("Rename note"), tr("File name"), QLineEdit::Normal, QFileInfo(rel).completeBaseName(), &ok);
    if (!ok) return;
    QString err;
    if (!renameNoteWithLinks(rel, name, LinkMode::Ask, &err) && !err.isEmpty()) emit errorOccurred(err);
}

void AppController::promptEditTags(const QString &rel, QWidget *parent) {
    if (!session(rel)) openInOrganizer(rel);   // tags are #tokens in the text, so the note must be open to edit them
    NoteSession *s = session(rel);
    if (!s) return;
    QStringList cur;
    for (const auto &g : s->tags()) cur << "#" + g;
    bool ok = false;
    const QString text = QInputDialog::getText(parent, tr("Edit tags"), tr("Tags are #tokens in the note text, e.g. #work #ideas"), QLineEdit::Normal, cur.join(' '), &ok);
    if (ok) editTags(rel, parseTagInput(text));
}

void AppController::showNoteContextMenu(const QString &rel, const QPoint &globalPos, QWidget *parent, bool inSticky) {
    QMenu menu(parent);
    const bool open = session(rel) != nullptr;
    const bool inWindow = stickyOf(rel) != nullptr;
    if (!inSticky) {
        connect(menu.addAction(ui::icon("visual"), tr("Open in organizer")), &QAction::triggered, this, [this, rel] { showOrganizer(true); openInOrganizer(rel); });
        connect(menu.addAction(ui::icon("popout"), inWindow ? tr("Focus sticky window") : tr("Open in sticky window")), &QAction::triggered, this, [this, rel] { openSticky(rel, true); });
        menu.addSeparator();
    }
    connect(menu.addAction(ui::icon("more"), tr("Rename…\tF2")), &QAction::triggered, this, [this, rel, parent] { promptRename(rel, parent); });
    connect(menu.addAction(ui::icon("tag"), tr("Edit tags…")), &QAction::triggered, this, [this, rel, parent] { promptEditTags(rel, parent); });
    populateNoteMenu(&menu, rel, false);   // color, mode switch, mod commands, shortcuts
    menu.addSeparator();
    connect(menu.addAction(tr("Duplicate")), &QAction::triggered, this, [this, rel] {
        QString err;
        if (duplicateNote(rel, &err).isEmpty()) emit errorOccurred(tr("Could not duplicate: %1").arg(err));
    });
    connect(menu.addAction(tr("Copy file path")), &QAction::triggered, this, [this, rel] { QGuiApplication::clipboard()->setText(m_repo->absolutePath(rel)); });
    connect(menu.addAction(tr("Show in folder")), &QAction::triggered, this, [this, rel] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(m_repo->absolutePath(rel)).absolutePath()));
    });
    menu.addSeparator();
    if (inSticky) {
        connect(menu.addAction(ui::icon("popin"), tr("Pop in to organizer")), &QAction::triggered, this, [this, rel] { popIn(rel); });
        connect(menu.addAction(tr("Close note")), &QAction::triggered, this, [this, rel] { closeNote(rel); });
    } else if (open) {
        connect(menu.addAction(tr("Close note")), &QAction::triggered, this, [this, rel] { closeNote(rel); });
    }
    auto *d = menu.addAction(ui::icon("close", m_theme.danger), tr("Delete note…"));
    connect(d, &QAction::triggered, this, [this, rel] {
        if (!ask(tr("Delete note?"), tr("“%1” will be permanently deleted from disk.").arg(QFileInfo(rel).completeBaseName()))) return;
        QString err;
        if (!deleteNote(rel, &err)) emit errorOccurred(tr("Could not delete: %1").arg(err));
    });
    menu.exec(globalPos);
}

bool AppController::ask(const QString &title, const QString &text) {
    if (m_opt.confirm) return m_opt.confirm(title, text);
    QMessageBox box(QMessageBox::Warning, title, text, QMessageBox::NoButton, m_organizer.data());
    auto *yes = box.addButton(tr("Continue"), QMessageBox::DestructiveRole);
    box.addButton(tr("Cancel"), QMessageBox::RejectRole);
    box.exec();
    return box.clickedButton() == yes;
}

// ---------------------------------------------------------------- recovery and session restore
void AppController::offerRecovery() {
    if (m_recoveryOffered) return;
    m_recoveryOffered = true;
    const QList<RecoveryEntry> drafts = m_repo->recoverableDrafts();
    if (drafts.isEmpty()) return;
    RecoveryChoice choice = RecoveryChoice::Later;
    if (m_opt.recoveryPrompt) choice = m_opt.recoveryPrompt(drafts);
    else {
        QMessageBox box(QMessageBox::Question, tr("Recover unsaved changes?"),
                        tr("Hyprnotes found unsaved changes from a previous session in %n note(s).", "", drafts.size()),
                        QMessageBox::NoButton, nullptr);
        auto *restore = box.addButton(tr("Restore"), QMessageBox::AcceptRole);
        auto *discard = box.addButton(tr("Discard"), QMessageBox::DestructiveRole);
        box.addButton(tr("Later"), QMessageBox::RejectRole);
        box.exec();
        choice = box.clickedButton() == restore ? RecoveryChoice::Restore : box.clickedButton() == discard ? RecoveryChoice::Discard : RecoveryChoice::Later;
    }
    for (const auto &e : drafts) {
        if (choice == RecoveryChoice::Discard) { m_repo->resolveRecovery(e.relPath); continue; }
        if (choice != RecoveryChoice::Restore) continue;
        if (e.conflict && !e.external.isEmpty()) {   // keep the other side of an unresolved conflict as its own note
            QString err;
            m_repo->create(QFileInfo(e.relPath).path() == "." ? QString() : QFileInfo(e.relPath).path(),
                           QFileInfo(e.relPath).completeBaseName() + " (external copy)", e.external, &err);
        }
        const QString abs = m_repo->absolutePath(e.relPath);
        if (!abs.isEmpty() && !QFile::exists(abs)) {
            QDir().mkpath(QFileInfo(abs).absolutePath());
            hn::core::atomicWrite(abs, e.local, nullptr);
        }
        NoteSession *s = session(e.relPath);
        if (!s) s = openSticky(e.relPath, false);
        if (s) s->loadDraft(e.local);
    }
    emit notesChanged();
}

// D13: at most kRestoreMax windows are restored, kRestoreBatch per event-loop turn (first batch synchronously, so a
// normal session is unchanged). Entries beyond the cap stay in session.json as closed-but-restorable.
void AppController::restoreSessionOnce() {
    if (m_sessionRestored) return;
    m_sessionRestored = true;
    QList<WindowState> todo;
    int skipped = 0;
    QStringList drop;
    for (const WindowState &ws : std::as_const(m_session.windows)) {
        if (ws.role != Role::Sticky) continue;
        const QString abs = m_repo->absolutePath(ws.noteKey);
        if (abs.isEmpty() || !QFile::exists(abs)) { drop << ws.noteKey; continue; }
        if (todo.size() < kRestoreMax) todo << ws; else ++skipped;
    }
    for (const QString &k : std::as_const(drop)) removeSessionEntry(k);
    if (skipped) emit errorOccurred(tr("Restored %1 of %2 notes from the last session; the other %3 stay in the session file and were not reopened.").arg(todo.size()).arg(todo.size() + skipped).arg(skipped));
    restoreBatch(std::move(todo));
}

void AppController::restoreBatch(QList<WindowState> todo) {
    for (int n = 0; n < kRestoreBatch && !todo.isEmpty() && !m_exiting; ++n) {
        const WindowState ws = todo.takeFirst();
        openSticky(ws.noteKey, /*focus=*/false, &ws);   // passive: never steals focus
    }
    if (!todo.isEmpty() && !m_exiting) QTimer::singleShot(0, this, [this, todo = std::move(todo)]() mutable { restoreBatch(std::move(todo)); });
}

void AppController::updateSessionEntry(const QString &rel) {
    StickyWindow *w = stickyOf(rel);
    if (!w) return;
    WindowState *st = nullptr;
    for (auto &x : m_session.windows) if (x.noteKey == rel) { st = &x; break; }
    if (!st) { WindowState ns; ns.noteKey = rel; m_session.windows.append(ns); st = &m_session.windows.last(); }
    st->role = Role::Sticky;
    st->mode = w->workspaceMode();
    if (!m_ipc || !m_refs.contains(w->token())) {
        // Wayland clients cannot know their global position: keep the last known one, update the size.
        const QRect old = st->geometry;
        st->geometry = QRect(old.isValid() ? old.topLeft() : QPoint(0, 0), w->size());
    }
}

void AppController::removeSessionEntry(const QString &rel) {
    for (int i = 0; i < m_session.windows.size(); ++i)
        if (m_session.windows[i].noteKey == rel) { m_session.windows.removeAt(i); break; }
}

void AppController::stickyGeometryChanged(const QString &rel) {
    if (m_exiting) return;
    updateSessionEntry(rel);
    scheduleSessionSave();
}

void AppController::flushSessionFile() {
    m_sessionTimer.stop();
    QString err;
    if (!saveSession(m_stateDir + "/session.json", m_session, &err)) qWarning("session not saved: %s", qPrintable(err));
}

// ---------------------------------------------------------------- Hyprland
void AppController::setWorkspaceMode(const QString &rel, WorkspaceMode m) {
    auto *w = stickyOf(rel);
    if (!w) return;
    w->setWorkspaceMode(m);
    updateSessionEntry(rel);
    scheduleSessionSave();
    if (m_ipc && m_refs.contains(w->token()))
        hn::platform::setWorkspaceMode(*m_ipc, m_refs.value(w->token()), m, [this](const Reply &r) { if (r.unknownState || !r.ok()) refreshClients(); });
}

void AppController::refreshClients() {
    if (!m_ipc || !m_ipc->connected() || (m_stickies.isEmpty() && !(m_organizer && m_settings.organizerFloating))) return;
    if (m_refreshing) { m_refreshAgain = true; return; }
    m_refreshing = true;
    auto done = [this] {
        m_refreshing = false;
        if (std::exchange(m_refreshAgain, false)) refreshClients();
    };
    m_ipc->queryClients([this, done](bool ok, QVector<HyprClient> clients, const Reply &) {
        if (!ok || !m_ipc) return done();
        m_ipc->queryMonitors([this, done, clients](bool ok2, QVector<HyprMonitor> mons, const Reply &) {
            if (!ok2 || !m_ipc) return done();
            m_ipc->queryWorkspaces([this, done, clients, mons](bool ok3, QVector<HyprWorkspace> wss, const Reply &) {
                if (ok3) applyClients(clients, mons, wss);
                done();
            });
        });
    });
}

void AppController::applyClients(const QVector<HyprClient> &clients, const QVector<HyprMonitor> &mons, const QVector<HyprWorkspace> &wss) {
    const QString appId = QGuiApplication::desktopFileName().isEmpty() ? QStringLiteral("hyprnotes") : QGuiApplication::desktopFileName();
    const auto mine = resolveClients(clients, QCoreApplication::applicationPid(), appId);
    bool changed = false;
    if (m_organizer && m_settings.organizerFloating && mine.contains(m_organizer->token()))
        ensureFloating(m_organizer->token(), mine[m_organizer->token()], m_settings.organizerSize, mons, true);
    for (auto it = m_stickies.begin(); it != m_stickies.end(); ++it) {
        StickyWindow *w = it.value();
        if (!w || !mine.contains(w->token())) continue;
        const HyprClient &c = mine[w->token()];
        const QString rel = it.key();
        m_refs.insert(w->token(), c.ref);
        if (m_pendingPlacement.contains(w->token())) {   // first time mapped: restore saved placement once
            const WindowState ps = m_pendingPlacement.take(w->token());
            m_floatTries[w->token()] = 2;   // applyPlacement floats
            applyPlacement(*m_ipc, c.ref, planPlacement(ps, mons, wss), [this](bool) { refreshClients(); });
            continue;
        }
        ensureFloating(w->token(), c, m_settings.stickySize, mons, false);   // first map only; no-op once settled
        if (!c.floating && m_floatTries.value(w->token()) == 1) continue;   // fallback in flight: do not record tiled geometry
        WindowState st = stateFromClient(c, rel, Role::Sticky, mons);
        st.mode = c.pinned ? WorkspaceMode::AllWorkspaces : WorkspaceMode::ThisWorkspace;
        w->setWorkspaceMode(st.mode);
        bool found = false;
        for (auto &x : m_session.windows)
            if (x.noteKey == rel) {
                changed = changed || x.geometry != st.geometry || x.mode != st.mode || x.workspace != st.workspace;
                x = st;
                found = true;
            }
        if (!found) { m_session.windows.append(st); changed = true; }
    }
    if (changed) scheduleSessionSave();
}

// Float-without-rules fallback. Idempotent sets, no polling: the reply (or a timeout) triggers one re-query, which
// verifies in the next applyClients pass (tries: 1 = sent, verify; a second attempt if still tiled; then give up).
void AppController::ensureFloating(const QString &tok, const HyprClient &c, const QSize &size, const QVector<HyprMonitor> &mons, bool center) {
    int &tries = m_floatTries[tok];
    if (c.floating) { tries = 2; return; }   // rules worked, or verified; never fight a later user re-tile
    if (tries >= 2 || !m_ipc) return;
    tries = tries == 1 ? 2 : 1;
    QRect r(QPoint(), size);
    if (center)
        for (const auto &m : mons)
            if (m.id == c.monitor) r.moveCenter(m.workArea().center());
    QPointer<HyprlandIpc> ip(m_ipc.get());
    ip->setFloating(c.ref, true, [this, ip, ref = c.ref, r, center](const Reply &a) {
        if (!ip || !a.dispatchOk()) { refreshClients(); return; }   // timeout/stale: reconcile from a fresh query
        ip->resizeExact(ref, r.width(), r.height(), [this, ip, ref, r, center](const Reply &) {
            if (!ip) return;
            if (center) ip->moveExact(ref, r.x(), r.y(), [this](const Reply &) { refreshClients(); });
            else refreshClients();
        });
    });
}

// ---------------------------------------------------------------- shutdown
void AppController::requestQuit() {
    if (m_exiting) return;
    m_quitting = true;
    for (auto *s : std::as_const(m_sessions)) {
        s->flush();
        connect(s, &NoteSession::statusChanged, this, [this] { if (m_quitting) checkQuit(); });
    }
    checkQuit();
}

void AppController::checkQuit() {
    if (!m_quitting) return;
    NoteSession *blocked = nullptr;
    for (auto *s : std::as_const(m_sessions)) {
        if (s->settled()) continue;
        using St = NoteSession::State;
        if (s->state() == St::Failed || s->state() == St::Conflict || s->state() == St::Removed) { blocked = s; break; }
        return;   // still saving: wait for its next status change
    }
    if (blocked) {   // never report a successful shutdown with unsaved work: keep the editor and error visible
        m_quitting = false;
        if (auto *w = stickyOf(blocked->rel())) w->present(true);
        else if (m_organizer && m_organizer->currentSession() == blocked) m_organizer->present(true);
        else createSticky(blocked, true, nullptr);
        emit quitAborted(blocked->stateLabel() + ": " + blocked->rel());
        return;
    }
    m_quitting = false;
    m_exiting = true;
    for (auto &s : m_sessions) m_noteState.setView(s->rel(), s->cursorPosition(), s->scrollValue());
    flushSessionFile();
    emit exitRequested();
}

// ---------------------------------------------------------------- shortcuts
void AppController::bindShortcuts(QWidget *host, std::function<NoteSession *()> current) {
    m_shortcutHosts.append({host, std::move(current)});
    rebindShortcuts();
}

void AppController::rebindShortcuts() {
    for (int i = m_shortcutHosts.size() - 1; i >= 0; --i)
        if (!m_shortcutHosts[i].w) m_shortcutHosts.removeAt(i);
    for (const auto &h : std::as_const(m_shortcutHosts)) {
        for (auto *old : h.w->findChildren<QShortcut *>(QStringLiteral("hn-sc"), Qt::FindDirectChildrenOnly)) delete old;
        QMap<QString, QKeySequence> kb = effectiveBindings();
        if (m_plugins) { const auto pk = m_plugins->pluginKeys(); for (auto it = pk.begin(); it != pk.end(); ++it) kb.insert(it.key(), it.value()); }
        for (auto it = kb.begin(); it != kb.end(); ++it) {
            if (it.value().isEmpty()) continue;
            auto *sc = new QShortcut(it.value(), h.w, nullptr, nullptr, Qt::WindowShortcut);
            sc->setObjectName(QStringLiteral("hn-sc"));
            connect(sc, &QShortcut::activated, this, [this, id = it.key(), w = h.w.data(), cur = h.cur] { runAction(id, cur ? cur() : nullptr, w); });
        }
    }
}

void AppController::runAction(const QString &id, NoteSession *s, QWidget *host) {
    using namespace hn::editor;
    if (id == "new-note") {
        const bool inOrg = host == m_organizer.data();
        newNote(inOrg && s ? s->folder() : QString(), inOrg);
        return;
    }
    if (id == "toggle-organizer") { toggleOrganizer(); return; }
    if (id == "search") { showOrganizer(true); m_organizer->focusSearch(); return; }
    if (id == "command-palette") { showPalette(host, s); return; }
    if (id.startsWith(QLatin1String("plugin:"))) { m_plugins->runQualified(id.mid(7), s); return; }
    if (!s) return;
    NoteEditor *ed = s->editor();
    if (id == "toggle-source") ed->setMode(ed->mode() == Mode::Visual ? Mode::Source : Mode::Visual);
    else if (id == "bold") ed->toggleInline(InlineStyle::Bold);
    else if (id == "italic") ed->toggleInline(InlineStyle::Italic);
    else if (id == "strike") ed->toggleInline(InlineStyle::Strike);
    else if (id == "code") ed->toggleInline(InlineStyle::Code);
    else if (id == "link") ed->editLink();
    else if (id == "h1") ed->setBlockStyle(ed->blockStyle() == BlockStyle::H1 ? BlockStyle::Paragraph : BlockStyle::H1);
    else if (id == "quote") ed->setBlockStyle(ed->blockStyle() == BlockStyle::Quote ? BlockStyle::Paragraph : BlockStyle::Quote);
    else if (id == "list-ul") ed->toggleList(ListKind::Bullet);
    else if (id == "list-ol") ed->toggleList(ListKind::Ordered);
    else if (id == "check") ed->toggleList(ListKind::Check);
    else return;
    ed->focusEditor();
}

// ---------------------------------------------------------------- plugins: UI glue
void AppController::announce(const QString &message) {
    m_lastNotice = message;
    StatusStrip *st = nullptr;
    QWidget *aw = QApplication::activeWindow();
    if (auto *sw = qobject_cast<StickyWindow *>(aw)) st = sw->status();
    else if (auto *ow = qobject_cast<OrganizerWindow *>(aw)) st = ow->status();
    if (!st && m_organizer) st = m_organizer->status();
    if (!st) for (auto *w : stickies()) { st = w->status(); break; }
    if (st) st->flash(message);
    emit notice(message);
}

namespace {
bool themeTokenField(hn::theme::Theme &t, const QString &token, QColor **out) {
    static const QStringList names{"bg", "surface", "text", "muted", "accent", "accentText", "border", "danger", "success", "selection"};
    QColor *f[] = {&t.bg, &t.surface, &t.text, &t.muted, &t.accent, &t.accentText, &t.border, &t.danger, &t.success, &t.selection};
    const int i = names.indexOf(token);
    if (i < 0) return false;
    *out = f[i];
    return true;
}
qreal luminance(const QColor &c) {
    auto lin = [](qreal v) { return v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
    return 0.2126 * lin(c.redF()) + 0.7152 * lin(c.greenF()) + 0.0722 * lin(c.blueF());
}
qreal contrast(const QColor &a, const QColor &b) {
    const qreal x = luminance(a) + 0.05, y = luminance(b) + 0.05;
    return x > y ? x / y : y / x;
}
}  // namespace

void AppController::applyThemeToken(hn::theme::Theme &t, const QString &token, const QColor &c) {
    QColor *f = nullptr;
    if (c.isValid() && themeTokenField(t, token, &f)) *f = c;
}

bool AppController::setThemeOverride(const QString &pluginId, const QString &token, const QString &value) {
    static const QRegularExpression hex(QStringLiteral("^#[0-9A-Fa-f]{6}$"));
    hn::theme::Theme probe = m_theme;
    QColor *f = nullptr;
    if (!themeTokenField(probe, token, &f) || !hex.match(value).hasMatch()) return false;
    *f = QColor(value);
    if (contrast(probe.text, probe.bg) < 3.0 || contrast(probe.muted, probe.bg) < 2.0) return false;   // keep the UI readable
    m_themeOverrides[pluginId][token] = QColor(value).name();
    applyThemeNow();
    return true;
}

QKeySequence AppController::paletteKey() const {
    return m_settings.keybindings.contains("command-palette") ? m_settings.keybindings.value("command-palette") : ui::defaultKey("command-palette");
}

QMap<QString, QKeySequence> AppController::effectiveBindings() const {
    QMap<QString, QKeySequence> kb = m_settings.keybindings;
    if (!kb.contains("command-palette")) kb.insert("command-palette", ui::defaultKey("command-palette"));
    return kb;
}

void AppController::showShortcuts(QWidget *window) {
    QList<QPair<QString, QKeySequence>> extra;
    const auto keys = m_plugins->pluginKeys();
    for (const auto &a : m_plugins->commands()) {
        const QKeySequence ks = keys.value(QStringLiteral("plugin:") + a.qid);
        if (!ks.isEmpty()) extra.append({QStringLiteral("%1: %2").arg(a.pluginName, a.title), ks});
    }
    ui::ShortcutSheet::toggle(window, effectiveBindings(), extra);
}

void AppController::showPalette(QWidget *host, NoteSession *s) {
    if (!host) host = QApplication::activeWindow();
    if (!host) return;
    QList<CommandPalette::Item> items;
    const auto keys = m_plugins->pluginKeys();
    const QList<PluginAction> all = m_plugins->allActions();
    for (const auto &a : all) {
        const QKeySequence ks = keys.value(QStringLiteral("plugin:") + a.qid);
        items.append({QString::number(items.size()), a.title, a.pluginName, ks.isEmpty() ? QString() : ks.toString(QKeySequence::NativeText)});
    }
    QPointer<NoteSession> sp(s);
    CommandPalette::toggle(host, items, [this, all, sp](const QString &id) {
        const int i = id.toInt();
        if (i >= 0 && i < all.size()) m_plugins->run(all[i], sp.data());
    });
}

void AppController::populateToolsMenu(QMenu *menu, NoteSession *s) {
    const auto items = m_plugins->menuItems(QStringLiteral("tools"));
    if (items.isEmpty()) return;
    menu->addSeparator();
    auto *tm = menu->addMenu(tr("Plugin tools"));
    tm->setObjectName(QStringLiteral("hnToolsMenu"));
    QPointer<NoteSession> sp(s);
    for (const auto &a : items) connect(tm->addAction(QStringLiteral("%1: %2").arg(a.pluginName, a.title)), &QAction::triggered, this, [this, a, sp] { m_plugins->run(a, sp.data()); });
}

} // namespace hn::app
