#include "hn/plugins/runtime.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <algorithm>

namespace hn::plugins {

namespace {
const QStringList &coalesced() {
    static const QStringList c{"note.changed", "selection.changed"};
    return c;
}
}  // namespace

PluginManager::PluginManager(ManagerConfig cfg, QObject *parent)
    : QObject(parent), cfg_(std::move(cfg)), trust_(cfg_.stateDir + QStringLiteral("/plugins.json")),
      audit_(cfg_.stateDir + QStringLiteral("/plugins-audit.jsonl")), native_(cfg_.native ? cfg_.native : &stub_) {
    trust_.load();
    HostEnv env;
    env.stateDir = cfg_.stateDir;
    env.trust = &trust_;
    env.audit = &audit_;
    env.registry = &registry_;
    env.bridges = cfg_.bridges;
    env.logger = cfg_.logger;
    env.appVersion = cfg_.appVersion;
    env.clock = cfg_.clock;
    host_ = std::make_unique<LuaPluginHost>(std::move(env));
    QFile cf(cfg_.stateDir + QStringLiteral("/plugins-registry.json"));
    if (cf.open(QIODevice::ReadOnly)) cache_ = QJsonDocument::fromJson(cf.read(8 * 1024 * 1024)).object();
    timer_.setSingleShot(true);
    timer_.setInterval(cfg_.coalesceMs);
    connect(&timer_, &QTimer::timeout, this, &PluginManager::flushEvents);
    panelTimer_.setSingleShot(true);
    panelTimer_.setInterval(cfg_.coalesceMs);
    connect(&panelTimer_, &QTimer::timeout, this, &PluginManager::flushPanels);
    connect(host_.get(), &LuaPluginHost::panelRefreshRequested, this, [this](const QString &pid, const QString &panelId, NoteBridge *note) {
        const QString q = pid + QLatin1Char(':') + panelId;
        if (!activePanels_.contains(q)) return;  // nobody is looking: nothing to do
        queuePanelRefresh(q, note, false);
        if (!pendingPanels_.isEmpty() && !panelTimer_.isActive()) panelTimer_.start();
    });
    connect(&registry_, &PluginRegistry::changed, this, &PluginManager::publishRegistry);
    connect(host_.get(), &LuaPluginHost::callbackFinished, this, [this] {
        for (auto it = noteLives_.begin(); it != noteLives_.end();) {
            if (!*it.value() && !host_->usesNote(it.key())) it = noteLives_.erase(it);
            else ++it;
        }
        schedulePanelDrain();
    });
    connect(host_.get(), &LuaPluginHost::autoDisabled, this, [this](const QString &id, const QString &why) {
        if (auto it = entries_.find(id); it != entries_.end()) it->lastError = why;
        invalidatePanels(id);
        registry_.remove(id);
        emit pluginAutoDisabled(id, why);
        emit pluginsChanged();
    });
}
PluginManager::~PluginManager() {
    for (auto it = entries_.begin(); it != entries_.end(); ++it)
        if (it->m.tier == Tier::Native && trust_.record(it.key()).enabled) native_->deactivate(it.key());
}

void PluginManager::publishRegistry() {
    if (discoveryDepth_) { registryPending_ = true; return; }
    registryPending_ = false;
    saveCache();
    syncPanels();
    emit pluginsChanged();
}

void PluginManager::invalidatePanels(const QString &id) {
    generations_[id] = ++nextGeneration_;
    for (auto it = pendingPanels_.begin(); it != pendingPanels_.end();)
        if (it.key().startsWith(id + QLatin1Char(':'))) it = pendingPanels_.erase(it); else ++it;
    deferredEvents_.removeIf([&](const DeferredEvent &event) { return event.pluginId == id; });
}

void PluginManager::saveCache() {
    if (cacheSuspended_) return;
    QJsonObject c;
    for (auto it = entries_.begin(); it != entries_.end(); ++it)
        if (const auto *r = registry_.of(it.key())) c[it.key()] = QJsonObject{{"hash", it->hash}, {"regs", r->toJson()}};
    if (c == cache_) return;
    cache_ = c;
    QDir().mkpath(cfg_.stateDir);
    QSaveFile f(cfg_.stateDir + QStringLiteral("/plugins-registry.json"));
    if (f.open(QIODevice::WriteOnly)) { f.write(QJsonDocument(c).toJson(QJsonDocument::Compact)); f.commit(); }
}

// Announces panel (un)registrations to the PanelBridge. Driven by registry changes, so cached registrations show up without Lua.
void PluginManager::syncPanels() {
    QMap<QString, QString> now;
    QMap<QString, PanelReg> regs;
    for (const auto &p : registry_.panels()) { now[p.qualifiedId()] = p.title + QLatin1Char('\x1f') + p.icon; regs[p.qualifiedId()] = p; }
    auto *pb = cfg_.bridges.panel;
    for (auto it = shownPanels_.begin(); it != shownPanels_.end();) {
        if (!now.contains(it.key())) {
            if (pb) pb->removePanel(it.key());
            pendingPanels_.remove(it.key());
            activePanels_.remove(it.key());
            it = shownPanels_.erase(it);
        } else ++it;
    }
    for (auto it = now.begin(); it != now.end(); ++it) {
        if (shownPanels_.value(it.key()) == it.value() && shownPanels_.contains(it.key())) continue;
        shownPanels_[it.key()] = it.value();
        if (pb) pb->showPanel(it.key(), regs[it.key()].title, regs[it.key()].icon);
    }
}

void PluginManager::activateEntry(const QString &id, Entry &e) {
    invalidatePanels(id);
    const auto rec = trust_.record(id);
    QString err;
    bool ok = true;
    if (e.m.tier == Tier::Native) {
        ok = native_->activate(e.m, &err);
    } else {
        const auto c = cache_.value(id).toObject();
        if (!c.isEmpty() && c.value(QLatin1String("hash")).toString() == e.hash) {
            const auto regs = PluginRegs::fromJson(id, c.value(QLatin1String("regs")).toObject());
            host_->setPlugin(e.m, rec.permissions, &regs);
            registry_.set(id, regs);
        } else {
            ++discoveryDepth_;
            host_->setPlugin(e.m, rec.permissions, nullptr);
            ok = host_->ensureLoaded(id, &err);  // no cache for this package yet: run main chunk once to learn registrations
            if (ok) host_->unload(id);           // ...then drop the state again; the first real hook recreates it
            --discoveryDepth_;
        }
    }
    if (!ok) {
        e.lastError = err;
        trust_.setEnabled(id, false);
        host_->forget(id);
        registry_.remove(id);
        audit_.log(QStringLiteral("failure"), id, QStringLiteral("activation failed: %1").arg(err));
    } else {
        e.lastError.clear();
    }
    // A visible dock may render synchronously from these notifications. Discovery
    // tokens must have been discarded before any registration reaches the UI.
    if (!discoveryDepth_ && registryPending_) publishRegistry();
}

void PluginManager::loadEntry(const QString &dir, const QString &name) {
    Entry e;
    e.m.id = name;
    e.m.name = name;
    e.m.dir = dir;
    auto bad = [&](const QStringList &msgs) {
        e.invalid = true;
        e.lastError = msgs.join(QStringLiteral("; "));
        for (const auto &m : msgs) errors_.append({name, m});
        entries_[name] = e;
    };
    if (QFileInfo(dir).isSymLink()) return bad({QStringLiteral("plugin folder is a symbolic link")});
    QFile pj(dir + QStringLiteral("/plugin.json"));
    if (!pj.open(QIODevice::ReadOnly)) return bad({QStringLiteral("plugin.json is missing or unreadable")});
    QList<PluginError> errs;
    Manifest m;
    if (!parseManifest(pj.read(64 * 1024 + 1), dir, &m, &errs, cfg_.appVersion)) {
        QStringList msgs;
        for (const auto &x : errs) msgs << x.message;
        return bad(msgs);
    }
    if (m.id != name) return bad({QStringLiteral("manifest id '%1' does not match its folder name '%2'").arg(m.id, name)});
    e.m = m;
    QString he;
    e.hash = hashDirectory(dir, &he);
    if (e.hash.isEmpty()) return bad({QStringLiteral("cannot hash package: %1").arg(he)});
    if (trust_.verifyHash(name, e.hash)) {
        audit_.log(QStringLiteral("tamper"), name, QStringLiteral("package files changed since consent; disabled"));
        e.lastError = QStringLiteral("files changed since you approved this plugin");
    }
    entries_[name] = e;
    if (trust_.record(name).enabled) activateEntry(name, entries_[name]);
}

void PluginManager::scan() {
    for (auto it = generations_.cbegin(); it != generations_.cend(); ++it)
        if (host_->isBusy(it.key())) {
            audit_.log(QStringLiteral("busy"), it.key(), QStringLiteral("scan refused during callback dispatch"));
            return;
        }
    cacheSuspended_ = true;
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        invalidatePanels(it.key());
        host_->forget(it.key());
        if (it->m.tier == Tier::Native) native_->deactivate(it.key());
    }
    registry_.clear();
    entries_.clear();
    errors_.clear();
    const QDir root(cfg_.pluginsDir);
    for (const auto &fi : root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDir::Name))
        if (!fi.fileName().startsWith(QLatin1Char('.'))) loadEntry(fi.absoluteFilePath(), fi.fileName());
    cacheSuspended_ = false;
    saveCache();
    emit pluginsChanged();
}

