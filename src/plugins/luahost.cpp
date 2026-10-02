#include "hn/plugins/runtime.h"
#include "luapattern.h"
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QDateTime>
#include <chrono>
#include <ctime>
#include <lua.hpp>

namespace hn::plugins {
namespace {

constexpr int kHookStep = 200;              // instructions between budget checks
constexpr int kMaxDepth = 400;              // Lua call depth cap
constexpr size_t kMaxStr = 4 * 1024 * 1024; // any string crossing the plugin/host boundary or built by string.rep
constexpr quint64 kHostReserve = 256 * 1024;  // heap headroom only host functions may use
constexpr int kMaxHandlers = 64, kMaxRegs = 64, kMaxNoteWrites = 50, kMaxLogLines = 200;
constexpr int kHttpPerMinute = 20, kNotifyPer10s = 10;

qint64 nowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct RateLimiter {
    QList<qint64> t;
    bool allow(int max, qint64 windowUs) {
        const qint64 n = nowUs();
        while (!t.isEmpty() && n - t.first() > windowUs) t.removeFirst();
        if (t.size() >= max) return false;
        t.append(n);
        return true;
    }
};

}  // namespace

void hookFn(lua_State *L, lua_Debug *ar);
struct PluginState;  // defined below
struct LuaPluginHost::Impl {
    LuaPluginHost *q = nullptr;
    HostEnv env;
    QHash<QString, PluginState *> plugins;  // owned; deleted in forget()/dtor
};

struct PluginState {
    LuaPluginHost::Impl *d = nullptr;
    Manifest m;
    QSet<QString> perms;
    PluginRegs regs;
    bool regsDirty = false;
    lua_State *L = nullptr;
    quint64 mem = 0;
    QHash<QString, int> cb;                 // "cmd:id" "tb:id" "menu:id" "trig:pattern" -> registry ref
    QHash<QString, QList<int>> ev;
    bool busy = false, disabled = false, pendingUnload = false;
    int hostDepth = 0;  // >0 while a host function runs: it may use the allocator reserve (see allocFn)
    NoteBridge *note = nullptr;
    bool inTx = false;
    int noteWrites = 0, logLines = 0;
    qint64 deadlineUs = 0, instrs = 0, maxInstrs = 0;
    bool aborted = false;
    char abortMsg[64] = {0};
    RateLimiter notify, http, index, panelRefresh;
    // API 2: index time spent in the running callback, note opens, panel click tokens (token -> registry ref, per panel)
    qint64 indexWallUs = 0;
    int opens = 0, clickSeq = 0;
    QHash<QString, QHash<int, int>> clicks;
    QString renderingPanel;
    bool storageLoaded = false, storageDirty = false;
    QJsonObject kv, settings;

