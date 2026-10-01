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
    connect(&registry_, &PluginRegistry::changed, this, [this] { saveCache(); emit pluginsChanged(); });
    connect(host_.get(), &LuaPluginHost::autoDisabled, this, [this](const QString &id, const QString &why) {
        if (auto it = entries_.find(id); it != entries_.end()) it->lastError = why;
        registry_.remove(id);
        emit pluginAutoDisabled(id, why);
        emit pluginsChanged();
    });
}
PluginManager::~PluginManager() {
    for (auto it = entries_.begin(); it != entries_.end(); ++it)
        if (it->m.tier == Tier::Native && trust_.record(it.key()).enabled) native_->deactivate(it.key());
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

void PluginManager::activateEntry(const QString &id, Entry &e) {
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
            host_->setPlugin(e.m, rec.permissions, nullptr);
            ok = host_->ensureLoaded(id, &err);  // no cache for this package yet: run main chunk once to learn registrations
            if (ok) host_->unload(id);           // ...then drop the state again; the first real hook recreates it
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
    cacheSuspended_ = true;
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
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
    const QString dir = it->m.dir;
    const bool wasEnabled = trust_.record(id).enabled;
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

void PluginManager::deliverNow(const QString &event, const QString &arg, NoteBridge *note) {
    for (const auto &id : registry_.subscribers(event)) host_->deliver(id, event, arg, note);
    for (auto it = entries_.constBegin(); it != entries_.constEnd(); ++it)
        if (it->m.tier == Tier::Native && trust_.record(it.key()).enabled) native_->postEvent(it.key(), event, arg);
}

void PluginManager::post(const QString &event, const QString &arg, NoteBridge *note) {
    bool any = !registry_.subscribers(event).isEmpty();
    if (!any)
        for (auto it = entries_.constBegin(); it != entries_.constEnd() && !any; ++it) any = it->m.tier == Tier::Native && trust_.record(it.key()).enabled;
    if (!any) return;  // nobody listens: no allocation, no timer
    if (coalesced().contains(event)) {
        pending_[event] = {arg, note};
        if (!timer_.isActive()) timer_.start();
        return;
    }
    if (!pending_.isEmpty()) flushEvents();  // keep ordering: coalesced events first
    deliverNow(event, arg, note);
}

void PluginManager::flushEvents() {
    timer_.stop();
    const auto p = pending_;
    pending_.clear();
    for (auto it = p.begin(); it != p.end(); ++it) deliverNow(it.key(), it->arg, it->note);
}

void PluginManager::detachNote(NoteBridge *note) {
    for (const auto &p : std::as_const(pending_))
        if (p.note == note) { flushEvents(); return; }
}

QString PluginManager::preSave(const QString &text, NoteBridge *note) {
    QString cur = text;
    for (const auto &id : registry_.subscribers(QStringLiteral("note.pre_save"))) cur = host_->preSave(id, cur, note);
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

}  // namespace hn::plugins
