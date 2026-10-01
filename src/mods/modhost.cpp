#include "hn/mods/mods.h"
#include "hyprnotes/mod_api.h"
#include <QDebug>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <dlfcn.h>
#include <elf.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <QLibrary>
#include <QPointer>
#include <QTimer>
#include <cstring>
#include <map>

namespace hn::mods {

// D12: mapping a truncated/damaged .so faults (SIGBUS) inside ld.so and cannot be caught. Check the ELF header and
// that every program/section header and loadable segment lies inside the file before dlopen. Empty = looks sane.
static QString elfProblem(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QStringLiteral("cannot open");
    const quint64 size = quint64(f.size());
    Elf64_Ehdr eh;
    if (f.read(reinterpret_cast<char *>(&eh), sizeof eh) != qint64(sizeof eh)) return QStringLiteral("file too short for an ELF header");
    if (memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0 || eh.e_ident[EI_CLASS] != ELFCLASS64 || eh.e_ident[EI_DATA] != ELFDATA2LSB) return QStringLiteral("not a 64-bit little-endian ELF file");
    if (eh.e_type != ET_DYN || eh.e_phentsize != sizeof(Elf64_Phdr) || eh.e_phnum == 0) return QStringLiteral("not a shared object");
    if (eh.e_phoff > size || quint64(eh.e_phnum) * sizeof(Elf64_Phdr) > size - eh.e_phoff) return QStringLiteral("program headers lie outside the file (truncated?)");
    if (eh.e_shnum && (eh.e_shentsize != sizeof(Elf64_Shdr) || eh.e_shoff > size || quint64(eh.e_shnum) * sizeof(Elf64_Shdr) > size - eh.e_shoff)) return QStringLiteral("section headers lie outside the file (truncated?)");
    bool dyn = false;
    for (int i = 0; i < eh.e_phnum; ++i) {
        Elf64_Phdr ph;
        if (!f.seek(qint64(eh.e_phoff) + qint64(i) * qint64(sizeof ph)) || f.read(reinterpret_cast<char *>(&ph), sizeof ph) != qint64(sizeof ph)) return QStringLiteral("unreadable program header");
        if (ph.p_type != PT_LOAD && ph.p_type != PT_DYNAMIC) continue;
        if (ph.p_offset > size || ph.p_filesz > size - ph.p_offset) return QStringLiteral("segment %1 extends past the end of the file (truncated?)").arg(i);
        if (ph.p_type == PT_LOAD && (ph.p_filesz > ph.p_memsz || (ph.p_align > 1 && (ph.p_align & (ph.p_align - 1)))
                                     || (ph.p_align > 1 && ph.p_vaddr % ph.p_align != ph.p_offset % ph.p_align))) return QStringLiteral("inconsistent segment %1").arg(i);
        dyn |= ph.p_type == PT_DYNAMIC;
    }
    if (!dyn) return QStringLiteral("no dynamic section");
    return {};
}

// D12: header checks cannot see damage inside the dynamic/relocation tables, which crashes ld.so. Load the library once
// in a forked child (dlopen only, then _exit) and look at how it ended; a crash or hang there costs the app nothing.
static QString probeLoad(const QString &path) {
    const QByteArray p = path.toUtf8();
    const pid_t pid = fork();
    if (pid < 0) return {};   // cannot probe: fall through to the normal load
    if (pid == 0) { void *h = dlopen(p.constData(), RTLD_NOW | RTLD_LOCAL); _exit(h ? 0 : 3); }
    int st = 0;
    for (int i = 0; i < 500; ++i) {   // 5 s
        const pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid) {
            if (WIFSIGNALED(st)) return QStringLiteral("library crashed the loader (signal %1)").arg(WTERMSIG(st));
            return WEXITSTATUS(st) == 0 ? QString() : QStringLiteral("library cannot be loaded");
        }
        if (r < 0) return {};
        usleep(10000);
    }
    kill(pid, SIGKILL); waitpid(pid, &st, 0);
    return QStringLiteral("library did not finish loading in 5 s");
}

struct Cmd { QString modId, title; hn_command_fn fn = nullptr; void *user = nullptr; };
struct Sub { QString modId; uint32_t type; hn_event_fn fn; void *user; };

struct ModCtx {
    hn_host_api api{};
    ModHostPrivate *d = nullptr;
    QString modId;
    bool inEntry = false;
    QList<QPair<QString, Cmd>> pendingCmds;
    QList<Sub> pendingSubs;
};

struct Loaded {
    std::unique_ptr<QLibrary> lib;
    std::unique_ptr<ModCtx> ctx;
    hn_mod_api api{};
};