    QString storagePath() const { return d->env.stateDir + QStringLiteral("/plugin-data/") + m.id + QStringLiteral(".json"); }
    void loadStorage() {
        if (storageLoaded) return;
        storageLoaded = true;
        QFile f(storagePath());
        if (!f.open(QIODevice::ReadOnly)) return;
        const auto o = QJsonDocument::fromJson(f.read(4 * 1024 * 1024)).object();
        kv = o["kv"].toObject();
        settings = o["settings"].toObject();
    }
    void flushStorage() {
        if (!storageDirty) return;
        storageDirty = false;
        QDir().mkpath(QFileInfo(storagePath()).absolutePath());
        QSaveFile f(storagePath());
        if (f.open(QIODevice::WriteOnly)) {
            f.write(QJsonDocument(QJsonObject{{"kv", kv}, {"settings", settings}}).toJson(QJsonDocument::Compact));
            f.commit();
        }
    }
    void abort(lua_State *cur, const char *why) {
        aborted = true;
        qstrncpy(abortMsg, why, sizeof abortMsg);
        // fire on every instruction so a pcall cannot swallow the abort and keep running
        lua_sethook(L, hookFn, LUA_MASKCOUNT, 1);
        if (cur != L) lua_sethook(cur, hookFn, LUA_MASKCOUNT, 1);
    }
};

namespace {

PluginState *P(lua_State *L) {
    void *ud;
    lua_getallocf(L, &ud);
    return static_cast<PluginState *>(ud);
}

void *allocFn(void *ud, void *ptr, size_t osize, size_t nsize) {
    auto *p = static_cast<PluginState *>(ud);
    if (nsize == 0) {
        if (ptr) { p->mem -= osize; free(ptr); }
        return nullptr;
    }
    const size_t old = ptr ? osize : 0;
    // Script code may only fill the heap to cap - reserve. Host functions (hn.*) run with the reserve available so their
    // small allocations cannot fail: an out-of-memory error raised inside a host function would longjmp over live C++
    // objects (leak/UB). Big host allocations are checked up front with hostFits().
    const quint64 limit = p->hostDepth > 0 ? p->d->env.memoryCap : p->d->env.memoryCap - qMin<quint64>(kHostReserve, p->d->env.memoryCap / 4);
    if (nsize > old && p->mem + (nsize - old) > limit) return nullptr;
    void *np = realloc(ptr, nsize);
    if (np) p->mem = p->mem - old + nsize;
    return np;
}

}  // namespace

void hookFn(lua_State *L, lua_Debug *) {
    PluginState *p = P(L);
    if (!p->aborted) {
        p->instrs += kHookStep;
        lua_Debug ar;
        if (nowUs() > p->deadlineUs) p->abort(L, "time budget exceeded");
        else if (p->instrs > p->maxInstrs) p->abort(L, "instruction budget exceeded");
        else if (lua_getstack(L, kMaxDepth, &ar)) p->abort(L, "call depth limit exceeded");
    }
    if (p->aborted) luaL_error(L, "%s", p->abortMsg);
}

namespace {

// ---- helpers: C++ work happens inside guarded(); the Lua error is raised only after every C++ object is gone ----
template <class F> int guarded(lua_State *L, F &&f) {
    char msg[700];
    msg[0] = 0;
    int rc;
    PluginState *p = P(L);
    ++p->hostDepth;  // reset by callLua if a Lua error ever unwinds past this frame
    {
        QString err;
        rc = f(err);
        if (rc < 0) qstrncpy(msg, err.toUtf8().constData(), sizeof msg);
    }
    --p->hostDepth;
    if (rc < 0) return luaL_error(L, "%s", msg);
    return rc;
}

void auditLog(lua_State *L, const char *ev, const QString &detail) {
    PluginState *p = P(L);
    if (p->d->env.audit) p->d->env.audit->log(QString::fromLatin1(ev), p->m.id, detail);
}
bool allow(lua_State *L, const char *perm, QString &err) {
    PluginState *p = P(L);
    if (p->perms.contains(QLatin1String(perm))) return true;
    if (p->d->env.audit) p->d->env.audit->log(QStringLiteral("denied"), p->m.id, QString::fromLatin1(perm));
    err = QStringLiteral("permission denied: %1").arg(QLatin1String(perm));
    return false;
}
#define NEED(perm) if (!allow(L, perm, err)) return -1

bool argStr(lua_State *L, int i, QString &out, size_t max, QString &err, const char *what = "string") {
    if (!lua_isstring(L, i)) { err = QStringLiteral("bad argument #%1 (%2 expected)").arg(i).arg(QLatin1String(what)); return false; }
    size_t n;
    const char *s = lua_tolstring(L, i, &n);
    if (n > max) { err = QStringLiteral("bad argument #%1 (string too large, max %2 bytes)").arg(i).arg(max); return false; }
    out = QString::fromUtf8(s, qsizetype(n));
    return true;
}
// Raw field read: a plugin-supplied __index metamethod must never run (and be aborted by the hook) while C++ objects are live.
int getRaw(lua_State *L, int t, const char *k) {
    t = lua_absindex(L, t);
    lua_pushstring(L, k);
    return lua_rawget(L, t);
}
bool fieldStr(lua_State *L, int t, const char *k, QString &out, size_t max, bool required, QString &err) {
    getRaw(L, t, k);
    const bool isS = lua_type(L, -1) == LUA_TSTRING, none = lua_isnil(L, -1);
    bool ok = true;
    if (isS) {
        size_t n;
        const char *s = lua_tolstring(L, -1, &n);
        if (n > max) { err = QStringLiteral("field '%1' is too long").arg(QLatin1String(k)); ok = false; }
        else out = QString::fromUtf8(s, qsizetype(n));
    } else if (!none || required) {
        err = QStringLiteral("field '%1' must be a string").arg(QLatin1String(k));
        ok = false;
    }
    lua_pop(L, 1);
    return ok;
}
// True if `bytes` more can be allocated for this plugin (one GC attempt first, like Lua's own emergency collection).
bool hostFits(lua_State *L, size_t bytes) {
    PluginState *p = P(L);
    const quint64 cap = p->d->env.memoryCap, need = quint64(bytes) + 2048;
    if (p->mem + need <= cap) return true;
    lua_gc(L, LUA_GCCOLLECT);
    return p->mem + need <= cap;
}
// Unchecked: only after hostFits() covered the whole aggregate (list/table) being pushed.
void pushQRaw(lua_State *L, const QString &s) {
    const QByteArray u = s.toUtf8();
    lua_pushlstring(L, u.constData(), size_t(u.size()));
}
// Checked push for a single value. Returns false (nothing pushed, nothing raised) if it would not fit under the memory cap.
bool pushQ(lua_State *L, const QString &s) {
    if (!hostFits(L, size_t(s.size()) * 3)) return false;
    pushQRaw(L, s);
    return true;
}
const char *const kTooBig = "value too large for the plugin memory limit";
#define PUSHQ(x) do { if (!pushQ(L, (x))) { err = QString::fromLatin1(kTooBig); return -1; } } while (0)
bool needNote(lua_State *L, NoteBridge **nb, QString &err) {
    *nb = P(L)->note;
    if (!*nb) { err = QStringLiteral("no active note in this context"); return false; }
    return true;
}
struct PauseScope {  // blocking host calls (ui/http) must not eat the callback budget
    PluginState *p; qint64 t0 = nowUs();
    explicit PauseScope(PluginState *s) : p(s) {}
    ~PauseScope() { p->deadlineUs += nowUs() - t0; }
};
bool validNotePath(const QString &s) {
    if (s.isEmpty() || s.size() > 512 || s.startsWith(QLatin1Char('/')) || s.contains(QLatin1Char('\\')) || s.contains(QChar(0))) return false;
    for (const auto &c : s.split(QLatin1Char('/'))) if (c == QLatin1String("..")) return false;
    return true;
}

// ---- Lua <-> JSON ----
bool toJson(lua_State *L, int idx, int depth, int &nodes, QJsonValue &out, QString &err) {
    idx = lua_absindex(L, idx);
    if (++nodes > 200000) { err = QStringLiteral("value has too many elements"); return false; }
    switch (lua_type(L, idx)) {
    case LUA_TNIL: out = QJsonValue(QJsonValue::Null); return true;
    case LUA_TBOOLEAN: out = bool(lua_toboolean(L, idx)); return true;
    case LUA_TNUMBER:
        if (lua_isinteger(L, idx)) { out = double(lua_tointeger(L, idx)); return true; }
        else {
            const double x = lua_tonumber(L, idx);
            if (x != x || x - x != 0) { err = QStringLiteral("NaN/infinity cannot be stored"); return false; }
            out = x;
            return true;
        }
    case LUA_TSTRING: { size_t n; const char *s = lua_tolstring(L, idx, &n); out = QString::fromUtf8(s, qsizetype(n)); return true; }
    case LUA_TTABLE: break;
    default: err = QStringLiteral("cannot store a %1").arg(QLatin1String(luaL_typename(L, idx))); return false;
    }
    if (depth > 16) { err = QStringLiteral("value is nested too deeply"); return false; }
    if (!lua_checkstack(L, 6)) { err = QStringLiteral("value is too complex"); return false; }
    const lua_Unsigned n = lua_rawlen(L, idx);
    lua_Unsigned cnt = 0;
    bool arr = true;
    lua_pushnil(L);
    while (lua_next(L, idx)) {
        ++cnt;
        if (lua_type(L, -2) == LUA_TNUMBER && lua_isinteger(L, -2)) {
            const lua_Integer k = lua_tointeger(L, -2);
            if (k < 1 || lua_Unsigned(k) > n) arr = false;
        } else if (lua_type(L, -2) == LUA_TSTRING) arr = false;
        else { lua_pop(L, 2); err = QStringLiteral("table keys must be strings or array indices"); return false; }
        lua_pop(L, 1);
    }
    if (cnt != n) arr = false;
    if (arr) {
        QJsonArray a;
        for (lua_Unsigned i = 1; i <= n; ++i) {
            lua_rawgeti(L, idx, lua_Integer(i));
            QJsonValue v;
            const bool ok = toJson(L, -1, depth + 1, nodes, v, err);
            lua_pop(L, 1);
            if (!ok) return false;
            a.append(v);
        }
        out = a;
    } else {
        QJsonObject o;
        lua_pushnil(L);
        while (lua_next(L, idx)) {
            if (lua_type(L, -2) != LUA_TSTRING) { lua_pop(L, 2); err = QStringLiteral("mixed table cannot be stored (use only string keys or a pure array)"); return false; }
            size_t kn;
            const char *ks = lua_tolstring(L, -2, &kn);
            const QString key = QString::fromUtf8(ks, qsizetype(kn));
            QJsonValue v;
            if (!toJson(L, -1, depth + 1, nodes, v, err)) { lua_pop(L, 2); return false; }
            o.insert(key, v);
            lua_pop(L, 1);
        }
        out = o;
    }
    return true;
}
void pushJson(lua_State *L, const QJsonValue &v, int depth = 0) {
    if (depth > 20 || !lua_checkstack(L, 6)) { lua_pushnil(L); return; }
    switch (v.type()) {
    case QJsonValue::Bool: lua_pushboolean(L, v.toBool()); break;
    case QJsonValue::Double: {
        const double x = v.toDouble();
        if (x == double(qint64(x)) && x > -9e15 && x < 9e15) lua_pushinteger(L, lua_Integer(x));
        else lua_pushnumber(L, x);
        break;
    }
    case QJsonValue::String: pushQRaw(L, v.toString()); break;
    case QJsonValue::Array: {
        const auto a = v.toArray();
        lua_createtable(L, int(a.size()), 0);
        for (int i = 0; i < a.size(); ++i) { pushJson(L, a[i], depth + 1); lua_rawseti(L, -2, i + 1); }
        break;
    }
    case QJsonValue::Object: {
        const auto o = v.toObject();
        lua_createtable(L, 0, int(o.size()));
        for (auto it = o.begin(); it != o.end(); ++it) { pushJson(L, it.value(), depth + 1); const QByteArray k = it.key().toUtf8(); lua_setfield(L, -2, k.constData()); }
        break;
    }
    default: lua_pushnil(L);
    }
}

size_t jsonCost(const QJsonValue &v, int depth = 0) {  // upper bound of the Lua heap pushJson needs
    if (depth > 20) return 0;
    switch (v.type()) {
    case QJsonValue::String: return 40 + size_t(v.toString().size()) * 3;
    case QJsonValue::Array: { size_t c = 64; for (const auto &x : v.toArray()) c += 16 + jsonCost(x, depth + 1); return c; }
    case QJsonValue::Object: { size_t c = 64; const auto o = v.toObject(); for (auto it = o.begin(); it != o.end(); ++it) c += 96 + size_t(it.key().size()) * 3 + jsonCost(it.value(), depth + 1); return c; }
    default: return 16;
    }
}
// Checked pushJson for use inside guarded().
bool pushJsonChecked(lua_State *L, const QJsonValue &v) {
    if (!hostFits(L, jsonCost(v))) return false;
    pushJson(L, v);
    return true;
}

// ---- registration ----
int addCb(lua_State *L, PluginState *p, const QString &key, int fnIdx) {
    lua_pushvalue(L, fnIdx);
    const int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    if (const auto it = p->cb.find(key); it != p->cb.end()) luaL_unref(L, LUA_REGISTRYINDEX, *it);
    p->cb[key] = ref;
    p->regsDirty = true;
    return ref;
}
bool checkRunField(lua_State *L, QString &err) {
    getRaw(L, 1, "run");
    const bool ok = lua_isfunction(L, -1);
    lua_pop(L, 1);
    if (!ok) err = QStringLiteral("field 'run' must be a function");
    return ok;
}
bool regHeader(lua_State *L, QString &id, QString &title, QString &err) {
    if (!lua_istable(L, 1)) { err = QStringLiteral("a table argument is required"); return false; }
    if (!fieldStr(L, 1, "id", id, 64, true, err) || !fieldStr(L, 1, "title", title, 120, true, err)) return false;
    if (!validPluginId(id)) { err = QStringLiteral("invalid id '%1'").arg(id); return false; }
    return true;
}

int l_command(lua_State *L) {
    return guarded(L, [&](QString &err) -> int {
        PluginState *p = P(L);
        QString id, title, key;
        if (!regHeader(L, id, title, err) || !fieldStr(L, 1, "key", key, 32, false, err) || !checkRunField(L, err)) return -1;
        if (p->cb.contains("cmd:" + id) || p->regs.commands.size() >= kMaxRegs) { err = QStringLiteral("duplicate command id or too many commands"); return -1; }
        getRaw(L, 1, "run");
        addCb(L, p, "cmd:" + id, -1);
        lua_pop(L, 1);
        p->regs.commands.append({p->m.id, id, title, key});
        return 0;
    });
}
int l_toolbar(lua_State *L) {
    return guarded(L, [&](QString &err) -> int {
        PluginState *p = P(L);
        QString id, title, icon;
        if (!regHeader(L, id, title, err) || !fieldStr(L, 1, "icon", icon, 64, false, err) || !checkRunField(L, err)) return -1;
        if (p->cb.contains("tb:" + id) || p->regs.toolbars.size() >= kMaxRegs) { err = QStringLiteral("duplicate toolbar button id or too many buttons"); return -1; }
        getRaw(L, 1, "run");
        addCb(L, p, "tb:" + id, -1);
        lua_pop(L, 1);
        p->regs.toolbars.append({p->m.id, id, title, icon});
        return 0;
    });
}
int l_menu(lua_State *L) {
    return guarded(L, [&](QString &err) -> int {
        PluginState *p = P(L);
        QString id, title, where = QStringLiteral("tools");
        if (!regHeader(L, id, title, err) || !fieldStr(L, 1, "where", where, 16, false, err) || !checkRunField(L, err)) return -1;
        if (where != QLatin1String("note") && where != QLatin1String("tools")) { err = QStringLiteral("where must be \"note\" or \"tools\""); return -1; }
        if (p->cb.contains("menu:" + id) || p->regs.menus.size() >= kMaxRegs) { err = QStringLiteral("duplicate menu item id or too many items"); return -1; }
        getRaw(L, 1, "run");
        addCb(L, p, "menu:" + id, -1);
        lua_pop(L, 1);
        p->regs.menus.append({p->m.id, id, title, where});
        return 0;
    });
}
const QStringList &eventNames() {
    static const QStringList e{"app.started", "note.opened", "note.changed", "note.saved", "note.closed", "selection.changed", "note.pre_save"};
    return e;
}
void markLinkHandler(PluginState *p, bool pattern);
int l_on(lua_State *L) {
    return guarded(L, [&](QString &err) -> int {
        PluginState *p = P(L);
        QString event;
        if (!argStr(L, 1, event, 64, err) || !lua_isfunction(L, 2)) { if (err.isEmpty()) err = QStringLiteral("bad argument #2 (function expected)"); return -1; }
        const bool linkEvent = event == QLatin1String("link.activate") && p->m.api >= 2;
        if (!eventNames().contains(event) && !linkEvent) { err = QStringLiteral("unknown event '%1'").arg(event); return -1; }
        if (linkEvent) NEED("editor.links");
        if (p->ev[event].size() >= kMaxHandlers) { err = QStringLiteral("too many handlers for %1").arg(event); return -1; }
        lua_pushvalue(L, 2);
        p->ev[event].append(luaL_ref(L, LUA_REGISTRYINDEX));
        if (!p->regs.events.contains(event)) p->regs.events << event;
        if (linkEvent) markLinkHandler(p, false);
        p->regsDirty = true;
        return 0;
    });
}
int l_trigger(lua_State *L) {
    return guarded(L, [&](QString &err) -> int {
        PluginState *p = P(L);
        QString pat;
        if (!lua_istable(L, 1)) { err = QStringLiteral("a table argument is required"); return -1; }
        if (!fieldStr(L, 1, "pattern", pat, 64, true, err)) return -1;
        getRaw(L, 1, "replace");
        const bool isFn = lua_isfunction(L, -1);
        lua_pop(L, 1);
        if (!isFn) { err = QStringLiteral("field 'replace' must be a function"); return -1; }
        if (pat.size() < 2 || p->cb.contains("trig:" + pat) || p->regs.triggers.size() >= kMaxRegs) { err = QStringLiteral("trigger pattern must be 2-64 characters and unique"); return -1; }
        getRaw(L, 1, "replace");
        addCb(L, p, "trig:" + pat, -1);
        lua_pop(L, 1);
        p->regs.triggers.append({p->m.id, pat});
        return 0;
    });
}
int l_setting(lua_State *L) {
    return guarded(L, [&](QString &err) -> int {
        PluginState *p = P(L);
        QString id, type, title;
        if (!lua_istable(L, 1)) { err = QStringLiteral("a table argument is required"); return -1; }
        if (!fieldStr(L, 1, "id", id, 64, true, err) || !fieldStr(L, 1, "type", type, 16, true, err) || !fieldStr(L, 1, "title", title, 120, true, err)) return -1;
        if (!validPluginId(id)) { err = QStringLiteral("invalid setting id"); return -1; }
        getRaw(L, 1, "default");
        QVariant def;
        const int t = lua_type(L, -1);
        if (type == QLatin1String("bool") && t == LUA_TBOOLEAN) def = bool(lua_toboolean(L, -1));
        else if (type == QLatin1String("number") && t == LUA_TNUMBER) def = lua_tonumber(L, -1);
        else if (type == QLatin1String("string") && t == LUA_TSTRING) { size_t n; const char *s = lua_tolstring(L, -1, &n); def = QString::fromUtf8(s, qsizetype(n)); }
        else err = QStringLiteral("type must be \"bool\", \"string\" or \"number\" and 'default' must match it");
        lua_pop(L, 1);
        if (!err.isEmpty()) return -1;
        for (const auto &s : p->regs.settings) if (s.id == id) { err = QStringLiteral("duplicate setting id"); return -1; }
        if (p->regs.settings.size() >= kMaxRegs) { err = QStringLiteral("too many settings"); return -1; }
        p->regs.settings.append({p->m.id, id, type, title, def});
        p->regsDirty = true;
        return 0;
    });
}

// ---- hn.note ----
int n_text(lua_State *L) { return guarded(L, [&](QString &err) -> int { NEED("note.read"); NoteBridge *n; if (!needNote(L, &n, err)) return -1; PUSHQ(n->text()); return 1; }); }
int n_sel(lua_State *L) { return guarded(L, [&](QString &err) -> int { NEED("note.read"); NoteBridge *n; if (!needNote(L, &n, err)) return -1; PUSHQ(n->selection()); return 1; }); }
int n_path(lua_State *L) { return guarded(L, [&](QString &err) -> int { NEED("note.read"); NoteBridge *n; if (!needNote(L, &n, err)) return -1; PUSHQ(n->path()); return 1; }); }
int n_title(lua_State *L) { return guarded(L, [&](QString &err) -> int { NEED("note.read"); NoteBridge *n; if (!needNote(L, &n, err)) return -1; PUSHQ(n->title()); return 1; }); }
int n_tags(lua_State *L) {
    return guarded(L, [&](QString &err) -> int {
        NEED("note.read");
        NoteBridge *n;
        if (!needNote(L, &n, err)) return -1;
        const auto tags = n->tags();
        size_t cost = 64;
        for (const auto &t : tags) cost += 64 + size_t(t.size()) * 3;
        if (!hostFits(L, cost)) { err = QString::fromLatin1(kTooBig); return -1; }
        lua_createtable(L, int(tags.size()), 0);
        for (int i = 0; i < tags.size(); ++i) { pushQRaw(L, tags[i]); lua_rawseti(L, -2, i + 1); }
        return 1;
    });
}
template <class F> int noteEdit(lua_State *L, F apply) {
    return guarded(L, [&](QString &err) -> int {
        NEED("note.edit");
        NoteBridge *n;
        QString s;
        if (!needNote(L, &n, err) || !argStr(L, 1, s, kMaxStr, err)) return -1;
        PluginState *p = P(L);
        if (!p->inTx) { n->beginTransaction(QStringLiteral("plugin: ") + p->m.id); p->inTx = true; }
        apply(n, s);
        return 0;
    });
}
int n_replace(lua_State *L) { return noteEdit(L, [](NoteBridge *n, const QString &s) { n->replaceSelection(s); }); }
int n_insert(lua_State *L) { return noteEdit(L, [](NoteBridge *n, const QString &s) { n->insert(s); }); }
int n_settext(lua_State *L) { return noteEdit(L, [](NoteBridge *n, const QString &s) { n->setText(s); }); }

// ---- hn.notes ----
bool needLib(lua_State *L, LibraryBridge **lb, QString &err) {
    *lb = P(L)->d->env.bridges.library;
    if (!*lb) err = QStringLiteral("note library is not available");
    return *lb;
}
int ns_list(lua_State *L) {
    return guarded(L, [&](QString &err) -> int {
        NEED("notes.read");
        LibraryBridge *lb;
        QString q;
        if (!needLib(L, &lb, err)) return -1;
        if (!lua_isnoneornil(L, 1) && !argStr(L, 1, q, 256, err)) return -1;
        const auto items = lb->list(q);
        const int n = int(qMin<qsizetype>(items.size(), 1000));
        size_t cost = 64;
        for (int i = 0; i < n; ++i) cost += 300 + size_t(items[i].path.size() + items[i].title.size()) * 3;
        if (!hostFits(L, cost)) { err = QString::fromLatin1(kTooBig); return -1; }
        lua_createtable(L, n, 0);
        for (int i = 0; i < n; ++i) {
            lua_createtable(L, 0, 2);
            pushQRaw(L, items[i].path); lua_setfield(L, -2, "path");
            pushQRaw(L, items[i].title); lua_setfield(L, -2, "title");
            lua_rawseti(L, -2, i + 1);
        }
        return 1;
    });
}
int ns_read(lua_State *L) {
    return guarded(L, [&](QString &err) -> int {
        NEED("notes.read");
        LibraryBridge *lb;
        QString path, text;
        if (!needLib(L, &lb, err) || !argStr(L, 1, path, 512, err)) return -1;
        if (!validNotePath(path)) { err = QStringLiteral("invalid note path"); return -1; }
        if (!lb->read(path, &text)) { lua_pushnil(L); lua_pushliteral(L, "not found"); return 2; }
        if (size_t(text.size()) * 3 > kMaxStr * 4) { err = QStringLiteral("note is too large to hand to a plugin"); return -1; }
        PUSHQ(text);
        return 1;
    });
}
bool writeBudget(lua_State *L, QString &err) {
    if (++P(L)->noteWrites > kMaxNoteWrites) { err = QStringLiteral("too many note writes in one callback (max %1)").arg(kMaxNoteWrites); return false; }
    return true;
}
int ns_create(lua_State *L) {
    return guarded(L, [&](QString &err) -> int {
        NEED("notes.write");
        LibraryBridge *lb;
        QString title, text;
        if (!needLib(L, &lb, err) || !argStr(L, 1, title, 256, err) || !argStr(L, 2, text, kMaxStr, err) || !writeBudget(L, err)) return -1;
        const QString path = lb->create(title, text);
        if (path.isEmpty()) { lua_pushnil(L); lua_pushliteral(L, "create failed"); return 2; }
        auditLog(L, "notes.create", path);
        PUSHQ(path);
        return 1;
    });
}
int ns_write(lua_State *L) {
    return guarded(L, [&](QString &err) -> int {
        NEED("notes.write");
        LibraryBridge *lb;
        QString path, text;
        if (!needLib(L, &lb, err) || !argStr(L, 1, path, 512, err) || !argStr(L, 2, text, kMaxStr, err) || !writeBudget(L, err)) return -1;
        if (!validNotePath(path)) { err = QStringLiteral("invalid note path"); return -1; }
        const bool ok = lb->write(path, text);
        auditLog(L, "notes.write", path);
        lua_pushboolean(L, ok);
        return 1;
    });
}
int ns_delete(lua_State *L) {
    return guarded(L, [&](QString &err) -> int {
        NEED("notes.write");
        LibraryBridge *lb;
        QString path;
        if (!needLib(L, &lb, err) || !argStr(L, 1, path, 512, err) || !writeBudget(L, err)) return -1;
        if (!validNotePath(path)) { err = QStringLiteral("invalid note path"); return -1; }
        const bool ok = lb->remove(path);
        auditLog(L, "notes.delete", path);
        lua_pushboolean(L, ok);
        return 1;
    });
}

// ================= Plugin API v2 (api >= 2 plugins only; the functions do not exist for api 1) =================
constexpr int kMaxIdxRows = 500, kMaxIdxStr = 64 * 1024, kMaxIdxDepth = 6, kMaxIdxNodes = 50000;
constexpr int kMaxOpensPerCallback = 5, kMaxPanels = 16, kMaxCompletes = 16;
constexpr int kMaxBlocks = 200, kMaxCompleteItems = 50;
constexpr qint64 kMaxPanelBytes = 256 * 1024;

// UTF-8 aware cut to at most `max` bytes (never splits a character).
QString capUtf8(const QString &s, int max) {
    if (s.size() * 3 <= max) return s;
    QByteArray b = s.toUtf8();
    if (b.size() <= max) return s;
    int cut = max;
    while (cut > 0 && (uchar(b[cut]) & 0xC0) == 0x80) --cut;
    return QString::fromUtf8(b.constData(), cut);
}
QJsonValue capJson(const QJsonValue &v, int depth, int &nodes) {  // result of a bridge call: bound strings, depth and element count
    if (++nodes > kMaxIdxNodes) return QJsonValue(QJsonValue::Null);
    switch (v.type()) {
    case QJsonValue::String: return capUtf8(v.toString(), kMaxIdxStr);
    case QJsonValue::Array: {
        if (depth >= kMaxIdxDepth) return QJsonValue(QJsonValue::Null);
        QJsonArray a;
        for (const auto &x : v.toArray()) { if (nodes > kMaxIdxNodes) break; a.append(capJson(x, depth + 1, nodes)); }
        return a;
    }
    case QJsonValue::Object: {
        if (depth >= kMaxIdxDepth) return QJsonValue(QJsonValue::Null);
        QJsonObject o;
        const auto src = v.toObject();
        for (auto it = src.begin(); it != src.end(); ++it) { if (nodes > kMaxIdxNodes) break; o.insert(capUtf8(it.key(), 256), capJson(it.value(), depth + 1, nodes)); }
        return o;
    }
    case QJsonValue::Double: { const double x = v.toDouble(); return (x != x || x - x != 0) ? QJsonValue(QJsonValue::Null) : v; }
    default: return v;
    }
}

bool indexRateOk(lua_State *L) {
    PluginState *p = P(L);
    if (p->index.allow(p->d->env.indexPerSecond, 1'000'000)) return true;
    auditLog(L, "denied", QStringLiteral("index rate limit (%1/s)").arg(p->d->env.indexPerSecond));
    return false;
}
// Runs one bridge call under the index budgets. The time spent is host time: it is excluded from the callback deadline
// (like ui/http) but bounded per call and per callback, so a plugin cannot turn it into a stall.
template <class F> BridgeStatus timedIndex(lua_State *L, F &&call) {
    PluginState *p = P(L);
    const auto &env = p->d->env;
    if (p->indexWallUs >= qint64(env.indexCallbackMs) * 1000) {
        auditLog(L, "denied", QStringLiteral("index time budget for one callback (%1 ms)").arg(env.indexCallbackMs));
        return BridgeStatus::Timeout;
    }
    const qint64 t0 = nowUs();
    BridgeStatus s = call();
    const qint64 dt = nowUs() - t0;
    p->deadlineUs += dt;
    p->indexWallUs += dt;
    if (s == BridgeStatus::Ok && dt > qint64(env.indexCallMs) * 1000) s = BridgeStatus::Timeout;  // too late: discard the result
    if (s == BridgeStatus::Timeout) auditLog(L, "denied", QStringLiteral("index call exceeded %1 ms").arg(env.indexCallMs));
    return s;
}
// nil, "<reason>" for the non-Ok outcomes (soft failures: the plugin can handle them without raising)
int statusResult(lua_State *L, BridgeStatus s, const QString &what = QString()) {
    const PluginState *p = P(L);
    QString m;
    switch (s) {
    case BridgeStatus::Timeout: m = QStringLiteral("timeout: index call exceeded %1 ms").arg(p->d->env.indexCallMs); break;
    case BridgeStatus::Unsupported: m = QStringLiteral("unsupported: this host does not provide %1").arg(what.isEmpty() ? QStringLiteral("the call") : what); break;
    case BridgeStatus::Invalid: m = QStringLiteral("invalid request"); break;
    case BridgeStatus::NotFound: m = QStringLiteral("not found"); break;
    case BridgeStatus::Ok: m = QStringLiteral("error"); break;
    }
    lua_pushnil(L);
    pushQRaw(L, m);
    return 2;
}
int rateLimited(lua_State *L) { lua_pushnil(L); lua_pushliteral(L, "rate limit exceeded"); return 2; }

bool optTable(lua_State *L, int i, QString &err) {
    if (lua_isnoneornil(L, i) || lua_istable(L, i)) return true;
    err = QStringLiteral("bad argument #%1 (table expected)").arg(i);
    return false;
}
// Optional integer option in [lo, hi]; absent keeps `out`.
bool optInt(lua_State *L, int t, const char *k, int lo, int hi, int &out, QString &err) {
    if (!lua_istable(L, t)) return true;
    getRaw(L, t, k);
    bool ok = true;
    if (!lua_isnil(L, -1)) {
        int isint = 0;
        const lua_Integer v = lua_tointegerx(L, -1, &isint);
        if (lua_type(L, -1) != LUA_TNUMBER || !isint || v < lo || v > hi) { err = QStringLiteral("option '%1' must be an integer between %2 and %3").arg(QLatin1String(k)).arg(lo).arg(hi); ok = false; }
        else out = int(v);
    }
    lua_pop(L, 1);
    return ok;
}
bool validNewName(const QString &s) {
    if (s.isEmpty() || s.toUtf8().size() > 200 || s == QLatin1String(".") || s == QLatin1String("..") || s.startsWith(QLatin1Char('.'))) return false;
    for (QChar c : s) if (c.unicode() < 0x20 || c.unicode() == 0x7f || c == QLatin1Char('/') || c == QLatin1Char('\\')) return false;
    return true;
}

bool pushLinkRows(lua_State *L, const QList<LinkRow> &rows, QString &err) {
    const int n = int(qMin<qsizetype>(rows.size(), kMaxIdxRows));
    size_t cost = 64;
    QList<LinkRow> r;
    for (int i = 0; i < n; ++i) {
        LinkRow x = rows[i];
        for (QString *f : {&x.kind, &x.target, &x.alias, &x.anchor, &x.resolved, &x.src, &x.context}) *f = capUtf8(*f, kMaxIdxStr);
        cost += 600 + size_t(x.kind.size() + x.target.size() + x.alias.size() + x.anchor.size() + x.resolved.size() + x.src.size() + x.context.size()) * 3;
        r << x;
    }
    if (!hostFits(L, cost)) { err = QString::fromLatin1(kTooBig); return false; }
    lua_createtable(L, n, 0);
    for (int i = 0; i < n; ++i) {
        const LinkRow &x = r[i];
        lua_createtable(L, 0, 8);
        auto put = [&](const char *k, const QString &v, bool always) { if (!always && v.isEmpty()) return; pushQRaw(L, v); lua_setfield(L, -2, k); };
        put("kind", x.kind.isEmpty() ? QStringLiteral("link") : x.kind, true);
        put("target", x.target, true);
        put("alias", x.alias, false);
        put("anchor", x.anchor, false);
        put("resolved", x.resolved, false);
        put("src", x.src, false);
        put("context", x.context, false);
        lua_pushinteger(L, x.line); lua_setfield(L, -2, "line");
        lua_rawseti(L, -2, i + 1);
    }
    return true;
}

#define NEED_LIB_PATH(var) \
    LibraryBridge *lb; QString var; \
    if (!needLib(L, &lb, err) || !argStr(L, 1, var, 512, err)) return -1; \
    if (!validNotePath(var)) { err = QStringLiteral("invalid note path"); return -1; }

int ns_links(lua_State *L) {
    return guarded(L, [&](QString &err) -> int {
        NEED("notes.index");
        NEED_LIB_PATH(path)
        if (!indexRateOk(L)) return rateLimited(L);
        QList<LinkRow> rows;
        const auto s = timedIndex(L, [&] { return lb->links(path, &rows); });
        if (s != BridgeStatus::Ok) return statusResult(L, s, QStringLiteral("links"));
        if (!pushLinkRows(L, rows, err)) return -1;
        return 1;
    });
}
int ns_backlinks(lua_State *L) {
    return guarded(L, [&](QString &err) -> int {
        NEED("notes.index");
        NEED_LIB_PATH(path)
        int limit = 50, offset = 0;
        if (!optTable(L, 2, err) || !optInt(L, 2, "limit", 1, kMaxIdxRows, limit, err) || !optInt(L, 2, "offset", 0, 100000, offset, err)) return -1;
        if (!indexRateOk(L)) return rateLimited(L);
        QList<LinkRow> rows;
        const auto s = timedIndex(L, [&] { return lb->backlinks(path, limit, offset, &rows); });
        if (s != BridgeStatus::Ok) return statusResult(L, s, QStringLiteral("backlinks"));
        if (rows.size() > limit) rows = rows.mid(0, limit);
        if (!pushLinkRows(L, rows, err)) return -1;
        return 1;
    });
}
int ns_resolve(lua_State *L) {
    return guarded(L, [&](QString &err) -> int {
        NEED("notes.index");
        LibraryBridge *lb;
        QString name;
        if (!needLib(L, &lb, err) || !argStr(L, 1, name, 512, err)) return -1;
        if (name.isEmpty() || name.size() > 256) { err = QStringLiteral("bad argument #1 (1-256 characters expected)"); return -1; }
        for (QChar c : name) if (c.unicode() < 0x20 || c.unicode() == 0x7f) { err = QStringLiteral("bad argument #1 (control characters are not allowed)"); return -1; }
        if (!indexRateOk(L)) return rateLimited(L);
        ResolveResult rr;
        const auto s = timedIndex(L, [&] { return lb->resolve(name, &rr); });
        if (s != BridgeStatus::Ok) return statusResult(L, s, QStringLiteral("resolve"));
        const QStringList cand = rr.candidates.mid(0, 20);
        size_t cost = 400 + size_t(rr.status.size() + rr.path.size()) * 3;
        for (const auto &c : cand) cost += 64 + size_t(c.size()) * 3;
        if (!hostFits(L, cost)) { err = QString::fromLatin1(kTooBig); return -1; }
        lua_createtable(L, 0, 3);
        pushQRaw(L, rr.status.isEmpty() ? QStringLiteral("unresolved") : capUtf8(rr.status, 32)); lua_setfield(L, -2, "status");
        if (!rr.path.isEmpty()) { pushQRaw(L, capUtf8(rr.path, 4096)); lua_setfield(L, -2, "path"); }
        lua_createtable(L, int(cand.size()), 0);
        for (int i = 0; i < cand.size(); ++i) { pushQRaw(L, capUtf8(cand[i], 4096)); lua_rawseti(L, -2, i + 1); }
        lua_setfield(L, -2, "candidates");
        return 1;
    });
}
int ns_frontmatter(lua_State *L) {
    return guarded(L, [&](QString &err) -> int {
        NEED("notes.index");
        NEED_LIB_PATH(path)
        if (!indexRateOk(L)) return rateLimited(L);
        QJsonObject fm;
        const auto s = timedIndex(L, [&] { return lb->frontmatter(path, &fm); });
        if (s != BridgeStatus::Ok) return statusResult(L, s, QStringLiteral("frontmatter"));
        int nodes = 0;
        if (!pushJsonChecked(L, capJson(fm, 0, nodes))) { err = QString::fromLatin1(kTooBig); return -1; }
        return 1;
    });
}

// ---- query spec: shape/size validation. Only a rebuilt, normalised spec ever reaches the bridge; there is no SQL anywhere. ----
bool validFieldName(const QString &f) {
    static const QRegularExpression re(QStringLiteral("^[A-Za-z_][A-Za-z0-9_]{0,31}(\\.[A-Za-z0-9_-]{1,32})?$"));
    return re.match(f).hasMatch();
}
bool scalarOk(const QJsonValue &v, int maxStr) { return v.isBool() || v.isDouble() || (v.isString() && v.toString().toUtf8().size() <= maxStr); }
bool validateQuery(const QJsonValue &in, QJsonObject *norm, QString &err) {
    auto bad = [&](const QString &m) { err = QStringLiteral("invalid query: ") + m; return false; };
    if (!in.isObject()) return bad(QStringLiteral("a table with keys from/where/order/select/limit/offset is required"));
    const QJsonObject o = in.toObject();
    static const QSet<QString> keys{"from", "where", "order", "select", "limit", "offset"};
    for (auto it = o.begin(); it != o.end(); ++it) if (!keys.contains(it.key())) return bad(QStringLiteral("unknown key '%1'").arg(it.key().left(40)));
    if (QJsonDocument(o).toJson(QJsonDocument::Compact).size() > 8192) return bad(QStringLiteral("spec is larger than 8 KiB"));
    QJsonObject n;
    const QString from = o.value("from").toString();
    if (!o.value("from").isString() || (from != QLatin1String("notes") && from != QLatin1String("tasks") && from != QLatin1String("links"))) return bad(QStringLiteral("'from' must be \"notes\", \"tasks\" or \"links\""));
    n["from"] = from;
    auto listOf = [&](const char *k, int max, QJsonArray &out) -> bool {
        const auto v = o.value(QLatin1String(k));
        if (v.isUndefined()) return true;
        if (!v.isArray() || v.toArray().size() > max) { err = QStringLiteral("invalid query: '%1' must be a list of at most %2 entries").arg(QLatin1String(k)).arg(max); return false; }
        out = v.toArray();
        return true;
    };
    QJsonArray where, order, select;
    if (!listOf("where", 16, where) || !listOf("order", 4, order) || !listOf("select", 16, select)) return false;
    static const QSet<QString> ops{"=", "!=", "<", ">", "<=", ">=", "like", "in", "contains"};
    QJsonArray w2;
    for (const auto &cv : where) {
        if (!cv.isObject()) return bad(QStringLiteral("each 'where' entry must be a table {field, op, value}"));
        const auto c = cv.toObject();
        for (auto it = c.begin(); it != c.end(); ++it) if (it.key() != QLatin1String("field") && it.key() != QLatin1String("op") && it.key() != QLatin1String("value")) return bad(QStringLiteral("unknown key '%1' in a 'where' entry").arg(it.key().left(40)));
        const QString f = c.value("field").toString(), op = c.value("op").toString();
        if (!c.value("field").isString() || !validFieldName(f)) return bad(QStringLiteral("bad field name"));
        if (!c.value("op").isString() || !ops.contains(op)) return bad(QStringLiteral("bad operator"));
        const auto val = c.value("value");
        if (op == QLatin1String("in")) {
            if (!val.isArray() || val.toArray().isEmpty() || val.toArray().size() > 64) return bad(QStringLiteral("'in' needs a list of 1-64 values"));
            for (const auto &x : val.toArray()) if (!scalarOk(x, 256)) return bad(QStringLiteral("'in' values must be short strings, numbers or booleans"));
        } else if (!scalarOk(val, op == QLatin1String("like") || op == QLatin1String("contains") ? 256 : 1024)) return bad(QStringLiteral("'value' must be a string, number or boolean of reasonable size"));
        w2.append(QJsonObject{{"field", f}, {"op", op}, {"value", val}});
    }
    QJsonArray o2;
    for (const auto &cv : order) {
        if (!cv.isObject()) return bad(QStringLiteral("each 'order' entry must be a table {field, dir}"));
        const auto c = cv.toObject();
        for (auto it = c.begin(); it != c.end(); ++it) if (it.key() != QLatin1String("field") && it.key() != QLatin1String("dir")) return bad(QStringLiteral("unknown key '%1' in an 'order' entry").arg(it.key().left(40)));
        const QString f = c.value("field").toString(), dir = c.contains("dir") ? c.value("dir").toString() : QStringLiteral("asc");
        if (!c.value("field").isString() || !validFieldName(f)) return bad(QStringLiteral("bad field name in 'order'"));
        if (dir != QLatin1String("asc") && dir != QLatin1String("desc")) return bad(QStringLiteral("'dir' must be \"asc\" or \"desc\""));
        o2.append(QJsonObject{{"field", f}, {"dir", dir}});
    }
    QJsonArray s2;
    for (const auto &sv : select) {
        if (!sv.isString() || !validFieldName(sv.toString())) return bad(QStringLiteral("bad field name in 'select'"));
        s2.append(sv.toString());
    }
    auto num = [&](const char *k, double lo, double hi, double def, bool clamp, double *out) {
        const auto v = o.value(QLatin1String(k));
        if (v.isUndefined()) { *out = def; return true; }
        const double d = v.toDouble();
        if (!v.isDouble() || d != double(qint64(d)) || d < lo || (!clamp && d > hi)) return false;
        *out = qMin(d, hi);
        return true;
    };
    double limit, offset;
    if (!num("limit", 1, kMaxIdxRows, 100, true, &limit)) return bad(QStringLiteral("'limit' must be an integer >= 1 (values above 500 are capped to 500)"));
    if (!num("offset", 0, 100000, 0, false, &offset)) return bad(QStringLiteral("'offset' must be an integer between 0 and 100000"));
    n["where"] = w2; n["order"] = o2; n["select"] = s2;
    n["limit"] = int(limit); n["offset"] = int(offset);
    *norm = n;
    return true;
}
int ns_query(lua_State *L) {
    return guarded(L, [&](QString &err) -> int {
        NEED("notes.index");
        LibraryBridge *lb;
        if (!needLib(L, &lb, err)) return -1;
        if (!lua_istable(L, 1)) { err = QStringLiteral("bad argument #1 (table expected)"); return -1; }
        QJsonValue spec;
        int nodes = 200000 - 1500;  // toJson allows 200 000 nodes; a query spec gets 1500
        if (!toJson(L, 1, 0, nodes, spec, err)) { err = QStringLiteral("invalid query: ") + err; auditLog(L, "denied", QStringLiteral("malformed query spec")); return -1; }
        QJsonObject norm;
        if (!validateQuery(spec, &norm, err)) { auditLog(L, "denied", QStringLiteral("rejected query spec")); return -1; }
        if (!indexRateOk(L)) return rateLimited(L);
        QJsonArray rows;
        const auto s = timedIndex(L, [&] { return lb->query(norm, &rows); });
        if (s != BridgeStatus::Ok) return statusResult(L, s, QStringLiteral("query"));
        QJsonArray capped;
        int n2 = 0;
        for (const auto &r : rows) { if (capped.size() >= kMaxIdxRows) break; capped.append(capJson(r, 0, n2)); }
        if (!pushJsonChecked(L, capped)) { err = QString::fromLatin1(kTooBig); return -1; }
        return 1;
    });
}
int ns_open(lua_State *L) {
    return guarded(L, [&](QString &err) -> int {
        NEED("notes.read");
        NEED_LIB_PATH(path)
        QString where = QStringLiteral("organizer");
        if (!optTable(L, 2, err)) return -1;
        if (lua_istable(L, 2) && !fieldStr(L, 2, "where", where, 16, false, err)) return -1;
        if (where != QLatin1String("organizer") && where != QLatin1String("sticky")) { err = QStringLiteral("where must be \"organizer\" or \"sticky\""); return -1; }
        PluginState *p = P(L);
        if (++p->opens > kMaxOpensPerCallback) { err = QStringLiteral("too many notes opened in one callback (max %1)").arg(kMaxOpensPerCallback); return -1; }
        const auto s = lb->open(path, where);
        if (s != BridgeStatus::Ok) return statusResult(L, s, QStringLiteral("open"));
        lua_pushboolean(L, 1);
        return 1;
    });
}
int ns_rename(lua_State *L) {
    return guarded(L, [&](QString &err) -> int {
        NEED("notes.write");
        NEED_LIB_PATH(path)
        QString name;
        if (!argStr(L, 2, name, 512, err)) return -1;
        if (!validNewName(name)) { err = QStringLiteral("invalid new name (a plain file name: no '/', no leading '.', at most 200 bytes)"); return -1; }
        bool update = false;
        if (!optTable(L, 3, err)) return -1;
        if (lua_istable(L, 3)) {
            getRaw(L, 3, "update_links");
            if (!lua_isnil(L, -1) && lua_type(L, -1) != LUA_TBOOLEAN) { lua_pop(L, 1); err = QStringLiteral("option 'update_links' must be a boolean"); return -1; }
            update = lua_toboolean(L, -1);
            lua_pop(L, 1);
        }
        if (!writeBudget(L, err)) return -1;
        QString np;
        const auto s = lb->rename(path, name, update, &np);
        auditLog(L, "notes.rename", QStringLiteral("%1 -> %2%3").arg(path, name, update ? QStringLiteral(" (update links)") : QString()));
        if (s != BridgeStatus::Ok) return statusResult(L, s, QStringLiteral("rename"));
        PUSHQ(capUtf8(np, 4096));
        return 1;
    });
}

// ---- panels ----
bool fieldFn(lua_State *L, int t, const char *k, bool required, bool *present, QString &err) {
    getRaw(L, t, k);
    const bool isFn = lua_isfunction(L, -1), none = lua_isnil(L, -1);
    lua_pop(L, 1);
    if (present) *present = isFn;
    if (isFn || (none && !required)) return true;
    err = QStringLiteral("field '%1' must be a function").arg(QLatin1String(k));
    return false;
}
int l_panel(lua_State *L) {
    return guarded(L, [&](QString &err) -> int {
        NEED("ui.panel");
        PluginState *p = P(L);
        QString id, title, icon;
        bool hasOnEvent = false;
        if (!regHeader(L, id, title, err) || !fieldStr(L, 1, "icon", icon, 64, false, err) || !fieldFn(L, 1, "render", true, nullptr, err) || !fieldFn(L, 1, "on_event", false, &hasOnEvent, err)) return -1;
        QStringList on;
        getRaw(L, 1, "refresh_on");
        if (lua_istable(L, -1)) {
            static const QStringList allowed{"note.opened", "note.changed", "note.saved", "note.closed", "selection.changed"};
            const lua_Unsigned n = lua_rawlen(L, -1);
            for (lua_Unsigned i = 1; i <= n && i <= 8; ++i) {
                lua_rawgeti(L, -1, lua_Integer(i));
                QString e;
                const bool ok = lua_type(L, -1) == LUA_TSTRING && argStr(L, -1, e, 64, err) && allowed.contains(e);
                lua_pop(L, 1);
                if (!ok) { lua_pop(L, 1); err = QStringLiteral("refresh_on may only list: %1").arg(allowed.join(QStringLiteral(", "))); return -1; }
                if (!on.contains(e)) on << e;
            }
        } else if (!lua_isnil(L, -1)) { lua_pop(L, 1); err = QStringLiteral("field 'refresh_on' must be a list of event names"); return -1; }
        lua_pop(L, 1);
        if (p->cb.contains("panel:" + id) || p->regs.panels.size() >= kMaxPanels) { err = QStringLiteral("duplicate panel id or too many panels"); return -1; }
        getRaw(L, 1, "render");
        addCb(L, p, "panel:" + id, -1);
        lua_pop(L, 1);
        if (hasOnEvent) { getRaw(L, 1, "on_event"); addCb(L, p, "pev:" + id, -1); lua_pop(L, 1); }
        p->regs.panels.append({p->m.id, id, title, icon, on, hasOnEvent});
        return 0;
    });
}
int l_panel_refresh(lua_State *L) {
    return guarded(L, [&](QString &err) -> int {
        NEED("ui.panel");
        PluginState *p = P(L);
        QString id;
        if (!argStr(L, 1, id, 64, err)) return -1;
        if (!p->cb.contains("panel:" + id)) { err = QStringLiteral("unknown panel '%1'").arg(id); return -1; }
        if (p->renderingPanel == id) { lua_pushboolean(L, 0); return 1; }  // a render cannot ask for itself
        if (!p->panelRefresh.allow(10, 1'000'000)) { auditLog(L, "denied", QStringLiteral("panel refresh rate limit (10/s)")); lua_pushboolean(L, 0); return 1; }
        emit p->d->q->panelRefreshRequested(p->m.id, id, p->note);
        lua_pushboolean(L, 1);
        return 1;
    });
}

// ---- completion / link handlers ----
int l_complete(lua_State *L) {
    return guarded(L, [&](QString &err) -> int {
        NEED("editor.complete");
        PluginState *p = P(L);
        QString id, trigger;
        if (!lua_istable(L, 1)) { err = QStringLiteral("a table argument is required"); return -1; }
        if (!fieldStr(L, 1, "id", id, 64, true, err) || !fieldStr(L, 1, "trigger", trigger, 32, true, err) || !fieldFn(L, 1, "items", true, nullptr, err)) return -1;
        if (!validPluginId(id)) { err = QStringLiteral("invalid id '%1'").arg(id); return -1; }
        bool okTrig = !trigger.isEmpty() && trigger.toUtf8().size() <= 8;
        for (QChar c : trigger) okTrig = okTrig && c.unicode() > 0x20 && c.unicode() != 0x7f;
        if (!okTrig) { err = QStringLiteral("trigger must be 1-8 visible characters without spaces, for example \"[[\" or \"/\""); return -1; }
        if (p->cb.contains("cmp:" + id) || p->regs.completions.size() >= kMaxCompletes) { err = QStringLiteral("duplicate completion id or too many completions"); return -1; }
        getRaw(L, 1, "items");
        addCb(L, p, "cmp:" + id, -1);
        lua_pop(L, 1);
        p->regs.completions.append({p->m.id, id, trigger});
        return 0;
    });
}
void markLinkHandler(PluginState *p, bool pattern) {
    p->regsDirty = true;
    for (auto &h : p->regs.linkHandlers) { h.hasPattern |= pattern; return; }
    p->regs.linkHandlers.append({p->m.id, pattern});
}
int l_link_handler(lua_State *L) {
    return guarded(L, [&](QString &err) -> int {
        NEED("editor.links");
        PluginState *p = P(L);
        bool hasPattern = false;
        if (!lua_istable(L, 1)) { err = QStringLiteral("a table argument is required"); return -1; }
        if (!fieldFn(L, 1, "pattern", false, &hasPattern, err)) return -1;
        if (p->cb.contains(QStringLiteral("linkpat"))) { err = QStringLiteral("hn.link_handler was already called"); return -1; }
        if (hasPattern) { getRaw(L, 1, "pattern"); addCb(L, p, QStringLiteral("linkpat"), -1); lua_pop(L, 1); }
        markLinkHandler(p, hasPattern);
        return 0;
    });
}


// ---- hn.ui ----
bool needUi(lua_State *L, UiBridge **ui, QString &err) {
    *ui = P(L)->d->env.bridges.ui;
    if (!*ui) err = QStringLiteral("UI is not available");
    return *ui;
}
int ui_notify(lua_State *L) {
    return guarded(L, [&](QString &err) -> int {
        NEED("ui");
        UiBridge *ui;
        QString msg;
        if (!needUi(L, &ui, err) || !argStr(L, 1, msg, 4096, err)) return -1;
        PluginState *p = P(L);
        if (!p->notify.allow(kNotifyPer10s, 10'000'000)) { lua_pushboolean(L, 0); return 1; }  // rate limited: dropped
        ui->notify(p->m.id, msg.left(500));
        lua_pushboolean(L, 1);
        return 1;
    });
}
int ui_prompt(lua_State *L) {
    return guarded(L, [&](QString &err) -> int {
        NEED("ui");
        UiBridge *ui;
        QString title, label, def;
        if (!needUi(L, &ui, err) || !argStr(L, 1, title, 200, err) || !argStr(L, 2, label, 400, err)) return -1;
        if (!lua_isnoneornil(L, 3) && !argStr(L, 3, def, 4096, err)) return -1;
        PluginState *p = P(L);
        std::optional<QString> r;
        { PauseScope ps(p); r = ui->prompt(p->m.id, title, label, def); }
        if (!r) lua_pushnil(L); else PUSHQ(r->left(int(kMaxStr / 4)));
        return 1;
    });
}
int ui_confirm(lua_State *L) {
    return guarded(L, [&](QString &err) -> int {
        NEED("ui");
        UiBridge *ui;
        QString msg;
        if (!needUi(L, &ui, err) || !argStr(L, 1, msg, 1000, err)) return -1;
        PluginState *p = P(L);
        bool r;
        { PauseScope ps(p); r = ui->confirm(p->m.id, msg); }
        lua_pushboolean(L, r);
        return 1;
    });
}
int ui_pick(lua_State *L) {
    return guarded(L, [&](QString &err) -> int {
        NEED("ui");
        UiBridge *ui;
        QString title;
        if (!needUi(L, &ui, err) || !argStr(L, 1, title, 200, err)) return -1;
        if (!lua_istable(L, 2)) { err = QStringLiteral("bad argument #2 (table of strings expected)"); return -1; }
        QStringList items;
        const lua_Unsigned n = lua_rawlen(L, 2);
        if (n > 200) { err = QStringLiteral("too many items (max 200)"); return -1; }
        for (lua_Unsigned i = 1; i <= n; ++i) {
            lua_rawgeti(L, 2, lua_Integer(i));
            QString s;
            const bool ok = argStr(L, -1, s, 400, err);
            lua_pop(L, 1);
            if (!ok) return -1;
            items << s;
        }
        PluginState *p = P(L);
        int idx;
        { PauseScope ps(p); idx = ui->pick(p->m.id, title, items); }
        if (idx < 0 || idx >= items.size()) lua_pushnil(L); else lua_pushinteger(L, idx + 1);
        return 1;
    });
}

// ---- storage / settings / json ----
int st_get(lua_State *L) {
    return guarded(L, [&](QString &err) -> int {
        NEED("storage");
        QString key;
        if (!argStr(L, 1, key, 128, err)) return -1;
        PluginState *p = P(L);
        p->loadStorage();
        if (!pushJsonChecked(L, p->kv.value(key))) { err = QString::fromLatin1(kTooBig); return -1; }
        return 1;
    });
}
int st_set(lua_State *L) {
    return guarded(L, [&](QString &err) -> int {
        NEED("storage");
        QString key;
        if (!argStr(L, 1, key, 128, err) || key.isEmpty()) { if (err.isEmpty()) err = QStringLiteral("empty key"); return -1; }
        PluginState *p = P(L);
        p->loadStorage();
        QJsonValue v;
        int nodes = 0;
        if (!toJson(L, 2, 0, nodes, v, err)) { err = QStringLiteral("cannot store value: ") + err; return -1; }
        QJsonObject next = p->kv;
        if (v.isNull()) next.remove(key); else next.insert(key, v);
        if (QJsonDocument(next).toJson(QJsonDocument::Compact).size() > kMaxStorageBytes) { err = QStringLiteral("storage quota exceeded (1 MiB per plugin)"); return -1; }
        p->kv = next;
        p->storageDirty = true;
        return 0;
    });
}
int se_get(lua_State *L) {
    return guarded(L, [&](QString &err) -> int {
        QString id;
        if (!argStr(L, 1, id, 64, err)) return -1;
        PluginState *p = P(L);
        p->loadStorage();
        if (p->settings.contains(id)) { if (!pushJsonChecked(L, p->settings.value(id))) { err = QString::fromLatin1(kTooBig); return -1; } }
        else {
            QVariant def;
            for (const auto &s : p->regs.settings) if (s.id == id) def = s.def;
            if (!pushJsonChecked(L, QJsonValue::fromVariant(def))) { err = QString::fromLatin1(kTooBig); return -1; }
        }
        return 1;
    });
}
int js_encode(lua_State *L) {
    return guarded(L, [&](QString &err) -> int {
        QJsonValue v;
        int nodes = 0;
        if (!toJson(L, 1, 0, nodes, v, err)) return -1;
        const QJsonDocument d = v.isArray() ? QJsonDocument(v.toArray()) : v.isObject() ? QJsonDocument(v.toObject()) : QJsonDocument(QJsonArray{v});
        QByteArray b = d.toJson(QJsonDocument::Compact);
        if (!v.isArray() && !v.isObject()) b = b.mid(1, b.size() - 2);  // strip the [] wrapper for scalars
        lua_pushlstring(L, b.constData(), size_t(b.size()));
        return 1;
    });
}
int js_decode(lua_State *L) {
    return guarded(L, [&](QString &err) -> int {
        QString s;
        if (!argStr(L, 1, s, kMaxStr, err)) return -1;
        QJsonParseError pe;
        const auto d = QJsonDocument::fromJson("[" + s.toUtf8() + "]", &pe);  // wrapper lets scalars parse
        if (pe.error != QJsonParseError::NoError || d.array().size() != 1) { lua_pushnil(L); lua_pushliteral(L, "invalid JSON"); return 2; }
        if (!pushJsonChecked(L, d.array().at(0))) { err = QString::fromLatin1(kTooBig); return -1; }
        return 1;
    });
}

// ---- clipboard / theme ----
int cb_get(lua_State *L) {
    return guarded(L, [&](QString &err) -> int {
        NEED("clipboard");
        auto *c = P(L)->d->env.bridges.clipboard;
        if (!c) { err = QStringLiteral("clipboard is not available"); return -1; }
        PUSHQ(c->get().left(int(kMaxStr / 4)));
        return 1;
    });
}
int cb_set(lua_State *L) {
    return guarded(L, [&](QString &err) -> int {
        NEED("clipboard");
        auto *c = P(L)->d->env.bridges.clipboard;
        QString s;
        if (!c) { err = QStringLiteral("clipboard is not available"); return -1; }
        if (!argStr(L, 1, s, kMaxStr, err)) return -1;
        c->set(s);
        return 0;
    });
}
int th_set(lua_State *L) {
    return guarded(L, [&](QString &err) -> int {
        NEED("theme");
        auto *t = P(L)->d->env.bridges.theme;
        QString tok, val;
        if (!t) { err = QStringLiteral("theme is not available"); return -1; }
        if (!argStr(L, 1, tok, 64, err) || !argStr(L, 2, val, 64, err)) return -1;
        static const QRegularExpression re(QStringLiteral("^[a-z0-9][a-z0-9._-]{0,63}$"));
        if (!re.match(tok).hasMatch()) { err = QStringLiteral("invalid token name"); return -1; }
        lua_pushboolean(L, t->setToken(P(L)->m.id, tok, val));
        return 1;
    });
}

// ---- http: policy, limits and audit live here; the bridge only performs the request ----
int http_do(lua_State *L, bool post) {
    return guarded(L, [&](QString &err) -> int {
        NEED("network");
        PluginState *p = P(L);
        auto *net = p->d->env.bridges.net;
        QString url;
        QByteArray body;
        int optIdx = 2;
        if (!argStr(L, 1, url, 2048, err)) return -1;
        if (post) {
            QString b;
            if (!argStr(L, 2, b, 256 * 1024, err)) return -1;
            body = b.toUtf8();
            optIdx = 3;
        }
        HttpRequest req;
        req.method = post ? QStringLiteral("POST") : QStringLiteral("GET");
        req.url = url;
        req.body = body;
        req.allowedHosts = p->m.netHosts;
        QString host, why;
        if (!checkHttpUrl(url, p->m.netHosts, &host, &why)) {
            if (p->d->env.audit) p->d->env.audit->log(QStringLiteral("denied"), p->m.id, QStringLiteral("network: %1 (%2)").arg(url.left(120), why));
            err = QStringLiteral("permission denied: network (%1)").arg(why);
            return -1;
        }
        if (lua_istable(L, optIdx)) {
            getRaw(L, optIdx, "headers");
            if (lua_istable(L, -1)) {
                lua_pushnil(L);
                int n = 0;
                while (lua_next(L, -2)) {
                    if (lua_type(L, -2) != LUA_TSTRING || !lua_isstring(L, -1) || ++n > 8) { lua_pop(L, 2); lua_pop(L, 1); err = QStringLiteral("headers must be up to 8 string pairs"); return -1; }
                    size_t kn, vn;
                    const char *k = lua_tolstring(L, -2, &kn);
                    const char *v = lua_tolstring(L, -1, &vn);
                    const QString name = QString::fromUtf8(k, qsizetype(kn)), val = QString::fromUtf8(v, qsizetype(vn));
                    static const QRegularExpression tok(QStringLiteral("^[A-Za-z0-9-]{1,40}$"));
                    static const QSet<QString> banned{"host", "content-length", "connection", "transfer-encoding", "cookie", "authorization-proxy"};
                    if (!tok.match(name).hasMatch() || banned.contains(name.toLower()) || val.size() > 512 || val.contains(QLatin1Char('\r')) || val.contains(QLatin1Char('\n'))) {
                        lua_pop(L, 2); lua_pop(L, 1);
                        err = QStringLiteral("header '%1' is not allowed").arg(name);
                        return -1;
                    }
                    req.headers.append({name, val});
                    lua_pop(L, 1);
                }
            }
            lua_pop(L, 1);
        }
        if (!net) { err = QStringLiteral("network is not available"); return -1; }
        if (!p->http.allow(kHttpPerMinute, 60'000'000)) {
            if (p->d->env.audit) p->d->env.audit->log(QStringLiteral("denied"), p->m.id, QStringLiteral("network rate limit (%1/min)").arg(kHttpPerMinute));
            lua_pushnil(L); lua_pushliteral(L, "rate limit exceeded"); return 2;
        }
        if (p->d->env.audit) p->d->env.audit->log(QStringLiteral("network"), p->m.id, QStringLiteral("%1 %2").arg(req.method, host));  // host only
        HttpResponse r;
        { PauseScope ps(p); r = net->perform(req); }
        if (!r.error.isEmpty()) { lua_pushnil(L); PUSHQ(r.error.left(500)); return 2; }
        if (r.body.size() > req.maxResponseBytes) { lua_pushnil(L); lua_pushliteral(L, "response too large"); return 2; }
        if (!hostFits(L, size_t(r.body.size()) + 1024)) { err = QString::fromLatin1(kTooBig); return -1; }
        lua_createtable(L, 0, 3);
        lua_pushinteger(L, r.status); lua_setfield(L, -2, "status");
        lua_pushlstring(L, r.body.constData(), size_t(r.body.size())); lua_setfield(L, -2, "body");
        lua_pushboolean(L, r.status >= 200 && r.status < 300); lua_setfield(L, -2, "ok");
        return 1;
    });
}
int http_get(lua_State *L) { return http_do(L, false); }
int http_post(lua_State *L) { return http_do(L, true); }

// ---- time (no permission: a clock is not sensitive; os.* is not available to plugins) ----
qint64 clockNow(lua_State *L) {
    const auto &c = P(L)->d->env.clock;
    return c ? c() : QDateTime::currentSecsSinceEpoch();
}
int tm_now(lua_State *L) { lua_pushinteger(L, lua_Integer(clockNow(L))); return 1; }
int tm_format(lua_State *L) {  // hn.time.format(fmt [, epoch [, utc]]) -> strftime text
    return guarded(L, [&](QString &err) -> int {
        QString fmt;
        if (!argStr(L, 1, fmt, 64, err)) return -1;
        qint64 t = clockNow(L);
        if (!lua_isnoneornil(L, 2)) {
            int isint = 0;
            const lua_Integer v = lua_tointegerx(L, 2, &isint);
            if (!isint) { err = QStringLiteral("bad argument #2 (integer epoch seconds expected)"); return -1; }
            t = v;
        }
        if (t < -62135596800LL || t > 253402300799LL) { err = QStringLiteral("epoch out of range (years 1-9999)"); return -1; }
        const bool utc = lua_toboolean(L, 3);
        const QByteArray f = fmt.toLatin1();
        for (char c : f) if (uchar(c) < 0x20 || uchar(c) > 0x7e) { err = QStringLiteral("format must be printable ASCII"); return -1; }
        const time_t tt = time_t(t);
        struct tm tmv;
        if (!(utc ? gmtime_r(&tt, &tmv) : localtime_r(&tt, &tmv))) { err = QStringLiteral("cannot convert that time"); return -1; }
        char buf[256];
        const size_t n = f.isEmpty() ? 0 : strftime(buf, sizeof buf, f.constData(), &tmv);
        if (n == 0 && !f.isEmpty()) { err = QStringLiteral("format produces no output or is too long"); return -1; }
        lua_pushlstring(L, buf, n);
        return 1;
    });
}

// ---- log ----
int l_log(lua_State *L) {
    PluginState *p = P(L);
    const int n = lua_gettop(L);
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    for (int i = 1; i <= n; ++i) {
        size_t len;
        const char *s = luaL_tolstring(L, i, &len);
        if (i > 1) luaL_addchar(&b, '\t');
        luaL_addlstring(&b, s, qMin<size_t>(len, 2000));
        lua_pop(L, 1);
        if (luaL_bufflen(&b) > 2000) break;
    }
    luaL_pushresult(&b);
    if (++p->logLines <= kMaxLogLines && p->d->env.logger) {
        size_t len;
        const char *s = lua_tolstring(L, -1, &len);
        p->d->env.logger(1, p->m.id, QString::fromUtf8(s, qsizetype(qMin<size_t>(len, 2000))));
    }
    return 0;
}

// ---- sandboxed base replacements ----
int s_load(lua_State *L) {
    if (lua_type(L, 1) != LUA_TSTRING) return luaL_error(L, "load: only string chunks are allowed");
    size_t n;
    const char *s = lua_tolstring(L, 1, &n);
    if (n > size_t(kMaxScriptFileBytes)) return luaL_error(L, "load: chunk too large");
    const char *name = luaL_optstring(L, 2, "=(load)");
    if (luaL_loadbufferx(L, s, n, name, "t") != LUA_OK) { lua_pushnil(L); lua_insert(L, -2); return 2; }
    if (!lua_isnoneornil(L, 4)) { lua_pushvalue(L, 4); if (!lua_setupvalue(L, -2, 1)) lua_pop(L, 1); }
    return 1;
}
int s_collect(lua_State *L) {
    const char *o = luaL_optstring(L, 1, "collect");
    if (!strcmp(o, "count")) { lua_pushnumber(L, lua_gc(L, LUA_GCCOUNT) + lua_gc(L, LUA_GCCOUNTB) / 1024.0); return 1; }
    if (!strcmp(o, "collect")) { lua_gc(L, LUA_GCCOLLECT); lua_pushinteger(L, 0); return 1; }
    return luaL_error(L, "collectgarbage: option '%s' is not available", o);
}
bool patExpired(lua_State *L) {  // polled by the bounded pattern matcher
    PluginState *p = P(L);
    if (!p->aborted && nowUs() > p->deadlineUs) p->abort(L, "time budget exceeded");
    return p->aborted;
}
// Wrappers for stock C functions whose running time is not covered by the instruction hook.
constexpr lua_Integer kMaxSortLen = 100000, kMaxMoveLen = 262144;
int s_sort(lua_State *L) {  // table.sort with a default comparator is one uninterruptible C loop
    luaL_checktype(L, 1, LUA_TTABLE);
    if (lua_rawlen(L, 1) > lua_Unsigned(kMaxSortLen)) return luaL_error(L, "table.sort: table too large (max %d elements)", int(kMaxSortLen));
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_call(L, lua_gettop(L) - 1, 0);
    return 0;
}
int s_move(lua_State *L) {  // table.move({}, 1, 2^40, 2) would loop in C for minutes
    const lua_Integer f = luaL_checkinteger(L, 2), e = luaL_checkinteger(L, 3);
    if (e >= f && lua_Unsigned(e) - lua_Unsigned(f) >= lua_Unsigned(kMaxMoveLen)) return luaL_error(L, "table.move: range too large (max %d elements)", int(kMaxMoveLen));
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_call(L, lua_gettop(L) - 1, 1);
    return 1;
}
int s_rep(lua_State *L) {  // string.rep with a result size cap (the allocator cap alone would only fail after the allocation attempt)
    size_t ln, sn = 0;
    luaL_checklstring(L, 1, &ln);
    const lua_Integer n = luaL_checkinteger(L, 2);
    if (!lua_isnoneornil(L, 3)) luaL_checklstring(L, 3, &sn);
    if (n > 0 && ln + sn == 0) { lua_pushliteral(L, ""); return 1; }  // the stock loop would spin n times copying nothing
    if (n > 0 && (double(ln) + double(sn)) * double(n) > double(kMaxStr)) return luaL_error(L, "resulting string too large");
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_call(L, lua_gettop(L) - 1, 1);
    return 1;
}
// Lua disables debug hooks while a __gc finalizer runs, so a looping finalizer could not be interrupted: forbid them.
// (A __gc added to a metatable after setmetatable() never marks the object for finalization, so checking here is enough.)
int s_setmetatable(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    if (lua_type(L, 2) == LUA_TTABLE) {
        lua_pushliteral(L, "__gc");
        lua_rawget(L, 2);
        if (!lua_isnil(L, -1)) return luaL_error(L, "__gc finalizers are not allowed in plugins");
        lua_pop(L, 1);
    }
    lua_settop(L, 2);
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_call(L, 2, 1);
    return 1;
}
int s_require(lua_State *L) {
    luaL_checktype(L, 1, LUA_TSTRING);
    lua_settop(L, 1);
    lua_getfield(L, LUA_REGISTRYINDEX, "hn.loaded");
    lua_pushvalue(L, 1);
    lua_rawget(L, -2);
    if (!lua_isnil(L, -1)) return 1;
    lua_pop(L, 1);
    const int rc = guarded(L, [&](QString &err) -> int {
        PluginState *p = P(L);
        QString name;
        if (!argStr(L, 1, name, 128, err)) return -1;
        static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9_]+(\\.[A-Za-z0-9_]+)*$"));
        if (!re.match(name).hasMatch()) { err = QStringLiteral("invalid module name '%1' (only modules inside the plugin folder can be required)").arg(name); return -1; }
        QString rel = name;
        rel.replace(QLatin1Char('.'), QLatin1Char('/'));
        rel += QStringLiteral(".lua");
        const QString root = QFileInfo(p->m.dir).canonicalFilePath();
        const QFileInfo fi(p->m.dir + QLatin1Char('/') + rel);
        const QString canon = fi.canonicalFilePath();
        if (canon.isEmpty() || !canon.startsWith(root + QLatin1Char('/')) || !QFileInfo(canon).isFile()) { err = QStringLiteral("module '%1' not found in the plugin folder").arg(name); return -1; }
        if (QFileInfo(canon).size() > kMaxScriptFileBytes) { err = QStringLiteral("module '%1' is larger than 1 MiB").arg(name); return -1; }
        QFile f(canon);
        if (!f.open(QIODevice::ReadOnly)) { err = QStringLiteral("cannot read module '%1'").arg(name); return -1; }
        const QByteArray src = f.readAll();
        if (luaL_loadbufferx(L, src.constData(), size_t(src.size()), ("@" + rel).toUtf8().constData(), "t") != LUA_OK) {
            err = QString::fromUtf8(lua_tostring(L, -1));
            lua_pop(L, 1);
            return -1;
        }
        return 2;
    });
    (void)rc;
    lua_pushvalue(L, 1);
    lua_call(L, 1, 1);
    if (lua_isnil(L, -1)) { lua_pop(L, 1); lua_pushboolean(L, 1); }
    lua_getfield(L, LUA_REGISTRYINDEX, "hn.loaded");
    lua_pushvalue(L, 1);
    lua_pushvalue(L, -3);
    lua_rawset(L, -3);
    lua_pop(L, 1);
    return 1;
}
int msgh(lua_State *L) {
    if (lua_type(L, 1) == LUA_TSTRING) return 1;
    lua_pushfstring(L, "(error object is a %s value)", luaL_typename(L, 1));
    return 1;
}

int buildSandbox(lua_State *L) {
    PluginState *p = P(L);
    luaL_requiref(L, "_G", luaopen_base, 1); lua_pop(L, 1);
    luaL_requiref(L, "string", luaopen_string, 1); lua_pop(L, 1);
    luaL_requiref(L, "table", luaopen_table, 1); lua_pop(L, 1);
    luaL_requiref(L, "math", luaopen_math, 1); lua_pop(L, 1);
    luaL_requiref(L, "utf8", luaopen_utf8, 1); lua_pop(L, 1);
    luaL_requiref(L, "coroutine", luaopen_coroutine, 1); lua_pop(L, 1);
    lua_getglobal(L, "string");
    lua_pushnil(L); lua_setfield(L, -2, "dump");
    lua_getfield(L, -1, "rep");
    lua_pushcclosure(L, s_rep, 1);
    lua_setfield(L, -2, "rep");
    lua_pop(L, 1);
    installSafePatterns(L, patExpired);
    lua_getglobal(L, "table");
    for (const auto &w : {std::pair<const char *, lua_CFunction>{"sort", s_sort}, {"move", s_move}}) {
        lua_getfield(L, -1, w.first);
        lua_pushcclosure(L, w.second, 1);
        lua_setfield(L, -2, w.first);
    }
    lua_pop(L, 1);
    lua_pushglobaltable(L);
    for (const char *n : {"dofile", "loadfile", "warn"}) { lua_pushnil(L); lua_setfield(L, -2, n); }
    lua_pushcfunction(L, s_load); lua_setfield(L, -2, "load");
    lua_getfield(L, -1, "setmetatable");
    lua_pushcclosure(L, s_setmetatable, 1);
    lua_setfield(L, -2, "setmetatable");
    lua_pushcfunction(L, s_collect); lua_setfield(L, -2, "collectgarbage");
    lua_pushcfunction(L, l_log); lua_setfield(L, -2, "print");
    lua_pushcfunction(L, s_require); lua_setfield(L, -2, "require");
    lua_pop(L, 1);
    lua_newtable(L);
    lua_setfield(L, LUA_REGISTRYINDEX, "hn.loaded");

    auto fn = [&](const char *name, lua_CFunction f) { lua_pushcfunction(L, f); lua_setfield(L, -2, name); };
    auto sub = [&](const char *name, std::initializer_list<std::pair<const char *, lua_CFunction>> fs) {
        lua_newtable(L);
        for (const auto &f : fs) { lua_pushcfunction(L, f.second); lua_setfield(L, -2, f.first); }
        lua_setfield(L, -2, name);
    };
    lua_newtable(L);
    fn("command", l_command); fn("toolbar_button", l_toolbar); fn("menu_item", l_menu); fn("on", l_on);
    fn("trigger", l_trigger); fn("setting", l_setting); fn("log", l_log);
    sub("note", {{"text", n_text}, {"selection", n_sel}, {"path", n_path}, {"title", n_title}, {"tags", n_tags},
                 {"replace_selection", n_replace}, {"insert", n_insert}, {"set_text", n_settext}});
    sub("notes", {{"list", ns_list}, {"read", ns_read}, {"create", ns_create}, {"write", ns_write}, {"delete", ns_delete}});
    sub("ui", {{"notify", ui_notify}, {"prompt", ui_prompt}, {"confirm", ui_confirm}, {"pick", ui_pick}});
    sub("storage", {{"get", st_get}, {"set", st_set}});
    sub("settings", {{"get", se_get}});
    sub("json", {{"encode", js_encode}, {"decode", js_decode}});
    sub("clipboard", {{"get", cb_get}, {"set", cb_set}});
    sub("http", {{"get", http_get}, {"post", http_post}});
    sub("theme", {{"set_token", th_set}});
    sub("time", {{"now", tm_now}, {"format", tm_format}});
    pushQRaw(L, p->d->env.appVersion); lua_setfield(L, -2, "version");
    if (p->m.api >= 2) {  // the API-2 surface does not exist at all for "api": 1 plugins
        fn("panel", l_panel); fn("panel_refresh", l_panel_refresh); fn("complete", l_complete); fn("link_handler", l_link_handler);
        lua_getfield(L, -1, "notes");
        for (const auto &f : {std::pair<const char *, lua_CFunction>{"links", ns_links}, {"backlinks", ns_backlinks}, {"resolve", ns_resolve},
                              {"frontmatter", ns_frontmatter}, {"query", ns_query}, {"open", ns_open}, {"rename", ns_rename}}) {
            lua_pushcfunction(L, f.second);
            lua_setfield(L, -2, f.first);
        }
        lua_pop(L, 1);
    }
    lua_setglobal(L, "hn");
    return 0;
}

int tramp(lua_State *L) {  // runs a std::function under lua_pcall so any Lua error is contained
    auto *f = static_cast<std::function<int(lua_State *)> *>(lua_touserdata(L, 1));
    lua_remove(L, 1);
    return (*f)(L);
}

}  // namespace