PluginInfo PluginManager::info(const QString &id) const {
    PluginInfo i;
    const auto it = entries_.constFind(id);
    if (it == entries_.constEnd()) return i;
    const auto rec = trust_.record(id);
    i.manifest = it->m;
    i.hash = it->hash;
    i.consented = rec.hasConsent;
    i.needsReconsent = rec.needsReconsent;
    i.consentedPermissions = rec.permissions;
    i.loaded = host_->isLoaded(id);
    if (it->invalid) { i.status = Status::Failed; i.detail = it->lastError; }
    else if (rec.enabled) i.status = Status::Enabled;
    else if (!rec.hasConsent || rec.needsReconsent) {
        i.status = Status::NeedsConsent;
        i.detail = rec.needsReconsent ? QStringLiteral("files changed since you approved this plugin; review and approve again") : QString();
    } else if (!it->lastError.isEmpty()) { i.status = Status::Failed; i.detail = it->lastError; }
    else i.status = Status::Disabled;
    return i;
}
QList<PluginInfo> PluginManager::plugins() const {
    QList<PluginInfo> r;
    for (auto it = entries_.constBegin(); it != entries_.constEnd(); ++it) r << info(it.key());
    return r;
}

InstallResult PluginManager::install(const QString &source, bool allowUpgrade) {
    if (allowUpgrade) {
        for (auto it = generations_.cbegin(); it != generations_.cend(); ++it)
            if (host_->isBusy(it.key())) {
                InstallResult result;
                result.errors.append({it.key(), QStringLiteral("plugin is busy (upgrade refused during callback dispatch)")});
                return result;
            }
    }
    PackageInstaller inst(cfg_.pluginsDir, &trust_, &audit_, cfg_.appVersion);
    // An upgrade replaces files under a running plugin: stop it first (it is re-activated below if consent is kept).
    if (allowUpgrade) {
        const auto pre = checkDirectory(QFileInfo(source).isDir() ? source : QString(), cfg_.appVersion);
        const QString pid = pre.manifest.id;
        if (!pid.isEmpty() && entries_.contains(pid)) { host_->forget(pid); registry_.remove(pid); }
    }
    InstallResult r = inst.install(source, allowUpgrade);
    if (r.ok) {
        if (r.upgraded) {
            host_->forget(r.id);
            registry_.remove(r.id);
        }
        entries_.remove(r.id);
        loadEntry(cfg_.pluginsDir + QLatin1Char('/') + r.id, r.id);
        emit pluginsChanged();
    } else if (allowUpgrade) {
        // failed upgrade: restore the old one
        for (auto it = entries_.begin(); it != entries_.end(); ++it)
            if (trust_.record(it.key()).enabled && !host_->isLoaded(it.key()) && it->m.tier == Tier::Script && !registry_.of(it.key())) activateEntry(it.key(), *it);
    }
    return r;
}

