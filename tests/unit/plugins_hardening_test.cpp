// Runtime hardening: bounded pattern matcher (differential vs stock Lua + measured explosions), other single-C-call
// stalls, and the out-of-memory regression for host functions (meaningful under ASan/LSan).
#include "plugins_test_util.h"
#include <QElapsedTimer>
#include <lua.hpp>

using namespace hn::plugins;
using namespace hn::plugins::test;

static QByteArray cmd(const QByteArray &body) { return "hn.command{id='c',title='C',run=function()\n" + body + "\nend}\n"; }

// Runs `body` (which must leave a table of strings in `results`) in a stock, unsandboxed Lua state.
static QStringList runStock(const QByteArray &body, double *ms = nullptr, int only = 0) {
    lua_State *L = luaL_newstate();
    luaL_openlibs(L);
    const QByteArray src = "local results = {}\nlocal ONLY = " + QByteArray::number(only) + "\n" + body + "\nreturn results";
    QStringList out;
    QElapsedTimer t;
    t.start();
    if (luaL_loadstring(L, src.constData()) == LUA_OK && lua_pcall(L, 0, 1, 0) == LUA_OK) {
        const int n = int(lua_rawlen(L, -1));
        for (int i = 1; i <= n; ++i) { lua_rawgeti(L, -1, i); out << QString::fromUtf8(lua_tostring(L, -1)); lua_pop(L, 1); }
    } else out << QString("STOCK ERROR: ") + lua_tostring(L, -1);
    if (ms) *ms = double(t.nsecsElapsed()) / 1e6;
    lua_close(L);
    return out;
}