// ================= LuaPluginHost =================
LuaPluginHost::LuaPluginHost(HostEnv env, QObject *parent) : QObject(parent), d(new Impl) {
    d->q = this;
    d->env = std::move(env);
}
LuaPluginHost::~LuaPluginHost() {
    for (auto *p : std::as_const(d->plugins)) { if (p->L) lua_close(p->L); delete p; }
}
HostEnv &LuaPluginHost::env() { return d->env; }

void LuaPluginHost::setPlugin(const Manifest &m, const QStringList &consented, const PluginRegs *cached) {
    forget(m.id);
    auto *p = new PluginState;
    p->d = d.get();
    p->m = m;
    for (const auto &c : consented) if (m.permissions.contains(c)) p->perms.insert(c);
    if (cached) p->regs = *cached;
    d->plugins[m.id] = p;
}
void LuaPluginHost::unload(const QString &id) {
    auto it = d->plugins.find(id);
    if (it == d->plugins.end()) return;
    PluginState *p = *it;
    if (!p->L) return;
    if (p->busy) { p->disabled = true; p->pendingUnload = true; return; }  // closed when the running callback returns
    p->flushStorage();
    // __gc finalizers run inside lua_close: give them the same hook/budget so they cannot hang the app
    p->aborted = false;
    p->instrs = 0;
    p->maxInstrs = 50'000'000;
    p->deadlineUs = nowUs() + 100'000;
    lua_sethook(p->L, hookFn, LUA_MASKCOUNT, kHookStep);
    lua_close(p->L);
    p->L = nullptr;
    p->mem = 0;
    p->cb.clear();
    p->ev.clear();
    p->clicks.clear();
}
void LuaPluginHost::forget(const QString &id) {
    unload(id);
    if (auto it = d->plugins.find(id); it != d->plugins.end() && !(*it)->busy) { delete *it; d->plugins.erase(it); }
}
bool LuaPluginHost::isLoaded(const QString &id) const { auto it = d->plugins.constFind(id); return it != d->plugins.constEnd() && (*it)->L; }
int LuaPluginHost::loadedCount() const { int n = 0; for (const auto &p : d->plugins) n += p->L ? 1 : 0; return n; }
quint64 LuaPluginHost::memoryUsed(const QString &id) const { auto it = d->plugins.constFind(id); return it == d->plugins.constEnd() ? 0 : (*it)->mem; }