bool PluginManager::consent(const QString &id, const QStringList &perms, QString *err) {
    auto fail = [&](const QString &m) { if (err) *err = m; return false; };
    auto it = entries_.find(id);
    if (it == entries_.end() || it->invalid) return fail(QStringLiteral("plugin is not installed or has an invalid manifest"));
    for (const auto &p : perms) if (!it->m.permissions.contains(p)) return fail(QStringLiteral("permission '%1' was not requested by the plugin").arg(p));
    if (it->m.tier == Tier::Native && !perms.contains(QStringLiteral("native"))) return fail(QStringLiteral("native plugins need the 'native' permission accepted"));
    QString he;
    const QString h = hashDirectory(it->m.dir, &he);
    if (h.isEmpty()) return fail(he);
    it->hash = h;
    if (!trust_.recordConsent(id, perms, h)) return fail(QStringLiteral("could not save consent"));
    audit_.log(QStringLiteral("consent"), id, QStringLiteral("permissions [%1] sha256 %2").arg(perms.join(QStringLiteral(", ")), h));
    it->lastError.clear();
    emit pluginsChanged();
    return true;
}

bool PluginManager::enable(const QString &id, QString *err) {
    auto fail = [&](const QString &m) { if (err) *err = m; return false; };
    auto it = entries_.find(id);
    if (it == entries_.end() || it->invalid) return fail(QStringLiteral("plugin is not installed or has an invalid manifest"));
    if (host_->isBusy(id)) return fail(QStringLiteral("plugin is busy (activation refused during callback)"));
    QString he;
    const QString h = hashDirectory(it->m.dir, &he);
    if (trust_.verifyHash(id, h)) {
        audit_.log(QStringLiteral("tamper"), id, QStringLiteral("package files changed since consent; disabled"));
        emit pluginsChanged();
        return fail(QStringLiteral("plugin files changed since you approved them; approve again"));
    }
    QString e2;
    if (!trust_.setEnabled(id, true, &e2)) return fail(e2);
    it->hash = h;
    it->lastError.clear();
    activateEntry(id, *it);
    if (!it->lastError.isEmpty()) { const QString m = it->lastError; emit pluginsChanged(); return fail(m); }
    audit_.log(QStringLiteral("enable"), id);
    emit pluginsChanged();
    return true;
}