static const char kDiffScript[] = R"LUA(
local function ser(...) local t = table.pack(...) for i = 1, t.n do t[i] = tostring(t[i]):gsub('^%[string .-%]:%d+: ', ''):gsub('^main%.lua:%d+: ', '') end return table.concat(t, ',') end
local function try(f, ...) return ser(pcall(f, ...)) end
local function gm(s, p) local o = {} for a, b in s:gmatch(p) do o[#o + 1] = tostring(a) .. ':' .. tostring(b) if #o > 40 then break end end return table.concat(o, ';') end
local function digest(s) local h = 5381 for i = 1, #s do h = (h * 33 + s:byte(i)) & 0xffffffff end return tostring(h) end
local function all(s, p)
  local line = table.concat({
    try(string.find, s, p), try(string.match, s, p), try(string.find, s, p, 3), try(string.find, s, p, -2), try(string.find, s, p, 1, true),
    try(gm, s, p), try(string.gsub, s, p, '<%0>'), try(string.gsub, s, p, '%1'),
    try(string.gsub, s, p, function(a) return '[' .. tostring(a) .. ']' end), try(string.gsub, s, p, {a = 'A', b = false}), try(string.gsub, s, p, 'x', 2),
  }, ' | ')
  results[#results + 1] = (ONLY == #results + 1) and line or digest(line)
end
local subjects = {'', 'a', 'abc', 'hello world', '  trim me  ', 'key = value', 'x=1, y=22, z=333', 'THE (quick) [brown] fox', 'aaa', 'a.b*c',
  '\xe9\xff', 'f(a(b)c)d', 'one\ntwo\n', 'abcabc abcabc', '2024-10-01', 'a]b^c-d%e'}
local pats = {'', 'a', 'a*', 'a-', 'a+', 'a?', '.', '.-', '.*', '^a', 'c$', '^$', '%a+', '%d+', '%s*(%S+)%s*', '(%w+)=(%w+)', '(%w+)%s*=%s*(%w+)', '()', '()a()',
  '(a)(b)(c)', '%b()', '%b[]', '%f[%w]%w+', '%f[%W]', '[%a_][%w_]*', '[^%s]+', '[a-c]+', '[%d%-]+', '[]]', '[^]]', '(.)%1', '(%a+) %1', '%1', '(', ')', '[a', '%', '%b',
  '%f', '(()', '[%', 'a%', '%g+', '%p', '%x+', '%u%l+', '(.-)%s*$', '^%s*(.-)%s*$', '(%d+)-(%d+)-(%d+)', '[+-]?%d+', 'a.-c', '.-$', '%.', 'a*$', '$a', '^^', 'a^',
  '[%a-z]', '[a-]', '[-a]', '[a-%%]', '%c', '%S+', '%W', '%A+', '%D', '(a*(.)%w(%s*))', '%z', '[%]]', '.-b.-c', '(a?)(b?)(c?)'}
for _, s in ipairs(subjects) do for _, p in ipairs(pats) do all(s, p) end end
math.randomseed(1234)
local toks = {'a', 'b', '.', '%a', '%d', '%s', '[ab]', '[^a]', '*', '+', '-', '?', '(', ')', '^', '$', '%1', '%b()', '%f[ab]', 'c', ' ', '%w', '()'}
local chars = {'a', 'b', ' ', 'c', '1', '(', ')'}
for i = 1, 6000 do
  local p = {} for j = 1, math.random(1, 8) do p[j] = toks[math.random(#toks)] end
  local s = {} for j = 1, math.random(0, 10) do s[j] = chars[math.random(#chars)] end
  all(table.concat(s), table.concat(p))
end
)LUA";

class HardeningTest : public QObject {
    Q_OBJECT
    qint64 timed(Rig &r, const QString &id, const QByteArray &body, QString *err, bool *ok, const QStringList &perms = {}) {
        if (!r.addLua(id, perms, cmd(body))) { *err = "setup failed"; *ok = false; return -1; }
        QElapsedTimer t;
        t.start();
        *ok = r.mgr->runCommand(id + ":c", &r.note, err);
        return t.elapsed();
    }
private slots:
    void boundedMatcherAgreesWithStockLua() {
        Rig r;
        QVERIFY(r.addLua("diff", {"storage"}, cmd("local results = {}\nlocal ONLY = 0\n" + QByteArray(kDiffScript) + "\nhn.storage.set('res', results)")));
        QString err;
        QElapsedTimer t;
        t.start();
        QVERIFY2(r.mgr->runCommand("diff:c", &r.note, &err), qPrintable(err));
        const qint64 sbMs = t.elapsed();
        const auto doc = QJsonDocument::fromJson(readFile(r.env.state() + "/plugin-data/diff.json")).object()["kv"].toObject()["res"].toArray();
        double stockMs = 0;
        const QStringList stock = runStock(kDiffScript, &stockMs);
        QVERIFY2(!stock.value(0).startsWith("STOCK ERROR"), qPrintable(stock.value(0)));
        QCOMPARE(int(doc.size()), int(stock.size()));
        QVERIFY(doc.size() > 7000);
        for (int i = 0; i < doc.size(); ++i)
            if (doc[i].toString() != stock[i]) {
                const QString want = runStock(kDiffScript, nullptr, i + 1).value(i);
                Rig r2;
                r2.addLua("diff2", {"storage"}, cmd("local results = {}\nlocal ONLY = " + QByteArray::number(i + 1) + "\n" + QByteArray(kDiffScript) + "\nhn.storage.set('res', results)"));
                r2.mgr->runCommand("diff2:c", &r2.note);
                const QString got = QJsonDocument::fromJson(readFile(r2.env.state() + "/plugin-data/diff2.json")).object()["kv"].toObject()["res"].toArray().at(i).toString();
                QFAIL(qPrintable(QString("case %1 differs\n stock:   %2\n sandbox: %3").arg(i + 1).arg(want, got)));
            }
        qInfo().noquote() << QString("pattern differential: %1 (subject, pattern) cases x 11 operations (find/match/gmatch/gsub incl. errors) identical to stock Lua 5.5; sandbox %2 ms, stock %3 ms")
                                 .arg(doc.size()).arg(sbMs).arg(stockMs, 0, 'f', 0);
    }

    void exponentialPatternsAreAbortedQuickly() {
        struct Case { const char *name, *body; };
        const Case cases[] = {
            {"six lazy groups, 5000 chars", "return (('a'):rep(5000)):find('(.-)(.-)(.-)(.-)(.-)(.-)c')"},
            {"six lazy groups via match", "return (('a'):rep(300) .. 'b'):match('(.-)(.-)(.-)(.-)(.-)(.-)c')"},
            {"a?^20 a^20 on a^20 (classic 2^n)", "local n = 20 return (('a'):rep(n)):find(('a?'):rep(n) .. ('a'):rep(n))"},
            {"a?^24 a^24 on a^24 (match)", "local n = 24 return (('a'):rep(n)):match(('a?'):rep(n) .. ('a'):rep(n))"},
            {"a?^30 a^30 on a^30 (static reject)", "local n = 30 return (('a'):rep(n)):find(('a?'):rep(n) .. ('a'):rep(n))"},
            {"nested star chain on 200 KiB", "return (('a'):rep(200000)):match('.*.*.*.*b')"},
            {"gsub with lazy groups", "return (('a'):rep(3000)):gsub('(.-)(.-)(.-)(.-)x', '%1')"},
            {"gmatch with lazy groups", "for a in (('ab'):rep(3000)):gmatch('(.-)(.-)(.-)(.-)(.-)z') do end"},
            {"backreference blowup", "return (('a'):rep(2000)):find('(a*)(a*)(a*)(a*)%1%2%3%4b')"},
            {"quadratic scan on 1 MiB", "return (('a'):rep(1048576)):find('a.-b')"},
        };
        Rig r;
        qint64 worst = 0;
        for (int i = 0; i < int(std::size(cases)); ++i) {
            QString err;
            bool ok;
            const qint64 ms = timed(r, QString("ex%1").arg(i), cases[i].body, &err, &ok);
            worst = qMax(worst, ms);
            qInfo().noquote() << QString("pattern bomb \"%1\": aborted after %2 ms with '%3'").arg(cases[i].name).arg(ms).arg(err.left(60));
            QVERIFY2(!ok, cases[i].name);
            QVERIFY2(err.contains("too complex") || err.contains("time budget"), qPrintable(err));
            QVERIFY2(ms < 250, qPrintable(QString("%1: %2 ms").arg(cases[i].name).arg(ms)));
        }
        qInfo().noquote() << QString("pattern bombs: worst abort time %1 ms (callback budget for commands is 2000 ms, events 50 ms)").arg(worst);
        // the same bomb inside an event handler is stopped by the 50 ms wall budget at the latest
        QVERIFY(r.addLua("exev", {}, "hn.on('note.opened', function() return (('a'):rep(5000)):find('(.-)(.-)(.-)(.-)(.-)(.-)c') end)\n"));
        QElapsedTimer t;
        t.start();
        r.mgr->post("note.opened", "x", &r.note);
        qInfo().noquote() << QString("pattern bomb in a 50 ms event handler: %1 ms").arg(t.elapsed());
        QVERIFY2(t.elapsed() < 120, qPrintable(QString::number(t.elapsed())));
        QCOMPARE(r.mgr->trust()->record("exev").failures, 1);
        // pcall cannot be used to retry the bomb in a loop for longer than the budget
        QString err;
        bool ok;
        const qint64 ms = timed(r, "exloop", "while true do pcall(string.find, ('a'):rep(5000), '(.-)(.-)(.-)(.-)(.-)(.-)c') end", &err, &ok);
        qInfo().noquote() << QString("pcall(pattern bomb) in a loop: command aborted after %1 ms (budget 2000)").arg(ms);
        QVERIFY(!ok);
        QVERIFY2(ms < 2400, qPrintable(QString::number(ms)));
    }

    void normalPatternsStillWorkAndAreAsFast() {
        const QByteArray work = R"LUA(
local words = {} for i = 1, 20000 do words[i] = 'word' .. i end
local text = table.concat(words, '  \t ')
local t1 = text:gsub('%s+', ' ')
results[#results + 1] = #t1
local n = 0 for w in text:gmatch('%w+') do n = n + 1 end
results[#results + 1] = n
local c = 0 for i = 1, 50000 do local a, b = ('key' .. i .. ' = value' .. i):match('^(%w+)%s*=%s*(%w+)$') if a then c = c + 1 end end
results[#results + 1] = c
local big = ('lorem ipsum dolor sit amet\n'):rep(30000)
local lines = 0 for l in big:gmatch('[^\n]+') do lines = lines + 1 end
results[#results + 1] = lines
results[#results + 1] = tostring(big:find('amet\nlorem ipsum dolor sit amet\nX', 1, true))
results[#results + 1] = select(2, big:gsub('ipsum', 'IPSUM'))
local up = big:gsub('(%a)(%a*)', function(a, b) return a:upper() .. b end)
results[#results + 1] = #up
results[#results + 1] = tostring(big:find('zzz'))
)LUA";
        Rig r;
        QVERIFY(r.addLua("fast", {"storage"}, "hn.command{id='c',title='C',run=function() local results = {}\n" + work + "\nhn.storage.set('res', results) end}\n"));
        // 2 s command budget: time inside the plugin
        QElapsedTimer t;
        t.start();
        QString err;
        QVERIFY2(r.mgr->runCommand("fast:c", &r.note, &err), qPrintable(err));
        const double sb = double(t.nsecsElapsed()) / 1e6;
        const auto got = QJsonDocument::fromJson(readFile(r.env.state() + "/plugin-data/fast.json")).object()["kv"].toObject()["res"].toArray();
        double stock = 0;
        const QStringList want = runStock(work, &stock);
        QCOMPARE(int(got.size()), int(want.size()));
        for (int i = 0; i < got.size(); ++i) QCOMPARE(got[i].toVariant().toString(), want[i]);
        qInfo().noquote() << QString("normal patterns: sandboxed %1 ms vs stock Lua %2 ms (ratio %3, includes the instruction hook and the 4M-step bookkeeping); results identical")
                                 .arg(sb, 0, 'f', 1).arg(stock, 0, 'f', 1).arg(sb / stock, 0, 'f', 2);
        QVERIFY2(sb < stock * 2.5 + 20, "sandboxed pattern work should be close to stock speed");
    }

    void limitsAreEnforced() {
        Rig r;
        QString err;
        bool ok;
        timed(r, "lim1", "return (('a'):rep(1048576)):find('b')", &err, &ok);
        QVERIFY2(ok, qPrintable(err));  // exactly at the cap is fine
        timed(r, "lim2", "return (('a'):rep(1048577)):find('b')", &err, &ok);
        QVERIFY(!ok);
        QVERIFY2(err.contains("string too long for pattern matching"), qPrintable(err));
        timed(r, "lim2b", "return (('a'):rep(1048577)):find('b', 1, true)", &err, &ok);
        QVERIFY(!ok);
        timed(r, "lim3", "return ('x'):find(('a'):rep(257))", &err, &ok);
        QVERIFY(!ok);
        QVERIFY2(err.contains("pattern too long"), qPrintable(err));
        timed(r, "lim4", "return ('x'):find(('a?'):rep(25))", &err, &ok);
        QVERIFY(!ok);
        QVERIFY2(err.contains("more than 24 quantifiers"), qPrintable(err));
        timed(r, "lim5", "return ('x'):find(('a?'):rep(24))", &err, &ok);
        QVERIFY2(ok, qPrintable(err));
        timed(r, "lim6", "return ('x'):find('[' .. ('a?'):rep(100) .. ']')", &err, &ok);  // inside a set they are literals
        QVERIFY2(ok, qPrintable(err));
        timed(r, "lim7", "for w in ('x'):rep(2000000):gmatch('x') do end", &err, &ok);
        QVERIFY(!ok);
        timed(r, "lim8", "local ok = pcall(string.gsub, ('a'):rep(1000), '.', ('x'):rep(100000)) hn.log(tostring(ok))", &err, &ok);  // 100 MB result: allocator cap stops it
        QVERIFY2(ok, qPrintable(err));
        QCOMPARE(r.logsOf("lim8"), QStringList{"false"});
        qInfo().noquote() << "pattern limits: subject <= 1 MiB, pattern <= 256 bytes, <= 24 quantifiers, 4M matcher steps per call, wall budget polled every 1024 steps";
    }

    void otherSingleCallStallsAreBounded() {
        struct Case { const char *name, *body; bool ok; const char *errPart; };
        const Case cases[] = {
            {"string.rep('', 1e12)", "return string.rep('', 1e12)", true, ""},
            {"string.rep('', 1e12, '')", "return #string.rep('', 1e12, '')", true, ""},
            {"string.rep('x', 1e12)", "return string.rep('x', 1e12)", false, "too large"},
            {"table.move huge", "return table.move({}, 1, 2^40, 2)", false, "range too large"},
            {"table.move negative-to-positive", "return table.move({}, -2^62, 2^62, 1)", false, "range too large"},
            {"table.move normal", "local t = table.move({1, 2, 3}, 1, 3, 2) return t[2] + t[3] + t[4]", true, ""},
            {"table.sort 110k", "local t = {} for i = 1, 110000 do t[i] = (i * 7919) % 100003 end table.sort(t)", false, "too large"},
            {"table.sort 90k", "local t = {} for i = 1, 90000 do t[i] = (i * 7919) % 100003 end table.sort(t) hn.log(t[1] .. ',' .. t[#t])", true, ""},
            {"table.sort bad comparator", "local t = {} for i = 1, 5000 do t[i] = i end table.sort(t, function() return true end)", false, "invalid order function"},
            {"table.sort comparator that loops", "table.sort({3, 2, 1}, function() while true do end end)", false, "budget"},
            {"table.sort mixed types", "table.sort({1, 'a', 2})", false, "attempt to compare"},
            {"table.concat past the end", "return table.concat({}, ',', 1, 1e9)", false, "invalid value"},
            {"table.concat metamethod flood", "return #table.concat(setmetatable({}, {__index = function() return 'x' end}), ',', 1, 1e9)", false, ""},
            {"table.unpack huge", "return table.unpack({}, 1, 1e8)", false, "too many results"},
            {"table.insert middle of 100k", "local t = {} for i = 1, 100000 do t[i] = i end for i = 1, 20 do table.insert(t, 1, i) end hn.log(#t)", true, ""},
            {"string.format('%99999d')", "return string.format('%99999d', 1)", false, "invalid conversion"},
            {"string.format('%.99f' huge)", "return #string.format('%99.99f', 1e308)", true, ""},
            {"string.format('%s' 2 MiB)", "return #string.format('%s', ('x'):rep(2000000))", true, ""},
            {"string.byte 3 MB slice", "return string.byte(('x'):rep(3000000), 1, -1)", false, "too long"},
            {"string.char 1M args", "return string.char(table.unpack({}, 1, 1e6))", false, ""},
            {"utf8.char many", "return utf8.char(table.unpack({}, 1, 1e6))", false, ""},
            {"utf8.codepoint slice", "return utf8.codepoint(('x'):rep(2000000), 1, -1)", false, "too long"},
            {"utf8.len 3 MB", "return utf8.len(('\\u{20AC}'):rep(1000000))", true, ""},
            {"utf8.offset huge n", "return utf8.offset(('x'):rep(1000), 1e18)", true, ""},
            {"utf8.codes loop", "local n = 0 for _, c in utf8.codes(('\\u{20AC}'):rep(200000)) do n = n + 1 end hn.log(n)", true, ""},
            {"string.pack c1e9", "return #string.pack('c1000000000', 'a')", false, ""},
            {"string.packsize huge format", "return string.packsize(('i8'):rep(1000000))", true, ""},
            {"string.unpack short data", "return string.unpack(('b'):rep(1000000), 'abc')", false, ""},
            {"math.random huge range", "return math.random(math.mininteger, math.maxinteger) ~= nil", true, ""},
            {"math.fmod/pow extremes", "return math.fmod(1e308, 1e-308) + 2^1e10 + math.tointeger(2^53)", true, ""},
            {"integer loop with tiny step", "for i = 1, 1e18 do end", false, "budget"},
            {"collectgarbage in a loop", "for i = 1, 20000 do collectgarbage() end", true, ""},
            {"load 1 MiB junk", "local s = ('x=1\\n'):rep(260000) local f = load(s) hn.log(type(f))", true, ""},
            {"load deep nesting", "return load('return ' .. ('('):rep(100000) .. '1' .. (')'):rep(100000))", true, ""},
            {"load huge table constructor", "local f = load('return {' .. ('1,'):rep(500000) .. '}') hn.log(#f())", true, ""},
            {"tostring/tonumber long digits", "return tonumber(('9'):rep(1000000))", true, ""},
            {"select with huge index", "return select(1e9, 1, 2)", true, ""},
            {"next on big table", "local t = {} for i = 1, 300000 do t[i] = i end local n = 0 for _ in pairs(t) do n = n + 1 end hn.log(n)", true, ""},
            {"setmetatable __index chain", "local a = {} for i = 1, 100000 do a = setmetatable({}, {__index = a}) end return a.x", false, ""},
            {"string.reverse/upper 3 MB", "local s = ('x'):rep(3000000) return #s:reverse():upper():lower()", true, ""},
            {"hn.json.encode wide", "local t = {} for i = 1, 150000 do t[i] = i end return #hn.json.encode(t)", true, ""},
            {"hn.json.decode 3 MB", "return hn.json.decode('[' .. ('1,'):rep(1500000) .. '1]')", false, ""},
        };
        Rig r;
        qint64 worst = 0;
        QString worstName;
        for (int i = 0; i < int(std::size(cases)); ++i) {
            QString err;
            bool ok;
            const qint64 ms = timed(r, QString("st%1").arg(i), cases[i].body, &err, &ok);
            if (ms > worst) { worst = ms; worstName = cases[i].name; }
            if (cases[i].ok) QVERIFY2(ok, qPrintable(QString("%1: %2").arg(cases[i].name, err)));
            else {
                QVERIFY2(!ok, cases[i].name);
                QVERIFY2(QString(err).contains(cases[i].errPart), qPrintable(QString("%1: %2").arg(cases[i].name, err)));
            }
            const bool loopsUntilBudget = QString(cases[i].errPart) == "budget";
            QVERIFY2(ms < (loopsUntilBudget ? 2400 : 400), qPrintable(QString("%1 took %2 ms").arg(cases[i].name).arg(ms)));
            if (ms > 100 && !loopsUntilBudget) qInfo().noquote() << QString("  slow-ish: %1 took %2 ms").arg(cases[i].name).arg(ms);
        }
        qInfo().noquote() << QString("%1 other single-C-call stall candidates checked; slowest non-looping case: %2 (%3 ms)").arg(int(std::size(cases))).arg(worstName).arg(worst);
    }

    void integratorHelpers() {
        QCOMPARE(QString(kNativeWarning), QString("This plugin runs native code with full access to your user account. It is not sandboxed."));
        QCOMPARE(QString(kThirdPartyWarning), QString("Third-party plugins are not reviewed by the Hyprnotes project. A plugin can read or change your notes and, depending on "
                                                      "its permissions, send data over the network. Only install plugins from sources you trust."));
        QCOMPARE(permissionTable().size(), knownPermissions().size());
        QStringList dangerous;
        for (const auto &e : permissionTable()) {
            QVERIFY(!e.description.isEmpty() && e.description != "Unknown permission");
            QVERIFY(QStringList({"low", "medium", "high", "critical"}).contains(e.risk));
            QCOMPARE(e.dangerous, isDangerous(e.name));
            QCOMPARE(describePermission(e.name), e.description);
            if (e.dangerous) dangerous << e.name;
        }
        QCOMPARE(dangerous, (QStringList{"notes.write", "clipboard", "network", "theme", "native"}));
        QVERIFY(!isDangerous("ui") && !isDangerous("bogus"));
        QCOMPARE(describePermission("bogus"), QString("Unknown permission"));

        const QStringList h{"api.example.com"};
        QVERIFY(isHostAllowed("https://api.example.com/v1/x?y=1", h));
        QVERIFY(isHostAllowed("HTTPS://API.Example.COM:443/x", h));
        for (const char *bad : {"http://api.example.com/x", "https://evil.example.com/x", "https://api.example.com.evil.com/x", "https://evil.com/?h=api.example.com",
                                "https://user:pw@api.example.com/x", "https://api.example.com:8443/x", "//api.example.com/x", "/relative", "ftp://api.example.com/",
                                "https://api.example.com\@evil.com/", "https://127.0.0.1/", "", "https://api.example.com/a b"})
            QVERIFY2(!isHostAllowed(bad, h), bad);
        QVERIFY(!isHostAllowed("https://api.example.com/x", {}));

        // app.started helper: delivered once, to subscribers only, with the app version
        Rig r;
        QVERIFY(r.addLua("starter", {}, "hn.on('app.started', function(v) hn.log('started ' .. v) end)\n"));
        r.mgr->appStarted();
        QCOMPARE(r.logsOf("starter"), QStringList{"started 0.1.0"});
        Rig quiet;
        quiet.mgr->appStarted();
        QCOMPARE(quiet.mgr->loadedStates(), 0);
    }

    // Under ASan, LeakSanitizer fails this binary if any C++ object was skipped by a longjmp from an out-of-memory error.
    void outOfMemoryInsideHostFunctionsDoesNotLeak() {
        Rig r;
        r.mgr->host()->env().memoryCap = 2 * 1024 * 1024;
        r.note.t = QString(600000, QLatin1Char('x'));  // a note too big to hand to a heap that is nearly full
        r.note.tg = QStringList{"alpha", "beta", QString(5000, QLatin1Char('t'))};
        const QByteArray body = R"LUA(
local J, FN, PICK, STR = {a = {1, 2, 3}, b = 'x'}, function() end, {'a', 'b'}, ('s'):rep(100000)
local calls = {
  {hn.note.tags}, {hn.note.text}, {hn.note.selection}, {hn.notes.list, 'q'}, {hn.notes.read, 'a.md'},
  {hn.json.decode, '{"a":[1,2,3,{"b":"c"}],"d":"eeeeeeeeeeeeeeeeeeeeeeeeeeeeee","n":[[],[],[],[]]}'}, {hn.json.encode, J},
  {hn.storage.set, 'k', J}, {hn.storage.set, 'big', STR}, {hn.storage.get, 'k'}, {hn.storage.get, 'big'}, {hn.settings.get, 's1'},
  {hn.ui.notify, 'x'}, {hn.ui.pick, 't', PICK}, {hn.ui.prompt, 'a', 'b', 'c'}, {hn.clipboard.get},
  {hn.command, {id = 'z', title = 'T', run = FN}}, {hn.on, 'note.opened', FN}, {hn.trigger, {pattern = '::pp', replace = FN}},
  {hn.setting, {id = 's1', type = 'string', title = 't', default = 'd'}},
  {hn.http.get, 'https://api.example.com/x', {headers = {A = 'b', B = 5}}}, {hn.http.post, 'https://api.example.com/x', 'body'},
  {require, 'nope'}, {string.rep, 'y', 1e6},
}
local errs = {} for i = 1, 64 do errs[i] = false end
local keep = {}
local cur
local function add() keep[#keep + 1] = cur .. #keep end
local function fill(sz)
  while true do
    local ok, s = pcall(string.rep, 'x', sz)
    if not ok then return end
    cur = s
    if not pcall(add) then return end
  end
end
fill(65536) fill(4096) fill(256) fill(16)
for i = 1, 40 do keep[#keep] = nil end  -- a little air for the plugin's own bookkeeping; the heap stays full
local oks, bad = 0, 0
local function round()
  for i = 1, #calls do
    local c = calls[i]
    local ok, e = pcall(c[1], c[2], c[3], c[4])
    if ok then oks = oks + 1 else bad = bad + 1 errs[bad % 64 + 1] = e end
  end
end
local function rounds() for r = 1, 400 do pcall(round) end end
pcall(rounds)
keep = nil collectgarbage()
hn.log('ok=' .. oks .. ' errs=' .. bad)
local seen = {}
for i = 1, 64 do local e = errs[i] if type(e) == 'string' then seen[(e:gsub('^.-:%d+: ', ''))] = true end end
local names = {} for k in pairs(seen) do names[#names + 1] = k end table.sort(names)
for i = 1, math.min(#names, 14) do hn.log('err: ' .. names[i]) end
)LUA";
        QString err;
        bool ok;
        const QStringList perms{"note.read", "notes.read", "storage", "ui", "clipboard", "network"};
        QVERIFY(r.addLua("oom", perms, cmd(body), {"api.example.com"}));
        QElapsedTimer t;
        t.start();
        ok = r.mgr->runCommand("oom:c", &r.note, &err);
        for (const auto &l : r.logsOf("oom")) qInfo().noquote() << "  oom plugin:" << l;
        QVERIFY2(ok, qPrintable(err));
        QVERIFY(r.mgr->host()->memoryUsed("oom") <= 2u * 1024 * 1024);
        const QStringList logs = r.logsOf("oom");
        QVERIFY(logs.value(0).startsWith("ok="));
        // the oversize note is refused cleanly instead of raising an out-of-memory error through C++ frames
        bool sawClean = false, sawRawOom = false;
        for (const auto &l : logs) { sawClean |= l.contains("value too large for the plugin memory limit"); sawRawOom |= l.contains("err: not enough memory"); }
        QVERIFY(sawClean);
        qInfo().noquote() << QString("oom regression: 400 rounds of every host call against a full 2 MiB heap in %1 ms; raw 'not enough memory' from Lua-side code seen: %2").arg(t.elapsed()).arg(sawRawOom);
        // the plugin and the host are still usable afterwards
        QVERIFY(r.addLua("after", {}, cmd("hn.log('alive')")));
        QVERIFY(r.mgr->runCommand("after:c", &r.note));
    }
};
QTEST_MAIN(HardeningTest)
#include "plugins_hardening_test.moc"