namespace {

int budgetMs(const HostEnv &e, Budget b) {
    switch (b) {
    case Budget::Event: return e.eventMs;
    case Budget::Command: return e.commandMs;
    case Budget::PreSave: return e.preSaveMs;
    case Budget::Trigger: return e.triggerMs;
    case Budget::Load: return e.loadMs;
    case Budget::Render: return e.renderMs;
    case Budget::Complete: return e.completeMs;
    }
    return e.eventMs;
}

// Records a failure; trips the breaker. Returns true if the plugin was disabled (state closed) - the caller must not touch it afterwards.
bool noteFailure(LuaPluginHost *q, LuaPluginHost::Impl *d, const QString &id, const QString &msg, bool *disabledOut = nullptr) {
    auto &env = d->env;
    if (env.audit) env.audit->log(QStringLiteral("failure"), id, msg);
    if (env.logger) env.logger(3, id, msg);
    if (!env.trust) return false;
    if (env.trust->addFailure(id) < env.breakerThreshold) return false;
    env.trust->setEnabled(id, false);
    if (env.audit) env.audit->log(QStringLiteral("auto-disable"), id, QStringLiteral("%1 consecutive failures; last: %2").arg(env.breakerThreshold).arg(msg));
    if (auto it = d->plugins.find(id); it != d->plugins.end()) (*it)->disabled = true;
    q->unload(id);
    if (env.registry) env.registry->remove(id);
    const QString reason = QStringLiteral("disabled after %1 consecutive failures: %2").arg(env.breakerThreshold).arg(msg);
    if (disabledOut) *disabledOut = true;
    emit q->autoDisabled(id, reason);
    return true;
}

struct CallResult { bool ok = false; QString err; bool disabled = false; };

CallResult callLua(LuaPluginHost *q, LuaPluginHost::Impl *d, const QString &id, Budget b, NoteBridge *note,
                   const std::function<int(lua_State *)> &setup, const std::function<void(lua_State *)> &onOk, bool countFailure = true) {
    CallResult r;
    auto it = d->plugins.find(id);
    if (it == d->plugins.end() || !(*it)->L || (*it)->disabled) { r.err = QStringLiteral("plugin is not loaded"); return r; }
    PluginState *p = *it;
    if (p->busy) { r.err = QStringLiteral("plugin is busy (re-entrant call refused)"); return r; }
    lua_State *L = p->L;
    p->busy = true;
    p->note = note;
    p->inTx = false;
    p->noteWrites = p->logLines = 0;
    p->indexWallUs = 0;
    p->opens = 0;
    p->aborted = false;
    p->instrs = 0;
    const int ms = budgetMs(d->env, b);
    p->deadlineUs = nowUs() + qint64(ms) * 1000;
    p->maxInstrs = qint64(ms) * 2000000;  // 2G instr/s ceiling: only a backstop, the wall clock is the real budget
    lua_sethook(L, hookFn, LUA_MASKCOUNT, kHookStep);
    const int top = lua_gettop(L);
    lua_pushcfunction(L, msgh);
    lua_pushcfunction(L, tramp);
    std::function<int(lua_State *)> f = [&](lua_State *LL) -> int {
        PluginState *pp = P(LL);
        ++pp->hostDepth;  // argument pushes are host allocations too
        const int n = setup(LL);  // pushes the function and its arguments; < 0 = argument does not fit in the plugin heap
        --pp->hostDepth;
        if (n < 0) return luaL_error(LL, "%s", kTooBig);
        lua_call(LL, n, 1);
        if (onOk) { ++pp->hostDepth; onOk(LL); --pp->hostDepth; }  // inside the pcall: onOk may raise (luaL_error) to reject a result
        return 1;
    };
    lua_pushlightuserdata(L, &f);
    const int rc = lua_pcall(L, 1, 1, top + 1);
    p->hostDepth = 0;
    if (rc == LUA_OK && p->aborted) r.err = QString::fromLatin1(p->abortMsg);
    else if (rc == LUA_OK) r.ok = true;
    else r.err = lua_type(L, -1) == LUA_TSTRING ? QString::fromUtf8(lua_tostring(L, -1)) : QStringLiteral("error");
    lua_settop(L, top);
    if (rc != LUA_OK) { p->aborted = false; p->deadlineUs = nowUs() + 50'000; lua_sethook(L, hookFn, LUA_MASKCOUNT, kHookStep); lua_gc(L, LUA_GCCOLLECT); }  // finalizers stay under the hook
    lua_sethook(L, nullptr, 0, 0);
    if (p->inTx && p->note) p->note->endTransaction();
    p->inTx = false;
    p->note = nullptr;
    p->busy = false;
    p->flushStorage();
    if (p->regsDirty) {
        p->regsDirty = false;
        if (d->env.registry) d->env.registry->set(id, p->regs);
    }
    if (p->pendingUnload) { p->pendingUnload = false; q->unload(id); }
    if (r.ok) {
        if (d->env.trust) d->env.trust->resetFailures(id);
    } else if (countFailure) {
        noteFailure(q, d, id, r.err, &r.disabled);
    }
    return r;
}

}  // namespace

bool LuaPluginHost::ensureLoaded(const QString &id, QString *err) {
    auto fail = [&](const QString &m) { if (err) *err = m; return false; };
    auto it = d->plugins.find(id);
    if (it == d->plugins.end()) return fail(QStringLiteral("unknown plugin"));
    PluginState *p = *it;
    if (p->L) return true;
    if (p->disabled) return fail(QStringLiteral("plugin is disabled"));
    // integrity: files must still match the hash that was consented to
    if (d->env.trust) {
        QString he;
        const QString h = hashDirectory(p->m.dir, &he);
        if (h.isEmpty() || d->env.trust->record(id).hash != h) {
            d->env.trust->verifyHash(id, h);
            d->env.trust->setEnabled(id, false);
            if (d->env.audit) d->env.audit->log(QStringLiteral("tamper"), id, QStringLiteral("package files changed since consent"));
            p->disabled = true;
            if (d->env.registry) d->env.registry->remove(id);
            const QString why = QStringLiteral("plugin files changed since you approved them; review and re-approve it");
            emit autoDisabled(id, why);
            return fail(why);
        }
    }
    const QString root = QFileInfo(p->m.dir).canonicalFilePath();
    const QString entry = QFileInfo(p->m.dir + QLatin1Char('/') + p->m.entry).canonicalFilePath();
    if (entry.isEmpty() || !entry.startsWith(root + QLatin1Char('/'))) return fail(QStringLiteral("entry file is missing or outside the plugin folder"));
    QFile f(entry);
    if (f.size() > kMaxScriptFileBytes || !f.open(QIODevice::ReadOnly)) return fail(QStringLiteral("cannot read entry file"));
    const QByteArray src = f.readAll();

    p->mem = 0;
    p->L = lua_newstate(allocFn, p, 0x9e3779b9u);
    if (!p->L) { p->mem = 0; return fail(QStringLiteral("out of memory creating Lua state")); }
    lua_State *L = p->L;
    const PluginRegs backup = p->regs;
    p->regs = PluginRegs();
    p->cb.clear();
    p->ev.clear();
    std::function<int(lua_State *)> bf = [](lua_State *LL) { return buildSandbox(LL); };
    lua_pushcfunction(L, tramp);
    lua_pushlightuserdata(L, &bf);
    bool ok = lua_pcall(L, 1, 0, 0) == LUA_OK;
    if (!ok) lua_settop(L, 0);
    CallResult r;
    if (ok) {
        r = callLua(this, d.get(), id, Budget::Load, nullptr,
                    [&](lua_State *LL) -> int {
                        if (luaL_loadbufferx(LL, src.constData(), size_t(src.size()), ("@" + p->m.entry.toUtf8()).constData(), "t") != LUA_OK) lua_error(LL);
                        return 0;
                    }, nullptr);
        ok = r.ok;
    } else r.err = QStringLiteral("could not build the sandbox");
    if (!ok) {
        if (auto it2 = d->plugins.find(id); it2 != d->plugins.end()) { (*it2)->regs = backup; unload(id); }
        return fail(QStringLiteral("failed to load: %1").arg(r.err));
    }
    p->regsDirty = false;
    if (d->env.registry) d->env.registry->set(id, p->regs);
    return true;
}

bool LuaPluginHost::run(const QString &id, Kind kind, const QString &localId, NoteBridge *note, QString *err) {
    auto fail = [&](const QString &m) { if (err) *err = m; return false; };
    if (!ensureLoaded(id, err)) return false;
    const QString key = QString::fromLatin1(kind == Kind::Command ? "cmd:" : kind == Kind::Toolbar ? "tb:" : "menu:") + localId;
    auto it = d->plugins.find(id);
    if (it == d->plugins.end() || !(*it)->cb.contains(key)) return fail(QStringLiteral("unknown command '%1'").arg(localId));
    const int ref = (*it)->cb.value(key);
    const auto r = callLua(this, d.get(), id, Budget::Command, note, [ref](lua_State *L) { lua_rawgeti(L, LUA_REGISTRYINDEX, ref); return 0; }, nullptr);
    if (!r.ok) return fail(r.err);
    return true;
}

void LuaPluginHost::deliver(const QString &id, const QString &event, const QString &arg, NoteBridge *note) {
    auto it = d->plugins.find(id);
    if (event == QLatin1String("link.activate")) return;  // only PluginManager::activateLink delivers it (with a table argument)
    if (it == d->plugins.end() || (*it)->disabled || !(*it)->regs.events.contains(event)) return;
    if (!ensureLoaded(id)) return;
    QList<int> refs = (*d->plugins.find(id))->ev.value(event);
    for (int ref : refs) {
        const auto r = callLua(this, d.get(), id, Budget::Event, note, [&](lua_State *L) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
            return pushQ(L, arg) ? 1 : -1;
        }, nullptr);
        if (r.disabled || !isLoaded(id)) break;
    }
}