bool PluginManager::disable(const QString &id) {
    auto it = entries_.find(id);
    if (it == entries_.end()) return false;
    trust_.setEnabled(id, false);
    invalidatePanels(id);
    host_->forget(id);
    registry_.remove(id);
    if (it->m.tier == Tier::Native) native_->deactivate(id);
    audit_.log(QStringLiteral("disable"), id);
    emit pluginsChanged();
    return true;
}

bool PluginManager::remove(const QString &id, QString *err) {
    disable(id);
    host_->forget(id);
    host_->deleteStorage(id);
    PackageInstaller inst(cfg_.pluginsDir, &trust_, &audit_, cfg_.appVersion);
    if (!inst.uninstall(id, err)) return false;
    entries_.remove(id);
    saveCache();
    emit pluginsChanged();
    return true;
}

bool PluginManager::reload(const QString &id, bool trustChanges, QString *err) {
    auto fail = [&](const QString &m) { if (err) *err = m; return false; };
    auto it = entries_.find(id);
    if (it == entries_.end()) return fail(QStringLiteral("unknown plugin"));
    if (host_->isBusy(id)) return fail(QStringLiteral("plugin is busy (reload refused during callback)"));
    const QString dir = it->m.dir;
    const bool wasEnabled = trust_.record(id).enabled;
    invalidatePanels(id);
    host_->forget(id);
    registry_.remove(id);
    if (it->m.tier == Tier::Native) native_->deactivate(id);
    if (trustChanges) {
        const auto rec = trust_.record(id);
        QFile pj(dir + QStringLiteral("/plugin.json"));
        Manifest m;
        QList<PluginError> ignore;
        QString he;
        const QString h = hashDirectory(dir, &he);
        if (rec.hasConsent && !h.isEmpty() && h != rec.hash && pj.open(QIODevice::ReadOnly) &&
            parseManifest(pj.read(64 * 1024 + 1), dir, &m, &ignore, cfg_.appVersion) && m.id == id) {
            bool subset = true;
            for (const auto &p : m.permissions) subset &= rec.permissions.contains(p);
            if (subset && m.tier == it->m.tier) {
                trust_.recordConsent(id, rec.permissions, h);
                if (wasEnabled) trust_.setEnabled(id, true);
                audit_.log(QStringLiteral("reload-trust"), id, QStringLiteral("developer reload accepted new sha256 %1").arg(h));
            }
        }
    }
    cacheSuspended_ = true;
    entries_.remove(id);
    errors_.erase(std::remove_if(errors_.begin(), errors_.end(), [&](const PluginError &e) { return e.pluginId == id; }), errors_.end());
    loadEntry(dir, id);
    cacheSuspended_ = false;
    saveCache();
    emit pluginsChanged();
    const auto after = info(id);
    if (after.status == Status::Failed) return fail(after.detail);
    return true;
}

