#include "plugin_service.h"
#include "controller.h"
#include "hn/mods/mods.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QSaveFile>

using namespace hn::plugins;

namespace hn::app {

namespace {
bool copyTree(const QString &from, const QString &to, int depth = 0) {
    if (depth > 8 || !QDir().mkpath(to)) return false;
    for (const auto &fi : QDir(from).entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDir::Name)) {
        if (fi.fileName().startsWith(QLatin1Char('.'))) continue;
        const QString dst = to + QLatin1Char('/') + fi.fileName();
        if (fi.isDir()) { if (!copyTree(fi.absoluteFilePath(), dst, depth + 1)) return false; }
        else if (!QFile::copy(fi.absoluteFilePath(), dst)) return false;
    }
    return true;
}
}  // namespace

PluginService::PluginService(AppController *c, QString dir, PluginHooks hooks) : QObject(c), m_c(c), m_dir(std::move(dir)), m_hooks(std::move(hooks)) {}

PluginService::~PluginService() {
    for (auto *b : std::as_const(m_bridges)) { b->detach(); delete b; }
}

// ---------------------------------------------------------------- lifecycle
bool PluginService::somethingEnabledOnDisk() const {
    QFile f(m_c->stateDir() + QStringLiteral("/plugins.json"));
    if (!f.exists()) return false;
    if (!f.open(QIODevice::ReadOnly)) return true;   // unreadable: let the manager decide (it disables everything on a corrupt file)
    const auto doc = QJsonDocument::fromJson(f.read(16 * 1024 * 1024));
    if (!doc.isObject()) return true;
    const auto ps = doc.object().value(QLatin1String("plugins")).toObject();
    for (auto it = ps.begin(); it != ps.end(); ++it) if (it->toObject().value(QLatin1String("enabled")).toBool()) return true;
    return false;
}

QString PluginService::legacyEnabledPath() const { return m_c->modsEnabledPath(); }

int PluginService::legacyModsAwaitingApproval() const {
    if (m_mgr) {
        int n = 0;
        for (const auto &id : m_legacyPending) if (m_mgr->info(id).status != Status::Enabled) ++n;
        return n;
    }
    // Not created yet: the legacy list names mods that no longer run until approved on the Plugins page.
    const QStringList enabled = hn::mods::loadEnabled(legacyEnabledPath());
    QList<hn::mods::ModError> ignore;
    int n = 0;
    TrustStore ts(m_c->stateDir() + QStringLiteral("/plugins.json"));
    ts.load();
    for (const auto &m : hn::mods::scanManifests(m_c->legacyModsDir(), &ignore))
        if (enabled.contains(m.id) && !ts.record(m.id).enabled) ++n;
    return n;
}

void PluginService::migrateLegacyMods() {
    // Existing mods become native plugins (a plugin.json is generated next to the copied mod.json + library). The legacy
    // mods-enabled.json is never modified; mods named in it simply wait for approval like any other native plugin.
    const QString marker = m_c->stateDir() + QStringLiteral("/plugins-migrated.json");
    QStringList done;
    if (QFile f(marker); f.open(QIODevice::ReadOnly)) for (const auto &v : QJsonDocument::fromJson(f.readAll()).array()) done << v.toString();
    QList<hn::mods::ModError> ignore;
    const QStringList enabled = hn::mods::loadEnabled(legacyEnabledPath());
    bool changed = false;
    for (const auto &m : hn::mods::scanManifests(m_c->legacyModsDir(), &ignore)) {
        if (enabled.contains(m.id) && !m_legacyPending.contains(m.id)) m_legacyPending << m.id;
        if (done.contains(m.id)) continue;
        const QString dst = m_dir + QLatin1Char('/') + m.id;
        if (QFileInfo::exists(dst)) continue;
        done << m.id;
        changed = true;
        if (!copyTree(m.dir, dst)) { QDir(dst).removeRecursively(); continue; }
        const QJsonObject pj{{"id", m.id}, {"name", m.name}, {"version", m.version}, {"author", "Unknown (migrated mod)"},
                             {"description", "Native mod migrated from your mods folder. It runs native code and is not sandboxed."},
                             {"api", kApiVersion}, {"tier", "native"}, {"entry", m.library}, {"permissions", QJsonArray{"native"}}, {"min_app", "0.1.0"}};
        QFile out(dst + QStringLiteral("/plugin.json"));
        if (out.open(QIODevice::WriteOnly)) out.write(QJsonDocument(pj).toJson());
    }
    if (changed) {
        QDir().mkpath(m_c->stateDir());
        QSaveFile f(marker);
        if (f.open(QIODevice::WriteOnly)) { f.write(QJsonDocument(QJsonArray::fromStringList(done)).toJson()); f.commit(); }
    }
}