QString LuaPluginHost::preSave(const QString &id, const QString &text, NoteBridge *note) {
    auto it = d->plugins.find(id);
    if (it == d->plugins.end() || (*it)->disabled || !(*it)->regs.events.contains(QStringLiteral("note.pre_save"))) return text;
    if (!ensureLoaded(id)) return text;
    QString cur = text;
    const QList<int> refs = (*d->plugins.find(id))->ev.value(QStringLiteral("note.pre_save"));
    for (int ref : refs) {
        QString out;
        bool got = false;
        const auto r = callLua(this, d.get(), id, Budget::PreSave, note, [&](lua_State *L) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
            return pushQ(L, cur) ? 1 : -1;
        }, [&](lua_State *L) {
            if (lua_type(L, -1) == LUA_TSTRING) {
                size_t n;
                const char *s = lua_tolstring(L, -1, &n);
                if (n <= kMaxStr) { out = QString::fromUtf8(s, qsizetype(n)); got = true; }
            }
        });
        if (r.ok && got) cur = out;
        if (r.disabled || !isLoaded(id)) break;
    }
    return cur;
}

bool LuaPluginHost::runTrigger(const QString &id, const QString &pattern, NoteBridge *note, QString *replacement) {
    if (!ensureLoaded(id)) return false;
    auto it = d->plugins.find(id);
    if (it == d->plugins.end() || !(*it)->cb.contains("trig:" + pattern)) return false;
    const int ref = (*it)->cb.value("trig:" + pattern);
    bool got = false;
    const auto r = callLua(this, d.get(), id, Budget::Trigger, note, [&](lua_State *L) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
        return pushQ(L, pattern) ? 1 : -1;
    }, [&](lua_State *L) {
        if (lua_type(L, -1) == LUA_TSTRING) {
            size_t n;
            const char *s = lua_tolstring(L, -1, &n);
            if (n <= kMaxStr) { *replacement = QString::fromUtf8(s, qsizetype(n)); got = true; }
        }
    });
    return r.ok && got;
}


