// Port of the Lua pattern matcher (lstrlib.c) with a step budget. Pure C-API code: no C++ objects with destructors
// may be live in this file because luaL_error longjmps.
#include "luapattern.h"
#include <cstring>
#include <lua.hpp>

namespace hn::plugins {
namespace {

constexpr int kMaxCaptures = 32, kMaxDepth = 200;
constexpr ptrdiff_t kCapUnfinished = -1, kCapPosition = -2;
constexpr char kEsc = '%';
const char *const kSpecials = "^$*+?.([%-";
PatternExpired g_expired = nullptr;

struct MS {
    const char *src_init, *src_end, *p_end;
    lua_State *L;
    int matchdepth;
    unsigned char level;
    long steps;
    struct { const char *init; ptrdiff_t len; } capture[kMaxCaptures];
};

inline int uc(char c) { return static_cast<unsigned char>(c); }
// ASCII-only classes: identical to the C locale, independent of whatever locale Qt installed.
inline bool isAlpha(int c) { return (c | 32) >= 'a' && (c | 32) <= 'z'; }
inline bool isDigit(int c) { return c >= '0' && c <= '9'; }
inline bool isLower(int c) { return c >= 'a' && c <= 'z'; }
inline bool isUpper(int c) { return c >= 'A' && c <= 'Z'; }
inline bool isSpace(int c) { return c == ' ' || (c >= 9 && c <= 13); }
inline bool isCntrl(int c) { return c < 32 || c == 127; }
inline bool isGraph(int c) { return c > 32 && c < 127; }
inline bool isPunct(int c) { return isGraph(c) && !isAlpha(c) && !isDigit(c); }
inline bool isXdigit(int c) { return isDigit(c) || ((c | 32) >= 'a' && (c | 32) <= 'f'); }

void spend(MS *ms, long n) {
    const long before = ms->steps;
    ms->steps += n;
    if (ms->steps > kPatSteps) luaL_error(ms->L, "pattern too complex (step budget exceeded)");
    if ((ms->steps >> 10) != (before >> 10) && g_expired && g_expired(ms->L)) luaL_error(ms->L, "time budget exceeded");
}

const char *match(MS *ms, const char *s, const char *p);

int checkCapture(MS *ms, int l) {
    l -= '1';
    if (l < 0 || l >= ms->level || ms->capture[l].len == kCapUnfinished) return luaL_error(ms->L, "invalid capture index %%%d", l + 1);
    return l;
}
int captureToClose(MS *ms) {
    int level = ms->level;
    for (level--; level >= 0; level--)
        if (ms->capture[level].len == kCapUnfinished) return level;
    return luaL_error(ms->L, "invalid pattern capture");
}
const char *classEnd(MS *ms, const char *p) {
    switch (*p++) {
    case kEsc:
        if (p == ms->p_end) luaL_error(ms->L, "malformed pattern (ends with '%%')");
        return p + 1;
    case '[':
        if (*p == '^') p++;
        do {
            if (p == ms->p_end) luaL_error(ms->L, "malformed pattern (missing ']')");
            if (*(p++) == kEsc && p < ms->p_end) p++;
        } while (*p != ']');
        return p + 1;
    default: return p;
    }
}
bool matchClass(int c, int cl) {
    bool res;
    switch (cl | 32) {
    case 'a': res = isAlpha(c); break;
    case 'c': res = isCntrl(c); break;
    case 'd': res = isDigit(c); break;
    case 'g': res = isGraph(c); break;
    case 'l': res = isLower(c); break;
    case 'p': res = isPunct(c); break;
    case 's': res = isSpace(c); break;
    case 'u': res = isUpper(c); break;
    case 'w': res = isAlpha(c) || isDigit(c); break;
    case 'x': res = isXdigit(c); break;
    case 'z': res = (c == 0); break;  // deprecated in Lua 5.2 but still honoured by the stock matcher
    default: return cl == c;
    }
    if (isUpper(cl)) res = !res;
    return res;
}
bool matchBracketClass(int c, const char *p, const char *ec) {
    bool sig = true;
    if (*(p + 1) == '^') { sig = false; p++; }
    while (++p < ec) {
        if (*p == kEsc) { p++; if (matchClass(c, uc(*p))) return sig; }
        else if (*(p + 1) == '-' && (p + 2 < ec)) { p += 2; if (uc(*(p - 2)) <= c && c <= uc(*p)) return sig; }
        else if (uc(*p) == c) return sig;
    }
    return !sig;
}
bool singleMatch(MS *ms, const char *s, const char *p, const char *ep) {
    if (s >= ms->src_end) return false;
    const int c = uc(*s);
    switch (*p) {
    case '.': return true;
    case kEsc: return matchClass(c, uc(*(p + 1)));
    case '[': return matchBracketClass(c, p, ep - 1);
    default: return uc(*p) == c;
    }
}
const char *matchBalance(MS *ms, const char *s, const char *p) {
    if (p >= ms->p_end - 1) luaL_error(ms->L, "malformed pattern (missing arguments to '%%b')");
    if (s >= ms->src_end || *s != *p) return nullptr;
    const int b = *p, e = *(p + 1);
    int cont = 1;
    while (++s < ms->src_end) {
        if (*s == e) { if (--cont == 0) return s + 1; }
        else if (*s == b) cont++;
    }
    return nullptr;
}
const char *maxExpand(MS *ms, const char *s, const char *p, const char *ep) {
    ptrdiff_t i = 0;
    while (singleMatch(ms, s + i, p, ep)) i++;
    spend(ms, i);
    while (i >= 0) {
        if (const char *res = match(ms, s + i, ep + 1)) return res;
        i--;
    }
    return nullptr;
}
const char *minExpand(MS *ms, const char *s, const char *p, const char *ep) {
    for (;;) {
        if (const char *res = match(ms, s, ep + 1)) return res;
        if (singleMatch(ms, s, p, ep)) s++;
        else return nullptr;
    }
}
const char *startCapture(MS *ms, const char *s, const char *p, ptrdiff_t what) {
    const int level = ms->level;
    if (level >= kMaxCaptures) luaL_error(ms->L, "too many captures");
    ms->capture[level].init = s;
    ms->capture[level].len = what;
    ms->level = static_cast<unsigned char>(level + 1);
    const char *res = match(ms, s, p);
    if (!res) ms->level--;
    return res;
}
const char *endCapture(MS *ms, const char *s, const char *p) {
    const int l = captureToClose(ms);
    ms->capture[l].len = s - ms->capture[l].init;
    const char *res = match(ms, s, p);
    if (!res) ms->capture[l].len = kCapUnfinished;
    return res;
}
const char *matchCapture(MS *ms, const char *s, int l) {
    l = checkCapture(ms, l);
    const size_t len = size_t(ms->capture[l].len);
    if (size_t(ms->src_end - s) >= len && memcmp(ms->capture[l].init, s, len) == 0) return s + len;
    return nullptr;
}

const char *match(MS *ms, const char *s, const char *p) {
    if (ms->matchdepth-- == 0) luaL_error(ms->L, "pattern too complex");
init:
    spend(ms, 1);
    if (p != ms->p_end) {
        switch (*p) {
        case '(':
            s = (*(p + 1) == ')') ? startCapture(ms, s, p + 2, kCapPosition) : startCapture(ms, s, p + 1, kCapUnfinished);
            break;
        case ')': s = endCapture(ms, s, p + 1); break;
        case '$':
            if ((p + 1) != ms->p_end) goto dflt;
            s = (s == ms->src_end) ? s : nullptr;
            break;
        case kEsc:
            switch (*(p + 1)) {
            case 'b':
                s = matchBalance(ms, s, p + 2);
                if (s) { p += 4; goto init; }
                break;
            case 'f': {
                p += 2;
                if (*p != '[') luaL_error(ms->L, "missing '[' after '%%f' in pattern");
                const char *ep = classEnd(ms, p);
                const char previous = (s == ms->src_init) ? '\0' : *(s - 1);
                const char cur = (s < ms->src_end) ? *s : '\0';
                if (!matchBracketClass(uc(previous), p, ep - 1) && matchBracketClass(uc(cur), p, ep - 1)) { p = ep; goto init; }
                s = nullptr;
                break;
            }
            case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8': case '9':
                s = matchCapture(ms, s, uc(*(p + 1)));
                if (s) { p += 2; goto init; }
                break;
            default: goto dflt;
            }
            break;
        default:
        dflt: {
            const char *ep = classEnd(ms, p);
            if (!singleMatch(ms, s, p, ep)) {
                if (*ep == '*' || *ep == '?' || *ep == '-') { p = ep + 1; goto init; }
                s = nullptr;
            } else {
                switch (*ep) {
                case '?': {
                    if (const char *res = match(ms, s + 1, ep + 1)) s = res;
                    else { p = ep + 1; goto init; }
                    break;
                }
                case '+': s++; [[fallthrough]];
                case '*': s = maxExpand(ms, s, p, ep); break;
                case '-': s = minExpand(ms, s, p, ep); break;
                default: s++; p = ep; goto init;
                }
            }
            break;
        }
        }
    }
    ms->matchdepth++;
    return s;
}

size_t getOneCapture(MS *ms, int l, const char *s, const char *e, const char **cap) {
    if (l >= ms->level) {
        if (l != 0) luaL_error(ms->L, "invalid capture index %%%d", l + 1);
        *cap = s;
        return size_t(e - s);
    }
    const ptrdiff_t capl = ms->capture[l].len;
    *cap = ms->capture[l].init;
    if (capl == kCapUnfinished) luaL_error(ms->L, "unfinished capture");
    else if (capl == kCapPosition) lua_pushinteger(ms->L, (ms->capture[l].init - ms->src_init) + 1);
    return size_t(capl);
}
void pushOneCapture(MS *ms, int i, const char *s, const char *e) {
    const char *cap;
    const ptrdiff_t l = ptrdiff_t(getOneCapture(ms, i, s, e, &cap));
    if (l != kCapPosition) lua_pushlstring(ms->L, cap, size_t(l));
}
int pushCaptures(MS *ms, const char *s, const char *e) {
    const int nlevels = (ms->level == 0 && s) ? 1 : ms->level;
    luaL_checkstack(ms->L, nlevels, "too many captures");
    for (int i = 0; i < nlevels; i++) pushOneCapture(ms, i, s, e);
    return nlevels;
}

// Static screening before any matching: sizes and a cap on quantified items (counted outside sets and escapes).
void screen(lua_State *L, size_t ls, const char *p, size_t lp) {
    if (ls > kPatSubjectMax) luaL_error(L, "string too long for pattern matching (max %d bytes)", int(kPatSubjectMax));
    if (lp > kPatPatternMax) luaL_error(L, "pattern too long (max %d bytes)", int(kPatPatternMax));
    int quant = 0;
    for (size_t i = 0; i < lp; ++i) {
        const char c = p[i];
        if (c == kEsc) { i += (i + 1 < lp && p[i + 1] == 'b') ? 3 : 1; }
        else if (c == '[') {
            size_t j = i + 1;
            if (j < lp && p[j] == '^') ++j;
            do {
                if (j >= lp) break;
                if (p[j++] == kEsc && j < lp) ++j;
            } while (j < lp && p[j] != ']');
            i = j;
        } else if ((c == '*' || c == '+' || c == '-' || c == '?') && i > 0 && ++quant > kPatMaxQuantifiers)
            luaL_error(L, "pattern too complex (more than %d quantifiers)", kPatMaxQuantifiers);
    }
}

size_t posRelatI(lua_Integer pos, size_t len) {
    if (pos > 0) return size_t(pos);
    if (pos == 0) return 1;
    if (pos < -lua_Integer(len)) return 1;
    return len + size_t(pos) + 1;
}
void prepState(MS *ms, lua_State *L, const char *s, size_t ls, const char *p, size_t lp) {
    ms->L = L;
    ms->matchdepth = kMaxDepth;
    ms->src_init = s;
    ms->src_end = s + ls;
    ms->p_end = p + lp;
    ms->level = 0;
    ms->steps = 0;
}
void reprepState(MS *ms) {
    ms->level = 0;
    ms->matchdepth = kMaxDepth;
}
bool noSpecials(const char *p, size_t l) {
    size_t upto = 0;
    do {
        if (strpbrk(p + upto, kSpecials)) return false;
        upto += strlen(p + upto) + 1;
    } while (upto <= l);
    return true;
}

int findAux(lua_State *L, bool find) {
    size_t ls, lp;
    const char *s = luaL_checklstring(L, 1, &ls);
    const char *p = luaL_checklstring(L, 2, &lp);
    const size_t init = posRelatI(luaL_optinteger(L, 3, 1), ls) - 1;
    screen(L, ls, p, lp);
    if (init > ls) { luaL_pushfail(L); return 1; }
    if (find && (lua_toboolean(L, 4) || noSpecials(p, lp))) {
        const char *s2 = lp == 0 ? s + init : static_cast<const char *>(memmem(s + init, ls - init, p, lp));  // linear (two-way), unlike the stock O(n*m) scan
        if (s2) {
            lua_pushinteger(L, (s2 - s) + 1);
            lua_pushinteger(L, lua_Integer(size_t(s2 - s) + lp));
            return 2;
        }
    } else {
        MS ms;
        const char *s1 = s + init;
        const bool anchor = (*p == '^');
        if (anchor) { p++; lp--; }
        prepState(&ms, L, s, ls, p, lp);
        do {
            reprepState(&ms);
            if (const char *res = match(&ms, s1, p)) {
                if (find) {
                    lua_pushinteger(L, (s1 - s) + 1);
                    lua_pushinteger(L, res - s);
                    return pushCaptures(&ms, nullptr, nullptr) + 2;
                }
                return pushCaptures(&ms, s1, res);
            }
        } while (s1++ < ms.src_end && !anchor);
    }
    luaL_pushfail(L);
    return 1;
}
int s_find(lua_State *L) { return findAux(L, true); }
int s_match(lua_State *L) { return findAux(L, false); }

struct GMatch { const char *src, *p, *lastmatch; MS ms; };
int gmatchAux(lua_State *L) {
    auto *gm = static_cast<GMatch *>(lua_touserdata(L, lua_upvalueindex(3)));
    gm->ms.L = L;
    gm->ms.steps = 0;  // budget is per iteration; the loop around it is under the instruction hook
    for (const char *src = gm->src; src <= gm->ms.src_end; src++) {
        reprepState(&gm->ms);
        const char *e = match(&gm->ms, src, gm->p);
        if (e && e != gm->lastmatch) {
            gm->src = gm->lastmatch = e;
            return pushCaptures(&gm->ms, src, e);
        }
    }
    return 0;
}
int s_gmatch(lua_State *L) {
    size_t ls, lp;
    const char *s = luaL_checklstring(L, 1, &ls);
    const char *p = luaL_checklstring(L, 2, &lp);
    size_t init = posRelatI(luaL_optinteger(L, 3, 1), ls) - 1;
    screen(L, ls, p, lp);
    lua_settop(L, 2);
    auto *gm = static_cast<GMatch *>(lua_newuserdatauv(L, sizeof(GMatch), 0));
    if (init > ls) init = ls + 1;
    prepState(&gm->ms, L, s, ls, p, lp);
    gm->src = s + init;
    gm->p = p;
    gm->lastmatch = nullptr;
    lua_pushcclosure(L, gmatchAux, 3);
    return 1;
}

void addS(MS *ms, luaL_Buffer *b, const char *s, const char *e) {
    size_t l;
    lua_State *L = ms->L;
    const char *news = lua_tolstring(L, 3, &l);
    const char *p;
    while ((p = static_cast<const char *>(memchr(news, kEsc, l))) != nullptr) {
        luaL_addlstring(b, news, size_t(p - news));
        p++;
        if (*p == kEsc) luaL_addchar(b, *p);
        else if (*p == '0') luaL_addlstring(b, s, size_t(e - s));
        else if (isDigit(uc(*p))) {
            const char *cap;
            const ptrdiff_t resl = ptrdiff_t(getOneCapture(ms, *p - '1', s, e, &cap));
            if (resl == kCapPosition) luaL_addvalue(b);
            else luaL_addlstring(b, cap, size_t(resl));
        } else luaL_error(L, "invalid use of '%c' in replacement string", kEsc);
        l -= size_t(p + 1 - news);
        news = p + 1;
    }
    luaL_addlstring(b, news, l);
}
bool addValue(MS *ms, luaL_Buffer *b, const char *s, const char *e, int tr) {
    lua_State *L = ms->L;
    switch (tr) {
    case LUA_TFUNCTION: {
        lua_pushvalue(L, 3);
        const int n = pushCaptures(ms, s, e);
        lua_call(L, n, 1);
        break;
    }
    case LUA_TTABLE:
        pushOneCapture(ms, 0, s, e);
        lua_gettable(L, 3);
        break;
    default: addS(ms, b, s, e); return true;
    }
    if (!lua_toboolean(L, -1)) { lua_pop(L, 1); luaL_addlstring(b, s, size_t(e - s)); return false; }
    if (!lua_isstring(L, -1)) luaL_error(L, "invalid replacement value (a %s)", luaL_typename(L, -1));
    luaL_addvalue(b);
    return true;
}
int s_gsub(lua_State *L) {
    size_t srcl, lp;
    const char *src = luaL_checklstring(L, 1, &srcl);
    const char *p = luaL_checklstring(L, 2, &lp);
    const char *lastmatch = nullptr;
    const int tr = lua_type(L, 3);
    const lua_Integer max_s = luaL_optinteger(L, 4, lua_Integer(srcl) + 1);
    const bool anchor = (*p == '^');
    lua_Integer n = 0;
    bool changed = false;
    MS ms;
    luaL_Buffer b;
    luaL_argexpected(L, tr == LUA_TNUMBER || tr == LUA_TSTRING || tr == LUA_TFUNCTION || tr == LUA_TTABLE, 3, "string/function/table");
    screen(L, srcl, p, lp);
    luaL_buffinit(L, &b);
    if (anchor) { p++; lp--; }
    prepState(&ms, L, src, srcl, p, lp);
    while (n < max_s) {
        reprepState(&ms);
        const char *e = match(&ms, src, p);
        if (e && e != lastmatch) {
            n++;
            changed = addValue(&ms, &b, src, e, tr) || changed;
            src = lastmatch = e;
        } else if (src < ms.src_end) luaL_addchar(&b, *src++);
        else break;
        if (anchor) break;
    }
    if (!changed) lua_pushvalue(L, 1);
    else {
        luaL_addlstring(&b, src, size_t(ms.src_end - src));
        luaL_pushresult(&b);
    }
    lua_pushinteger(L, n);
    return 2;
}

}  // namespace

void installSafePatterns(lua_State *L, PatternExpired expired) {
    g_expired = expired;
    lua_getglobal(L, "string");
    lua_pushcfunction(L, s_find); lua_setfield(L, -2, "find");
    lua_pushcfunction(L, s_match); lua_setfield(L, -2, "match");
    lua_pushcfunction(L, s_gmatch); lua_setfield(L, -2, "gmatch");
    lua_pushcfunction(L, s_gsub); lua_setfield(L, -2, "gsub");
    lua_pop(L, 1);
}

}  // namespace hn::plugins