bool PluginManager::run(const QString &q, LuaPluginHost::Kind k, NoteBridge *note, QString *err) {
    auto fail = [&](const QString &m) { if (err) *err = m; return false; };
    const int c = int(q.indexOf(QLatin1Char(':')));
    if (c <= 0) return fail(QStringLiteral("command id must look like plugin:command"));
    const QString pid = q.left(c), local = q.mid(c + 1);
    const auto it = entries_.constFind(pid);
    if (it == entries_.constEnd() || !trust_.record(pid).enabled) return fail(QStringLiteral("plugin '%1' is not enabled").arg(pid));
    if (it->m.tier == Tier::Native) return native_->runCommand(pid, local, note, err);
    const PluginRegs *r = registry_.of(pid);
    bool known = false;
    if (r) {
        if (k == LuaPluginHost::Kind::Command) for (const auto &x : r->commands) known |= x.id == local;
        else if (k == LuaPluginHost::Kind::Toolbar) for (const auto &x : r->toolbars) known |= x.id == local;
        else for (const auto &x : r->menus) known |= x.id == local;
    }
    if (!known) return fail(QStringLiteral("unknown command '%1'").arg(q));
    return host_->run(pid, k, local, note, err);
}
bool PluginManager::runCommand(const QString &q, NoteBridge *n, QString *e) { return run(q, LuaPluginHost::Kind::Command, n, e); }
bool PluginManager::runToolbarButton(const QString &q, NoteBridge *n, QString *e) { return run(q, LuaPluginHost::Kind::Toolbar, n, e); }
bool PluginManager::runMenuItem(const QString &q, NoteBridge *n, QString *e) { return run(q, LuaPluginHost::Kind::Menu, n, e); }

QList<PanelReg> PluginManager::activePanelsFor(const QString &event) const {
    QList<PanelReg> r;
    if (activePanels_.isEmpty()) return r;
    for (const auto &p : registry_.panelsRefreshingOn(event))
        if (activePanels_.contains(p.qualifiedId()) && trust_.record(p.pluginId).enabled) r << p;
    return r;
}

void PluginManager::deliverNow(const QString &event, const QString &arg, NoteBridge *note) {
    const auto eventNote = noteRef(note);
    if (!eventNote.valid()) return;
    // Capture every recipient before invoking any Lua. An ordinary subscriber
    // can post another event, whose panel payload must follow this event's.
    for (const auto &id : registry_.subscribers(event)) {
        deferredEvents_.append({id, {}, event, arg, {eventNote, generations_.value(id), false,
            !host_->isLoaded(id) && !host_->isBusy(id)}});
    }
    for (const auto &pr : activePanelsFor(event)) {
        queuePanelRefresh(pr.qualifiedId(), note, false);
        if (pr.onEvent) deferredEvents_.append({pr.pluginId, pr.id, event, arg,
            {eventNote, generations_.value(pr.pluginId), false, !host_->isLoaded(pr.pluginId) && !host_->isBusy(pr.pluginId)}});
    }
    flushPanels();
    for (auto it = entries_.constBegin(); it != entries_.constEnd(); ++it)
        if (it->m.tier == Tier::Native && trust_.record(it.key()).enabled) native_->postEvent(it.key(), event, arg);
}