void PluginService::create() {
    m_native = std::make_unique<ModNativeAdapter>(m_c->stateDir());
    m_net = std::make_unique<PluginNetBridge>();           // no QNetworkAccessManager until the first request
    m_lib = std::make_unique<PluginLibraryBridge>(m_c);
    m_ui = std::make_unique<PluginUiBridge>(this);
    m_clip = std::make_unique<PluginClipboardBridge>();
    m_theme = std::make_unique<PluginThemeBridge>(m_c);
    ManagerConfig cfg;
    cfg.pluginsDir = m_dir;
    cfg.stateDir = m_c->stateDir();
    cfg.bridges = {m_lib.get(), m_ui.get(), m_net.get(), m_clip.get(), m_theme.get()};
    cfg.logger = [](int, const QString &id, const QString &msg) { qWarning("[plugin %s] %s", qPrintable(id), qPrintable(msg)); };
    cfg.native = m_native.get();
    m_mgr = std::make_unique<PluginManager>(cfg);
    migrateLegacyMods();
    m_mgr->scan();
    auto rebuild = [this] {
        m_names.clear();
        for (const auto &i : m_mgr->plugins()) m_names.insert(i.manifest.id, i.manifest.name.isEmpty() ? i.manifest.id : i.manifest.name);
    };
    rebuild();
    connect(m_mgr.get(), &PluginManager::pluginsChanged, this, [this, rebuild] { rebuild(); refreshSessions(); emit changed(); });
    connect(m_mgr.get(), &PluginManager::pluginAutoDisabled, this, [this](const QString &id, const QString &why) {
        m_c->announce(tr("Plugin %1 was disabled: %2").arg(nameOf(id), why));
    });
    for (const QString &rel : m_c->openNotes()) if (auto *s = m_c->session(rel)) attach(s);
    m_started = true;
    emit changed();
}

PluginManager *PluginService::manager() {
    if (!m_mgr) create();
    return m_mgr.get();
}

void PluginService::startIfNeeded() {
    if (m_mgr || !somethingEnabledOnDisk()) return;
    create();
    m_mgr->post(QStringLiteral("app.started"), QString(), nullptr);
}

QString PluginService::nameOf(const QString &id) const { return m_names.value(id, id); }

// ---------------------------------------------------------------- sessions
PluginNoteBridge *PluginService::bridgeFor(NoteSession *s) {
    if (!s) return nullptr;
    auto *&b = m_bridges[s];
    if (!b) b = new PluginNoteBridge(s);
    return b;
}

void PluginService::post(const QString &event, NoteSession *s) {
    if (!m_mgr || !s) return;
    m_mgr->post(event, s->rel(), bridgeFor(s));
}