class ModHostPrivate {
public:
    ModHost *q = nullptr;
    QHash<QString, Manifest> manifests;
    QStringList enabledIds;
    QSet<QString> failed;
    QList<ModError> errors;
    QHash<QString, Cmd> cmds;
    QList<Sub> subs;
    std::map<QString, Loaded> loaded;
    QList<QPair<int, QString>> pending;
    bool flushScheduled = false;
    QObject *workParent = nullptr;
    DocumentBridge *doc = nullptr;
    int txDepth = 0;
    ModHost::Logger logger;
    int slowMs = 4, coalesceMs = 16;
    Stats st;

    void log(int level, const QString &mod, const QString &msg) {
        if (logger) logger(level, mod, msg);
        else if (level >= HN_LOG_WARN) qWarning().noquote() << "[mod" << mod << "]" << msg;
        else qDebug().noquote() << "[mod" << mod << "]" << msg;
    }
    void err(const QString &mod, const QString &msg) { errors.append({mod, msg}); log(HN_LOG_ERROR, mod, msg); }

    template <class F> void timed(const QString &mod, const char *what, DocumentBridge *bound, F &&f) {
        DocumentBridge *prevDoc = doc; int prevTx = txDepth;
        doc = bound; txDepth = 0;
        QElapsedTimer t; t.start();
        f();
        const qint64 ns = t.nsecsElapsed();
        if (txDepth > 0 && doc) {
            log(HN_LOG_WARN, mod, QStringLiteral("%1 left a transaction open; closed by host").arg(QLatin1String(what)));
            doc->endTransaction();
        }
        doc = prevDoc; txDepth = prevTx;
        ++st.callbacks;
        st.maxCallbackNs = qMax(st.maxCallbackNs, ns);
        if (ns > qint64(slowMs) * 1000000) {
            ++st.slowCallbacks;
            log(HN_LOG_WARN, mod, QStringLiteral("%1 took %2 ms (budget %3 ms)").arg(QLatin1String(what)).arg(ns / 1e6, 0, 'f', 2).arg(slowMs));
        }
    }

    void rebuildDeclared() {
        cmds.clear();
        for (const auto &id : enabledIds) {
            auto it = manifests.constFind(id);
            if (it == manifests.constEnd()) continue;
            for (const auto &c : it->commands) cmds.insert(c.id, {id, c.title, nullptr, nullptr});
            for (const auto &a : it->activation)
                if (a.startsWith(QStringLiteral("on-command:")) && !cmds.contains(a.mid(11)))
                    cmds.insert(a.mid(11), {id, a.mid(11), nullptr, nullptr});
        }
    }

    bool activate(const QString &id);
    void scheduleFlush() {
        if (flushScheduled) return;
        flushScheduled = true;
        QTimer::singleShot(coalesceMs, q, [this] { q->flushEvents(); });
    }
};

// ---- C host functions ----
static ModCtx *C(hn_host *h) { return reinterpret_cast<ModCtx *>(h); }
static QString utf8(const char *s, size_t n) { return QString::fromUtf8(s, qsizetype(n)); }
static constexpr size_t kMaxText = 16u << 20;