// ================= API 2: panels, completion, link activation =================
namespace {

struct BlockCtx {
    PluginState *p;
    QList<int> refs;        // registry refs created for on_click (released again if the render is rejected)
    QHash<int, int> tokens; // token -> ref
    int count = 0;
    qint64 textBytes = 0;
};

// Raw string field with truncation (never rejects for length: the host draws a bounded text).
bool blkStr(lua_State *L, int t, const char *k, int maxBytes, bool required, QString &out, qint64 &total, QString &err) {
    getRaw(L, t, k);
    bool ok = true;
    if (lua_type(L, -1) == LUA_TSTRING) {
        size_t n;
        const char *s = lua_tolstring(L, -1, &n);
        out = QString::fromUtf8(s, qsizetype(qMin<size_t>(n, size_t(maxBytes))));
        total += qint64(qMin<size_t>(n, size_t(maxBytes)));
    } else if (!lua_isnil(L, -1) || required) {
        err = QStringLiteral("field '%1' must be a string").arg(QLatin1String(k));
        ok = false;
    }
    lua_pop(L, 1);
    return ok;
}
bool blkClick(lua_State *L, int t, BlockCtx &c, bool required, int &token, QString &err) {
    getRaw(L, t, "on_click");
    bool ok = true;
    if (lua_isfunction(L, -1)) {
        if (c.refs.size() >= kMaxBlocks) { err = QStringLiteral("too many click handlers"); ok = false; }
        else {
            lua_pushvalue(L, -1);
            const int ref = luaL_ref(L, LUA_REGISTRYINDEX);
            token = ++c.p->clickSeq;
            c.refs << ref;
            c.tokens[token] = ref;
        }
    } else if (!lua_isnil(L, -1) || required) { err = QStringLiteral("field 'on_click' must be a function"); ok = false; }
    lua_pop(L, 1);
    return ok;
}
bool parseBlock(lua_State *L, int idx, int depth, BlockCtx &c, PanelBlock &b, QString &err) {
    idx = lua_absindex(L, idx);
    if (!lua_istable(L, idx)) { err = QStringLiteral("a block must be a table"); return false; }
    if (++c.count > kMaxBlocks) { err = QStringLiteral("too many blocks (max %1)").arg(kMaxBlocks); return false; }
    if (!lua_checkstack(L, 8)) { err = QStringLiteral("blocks are too complex"); return false; }
    QString type;
    qint64 &tb = c.textBytes;
    if (!blkStr(L, idx, "type", 16, true, type, tb, err)) return false;
    b.type = type;
    if (type == QLatin1String("heading")) {
        if (!blkStr(L, idx, "text", 200, true, b.text, tb, err)) return false;
        getRaw(L, idx, "level");
        if (!lua_isnil(L, -1)) {
            int isint = 0;
            const lua_Integer v = lua_tointegerx(L, -1, &isint);
            if (lua_type(L, -1) != LUA_TNUMBER || !isint || v < 1 || v > 3) { lua_pop(L, 1); err = QStringLiteral("heading 'level' must be 1, 2 or 3"); return false; }
            b.level = int(v);
        }
        lua_pop(L, 1);
    } else if (type == QLatin1String("text") || type == QLatin1String("markdown")) {
        if (!blkStr(L, idx, "text", 16 * 1024, true, b.text, tb, err)) return false;
    } else if (type == QLatin1String("empty")) {
        if (!blkStr(L, idx, "text", 400, false, b.text, tb, err)) return false;
    } else if (type == QLatin1String("button")) {
        if (!blkStr(L, idx, "label", 200, true, b.text, tb, err) || !blkClick(L, idx, c, true, b.click, err)) return false;
    } else if (type == QLatin1String("item")) {
        if (!blkStr(L, idx, "title", 200, true, b.title, tb, err) || !blkStr(L, idx, "subtitle", 400, false, b.subtitle, tb, err) || !blkStr(L, idx, "path", 512, false, b.path, tb, err)) return false;
        if (!b.path.isEmpty() && !validNotePath(b.path)) { err = QStringLiteral("item 'path' is not a valid note path"); return false; }
        getRaw(L, idx, "line");
        if (!lua_isnil(L, -1)) {
            int isint = 0;
            const lua_Integer v = lua_tointegerx(L, -1, &isint);
            if (lua_type(L, -1) != LUA_TNUMBER || !isint || v < 1 || v > 10'000'000) { lua_pop(L, 1); err = QStringLiteral("item 'line' must be an integer between 1 and 10000000"); return false; }
            b.line = int(v);
        }
        lua_pop(L, 1);
        if (!blkClick(L, idx, c, false, b.click, err)) return false;
    } else if (type == QLatin1String("list")) {
        if (depth > 0) { err = QStringLiteral("a list cannot contain a list"); return false; }
        getRaw(L, idx, "items");
        if (!lua_istable(L, -1)) { lua_pop(L, 1); err = QStringLiteral("list 'items' must be an array of item blocks"); return false; }
        const lua_Unsigned n = lua_rawlen(L, -1);
        if (n > lua_Unsigned(kMaxBlocks)) { lua_pop(L, 1); err = QStringLiteral("too many blocks (max %1)").arg(kMaxBlocks); return false; }
        for (lua_Unsigned i = 1; i <= n; ++i) {
            lua_rawgeti(L, -1, lua_Integer(i));
            PanelBlock it;
            const bool ok = parseBlock(L, -1, depth + 1, c, it, err);
            lua_pop(L, 1);
            if (!ok) { lua_pop(L, 1); return false; }
            if (it.type != QLatin1String("item")) { lua_pop(L, 1); err = QStringLiteral("a list may only contain item blocks"); return false; }
            b.items << it;
        }
        lua_pop(L, 1);
    } else {
        err = QStringLiteral("unknown block type '%1'").arg(type.left(24));
        return false;
    }
    if (tb > kMaxPanelBytes) { err = QStringLiteral("panel text is larger than %1 KiB").arg(kMaxPanelBytes / 1024); return false; }
    return true;
}
// Converts the table on top of the stack into blocks. Raises (after all C++ objects are gone) when the result is rejected.
void convertBlocks(lua_State *L, PluginState *p, const QString &panelId, QList<PanelBlock> *out) {
    char msg[300];
    msg[0] = 0;
    {
        BlockCtx c{p, {}, {}, 0, 0};
        QList<PanelBlock> blocks;
        QString err;
        bool ok = true;
        if (!lua_istable(L, -1)) { err = QStringLiteral("render must return an array of blocks"); ok = false; }
        else {
            const lua_Unsigned n = lua_rawlen(L, -1);
            if (n > lua_Unsigned(kMaxBlocks)) { err = QStringLiteral("too many blocks (max %1)").arg(kMaxBlocks); ok = false; }
            for (lua_Unsigned i = 1; ok && i <= n; ++i) {
                lua_rawgeti(L, -1, lua_Integer(i));
                PanelBlock b;
                ok = parseBlock(L, -1, 0, c, b, err);
                lua_pop(L, 1);
                if (ok) blocks << b;
                else err = QStringLiteral("block #%1: %2").arg(i).arg(err);
            }
        }
        if (ok) {
            // swap in the new click table; the previous render's callbacks are released
            auto &old = p->clicks[panelId];
            for (int ref : std::as_const(old)) luaL_unref(L, LUA_REGISTRYINDEX, ref);
            old = c.tokens;
            *out = blocks;
        } else {
            for (int ref : std::as_const(c.refs)) luaL_unref(L, LUA_REGISTRYINDEX, ref);
            qstrncpy(msg, ("invalid panel: " + err).toUtf8().constData(), sizeof msg);
        }
    }
    if (msg[0]) luaL_error(L, "%s", msg);
}

// Context table for render/items: no note data unless the plugin may read the note.
void pushCtx(lua_State *L, PluginState *p, const char *k1, const QString &v1, NoteBridge *note) {
    lua_createtable(L, 0, 4);
    pushQRaw(L, v1); lua_setfield(L, -2, k1);
    if (note && p->perms.contains(QStringLiteral("note.read"))) {
        pushQRaw(L, capUtf8(note->path(), 4096)); lua_setfield(L, -2, "path");
        pushQRaw(L, capUtf8(note->title(), 4096)); lua_setfield(L, -2, "title");
    }
}
bool fitsStr(lua_State *L, std::initializer_list<const QString *> v) {
    size_t c = 1024;
    for (auto *s : v) c += size_t(s->size()) * 3 + 64;
    return hostFits(L, c);
}

}  // namespace