void PluginManager::post(const QString &event, const QString &arg, NoteBridge *note) {
    bool any = !registry_.subscribers(event).isEmpty() || !activePanelsFor(event).isEmpty();
    if (!any)
        for (auto it = entries_.constBegin(); it != entries_.constEnd() && !any; ++it) any = it->m.tier == Tier::Native && trust_.record(it.key()).enabled;
    if (!any) return;  // nobody listens: no allocation, no timer
    if (coalesced().contains(event)) {
        pending_[event] = {arg, noteRef(note)};
        if (!timer_.isActive()) timer_.start();
        return;
    }
    const auto context = noteRef(note);
    const QString incomingEvent = event, payload = arg;
    if (!pending_.isEmpty()) flushEvents();  // keep ordering: coalesced events first
    if (context.valid()) deliverNow(incomingEvent, payload, note);
}

void PluginManager::flushEvents() {
    timer_.stop();
    const auto p = pending_;
    pending_.clear();
    for (auto it = p.begin(); it != p.end(); ++it)
        if (it->note.valid()) deliverNow(it.key(), it->arg, it->note.note);
}

void PluginManager::detachNote(NoteBridge *note) {
    if (!note) return;
    // Preserve delivery before an ordinary close. deliverNow defers busy hosts;
    // those queued references are cancelled below before the bridge can die.
    for (const auto &event : std::as_const(pending_))
        if (event.note.note == note) { flushEvents(); break; }
    auto &alive = noteLives_[note];
    if (!alive) alive = std::make_shared<bool>(false); else *alive = false;
    for (auto it = pending_.begin(); it != pending_.end();)
        if (it->note.note == note) it = pending_.erase(it); else ++it;
    for (auto it = pendingPanels_.begin(); it != pendingPanels_.end();)
        if (it->note.note == note) it = pendingPanels_.erase(it); else ++it;
    deferredEvents_.removeIf([&](const DeferredEvent &event) { return event.request.note.note == note; });
    if (!host_->usesNote(note)) noteLives_.remove(note);
    if (pending_.isEmpty()) timer_.stop();
    if (pendingPanels_.isEmpty()) panelTimer_.stop();
}

QString PluginManager::preSave(const QString &text, NoteBridge *note) {
    const auto context = noteRef(note);
    QString cur = text;
    for (const auto &id : registry_.subscribers(QStringLiteral("note.pre_save"))) {
        if (!context.valid()) break;
        cur = host_->preSave(id, cur, note);
    }
    return cur;
}

std::optional<TriggerMatch> PluginManager::matchTrigger(const QString &before, NoteBridge *note) {
    TriggerReg best;
    for (const auto &t : registry_.triggers())
        if (before.endsWith(t.pattern) && t.pattern.size() > best.pattern.size()) best = t;
    if (best.pattern.isEmpty()) return std::nullopt;
    TriggerMatch m{best.pluginId, best.pattern, QString()};
    if (!host_->runTrigger(best.pluginId, best.pattern, note, &m.replacement)) return std::nullopt;
    return m;
}

// ================= API 2 =================
bool PluginManager::renderPanel(const QString &pid, const QString &panelId, NoteBridge *note, QList<PanelBlock> *out, QString *err) {
    auto fail = [&](const QString &m) { if (err) *err = m; return false; };
    const auto it = entries_.constFind(pid);
    if (it == entries_.constEnd() || it->m.tier != Tier::Script || !trust_.record(pid).enabled) return fail(QStringLiteral("plugin '%1' is not enabled").arg(pid));
    bool known = false;
    for (const auto &p : registry_.panels(pid)) known |= p.id == panelId;
    if (!known) return fail(QStringLiteral("unknown panel '%1:%2'").arg(pid, panelId));
    return host_->renderPanel(pid, panelId, note, out, err);
}

bool PluginManager::refreshPanel(const QString &pid, const QString &panelId, NoteBridge *note, QString *err) {
    QList<PanelBlock> blocks;
    QString e;
    const bool ok = renderPanel(pid, panelId, note, &blocks, &e);
    if (err) *err = e;
    if (auto *pb = cfg_.bridges.panel) pb->updatePanel(pid + QLatin1Char(':') + panelId, ok ? blocks : QList<PanelBlock>(), ok ? QString() : e);
    return ok;
}

bool PluginManager::panelClick(const QString &pid, const QString &panelId, int token, NoteBridge *note, QString *err) {
    auto fail = [&](const QString &m) { if (err) *err = m; return false; };
    const auto it = entries_.constFind(pid);
    if (it == entries_.constEnd() || !trust_.record(pid).enabled) return fail(QStringLiteral("plugin '%1' is not enabled").arg(pid));
    return host_->panelClick(pid, panelId, token, note, err);
}