static hn_status h_register_command(hn_host *h, const char *id, const char *title, hn_command_fn cb, void *user) {
    auto *c = C(h);
    if (!c || !c->inEntry || !id || !*id || !cb) return HN_ERR_INVALID;
    const QString sid = QString::fromUtf8(id);
    const auto it = c->d->cmds.constFind(sid);
    if (it != c->d->cmds.constEnd() && it->modId != c->modId) return HN_ERR_STATE;
    c->pendingCmds.append({sid, {c->modId, title ? QString::fromUtf8(title) : sid, cb, user}});
    return HN_OK;
}
static void h_log(hn_host *h, int level, const char *msg) { if (h && msg) C(h)->d->log(level, C(h)->modId, QString::fromUtf8(msg)); }
static char *h_get_selection(hn_host *h, size_t *len) {
    auto *c = C(h);
    if (len) *len = 0;
    if (!c || !c->d->doc) return nullptr;
    const QByteArray b = c->d->doc->selectionText().toUtf8();
    char *p = static_cast<char *>(malloc(size_t(b.size()) + 1));
    if (!p) return nullptr;
    memcpy(p, b.constData(), size_t(b.size()) + 1);
    if (len) *len = size_t(b.size());
    return p;
}
static hn_status h_begin(hn_host *h, const char *name) {
    auto *d = C(h)->d;
    if (!d->doc) return HN_ERR_NO_DOCUMENT;
    if (d->txDepth) return HN_ERR_STATE;
    d->txDepth = 1;
    d->doc->beginTransaction(name ? QString::fromUtf8(name) : QString());
    return HN_OK;
}
static hn_status mutate(hn_host *h, const char *s, size_t n, bool replace) {
    auto *d = C(h)->d;
    if (!d->doc) return HN_ERR_NO_DOCUMENT;
    if (d->txDepth != 1) return HN_ERR_STATE;
    if ((!s && n) || n > kMaxText) return HN_ERR_INVALID;
    replace ? d->doc->replaceSelection(utf8(s ? s : "", n)) : d->doc->insertText(utf8(s ? s : "", n));
    return HN_OK;
}
static hn_status h_replace(hn_host *h, const char *s, size_t n) { return mutate(h, s, n, true); }
static hn_status h_insert(hn_host *h, const char *s, size_t n) { return mutate(h, s, n, false); }
static hn_status h_end(hn_host *h) {
    auto *d = C(h)->d;
    if (!d->doc) return HN_ERR_NO_DOCUMENT;
    if (d->txDepth != 1) return HN_ERR_STATE;
    d->txDepth = 0;
    d->doc->endTransaction();
    return HN_OK;
}
static hn_status h_subscribe(hn_host *h, uint32_t type, hn_event_fn cb, void *user) {
    auto *c = C(h);
    if (!c->inEntry) return HN_ERR_STATE;
    if (!cb || type < HN_EVENT_NOTE_OPENED || type > HN_EVENT_SELECTION_CHANGED) return HN_ERR_INVALID;
    c->pendingSubs.append({c->modId, type, cb, user});
    return HN_OK;
}
static hn_status h_schedule(hn_host *h, hn_work_fn cb, void *user, uint32_t delay) {
    auto *c = C(h);
    if (!cb) return HN_ERR_INVALID;
    auto *d = c->d;
    if (!d->workParent) d->workParent = new QObject(d->q);
    auto *t = new QTimer(d->workParent);
    t->setSingleShot(true);
    QObject::connect(t, &QTimer::timeout, t, [d, c, cb, user, t] {
        d->timed(c->modId, "scheduled work", nullptr, [&] { cb(reinterpret_cast<hn_host *>(c), user); });
        t->deleteLater();
    });
    t->start(int(qMin<uint32_t>(delay, 3600u * 1000u)));
    return HN_OK;
}
static void h_free(hn_host *, void *p) { free(p); }

bool ModHostPrivate::activate(const QString &id) {
    if (loaded.count(id)) return true;
    if (failed.contains(id) || !enabledIds.contains(id)) return false;
    const auto mit = manifests.constFind(id);
    if (mit == manifests.constEnd()) return false;
    const Manifest &m = *mit;
    auto fail = [&](const QString &msg) { err(id, msg); failed.insert(id); return false; };

    const QFileInfo lfi(m.dir + QLatin1Char('/') + m.library);
    if (!lfi.isFile()) return fail(QStringLiteral("library not found: %1").arg(m.library));
    if (!lfi.canonicalFilePath().startsWith(QFileInfo(m.dir).canonicalFilePath() + QLatin1Char('/')))
        return fail(QStringLiteral("library resolves outside the mod directory"));

    Loaded L;
    QString why = elfProblem(lfi.canonicalFilePath());
    if (why.isEmpty()) why = probeLoad(lfi.canonicalFilePath());
    if (!why.isEmpty()) return fail(QStringLiteral("refusing to load library: %1").arg(why));
    L.lib = std::make_unique<QLibrary>(lfi.canonicalFilePath());
    if (!L.lib->load()) return fail(QStringLiteral("cannot load library: %1").arg(L.lib->errorString()));
    auto entry = reinterpret_cast<hn_mod_entry_fn>(L.lib->resolve(m.entry.toUtf8().constData()));
    if (!entry) { L.lib->unload(); return fail(QStringLiteral("entry symbol '%1' not found").arg(m.entry)); }

    L.ctx = std::make_unique<ModCtx>();
    ModCtx *c = L.ctx.get();
    c->d = this; c->modId = id; c->inEntry = true;
    c->api = {sizeof(hn_host_api), HN_MOD_API_VERSION, reinterpret_cast<hn_host *>(c), h_register_command, h_log,
              h_get_selection, h_begin, h_replace, h_insert, h_end, h_subscribe, h_schedule, h_free};
    L.api.struct_size = sizeof(hn_mod_api);
    hn_status rc = HN_ERR_FAILED;
    timed(id, "entry", nullptr, [&] { rc = entry(&c->api, &L.api); });
    c->inEntry = false;
    if (rc != HN_OK) { L.lib->unload(); return fail(QStringLiteral("entry returned status %1").arg(int(rc))); }
    if (L.api.api_version != HN_MOD_API_VERSION) { L.lib->unload(); return fail(QStringLiteral("module api_version %1 incompatible with host %2").arg(L.api.api_version).arg(HN_MOD_API_VERSION)); }
    if (L.api.struct_size < HN_MOD_API_V1_SIZE) { L.lib->unload(); return fail(QStringLiteral("module hn_mod_api struct_size %1 too small").arg(L.api.struct_size)); }

    for (auto &pc : c->pendingCmds) cmds.insert(pc.first, pc.second);
    for (auto &s : c->pendingSubs) subs.append(s);
    c->pendingCmds.clear(); c->pendingSubs.clear();
    loaded.emplace(id, std::move(L));
    return true;
}