bool LuaPluginHost::renderPanel(const QString &id, const QString &panelId, NoteBridge *note, QList<PanelBlock> *out, QString *err) {
    auto fail = [&](const QString &m) { if (err) *err = m; return false; };
    if (!ensureLoaded(id, err)) return false;
    auto it = d->plugins.find(id);
    if (it == d->plugins.end() || !(*it)->cb.contains("panel:" + panelId)) return fail(QStringLiteral("unknown panel '%1'").arg(panelId));
    PluginState *p = *it;
    const int ref = p->cb.value("panel:" + panelId);
    p->renderingPanel = panelId;
    QList<PanelBlock> blocks;
    const auto r = callLua(this, d.get(), id, Budget::Render, note, [&](lua_State *L) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
        if (!fitsStr(L, {&panelId})) return -1;
        pushCtx(L, p, "panel", panelId, note);
        return 1;
    }, [&](lua_State *L) { convertBlocks(L, p, panelId, &blocks); });
    if (auto it2 = d->plugins.find(id); it2 != d->plugins.end()) (*it2)->renderingPanel.clear();
    if (!r.ok) return fail(r.err);
    if (out) *out = blocks;
    return true;
}

bool LuaPluginHost::panelClick(const QString &id, const QString &panelId, int token, NoteBridge *note, QString *err) {
    auto fail = [&](const QString &m) { if (err) *err = m; return false; };
    auto it = d->plugins.find(id);
    if (it == d->plugins.end() || !(*it)->L || (*it)->disabled) return fail(QStringLiteral("stale click: the panel has no live state"));
    const auto pc = (*it)->clicks.constFind(panelId);
    if (pc == (*it)->clicks.constEnd() || !pc->contains(token)) return fail(QStringLiteral("stale click: the panel was re-rendered"));
    const int ref = pc->value(token);
    const auto r = callLua(this, d.get(), id, Budget::Command, note, [ref](lua_State *L) { lua_rawgeti(L, LUA_REGISTRYINDEX, ref); return 0; }, nullptr);
    if (!r.ok) return fail(r.err);
    return true;
}