void PluginManager::setPanelActive(const QString &pid, const QString &panelId, bool active) {
    const QString q = pid + QLatin1Char(':') + panelId;
    if (active) activePanels_.insert(q);
    else {
        activePanels_.remove(q); pendingPanels_.remove(q);
        deferredEvents_.removeIf([&](const DeferredEvent &event) { return event.pluginId == pid && event.panelId == panelId; });
    }
}

PluginManager::NoteRef PluginManager::noteRef(NoteBridge *note) {
    if (!note) return {};
    auto &alive = noteLives_[note];
    if (!alive) alive = std::make_shared<bool>(true);
    return {note, alive};
}

bool PluginManager::validPanel(const QString &qid, const PanelRequest &request) const {
    const int c = int(qid.indexOf(QLatin1Char(':')));
    const QString pid = qid.left(c), panel = qid.mid(c + 1);
    if (c <= 0 || !request.note.valid() || !activePanels_.contains(qid) ||
        request.generation != generations_.value(pid) || !trust_.record(pid).enabled || !entries_.contains(pid)) return false;
    for (const auto &registration : registry_.panels(pid)) if (registration.id == panel) return true;
    return false;
}

void PluginManager::queuePanelRefresh(const QString &qid, NoteBridge *note, bool explicitContext) {
    const QString pid = qid.section(QLatin1Char(':'), 0, 0);
    PanelRequest request{noteRef(note), generations_.value(pid), explicitContext, !host_->isLoaded(pid) && !host_->isBusy(pid)};
    if (!validPanel(qid, request)) return;
    const auto old = pendingPanels_.constFind(qid);
    if (old != pendingPanels_.cend() && old->explicitContext && !explicitContext) return;
    pendingPanels_[qid] = request;
}

void PluginManager::requestPanelRefresh(const QString &pid, const QString &panelId, NoteBridge *note) {
    const QString qid = pid + QLatin1Char(':') + panelId;
    PanelRequest request{noteRef(note), generations_.value(pid), true};
    if (!validPanel(qid, request)) return;
    const bool pendingEvents = std::any_of(deferredEvents_.cbegin(), deferredEvents_.cend(), [&](const DeferredEvent &event) { return event.pluginId == pid; });
    if (!host_->isBusy(pid) && !pendingEvents && !drainingPanels_) {
        pendingPanels_.remove(qid);
        refreshPanel(pid, panelId, note);
    } else queuePanelRefresh(qid, note, true);
}

void PluginManager::schedulePanelDrain() {
    if (drainScheduled_ || drainingPanels_ || (pendingPanels_.isEmpty() && deferredEvents_.isEmpty())) return;
    drainScheduled_ = true;
    QTimer::singleShot(0, this, [this] {
        drainScheduled_ = false;
        flushPanels();
    });
}