ModHost::ModHost(QObject *parent) : QObject(parent), d(new ModHostPrivate) { d->q = this; }
ModHost::~ModHost() { shutdown(); }
void ModHost::setLogger(Logger l) { d->logger = std::move(l); }
void ModHost::setSlowThresholdMs(int ms) { d->slowMs = ms; }
void ModHost::setCoalesceMs(int ms) { d->coalesceMs = ms; }
QList<Manifest> ModHost::installed() const { return d->manifests.values(); }
QStringList ModHost::enabled() const { return d->enabledIds; }
QList<ModError> ModHost::errors() const { return d->errors; }
bool ModHost::isLoaded(const QString &id) const { return d->loaded.count(id) > 0; }
Stats ModHost::stats() const { return d->st; }

QList<CommandInfo> ModHost::commands() const {
    QList<CommandInfo> l;
    for (auto it = d->cmds.cbegin(); it != d->cmds.cend(); ++it) l.append({it.key(), it->title, it->modId});
    return l;
}

void ModHost::start(const QString &modsDir, const QString &enabledPath) {
    shutdown();
    d->errors.clear(); d->failed.clear(); d->manifests.clear();
    for (const auto &m : scanManifests(modsDir, &d->errors)) d->manifests.insert(m.id, m);
    QList<ModError> e;
    const auto want = loadEnabled(enabledPath, &e);
    d->errors += e;
    d->enabledIds.clear();
    for (const auto &id : want) {
        if (d->manifests.contains(id)) d->enabledIds << id;
        else d->errors.append({id, QStringLiteral("enabled mod not installed or invalid manifest")});
    }
    d->rebuildDeclared();
    for (const auto &id : std::as_const(d->enabledIds))
        if (d->manifests[id].activation.contains(QStringLiteral("on-startup"))) d->activate(id);
}

bool ModHost::runCommand(const QString &commandId, DocumentBridge *doc) {
    auto it = d->cmds.constFind(commandId);
    if (it == d->cmds.constEnd()) return false;
    if (!it->fn) {
        d->activate(it->modId);
        it = d->cmds.constFind(commandId);
        if (it == d->cmds.constEnd() || !it->fn) {
            if (!d->failed.contains(d->cmds.value(commandId).modId))
                d->err(d->cmds.value(commandId).modId, QStringLiteral("command '%1' not registered by module").arg(commandId));
            return false;
        }
    }
    const Cmd c = *it;
    auto *ctx = d->loaded.at(c.modId).ctx.get();
    d->timed(c.modId, "command", doc, [&] { c.fn(reinterpret_cast<hn_host *>(ctx), c.user); });
    return true;
}

void ModHost::post(EventType t, const QString &noteId) {
    if (t == EventType::NoteOpened)
        for (const auto &id : std::as_const(d->enabledIds))
            if (!d->loaded.count(id) && d->manifests[id].activation.contains(QStringLiteral("on-note-open"))) d->activate(id);
    bool any = false;
    for (const auto &s : std::as_const(d->subs)) if (s.type == uint32_t(t)) { any = true; break; }
    if (!any) return;
    const QPair<int, QString> key{int(t), noteId};
    // selection-changed and note-saved coalesce to the latest per note; opened/closed are never dropped.
    const bool coalesce = t == EventType::SelectionChanged || t == EventType::NoteSaved;
    if (!(coalesce && d->pending.contains(key))) d->pending.append(key);
    d->scheduleFlush();
}

void ModHost::flushEvents() {
    d->flushScheduled = false;
    const auto batch = std::exchange(d->pending, {});
    for (const auto &e : batch) {
        const QByteArray id = e.second.toUtf8();
        hn_event ev{sizeof(hn_event), uint32_t(e.first), id.constData()};
        const auto subs = d->subs; // copy: callbacks must not invalidate iteration
        for (const auto &s : subs)
            if (s.type == ev.type && d->loaded.count(s.modId))
                d->timed(s.modId, "event", nullptr, [&] { s.fn(reinterpret_cast<hn_host *>(d->loaded.at(s.modId).ctx.get()), &ev, s.user); });
    }
}

void ModHost::shutdown() {
    delete d->workParent; d->workParent = nullptr;
    d->pending.clear();
    for (auto &[lid, L] : d->loaded) {
        if (L.api.shutdown) d->timed(L.ctx->modId, "shutdown", nullptr, [&] { L.api.shutdown(reinterpret_cast<hn_host *>(L.ctx.get())); });
        L.lib->unload();
    }
    d->loaded.clear(); d->subs.clear();
    d->rebuildDeclared();
}

} // namespace hn::mods