void PluginService::wireSession(NoteSession *s) {
    if (m_conns.contains(s)) return;
    QList<QMetaObject::Connection> cs;
    cs << connect(s, &NoteSession::contentEdited, this, [this, s] { post(QStringLiteral("note.changed"), s); });
    cs << connect(s, &NoteSession::savedOk, this, [this, s] { post(QStringLiteral("note.saved"), s); });
    cs << connect(s->editor(), &hn::editor::NoteEditor::cursorInfoChanged, this, [this, s] { post(QStringLiteral("selection.changed"), s); });
    cs << connect(s->toolbar(), &hn::editor::FormattingToolbar::pluginButtonTriggered, this, [this, s](const QString &qid) { runQualified(qid, s); });
    m_conns.insert(s, cs);
    s->setPreSaveHook([this, s](const QByteArray &bytes) -> QByteArray {
        if (!m_mgr || m_mgr->registry()->subscribers(QStringLiteral("note.pre_save")).isEmpty()) return bytes;
        const QString in = QString::fromUtf8(bytes);
        const QString out = m_mgr->preSave(in, bridgeFor(s));   // strict budget; any failure returns the text unchanged
        if (out == in || (out.isEmpty() && !in.isEmpty()) || out.size() > in.size() * 4 + 65536) return bytes;   // never lose or balloon a note
        return out.toUtf8();
    });
}

void PluginService::attach(NoteSession *s) {
    if (!m_mgr || !s) return;
    wireSession(s);
    refreshSessions();
    post(QStringLiteral("note.opened"), s);
}

void PluginService::release(NoteSession *s) {
    if (!m_mgr) return;
    post(QStringLiteral("note.closed"), s);
    if (auto *b = m_bridges.take(s)) {
        m_mgr->detachNote(b);   // flushes coalesced events that still reference it
        b->detach();
        delete b;
    }
    for (const auto &c : m_conns.take(s)) disconnect(c);
    s->setPreSaveHook({});
}

void PluginService::refreshSessions() {
    if (!m_mgr) return;
    QList<hn::editor::FormattingToolbar::PluginButton> buttons;
    for (const auto &a : toolbarButtons()) buttons.append({a.qid, QStringLiteral("%1: %2").arg(a.pluginName, a.title), a.icon});
    const bool triggers = !m_mgr->registry()->triggers().isEmpty();
    for (const QString &rel : m_c->openNotes()) {
        NoteSession *s = m_c->session(rel);
        if (!s) continue;
        s->toolbar()->setPluginButtons(buttons);
        if (!triggers) { s->editor()->setTriggerHandler({}); continue; }
        s->editor()->setTriggerHandler([this, s](const QString &token) -> std::optional<hn::editor::TriggerResult> {
            if (!m_mgr) return std::nullopt;
            bool known = false;
            for (const auto &t : m_mgr->registry()->triggers()) if (t.pattern == token) { known = true; break; }
            if (!known) return std::nullopt;   // no Lua unless a trigger covers exactly this token
            const auto m = m_mgr->matchTrigger(token, bridgeFor(s));
            if (!m || m->pattern != token) return std::nullopt;
            return hn::editor::TriggerResult{int(token.size()), m->replacement};
        });
    }
}

// ---------------------------------------------------------------- command model
QList<PluginAction> PluginService::commands() const {
    QList<PluginAction> out;
    if (!m_mgr) return out;
    for (const auto &c : m_mgr->registry()->commands()) out.append({PluginAction::Command, c.qualifiedId(), c.title, c.pluginId, nameOf(c.pluginId), c.key, {}, {}});
    if (m_native)
        for (const auto &c : m_native->commands()) out.append({PluginAction::Command, c.qualifiedId(), c.title.isEmpty() ? c.id : c.title, c.pluginId, nameOf(c.pluginId), {}, {}, {}});
    return out;
}

QList<PluginAction> PluginService::toolbarButtons() const {
    QList<PluginAction> out;
    if (!m_mgr) return out;
    for (const auto &t : m_mgr->registry()->toolbarButtons()) out.append({PluginAction::Toolbar, t.pluginId + QLatin1Char(':') + t.id, t.title, t.pluginId, nameOf(t.pluginId), {}, t.icon, {}});
    return out;
}

QList<PluginAction> PluginService::menuItems(const QString &where) const {
    QList<PluginAction> out;
    if (!m_mgr) return out;
    for (const auto &m : m_mgr->registry()->menuItems(where)) out.append({PluginAction::Menu, m.pluginId + QLatin1Char(':') + m.id, m.title, m.pluginId, nameOf(m.pluginId), {}, {}, m.where});
    return out;
}