void PluginManager::flushPanels() {
    if (drainingPanels_) return;
    panelTimer_.stop();
    drainingPanels_ = true;
    const auto consumeLoad = [this](const QString &pid) {
        // Only the first callback/render may load a cold plugin. If it unloads
        // the state, remaining work from that batch must not recreate it.
        for (auto &event : deferredEvents_) if (event.pluginId == pid) event.request.allowLoad = false;
        for (auto it = pendingPanels_.begin(); it != pendingPanels_.end(); ++it)
            if (it.key().section(QLatin1Char(':'), 0, 0) == pid) it->allowLoad = false;
    };
    // Remove one event at a time: callbacks may invalidate the remaining queue.
    // A handler that keeps generating events (A<->B note.opened) must not hold
    // the UI: run a bounded batch per turn and cap the cumulative cascade.
    constexpr int kBatch = 64, kMaxCascade = 1024;
    int ran = 0;
    for (int i = 0; i < deferredEvents_.size();) {
        if (drainCascade_ >= kMaxCascade) {
            audit_.log(QStringLiteral("failure"), deferredEvents_.first().pluginId, QStringLiteral("runaway event cascade; %1 queued events dropped").arg(deferredEvents_.size()));
            deferredEvents_.clear();
            drainCascade_ = 0;
            break;
        }
        if (ran >= kBatch) break;
        const auto event = deferredEvents_[i];
        const QString qid = event.pluginId + QLatin1Char(':') + event.panelId;
        const bool valid = event.panelId.isEmpty()
            ? event.request.note.valid() && event.request.generation == generations_.value(event.pluginId) &&
                trust_.record(event.pluginId).enabled && registry_.subscribers(event.event).contains(event.pluginId)
            : validPanel(qid, event.request);
        if (!valid || (!host_->isLoaded(event.pluginId) && !event.request.allowLoad)) { deferredEvents_.removeAt(i); continue; }
        if (host_->isBusy(event.pluginId)) { ++i; continue; }
        deferredEvents_.removeAt(i);
        ++ran; ++drainCascade_;
        consumeLoad(event.pluginId);
        if (event.panelId.isEmpty()) host_->deliver(event.pluginId, event.event, event.arg, event.request.note.note);
        else host_->deliverPanelEvent(event.pluginId, event.panelId, event.event, event.arg, event.request.note.note);
        i = 0;
    }
    const auto ids = pendingPanels_.keys();
    for (const auto &qid : ids) {
        auto it = pendingPanels_.find(qid);
        if (it == pendingPanels_.end()) continue;
        const QString pid = qid.section(QLatin1Char(':'), 0, 0);
        if (!validPanel(qid, *it) || (!host_->isLoaded(pid) && !it->allowLoad)) { pendingPanels_.erase(it); continue; }
        if (host_->isBusy(pid)) continue; // callbackFinished will schedule a drain; never spin a nested event loop.
        const auto request = *it;
        pendingPanels_.erase(it);
        consumeLoad(pid);
        refreshPanel(pid, qid.section(QLatin1Char(':'), 1), request.note.note);
    }
    drainingPanels_ = false;
    if (deferredEvents_.isEmpty()) drainCascade_ = 0;
    // A render can open a note and enqueue a newer context after its own request
    // was consumed. Schedule another turn only for work that can make progress.
    bool ready = std::any_of(deferredEvents_.cbegin(), deferredEvents_.cend(), [&](const DeferredEvent &event) { return !host_->isBusy(event.pluginId); });
    for (auto it = pendingPanels_.cbegin(); !ready && it != pendingPanels_.cend(); ++it)
        ready = !host_->isBusy(it.key().section(QLatin1Char(':'), 0, 0));
    if (ready) schedulePanelDrain();
}

QList<CompletionItem> PluginManager::complete(const QString &trigger, const QString &query, NoteBridge *note, bool atLineStart) {
    const auto context = noteRef(note);
    const QString input = query;
    QList<CompletionItem> all;
    for (const auto &c : registry_.completions(trigger)) {
        if (!context.valid() || all.size() >= 50) break;
        const auto it = entries_.constFind(c.pluginId);
        if (it == entries_.constEnd() || !trust_.record(c.pluginId).enabled) continue;
        for (const auto &item : host_->complete(c.pluginId, c.id, input, note, nullptr, nullptr, atLineStart)) {
            if (all.size() >= 50) break;
            all << item;
        }
    }
    return all;
}
void PluginManager::requestCompletion(quint64 token, const QString &trigger, const QString &query, NoteBridge *note, bool atLineStart) {
    const auto items = complete(trigger, query, note, atLineStart);
    if (auto *e = cfg_.bridges.editor) e->completionReply(token, items);
}

bool PluginManager::activateLink(const LinkActivation &ref, NoteBridge *note) {
    const auto context = noteRef(note);
    const LinkActivation link = ref;
    for (const auto &h : registry_.linkHandlers()) {
        if (!context.valid()) break;
        const auto it = entries_.constFind(h.pluginId);
        if (it == entries_.constEnd() || !trust_.record(h.pluginId).enabled) continue;
        if (host_->activateLink(h.pluginId, link, note)) return true;
    }
    return false;
}
void PluginManager::requestLinkActivation(quint64 token, const LinkActivation &ref, NoteBridge *note) {
    const bool handled = activateLink(ref, note);
    if (auto *e = cfg_.bridges.editor) e->linkActivationReply(token, handled);
}

}  // namespace hn::plugins