void LuaPluginHost::deliverPanelEvent(const QString &id, const QString &panelId, const QString &event, const QString &arg, NoteBridge *note) {
    auto it = d->plugins.find(id);
    if (it == d->plugins.end() || (*it)->disabled) return;
    if (!ensureLoaded(id)) return;
    it = d->plugins.find(id);
    if (it == d->plugins.end() || !(*it)->cb.contains("pev:" + panelId)) return;
    const int ref = (*it)->cb.value("pev:" + panelId);
    callLua(this, d.get(), id, Budget::Event, note, [&](lua_State *L) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
        if (!pushQ(L, event) || !pushQ(L, arg)) return -1;
        return 2;
    }, nullptr);
}

QList<CompletionItem> LuaPluginHost::complete(const QString &id, const QString &completeId, const QString &query, NoteBridge *note, int *dropped, QString *err) {
    QList<CompletionItem> items;
    int drop = 0;
    if (dropped) *dropped = 0;
    if (!ensureLoaded(id, err)) return items;
    auto it = d->plugins.find(id);
    if (it == d->plugins.end() || !(*it)->cb.contains("cmp:" + completeId)) { if (err) *err = QStringLiteral("unknown completion '%1'").arg(completeId); return items; }
    PluginState *p = *it;
    const int ref = p->cb.value("cmp:" + completeId);
    QString trigger;
    for (const auto &c : p->regs.completions) if (c.id == completeId) trigger = c.trigger;
    const QString q = capUtf8(query, 256);
    const auto r = callLua(this, d.get(), id, Budget::Complete, note, [&](lua_State *L) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
        if (!fitsStr(L, {&q, &trigger})) return -1;
        pushQRaw(L, q);
        pushCtx(L, p, "trigger", trigger, note);
        return 2;
    }, [&](lua_State *L) {
        if (!lua_istable(L, -1)) { drop = 1; return; }
        const lua_Unsigned n = lua_rawlen(L, -1);
        for (lua_Unsigned i = 1; i <= n && i <= 200; ++i) {
            if (items.size() >= kMaxCompleteItems) { drop += int(n - i + 1); break; }
            lua_rawgeti(L, -1, lua_Integer(i));
            bool ok = lua_istable(L, -1);
            CompletionItem ci;
            ci.pluginId = id;
            auto str = [&](const char *k, int maxChars, bool required, QString &dst) {
                if (!ok) return;
                getRaw(L, -1, k);
                if (lua_type(L, -1) == LUA_TSTRING) {
                    size_t len;
                    const char *s = lua_tolstring(L, -1, &len);
                    if (len > size_t(maxChars) * 4) ok = false;
                    else { dst = QString::fromUtf8(s, qsizetype(len)); if (dst.size() > maxChars || (required && dst.isEmpty())) ok = false; }
                } else if (required || !lua_isnil(L, -1)) ok = false;
                lua_pop(L, 1);
            };
            str("label", 200, true, ci.label);
            str("insert", 8192, true, ci.insert);
            str("detail", 200, false, ci.detail);
            if (ok) {
                getRaw(L, -1, "cursor_offset");
                if (!lua_isnil(L, -1)) {
                    int isint = 0;
                    const lua_Integer v = lua_tointegerx(L, -1, &isint);
                    if (lua_type(L, -1) != LUA_TNUMBER || !isint || v < 0 || v > ci.insert.size()) ok = false;
                    else ci.cursorOffset = int(v);
                }
                lua_pop(L, 1);
            }
            lua_pop(L, 1);
            if (ok) items << ci; else ++drop;
        }
        if (n > 200 && items.size() < kMaxCompleteItems) drop += int(n - 200);
    });
    if (dropped) *dropped = drop;
    if (!r.ok) { if (err) *err = r.err; return {}; }
    if (drop > 0 && d->env.logger) d->env.logger(2, id, QStringLiteral("completion '%1': %2 invalid or surplus items dropped").arg(completeId).arg(drop));
    return items;
}

bool LuaPluginHost::activateLink(const QString &id, const LinkActivation &ref, NoteBridge *note) {
    auto it = d->plugins.find(id);
    if (it == d->plugins.end() || (*it)->disabled || !(*it)->regs.events.contains(QStringLiteral("link.activate"))) return false;
    if (!ensureLoaded(id)) return false;
    auto pushRef = [&](lua_State *L) -> bool {
        const QString kind = capUtf8(ref.kind, 16), target = capUtf8(ref.target, 1024), alias = capUtf8(ref.alias, 512), anchor = capUtf8(ref.anchor, 512), resolved = capUtf8(ref.resolved, 1024);
        if (!fitsStr(L, {&kind, &target, &alias, &anchor, &resolved})) return false;
        lua_createtable(L, 0, 5);
        auto put = [&](const char *k, const QString &v, bool always) { if (!always && v.isEmpty()) return; pushQRaw(L, v); lua_setfield(L, -2, k); };
        put("kind", kind.isEmpty() ? QStringLiteral("link") : kind, true);
        put("target", target, true);
        put("alias", alias, false);
        put("anchor", anchor, false);
        put("resolved", resolved, false);
        return true;
    };
    it = d->plugins.find(id);
    if (const auto pat = (*it)->cb.constFind(QStringLiteral("linkpat")); pat != (*it)->cb.constEnd()) {
        const int pref = *pat;
        bool pass = false;
        const auto r = callLua(this, d.get(), id, Budget::Event, note, [&](lua_State *L) { lua_rawgeti(L, LUA_REGISTRYINDEX, pref); return pushRef(L) ? 1 : -1; },
                               [&](lua_State *L) { pass = lua_toboolean(L, -1); });
        if (!r.ok || !pass || r.disabled) return false;
    }
    it = d->plugins.find(id);
    if (it == d->plugins.end()) return false;
    const QList<int> refs = (*it)->ev.value(QStringLiteral("link.activate"));
    for (int fref : refs) {
        bool handled = false;
        const auto r = callLua(this, d.get(), id, Budget::Event, note, [&](lua_State *L) { lua_rawgeti(L, LUA_REGISTRYINDEX, fref); return pushRef(L) ? 1 : -1; },
                               [&](lua_State *L) { handled = lua_toboolean(L, -1); });
        if (r.ok && handled) return true;
        if (r.disabled || !isLoaded(id)) break;
    }
    return false;
}

bool LuaPluginHost::setSetting(const QString &id, const QString &settingId, const QVariant &v) {
    auto it = d->plugins.find(id);
    if (it == d->plugins.end()) return false;
    PluginState *p = *it;
    for (const auto &s : p->regs.settings) {
        if (s.id != settingId) continue;
        const bool ok = (s.type == QLatin1String("bool") && v.typeId() == QMetaType::Bool) ||
                        (s.type == QLatin1String("string") && v.typeId() == QMetaType::QString) ||
                        (s.type == QLatin1String("number") && v.canConvert<double>() && v.typeId() != QMetaType::QString && v.typeId() != QMetaType::Bool);
        if (!ok) return false;
        p->loadStorage();
        p->settings.insert(settingId, QJsonValue::fromVariant(v));
        p->storageDirty = true;
        p->flushStorage();
        return true;
    }
    return false;
}
QVariant LuaPluginHost::setting(const QString &id, const QString &settingId) {
    auto it = d->plugins.find(id);
    if (it == d->plugins.end()) return {};
    PluginState *p = *it;
    p->loadStorage();
    if (p->settings.contains(settingId)) return p->settings.value(settingId).toVariant();
    for (const auto &s : p->regs.settings) if (s.id == settingId) return s.def;
    return {};
}
void LuaPluginHost::deleteStorage(const QString &id) {
    if (auto it = d->plugins.find(id); it != d->plugins.end()) { (*it)->kv = {}; (*it)->settings = {}; (*it)->storageDirty = false; (*it)->storageLoaded = false; }
    QFile::remove(d->env.stateDir + QStringLiteral("/plugin-data/") + id + QStringLiteral(".json"));
}

}  // namespace hn::plugins