QList<PluginAction> PluginService::allActions() const { return commands() + menuItems({}) + toolbarButtons(); }

QMap<QString, QKeySequence> PluginService::pluginKeys() const {
    QMap<QString, QKeySequence> out;
    if (!m_mgr) return out;
    QSet<QKeySequence> used;
    const auto &kb = m_c->settings().keybindings;
    for (auto it = kb.begin(); it != kb.end(); ++it) if (!it.value().isEmpty() && !it.key().startsWith(QLatin1String("plugin:"))) used.insert(it.value());
    used.insert(QKeySequence(Qt::Key_F1));
    used.insert(QKeySequence(Qt::Key_F2));
    used.insert(QKeySequence(Qt::Key_Escape));
    used.insert(m_c->paletteKey());
    for (const auto &c : m_mgr->registry()->commands()) {
        const QString id = QStringLiteral("plugin:") + c.qualifiedId();
        QKeySequence ks;
        if (kb.contains(id)) ks = kb.value(id);   // the user's choice (empty = unbound) wins
        else {
            ks = QKeySequence::fromString(c.key, QKeySequence::PortableText);
            const auto mods = ks.count() == 1 ? ks[0].keyboardModifiers() : Qt::KeyboardModifiers();
            if (ks.count() != 1 || !(mods & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))) ks = QKeySequence();   // a plugin cannot take plain typing keys
        }
        if (ks.isEmpty() || used.contains(ks)) continue;   // built-in and earlier bindings always win
        used.insert(ks);
        out.insert(id, ks);
    }
    return out;
}

bool PluginService::run(const PluginAction &a, NoteSession *s, QString *err) {
    QString e;
    bool ok = false;
    if (!m_mgr) e = tr("plugins are not active");
    else {
        PluginNoteBridge *b = s ? bridgeFor(s) : nullptr;
        ok = a.kind == PluginAction::Command ? m_mgr->runCommand(a.qid, b, &e)
           : a.kind == PluginAction::Toolbar ? m_mgr->runToolbarButton(a.qid, b, &e) : m_mgr->runMenuItem(a.qid, b, &e);
    }
    if (!ok) m_c->announce(tr("%1: %2").arg(a.pluginName.isEmpty() ? a.pluginId : a.pluginName, e.isEmpty() ? tr("failed") : e));
    if (err) *err = e;
    return ok;
}

bool PluginService::runQualified(const QString &qid, NoteSession *s, QString *err) {
    for (const auto &a : allActions())
        if (a.qid == qid) return run(a, s, err);
    if (err) *err = tr("unknown plugin action '%1'").arg(qid);
    return false;
}

// ---------------------------------------------------------------- flows
QString PluginService::sourceOf(const QString &id) const {
    QFile f(m_c->stateDir() + QStringLiteral("/plugin-sources.json"));
    if (f.open(QIODevice::ReadOnly)) {
        const QString s = QJsonDocument::fromJson(f.readAll()).object().value(id).toString();
        if (!s.isEmpty()) return s;
    }
    return tr("Installed folder %1").arg(m_dir + QLatin1Char('/') + id);
}

void PluginService::rememberSource(const QString &id, const QString &source) {
    const QString path = m_c->stateDir() + QStringLiteral("/plugin-sources.json");
    QJsonObject o;
    if (QFile f(path); f.open(QIODevice::ReadOnly)) o = QJsonDocument::fromJson(f.readAll()).object();
    o[id] = QFileInfo(source).absoluteFilePath();
    QDir().mkpath(m_c->stateDir());
    QSaveFile f(path);
    if (f.open(QIODevice::WriteOnly)) { f.write(QJsonDocument(o).toJson()); f.commit(); }
}

ConsentRequest PluginService::requestFor(const QString &id) const {
    ConsentRequest r;
    if (!m_mgr) return r;
    const PluginInfo i = m_mgr->info(id);
    const Manifest &m = i.manifest;
    r.id = m.id; r.name = m.name; r.author = m.author; r.version = m.version; r.description = m.description;
    r.permissions = m.permissions;
    r.native = m.tier == Tier::Native;
    QString he;
    r.sha256 = hashDirectory(m.dir, &he);
    if (r.sha256.isEmpty()) r.sha256 = tr("(could not hash: %1)").arg(he);
    r.source = sourceOf(id);
    if (i.needsReconsent) {
        for (const auto &p : m.permissions) if (!i.consentedPermissions.contains(p)) r.newPermissions << p;
        r.reconsentReason = r.newPermissions.isEmpty() ? tr("The plugin's files changed since you approved it. Review it again before it runs.")
                                                        : tr("The plugin's files changed since you approved it and it now asks for more permissions.");
    }
    return r;
}

bool PluginService::review(const QString &id, QWidget *parent) {
    auto *m = manager();
    if (m->info(id).manifest.id.isEmpty()) return false;
    const ConsentRequest req = requestFor(id);
    const bool ok = m_hooks.consent ? m_hooks.consent(req) : ConsentDialog::run(req, parent);
    if (!ok) return false;
    QString err;
    if (!m->consent(id, req.permissions, &err) || !m->enable(id, &err)) {
        m_c->announce(tr("Could not enable %1: %2").arg(req.name, err));
        return false;
    }
    return true;
}

bool PluginService::enable(const QString &id, QWidget *parent, QString *message) {
    auto *m = manager();
    const PluginInfo i = m->info(id);
    if (!i.consented || i.needsReconsent) {
        const bool ok = review(id, parent);
        if (message) *message = ok ? tr("Enabled.") : tr("Not enabled: approval is required.");
        return ok;
    }
    QString err;
    const bool ok = m->enable(id, &err);
    if (!ok && err.contains(QStringLiteral("approve"))) return review(id, parent);   // files changed meanwhile: ask again
    if (message) *message = ok ? tr("Enabled.") : err;
    return ok;
}

InstallResult PluginService::install(const QString &source, QWidget *parent, QString *message) {
    auto *m = manager();
    InstallResult r = m->install(source, false);
    if (!r.ok && r.errors.size() == 1 && r.errors.first().message.contains(QStringLiteral("already installed"))) {
        const QString q = tr("A plugin with this id is already installed. Replace it with this version? The new files must be approved again unless they are identical.");
        const bool yes = m_hooks.confirm ? m_hooks.confirm(r.id, q)
                                         : QMessageBox::question(parent, tr("Replace plugin?"), q) == QMessageBox::Yes;
        if (yes) r = m->install(source, true);
    }
    if (!r.ok) {
        QStringList msgs;
        for (const auto &e : r.errors) msgs << e.message;
        if (message) *message = tr("Not installed: %1").arg(msgs.join(QStringLiteral("; ")));
        return r;
    }
    rememberSource(r.id, source);
    if (r.upgraded && r.consentKept) { if (message) *message = tr("Updated %1; the files are identical to the ones you approved.").arg(nameOf(r.id)); return r; }
    const bool ok = review(r.id, parent);
    if (message) *message = ok ? tr("Installed and enabled %1.").arg(nameOf(r.id)) : tr("Installed %1 but not enabled. Review and enable it from the Plugins page.").arg(nameOf(r.id));
    return r;
}

QString PluginService::auditText(int maxLines) const {
    QFile f(m_c->stateDir() + QStringLiteral("/plugins-audit.jsonl"));
    if (!f.open(QIODevice::ReadOnly)) return tr("The audit log is empty.");
    QStringList lines;
    while (!f.atEnd()) {
        const auto o = QJsonDocument::fromJson(f.readLine()).object();
        if (o.isEmpty()) continue;
        lines << QStringLiteral("%1  %2  %3: %4").arg(o.value("ts").toString().left(19).replace('T', ' '), o.value("plugin").toString().leftJustified(16), o.value("event").toString(), o.value("detail").toString());
    }
    if (lines.size() > maxLines) lines = lines.mid(lines.size() - maxLines);
    return lines.isEmpty() ? tr("The audit log is empty.") : lines.join('\n');
}

} // namespace hn::app
