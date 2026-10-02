// Plugin API v2 runtime: permissions, api gate, index calls (caps, budgets, rate limit, query validation), panels,
// completion, link activation, registry cache, laziness. Everything runs against fake bridges.
#include "plugins_test_util.h"
#include <QElapsedTimer>
#include <QJsonDocument>

using namespace hn::plugins;
using namespace hn::plugins::test;

static QByteArray cmd(const QString &body, const QString &extra = QString()) {
    return (extra + "\nhn.command{id='t',title='T',run=function()\n" + body + "\nend}\n").toUtf8();
}
static const QStringList kAll{"note.read", "notes.read", "notes.write", "notes.index", "ui.panel", "editor.complete", "editor.links", "storage", "ui"};

static QJsonValue jlog(Rig &r, const QString &id) {  // the last hn.log line parsed as JSON
    const auto l = r.logsOf(id);
    return l.isEmpty() ? QJsonValue() : QJsonDocument::fromJson("[" + l.last().toUtf8() + "]").array().at(0);
}
static QString auditText(Rig &r) { return QString::fromUtf8(readFile(r.env.state() + "/plugins-audit.jsonl")); }

class V2Test : public QObject {
    Q_OBJECT
    static bool add(Rig &r, const QString &id, const QStringList &perms, const QByteArray &lua, QString *err = nullptr) { return r.addLua(id, perms, lua, {}, err, 2); }
    static bool runT(Rig &r, const QString &id, QString *err = nullptr) { return r.mgr->runCommand(id + ":t", &r.note, err); }

private slots:
    // ---------------------------------------------------------------- manifest / permissions
    void permissionTableHasTheNewEntries() {
        QMap<QString, PermissionInfo> t;
        for (const auto &p : permissionTable()) t[p.name] = p;
        QCOMPARE(t.size(), 14);
        QCOMPARE(t["notes.index"].risk, QString("medium"));
        QCOMPARE(t["ui.panel"].risk, QString("low"));
        QCOMPARE(t["editor.complete"].risk, QString("low"));
        QCOMPARE(t["editor.links"].risk, QString("medium"));
        QCOMPARE(t["notes.index"].description, QString("Read link, tag and task data about all notes"));
        for (const char *p : {"notes.index", "ui.panel", "editor.complete", "editor.links"}) {
            QVERIFY(!t[p].dangerous);
            QVERIFY(!t[p].description.contains("Unknown"));
            QCOMPARE(permissionMinApi(p), 2);
        }
        QCOMPARE(permissionMinApi("notes.read"), 1);
    }
    void manifestGatesNewPermissionsByApiVersion() {
        auto parse = [](int api, const QStringList &perms, QString *msg = nullptr) {
            Manifest m;
            QList<PluginError> e;
            const bool ok = parseManifest(manifestJson("v2p", perms, {}, "script", "main.lua", api), "/x/v2p", &m, &e);
            if (msg && !e.isEmpty()) *msg = e[0].message;
            return ok;
        };
        QString msg;
        QVERIFY(parse(2, {"notes.index", "ui.panel", "editor.complete", "editor.links", "notes.read"}));
        QVERIFY(parse(1, {"notes.read", "ui"}));
        QVERIFY(!parse(1, {"notes.index"}, &msg));
        QVERIFY2(msg.contains("needs \"api\": 2"), qPrintable(msg));
        QVERIFY(!parse(3, {}, &msg));
        QVERIFY2(msg.contains("supports API 2"), qPrintable(msg));
        QVERIFY(!parse(0, {}));
        // the installer refuses it too
        Rig r;
        const QString dir = writePlugin(r.env.src(), "bad-gate", {"ui.panel"}, "-- x", {}, 1);
        QVERIFY(!r.mgr->install(dir).ok);
    }
    void api1PluginsCannotUseApi2() {
        Rig r;
        QVERIFY(r.addLua("old", {"notes.read", "notes.write", "ui"}, cmd(R"LUA(
          hn.log(tostring(hn.panel), tostring(hn.panel_refresh), tostring(hn.complete), tostring(hn.link_handler))
          hn.log(tostring(hn.notes.links), tostring(hn.notes.backlinks), tostring(hn.notes.resolve), tostring(hn.notes.frontmatter),
                 tostring(hn.notes.query), tostring(hn.notes.open), tostring(hn.notes.rename))
          local ok, e = pcall(hn.on, 'link.activate', function() end)
          hn.log(tostring(ok), e)
        )LUA")));
        QVERIFY(runT(r, "old"));
        const auto l = r.logsOf("old");
        QCOMPARE(l[0], QString("nil\tnil\tnil\tnil"));
        QCOMPARE(l[1], QString("nil\tnil\tnil\tnil\tnil\tnil\tnil"));
        QVERIFY2(l[2].contains("false") && l[2].contains("unknown event"), qPrintable(l[2]));
        // and an api-1 plugin keeps working exactly as before
        QVERIFY(r.mgr->registry()->panels().isEmpty() && r.mgr->registry()->completions().isEmpty());
    }
    void everyNewCallIsPermissionChecked() {
        Rig r;
        QString err;
        const QString dir = writePlugin(r.env.src(), "nogrant", kAll, cmd(R"LUA(
          local function try(name, f, ...)
            local ok, e = pcall(f, ...)
            hn.log(name .. '|' .. tostring(ok) .. '|' .. tostring(e))
          end
          try('links', hn.notes.links, 'notes/a.md')
          try('backlinks', hn.notes.backlinks, 'notes/a.md')
          try('resolve', hn.notes.resolve, 'b')
          try('frontmatter', hn.notes.frontmatter, 'notes/a.md')
          try('query', hn.notes.query, {from = 'notes'})
          try('open', hn.notes.open, 'notes/a.md')
          try('rename', hn.notes.rename, 'notes/a.md', 'z.md')
          try('panel', hn.panel, {id = 'p', title = 'P', render = function() return {} end})
          try('panel_refresh', hn.panel_refresh, 'p')
          try('complete', hn.complete, {id = 'c', trigger = '[[', items = function() return {} end})
          try('link_handler', hn.link_handler, {})
          try('on_link', hn.on, 'link.activate', function() end)
        )LUA"), {}, 2);
        QVERIFY2(r.setup(dir, {}, false, &err), qPrintable(err));  // consent to nothing
        QVERIFY(runT(r, "nogrant", &err));
        const QMap<QString, QString> need{{"links", "notes.index"}, {"backlinks", "notes.index"}, {"resolve", "notes.index"}, {"frontmatter", "notes.index"},
                                          {"query", "notes.index"}, {"open", "notes.read"}, {"rename", "notes.write"}, {"panel", "ui.panel"},
                                          {"panel_refresh", "ui.panel"}, {"complete", "editor.complete"}, {"link_handler", "editor.links"}, {"on_link", "editor.links"}};
        const auto l = r.logsOf("nogrant");
        QCOMPARE(l.size(), need.size());
        for (const auto &line : l) {
            const auto p = line.split('|');
            QVERIFY2(p[1] == "false" && p[2].contains("permission denied: " + need[p[0]]), qPrintable(line));
        }
        QVERIFY(r.lib.indexCalls == 0 && r.lib.opened.isEmpty() && r.lib.renamed.isEmpty());  // nothing reached a bridge
        const QString a = auditText(r);
        for (const auto &perm : QSet<QString>(need.begin(), need.end())) QVERIFY2(a.contains("\"denied\"") && a.contains(perm), qPrintable(perm));
        QCOMPARE(a.count("\"denied\""), need.size());
        QVERIFY(r.mgr->registry()->panels().isEmpty());
    }

    // ---------------------------------------------------------------- index calls
    void indexCallsReturnPlainTables() {
        Rig r;
        QString err;
        QVERIFY(add(r, "idx", kAll, cmd(R"LUA(
          local out = {}
          out.links = hn.notes.links('notes/a.md')
          out.back = hn.notes.backlinks('notes/a.md', {limit = 1, offset = 1})
          out.res = hn.notes.resolve('b')
          out.dup = hn.notes.resolve('dup')
          out.fm = hn.notes.frontmatter('notes/a.md')
          out.q = hn.notes.query{from = 'tasks', where = {{field = 'done', op = '=', value = false}}, order = {{field = 'path'}}, limit = 5}
          hn.log(hn.json.encode(out))
        )LUA"), &err));
        QVERIFY2(runT(r, "idx", &err), qPrintable(err));
        const auto o = jlog(r, "idx").toObject();
        QCOMPARE(o["links"].toArray().size(), 2);
        const auto l0 = o["links"].toArray()[0].toObject();
        QCOMPARE(l0["target"].toString(), QString("b"));
        QCOMPARE(l0["resolved"].toString(), QString("b.md"));
        QCOMPARE(l0["line"].toInt(), 3);
        QVERIFY(!l0.contains("alias") && !l0.contains("anchor"));  // empty strings are absent, not ""
        QCOMPARE(o["links"].toArray()[1].toObject()["kind"].toString(), QString("embed"));
        QCOMPARE(o["back"].toArray().size(), 1);
        QCOMPARE(o["back"].toArray()[0].toObject()["src"].toString(), QString("c.md"));
        QCOMPARE(o["back"].toArray()[0].toObject()["anchor"].toString(), QString("intro"));
        QCOMPARE(r.lib.backlinkLimit, 1);
        QCOMPARE(r.lib.backlinkOffset, 1);
        QCOMPARE(o["res"].toObject()["status"].toString(), QString("resolved"));
        QCOMPARE(o["res"].toObject()["path"].toString(), QString("b.md"));
        QCOMPARE(o["dup"].toObject()["candidates"].toArray().size(), 2);
        QCOMPARE(o["fm"].toObject()["title"].toString(), QString("A"));
        QCOMPARE(o["fm"].toObject()["tags"].toArray().size(), 2);
        QCOMPARE(o["q"].toArray().size(), 2);
        QCOMPARE(o["q"].toArray()[0].toObject()["text"].toString(), QString("buy milk"));
        // spec arrived normalised
        const auto spec = r.lib.querySpecs.value(0);
        QCOMPARE(spec["from"].toString(), QString("tasks"));
        QCOMPARE(spec["limit"].toInt(), 5);
        QCOMPARE(spec["offset"].toInt(), 0);
        QCOMPARE(spec["order"].toArray()[0].toObject()["dir"].toString(), QString("asc"));
    }
    void notFoundIsASoftError() {
        Rig r;
        QVERIFY(add(r, "nf", kAll, cmd(R"LUA(
          local a, b = hn.notes.links('nope.md')
          hn.log(tostring(a) .. '|' .. tostring(b))
        )LUA")));
        QVERIFY(runT(r, "nf"));
        QCOMPARE(r.logsOf("nf").last(), QString("nil|not found"));
        QCOMPARE(r.mgr->trust()->record("nf").failures, 0);
    }
    void defaultBridgeSaysUnsupported() {  // an old LibraryBridge (no API-2 overrides) must degrade to nil, "unsupported..."
        struct OldLib : LibraryBridge {
            QList<NoteInfo> list(const QString &) override { return {}; }
            bool read(const QString &, QString *) override { return false; }
            QString create(const QString &, const QString &) override { return {}; }
            bool write(const QString &, const QString &) override { return true; }
            bool remove(const QString &) override { return true; }
        } old;
        TmpEnv env;
        ManagerConfig c;
        c.bridges.library = &old;
        QStringList logs;
        c.logger = [&](int, const QString &, const QString &m) { logs << m; };
        PluginManager mgr(c);
        mgr.scan();
        const QString dir = writePlugin(env.src(), "unsup", {"notes.index", "notes.read", "notes.write"}, cmd(R"LUA(
          for _, f in ipairs{function() return hn.notes.links('a.md') end, function() return hn.notes.resolve('b') end,
                             function() return hn.notes.query{from='notes'} end, function() return hn.notes.open('a.md') end,
                             function() return hn.notes.rename('a.md', 'b.md') end} do
            local a, b = f()
            hn.log(tostring(a) .. '|' .. tostring(b))
          end
        )LUA"), {}, 2);
        QVERIFY(mgr.install(dir, true).ok);
        QVERIFY(mgr.consent("unsup", {"notes.index", "notes.read", "notes.write"}));
        QVERIFY(mgr.enable("unsup"));
        QVERIFY(mgr.runCommand("unsup:t", nullptr));
        QCOMPARE(logs.size(), 5);
        for (const auto &l : logs) QVERIFY2(l.startsWith("nil|unsupported"), qPrintable(l));
    }
    void openAndRename() {
        Rig r;
        QString err;
        QVERIFY(add(r, "or", kAll, cmd(R"LUA(
          hn.log(tostring(hn.notes.open('notes/a.md')))
          hn.log(tostring(hn.notes.open('notes/a.md', {where = 'sticky'})))
          hn.log(select(2, pcall(hn.notes.open, 'notes/a.md', {where = 'window'})))
          hn.log(select(2, pcall(hn.notes.open, '../x.md')))
          hn.log(hn.notes.rename('notes/a.md', 'z.md'))
          hn.log(hn.notes.rename('notes/a.md', 'y.md', {update_links = true}))
          for _, bad in ipairs{"", 'a/b.md', '../x', '.hidden', '..', 'a' .. string.char(92) .. 'b', ('x'):rep(300), 'a' .. string.char(0) .. 'b'} do
            hn.log('bad:' .. tostring(pcall(hn.notes.rename, 'notes/a.md', bad)))
          end
          hn.log(select(2, pcall(hn.notes.rename, '/abs.md', 'x.md')))
        )LUA"), &err));
        QVERIFY2(runT(r, "or", &err), qPrintable(err));
        QCOMPARE(r.lib.opened, (QStringList{"notes/a.md@organizer", "notes/a.md@sticky"}));
        QCOMPARE(r.lib.renamed, (QStringList{"notes/a.md->z.md", "notes/a.md->y.md+links"}));  // update_links is opt-in
        const auto l = r.logsOf("or");
        QCOMPARE(l[0], QString("true"));
        QVERIFY(l[2].contains("where must be"));
        QVERIFY(l[3].contains("invalid note path"));
        QCOMPARE(l[4], QString("notes/z.md"));
        for (int i = 6; i < 14; ++i) QCOMPARE(l[i], QString("bad:false"));
        QVERIFY(l[14].contains("invalid note path"));
        QVERIFY(auditText(r).contains("notes.rename"));
    }
    void renameWithoutWriteNeverReachesTheBridge() {
        Rig r;
        QString err;
        // notes.index + notes.read approved, notes.write not
        QVERIFY(r.setup(writePlugin(r.env.src(), "nowrite", {"notes.index", "notes.read", "notes.write"}, cmd("hn.notes.rename('notes/a.md', 'evil.md', {update_links = true})"), {}, 2), {"notes.index", "notes.read"}, false, &err));
        QVERIFY(!runT(r, "nowrite", &err));
        QVERIFY2(err.contains("permission denied: notes.write"), qPrintable(err));
        QVERIFY(r.lib.renamed.isEmpty());
    }
    void openIsLimitedPerCallback() {
        Rig r;
        QVERIFY(add(r, "spam", kAll, cmd("for i = 1, 20 do hn.notes.open('notes/a.md') end")));
        QString err;
        QVERIFY(!runT(r, "spam", &err));
        QVERIFY2(err.contains("too many notes opened"), qPrintable(err));
        QCOMPARE(r.lib.opened.size(), 5);
    }
    void resultCaps() {
        Rig r;
        // 1000 rows, long strings, 30-deep nesting from a hostile bridge
        for (int i = 0; i < 1000; ++i) r.lib.incoming["big.md"] << LinkRow{"link", QString("t%1").arg(i), "", "", "", "src.md", QString(2000, QLatin1Char('x')), i};
        r.lib.outgoing["big.md"] = r.lib.incoming["big.md"];
        for (int i = 0; i < 10; ++i) r.lib.incoming["fat.md"] << LinkRow{"link", "t", "", "", "", "src.md", QString(200 * 1024, QLatin1Char('x')), i};
        QJsonValue deep = "leaf";
        for (int i = 0; i < 30; ++i) deep = QJsonObject{{"d", deep}};
        r.lib.fm["big.md"] = QJsonObject{{"long", QString(200 * 1024, QLatin1Char('y'))}, {"deep", deep}};
        QJsonArray rows;
        for (int i = 0; i < 1000; ++i) rows.append(QJsonObject{{"i", i}, {"s", QString(1000, QLatin1Char('z'))}});
        r.lib.queryRows = rows;
        QVERIFY(add(r, "caps", kAll, cmd(R"LUA(
          local b = hn.notes.backlinks('big.md', {limit = 500})
          local l = hn.notes.links('big.md')
          local fm = hn.notes.frontmatter('big.md')
          local depth, t = 0, fm.deep
          while type(t) == 'table' do depth = depth + 1; t = t.d end
          local q = hn.notes.query{from = 'notes', limit = 500}
          local fat = hn.notes.backlinks('fat.md', {limit = 10})
          hn.log(#b, #l, #fm.long, depth, #q, #q[1].s, #fat, #fat[1].context)
          hn.log(tostring(select(2, pcall(hn.notes.backlinks, 'big.md', {limit = 501}))))
        )LUA")));
        QString err;
        QVERIFY2(runT(r, "caps", &err), qPrintable(err));
        const auto l = r.logsOf("caps");
        const auto f = l[0].split('\t');
        QCOMPARE(f[0], QString("500")); QCOMPARE(f[1], QString("500")); QCOMPARE(f[2], QString("65536"));
        QVERIFY2(f[3].toInt() >= 1 && f[3].toInt() <= 6, qPrintable(l[0]));  // 30 levels in, at most 6 out
        QCOMPARE(f[4], QString("500")); QCOMPARE(f[5], QString("1000")); QCOMPARE(f[6], QString("10")); QCOMPARE(f[7], QString("65536"));  // strings cut at 64 KiB
        QVERIFY2(l[1].contains("limit"), qPrintable(l[1]));
    }
    void resultsThatCannotFitTheHeapFailCleanly() {  // 500 rows x 64 KiB cannot exist in a 16 MiB heap: an error, never a crash
        Rig r;
        for (int i = 0; i < 500; ++i) r.lib.incoming["huge.md"] << LinkRow{"link", "t", "", "", "", "src.md", QString(200 * 1024, QLatin1Char('x')), i};
        QVERIFY(add(r, "fit", kAll, cmd("hn.notes.backlinks('huge.md', {limit = 500})")));
        QString err;
        QVERIFY(!runT(r, "fit", &err));
        QVERIFY2(err.contains("too large for the plugin memory limit"), qPrintable(err));
        QVERIFY(r.mgr->loadedStates() == 1);
    }

    // ---------------------------------------------------------------- query spec validation (malicious plugins)
    void maliciousQuerySpecsNeverReachTheBridge() {
        Rig r;
        QVERIFY(add(r, "inj", kAll, cmd(R"LUA(
          local long = ('x'):rep(5000)
          local specs = {
            'SELECT * FROM notes', 42, {},                                              -- not a spec / no from
            {from = 'sqlite_master'}, {from = 'notes; DROP TABLE notes'}, {from = 7},
            {from = 'notes', sql = 'select 1'}, {from = 'notes', raw = true},          -- unknown keys
            {from = 'notes', where = {{field = 'path); DROP TABLE x;--', op = '=', value = 1}}},
            {from = 'notes', where = {{field = [[path' OR '1'='1]], op = '=', value = 1}}},
            {from = 'notes', where = {{field = 'path', op = '= 1 OR 1=1 --', value = 1}}},
            {from = 'notes', where = {{field = 'path', op = 'like', value = {}}}},       -- non-scalar
            {from = 'notes', where = {{field = 'path', op = 'in', value = 'x'}}},
            {from = 'notes', where = {{field = 'path', op = 'in', value = {}}}},
            {from = 'notes', where = {{field = 'path', op = '=', value = long}}},          -- oversize value
            {from = 'notes', where = {{field = 'path', op = '=', value = 1, extra = 1}}},
            {from = 'notes', where = {'path = 1'}},
            {from = 'notes', where = {{field = 'path', op = 'in', value = {{1}}}}},
            {from = 'notes', order = {{field = 'path', dir = 'asc; DROP TABLE x'}}},
            {from = 'notes', order = {{field = 'mtime desc, (select 1)'}}},
            {from = 'notes', select = {'path, (select password from users)'}},
            {from = 'notes', limit = 0}, {from = 'notes', limit = -5}, {from = 'notes', limit = 1.5}, {from = 'notes', limit = 'all'},
            {from = 'notes', offset = -1}, {from = 'notes', offset = 1e18}, {from = 'notes', offset = 0.5},
            {from = 'notes', limit = 1e308 * 10},                                         -- inf
            {from = 'notes', where = {{field = 'x', op = '=', value = 0/0}}},               -- NaN
          }
          local many = {}
          for i = 1, 40 do many[i] = {field = 'path', op = '=', value = i} end
          specs[#specs + 1] = {from = 'notes', where = many}                                 -- too many clauses
          local cyc = {from = 'notes'}; cyc.where = cyc
          specs[#specs + 1] = cyc                                                            -- cycle
          local deep = {from = 'notes'}; local t = deep
          for i = 1, 30 do t.where = {}; t = t.where end
          specs[#specs + 1] = deep
          local huge = {}
          for i = 1, 5000 do huge[i] = {field = 'path', op = '=', value = i} end
          specs[#specs + 1] = {from = 'notes', where = huge}                                 -- node/size cap
          specs[#specs + 1] = {from = 'notes', select = {fn = print}}                        -- function inside
          local rejected = 0
          for i, s in ipairs(specs) do
            local ok, e = pcall(hn.notes.query, s)
            if not ok and tostring(e):find('invalid query') or (not ok and tostring(e):find('bad argument')) then rejected = rejected + 1
            else hn.log('NOT REJECTED #' .. i .. ' ' .. tostring(e)) end
          end
          hn.log('rejected ' .. rejected .. ' of ' .. #specs)
        )LUA")));
        QString err;
        QVERIFY2(runT(r, "inj", &err), qPrintable(err));
        const auto l = r.logsOf("inj");
        for (const auto &x : l) QVERIFY2(!x.startsWith("NOT REJECTED"), qPrintable(x));
        QVERIFY(l.last().startsWith("rejected "));
        const auto n = l.last().split(' ');
        QCOMPARE(n[1], n[3]);
        QVERIFY(n[1].toInt() >= 35);
        QCOMPARE(r.lib.querySpecs.size(), 0);   // not one of them reached the bridge
        QCOMPARE(r.lib.indexCalls, 0);
        QVERIFY(auditText(r).contains("rejected query spec"));
        qInfo().noquote() << "v2: " + l.last() + " malicious query specs rejected before the bridge, bridge saw 0 queries";
    }
    void validQuerySpecIsRebuiltNotForwarded() {
        Rig r;
        QVERIFY(add(r, "okq", kAll, cmd(R"LUA(
          hn.notes.query{from = 'notes', limit = 100000, where = {{field = 'meta.status', op = 'in', value = {'a', 'b', 3, true}}, {field = 'tag', op = 'contains', value = 'x'}},
                         select = {'path', 'title'}, order = {{field = 'mtime', dir = 'desc'}}}
          hn.notes.query{from = 'links'}
        )LUA")));
        QString err;
        QVERIFY2(runT(r, "okq", &err), qPrintable(err));
        QCOMPARE(r.lib.querySpecs.size(), 2);
        const auto s = r.lib.querySpecs[0];
        QCOMPARE(s["limit"].toInt(), 500);  // clamped
        QCOMPARE(s["where"].toArray().size(), 2);
        QCOMPARE(s["where"].toArray()[0].toObject()["value"].toArray().size(), 4);
        QCOMPARE(s["select"].toArray().size(), 2);
        QCOMPARE(s.keys().size(), 6);
        const auto s2 = r.lib.querySpecs[1];
        QCOMPARE(s2["limit"].toInt(), 100);  // default is always explicit
        QVERIFY(s2["where"].toArray().isEmpty());
    }

    // ---------------------------------------------------------------- budgets
    void bridgeTimeoutIsReportedToLua() {
        Rig r;
        r.lib.forced = BridgeStatus::Timeout;
        QVERIFY(add(r, "to", kAll, cmd(R"LUA(
          local a, b = hn.notes.query{from = 'notes'}
          hn.log(tostring(a) .. '|' .. tostring(b))
        )LUA")));
        QVERIFY(runT(r, "to"));
        QVERIFY2(r.logsOf("to").last().startsWith("nil|timeout"), qPrintable(r.logsOf("to").last()));
        QVERIFY(auditText(r).contains("index call exceeded 100 ms"));
    }
    void slowBridgeResultIsDiscarded() {
        Rig r;
        r.lib.delayMs = 130;  // the bridge "forgot" its own 100 ms budget and still answered Ok
        QVERIFY(add(r, "slow", kAll, cmd(R"LUA(
          local a, b = hn.notes.links('notes/a.md')
          hn.log(tostring(a) .. '|' .. tostring(b))
        )LUA")));
        QVERIFY(runT(r, "slow"));
        QVERIFY2(r.logsOf("slow").last().startsWith("nil|timeout"), qPrintable(r.logsOf("slow").last()));
    }
    void indexTimeIsBoundedPerCallback() {
        Rig r;
        r.mgr->host()->env().indexCallbackMs = 150;
        r.lib.delayMs = 60;
        QVERIFY(add(r, "cb", kAll, cmd(R"LUA(
          local ok, to = 0, 0
          for i = 1, 6 do
            local a, b = hn.notes.links('notes/a.md')
            if a then ok = ok + 1 else to = to + 1 end
          end
          hn.log(ok .. ' ' .. to)
        )LUA")));
        QString err;
        QVERIFY2(runT(r, "cb", &err), qPrintable(err));
        QCOMPARE(r.logsOf("cb").last(), QString("3 3"));  // 60+60+60 >= 150, then refused without calling the bridge
        QCOMPARE(r.lib.indexCalls, 3);
    }
    void indexCallsAreRateLimited() {
        Rig r;
        QVERIFY(add(r, "rate", kAll, cmd(R"LUA(
          local ok, limited = 0, 0
          for i = 1, 260 do
            local a, b = hn.notes.resolve('b')
            if a then ok = ok + 1 elseif b == 'rate limit exceeded' then limited = limited + 1 end
          end
          hn.log(ok .. ' ' .. limited)
        )LUA")));
        QString err;
        QVERIFY2(runT(r, "rate", &err), qPrintable(err));
        const auto p = r.logsOf("rate").last().split(' ');
        QCOMPARE(p[0].toInt() + p[1].toInt(), 260);
        QVERIFY2(p[0].toInt() >= 200 && p[0].toInt() <= 215, qPrintable(r.logsOf("rate").last()));  // 200/s (+ a few if the second rolled over)
        QCOMPARE(r.lib.indexCalls, p[0].toInt());  // limited calls never reached the bridge
        QVERIFY(auditText(r).contains("index rate limit (200/s)"));
        qInfo().noquote() << "v2: index rate limit: " + r.logsOf("rate").last() + " (ok limited) of 260 calls in one callback";
    }

    // ---------------------------------------------------------------- panels
    static QByteArray panelPlugin() {
        return R"LUA(
hn.panel{id = 'p', title = 'Panel', icon = 'star', refresh_on = {'note.saved', 'note.opened'}, render = function(ctx)
  hn.log('render ' .. ctx.panel .. ' ' .. tostring(ctx.path))
  return {
    {type = 'heading', text = 'H', level = 1},
    {type = 'text', text = 'plain'},
    {type = 'list', items = {
      {type = 'item', title = 'a', subtitle = 's', path = 'n/a.md', line = 3, on_click = function() hn.log('clicked a') end},
      {type = 'item', title = 'b'},
    }},
    {type = 'markdown', text = '**m**'},
    {type = 'button', label = 'B', on_click = function() hn.log('clicked button') end},
    {type = 'empty', text = 'nothing'},
  }
end}
)LUA";
    }
    void panelRegistersWithoutAState() {
        Rig r;
        QString err;
        QVERIFY2(r.addLua("pan", {"ui.panel", "note.read"}, panelPlugin(), {}, &err, 2), qPrintable(err));
        const auto panels = r.mgr->registry()->panels();
        QCOMPARE(panels.size(), 1);
        QCOMPARE(panels[0].qualifiedId(), QString("pan:p"));
        QCOMPARE(panels[0].title, QString("Panel"));
        QCOMPARE(panels[0].icon, QString("star"));
        QCOMPARE(panels[0].refreshOn, (QStringList{"note.saved", "note.opened"}));
        QCOMPARE(r.panel.shown, QStringList{"pan:p|Panel|star"});
        QCOMPARE(r.mgr->loadedStates(), 0);  // registrations were learned, then the state dropped
    }
    void panelRenderClickAndStaleTokens() {
        Rig r;
        QVERIFY(r.addLua("pan", {"ui.panel", "note.read"}, panelPlugin(), {}, nullptr, 2));
        QList<PanelBlock> b;
        QString err;
        QCOMPARE(r.mgr->loadedStates(), 0);
        QVERIFY2(r.mgr->renderPanel("pan", "p", &r.note, &b, &err), qPrintable(err));
        QCOMPARE(r.mgr->loadedStates(), 1);  // created on demand by the render
        QCOMPARE(r.logsOf("pan").last(), QString("render p notes/a.md"));
        QCOMPARE(b.size(), 6);
        QCOMPARE(b[0].type, QString("heading")); QCOMPARE(b[0].level, 1); QCOMPARE(b[0].text, QString("H"));
        QCOMPARE(b[2].type, QString("list"));
        QCOMPARE(b[2].items.size(), 2);
        QCOMPARE(b[2].items[0].title, QString("a")); QCOMPARE(b[2].items[0].subtitle, QString("s"));
        QCOMPARE(b[2].items[0].path, QString("n/a.md")); QCOMPARE(b[2].items[0].line, 3);
        QVERIFY(b[2].items[0].click > 0 && b[2].items[1].click == 0);
        QVERIFY(b[4].click > 0 && b[4].click != b[2].items[0].click);
        QCOMPARE(b[4].text, QString("B"));
        const int t1 = b[2].items[0].click, t2 = b[4].click;
        QVERIFY2(r.mgr->panelClick("pan", "p", t1, &r.note, &err), qPrintable(err));
        QVERIFY2(r.mgr->panelClick("pan", "p", t2, &r.note, &err), qPrintable(err));
        QCOMPARE(r.logsOf("pan").mid(1), (QStringList{"clicked a", "clicked button"}));
        QVERIFY(!r.mgr->panelClick("pan", "p", 99999, &r.note, &err));
        QVERIFY(err.contains("stale"));
        QVERIFY(!r.mgr->panelClick("pan", "nope", t1, &r.note, &err));
        // a second render invalidates the old tokens
        QVERIFY(r.mgr->renderPanel("pan", "p", &r.note, &b, &err));
        QVERIFY(!r.mgr->panelClick("pan", "p", t1, &r.note, &err));
        QVERIFY(err.contains("stale"));
        QVERIFY(r.mgr->panelClick("pan", "p", b[2].items[0].click, &r.note, &err));
        QVERIFY(!r.mgr->renderPanel("pan", "missing", &r.note, &b, &err));
        QVERIFY(!r.mgr->renderPanel("nobody", "p", &r.note, &b, &err));
    }
    void panelWithoutNoteReadGetsNoPath() {
        Rig r;
        QVERIFY(r.addLua("pn", {"ui.panel"}, panelPlugin().replace("'note.saved', 'note.opened'", "'note.saved'"), {}, nullptr, 2));
        QList<PanelBlock> b;
        QVERIFY(r.mgr->renderPanel("pn", "p", &r.note, &b));
        QCOMPARE(r.logsOf("pn").last(), QString("render p nil"));  // ctx.path only with note.read
    }
    void panelClickRunsWithThePanelsIdentityAndBudget() {
        Rig r;
        // clicking calls a host function that needs a permission the plugin has; another plugin's rights are irrelevant
        QVERIFY(r.addLua("a-pan", {"ui.panel", "notes.read"}, R"LUA(
hn.panel{id = 'p', title = 'P', render = function() return {{type = 'button', label = 'go', on_click = function()
  local ok = pcall(hn.notes.rename, 'x.md', 'y.md')   -- no notes.write: denied under THIS plugin's identity
  hn.log('rename ok=' .. tostring(ok))
  hn.notes.open('notes/a.md')
  while true do end
end}} end}
)LUA", {}, nullptr, 2));
        QList<PanelBlock> b;
        QString err;
        QVERIFY(r.mgr->renderPanel("a-pan", "p", &r.note, &b, &err));
        QElapsedTimer t;
        t.start();
        QVERIFY(!r.mgr->panelClick("a-pan", "p", b[0].click, &r.note, &err));
        QVERIFY2(err.contains("budget"), qPrintable(err));
        QVERIFY2(t.elapsed() < 3500, qPrintable(QString::number(t.elapsed())));  // command budget is 2 s
        QVERIFY(r.logsOf("a-pan").contains("rename ok=false"));
        QCOMPARE(r.lib.opened, QStringList{"notes/a.md@organizer"});
        QVERIFY(r.lib.renamed.isEmpty());
        QCOMPARE(r.mgr->trust()->record("a-pan").failures, 1);  // counts for the circuit breaker like any callback
        qInfo().noquote() << QString("v2: infinite loop in on_click aborted after %1 ms (command budget 2000 ms)").arg(t.elapsed());
    }
    void infiniteLoopInRenderIsAborted() {
        Rig r;
        QVERIFY(r.addLua("loop", {"ui.panel"}, "hn.panel{id='p', title='P', render=function() while true do end end}", {}, nullptr, 2));
        QList<PanelBlock> b;
        QString err;
        QElapsedTimer t;
        t.start();
        QVERIFY(!r.mgr->renderPanel("loop", "p", &r.note, &b, &err));
        const qint64 ms = t.elapsed();
        QVERIFY2(err.contains("time budget exceeded"), qPrintable(err));
        QVERIFY2(ms < 400, qPrintable(QString::number(ms)));
        QVERIFY(b.isEmpty());
        qInfo().noquote() << QString("v2: infinite loop in panel render aborted after %1 ms (budget 50 ms)").arg(ms);
        // refreshPanel pushes the error to the bridge, with no blocks
        QVERIFY(!r.mgr->refreshPanel("loop", "p", &r.note, &err));
        QVERIFY(r.panel.errors["loop:p"].contains("budget"));
        QVERIFY(r.panel.blocks["loop:p"].isEmpty());
        // three failures in a row switch the plugin off and the panel disappears
        r.mgr->renderPanel("loop", "p", &r.note, &b, &err);
        QVERIFY(r.mgr->info("loop").status != Status::Enabled);
        QVERIFY(r.panel.removed.contains("loop:p"));
        QVERIFY(r.mgr->registry()->panels().isEmpty());
    }
    void maliciousPanelsAreRejected() {
        struct Case { const char *name; const char *lua; const char *why; };
        const QList<Case> cases{
            {"too many blocks", "local t = {} for i = 1, 201 do t[i] = {type='text', text='x'} end return t", "too many blocks"},
            {"too many items", "local it = {} for i = 1, 300 do it[i] = {type='item', title='x'} end return {{type='list', items=it}}", "too many blocks"},
            {"items spread over lists", "local t = {} for i = 1, 3 do local it = {} for j = 1, 80 do it[j] = {type='item', title='x'} end t[i] = {type='list', items=it} end return t", "too many blocks"},
            {"text bomb", "local t = {} for i = 1, 100 do t[i] = {type='text', text=('x'):rep(16384)} end return t", "panel text is larger"},
            {"unknown type", "return {{type='script', text='alert(1)'}}", "unknown block type"},
            {"widget", "return {{type='webview', url='https://x'}}", "unknown block type"},
            {"not a table", "return 'text'", "array of blocks"},
            {"block not a table", "return {'x'}", "must be a table"},
            {"missing type", "return {{text='x'}}", "field 'type'"},
            {"heading no text", "return {{type='heading'}}", "field 'text'"},
            {"bad level", "return {{type='heading', text='x', level=9}}", "level"},
            {"button without click", "return {{type='button', label='x'}}", "on_click"},
            {"click not a function", "return {{type='item', title='x', on_click='os.execute()'}}", "on_click"},
            {"item path traversal", "return {{type='item', title='x', path='../../etc/passwd'}}", "valid note path"},
            {"item absolute path", "return {{type='item', title='x', path='/etc/passwd'}}", "valid note path"},
            {"bad line", "return {{type='item', title='x', path='a.md', line=0}}", "line"},
            {"list in list", "return {{type='list', items={{type='list', items={}}}}}", "cannot contain a list"},
            {"non-item in list", "return {{type='list', items={{type='text', text='x'}}}}", "only contain item"},
            {"list without items", "return {{type='list'}}", "items"},
            {"function as text", "return {{type='text', text=print}}", "field 'text'"},
        };
        for (const auto &c : cases) {
            Rig r;
            QVERIFY(r.addLua("mal", {"ui.panel"}, QByteArray("hn.panel{id='p', title='P', render=function() ") + c.lua + " end}", {}, nullptr, 2));
            QList<PanelBlock> b;
            QString err;
            QVERIFY2(!r.mgr->renderPanel("mal", "p", &r.note, &b, &err), c.name);
            QVERIFY2(err.contains(c.why), qPrintable(QString("%1: %2").arg(c.name, err)));
            QVERIFY2(b.isEmpty(), c.name);
        }
        qInfo().noquote() << QString("v2: %1 malicious/invalid panel shapes rejected with a clear error").arg(cases.size());
    }
    void panelStringsAreTruncatedNotTrusted() {
        Rig r;
        QVERIFY(r.addLua("trunc", {"ui.panel"}, R"LUA(hn.panel{id='p', title='P', render=function()
          local big = ('é'):rep(1500000)
          return {{type='heading', text=big}, {type='text', text=big}, {type='item', title=big, subtitle=big}, {type='empty', text=big}}
        end})LUA", {}, nullptr, 2));
        QList<PanelBlock> b;
        QString err;
        QVERIFY2(r.mgr->renderPanel("trunc", "p", &r.note, &b, &err), qPrintable(err));
        QCOMPARE(b[0].text.toUtf8().size() <= 200, true);
        QCOMPARE(b[1].text.toUtf8().size() <= 16 * 1024, true);
        QCOMPARE(b[2].title.toUtf8().size() <= 200, true);
        QCOMPARE(b[2].subtitle.toUtf8().size() <= 400, true);
        QCOMPARE(b[3].text.toUtf8().size() <= 400, true);
        QVERIFY(!b[1].text.contains(QChar(0xFFFD)));
    }
    void panelRegistrationRules() {
        Rig r;
        struct C { const char *lua; const char *why; };
        for (const C &c : QList<C>{
                 {"hn.panel{id='p', title='P'}", "render"},
                 {"hn.panel{id='p', title='P', render=1}", "render"},
                 {"hn.panel{id='P x', title='P', render=function() end}", "invalid id"},
                 {"hn.panel{id='p', render=function() end}", "title"},
                 {"hn.panel{id='p', title='P', render=function() end, on_event='x'}", "on_event"},
                 {"hn.panel{id='p', title='P', render=function() end, refresh_on={'app.started'}}", "refresh_on"},
                 {"hn.panel{id='p', title='P', render=function() end, refresh_on='note.saved'}", "refresh_on"},
                 {"hn.panel{id='p', title='P', render=function() end} hn.panel{id='p', title='P', render=function() end}", "duplicate"},
                 {"hn.panel_refresh('nope')", "unknown panel"},
             }) {
            QString err;
            QVERIFY2(!r.addLua(QString("reg%1").arg(QString::number(quintptr(c.lua) % 9973)), {"ui.panel"}, c.lua, {}, &err, 2), c.lua);
            QVERIFY2(err.contains(c.why), qPrintable(QString("%1 -> %2").arg(c.lua, err)));
        }
        QString err;
        QByteArray many = "for i = 1, 17 do hn.panel{id='p'..i, title='P', render=function() return {} end} end";
        QVERIFY(!r.addLua("manyp", {"ui.panel"}, many, {}, &err, 2));
        QVERIFY(err.contains("too many panels"));
    }
    void eventsRefreshOnlyActivePanels() {
        Rig r;
        QVERIFY(r.addLua("ev", {"ui.panel", "note.read"}, panelPlugin(), {}, nullptr, 2));
        r.mgr->post("note.saved", "notes/a.md", &r.note);
        r.mgr->flushEvents();
        QCOMPARE(r.panel.updates, 0);            // hidden panel: no render, no Lua
        QCOMPARE(r.mgr->loadedStates(), 0);
        QVERIFY(!r.mgr->timerActive());
        r.mgr->setPanelActive("ev", "p", true);
        r.mgr->post("note.saved", "notes/a.md", &r.note);
        QCOMPARE(r.panel.updates, 1);
        QCOMPARE(r.panel.blocks["ev:p"].size(), 6);
        QVERIFY(r.panel.errors["ev:p"].isEmpty());
        r.mgr->post("note.closed", "notes/a.md", &r.note);  // not in refresh_on
        QCOMPARE(r.panel.updates, 1);
        r.mgr->setPanelActive("ev", "p", false);
        r.mgr->post("note.opened", "notes/a.md", &r.note);
        QCOMPARE(r.panel.updates, 1);
    }
    void onEventIsDeliveredBeforeTheRefresh() {
        Rig r;
        QVERIFY(r.addLua("oe", {"ui.panel"}, R"LUA(
local seen = 0
hn.panel{id='p', title='P', refresh_on={'note.changed'}, on_event=function(ev, arg) seen = seen + 1 hn.log('event ' .. ev .. ' ' .. arg) end,
  render=function() return {{type='text', text='seen ' .. seen}} end}
)LUA", {}, nullptr, 2));
        r.mgr->setPanelActive("oe", "p", true);
        r.mgr->post("note.changed", "notes/a.md", &r.note);  // coalesced
        QCOMPARE(r.panel.updates, 0);
        QVERIFY(r.mgr->timerActive());
        r.mgr->flushEvents();
        QCOMPARE(r.panel.updates, 1);
        QCOMPARE(r.logsOf("oe").last(), QString("event note.changed notes/a.md"));
        QCOMPARE(r.panel.blocks["oe:p"][0].text, QString("seen 1"));
        QVERIFY(!r.mgr->timerActive());
    }
    void panelRefreshFromLuaIsQueuedAndCoalesced() {
        Rig r;
        QVERIFY(r.addLua("rf", {"ui.panel", "note.read"}, R"LUA(
hn.panel{id='p', title='P', render=function(ctx) hn.log('render') hn.panel_refresh('p') return {{type='text', text='x'}} end}
hn.command{id='t', title='T', run=function()
  for i = 1, 25 do hn.log('r' .. tostring(hn.panel_refresh('p'))) end
end}
)LUA", {}, nullptr, 2));
        r.mgr->setPanelActive("rf", "p", true);
        QVERIFY(runT(r, "rf"));
        const auto l = r.logsOf("rf");
        int accepted = 0;
        for (const auto &x : l) accepted += x == "rtrue";
        QCOMPARE(accepted, 10);                    // rate limit 10/s per plugin
        QCOMPARE(r.panel.updates, 0);              // nothing rendered re-entrantly
        QVERIFY(r.mgr->timerActive());
        r.mgr->flushPanels();
        QCOMPARE(r.panel.updates, 1);              // 10 requests, one render (a render cannot re-request itself)
        QCOMPARE(r.logsOf("rf").count("render"), 1);
        QVERIFY(!r.mgr->timerActive());
        r.mgr->setPanelActive("rf", "p", false);
        QVERIFY(runT(r, "rf"));
        QVERIFY(!r.mgr->timerActive());            // inactive panel: the request is dropped, no timer
    }
    void panelNotificationsFollowEnableAndDisable() {
        Rig r;
        QVERIFY(r.addLua("life", {"ui.panel"}, panelPlugin(), {}, nullptr, 2));
        QCOMPARE(r.panel.shown, QStringList{"life:p|Panel|star"});
        r.mgr->setPanelActive("life", "p", true);
        QVERIFY(r.mgr->disable("life"));
        QCOMPARE(r.panel.removed, QStringList{"life:p"});
        r.mgr->post("note.saved", "a.md", &r.note);   // disabled: nothing happens
        QCOMPARE(r.panel.updates, 0);
        QVERIFY(r.mgr->enable("life"));
        QCOMPARE(r.panel.shown.size(), 2);
        QString err;
        QList<PanelBlock> b;
        QVERIFY(r.mgr->disable("life"));
        QVERIFY(!r.mgr->renderPanel("life", "p", &r.note, &b, &err));
        QVERIFY(err.contains("not enabled"));
    }

    // ---------------------------------------------------------------- completion
    void completionReturnsValidatedItems() {
        Rig r;
        QVERIFY(r.addLua("cmp", {"editor.complete", "note.read"}, R"LUA(
hn.complete{id = 'links', trigger = '[[', items = function(query, ctx)
  hn.log('items ' .. query .. ' ' .. ctx.trigger .. ' ' .. tostring(ctx.path))
  return {
    {label = 'Alpha', detail = 'a note', insert = 'Alpha]]', cursor_offset = 7},
    {label = 'Beta', insert = 'Beta]]'},
    {label = '', insert = 'x'},                          -- empty label
    {label = 'NoInsert'},                                -- no insert
    {label = 5, insert = 'x'},                           -- wrong type
    {label = 'BadOffset', insert = 'ab', cursor_offset = 3},
    {label = 'NegOffset', insert = 'ab', cursor_offset = -1},
    {label = 'FloatOffset', insert = 'ab', cursor_offset = 0.5},
    {label = ('x'):rep(201), insert = 'x'},              -- label too long
    {label = 'Big', insert = ('x'):rep(9000)},           -- insert too long
    {label = 'BadDetail', insert = 'x', detail = {}},
    'garbage', 42,
    {label = 'Gamma', insert = 'Gamma]]', cursor_offset = 0},
  }
end}
)LUA", {}, nullptr, 2));
        QCOMPARE(r.mgr->registry()->completions("[[").size(), 1);
        QCOMPARE(r.mgr->registry()->completions("/").size(), 0);
        QCOMPARE(r.mgr->loadedStates(), 0);
        const auto items = r.mgr->complete("[[", "al", &r.note);
        QCOMPARE(items.size(), 3);
        QCOMPARE(items[0].label, QString("Alpha"));
        QCOMPARE(items[0].detail, QString("a note"));
        QCOMPARE(items[0].insert, QString("Alpha]]"));
        QCOMPARE(items[0].cursorOffset, 7);
        QCOMPARE(items[0].pluginId, QString("cmp"));
        QCOMPARE(items[1].cursorOffset, -1);
        QCOMPARE(items[2].cursorOffset, 0);
        QCOMPARE(r.logsOf("cmp").first(), QString("items al [[ notes/a.md"));
        QVERIFY(r.mgr->complete("/", "al", &r.note).isEmpty());
        // request/reply through the editor bridge
        r.mgr->requestCompletion(42, "[[", "be", &r.note);
        QCOMPARE(r.editor.completions.size(), 1);
        QCOMPARE(r.editor.completions[0].first, quint64(42));
        QCOMPARE(r.editor.completions[0].second.size(), 3);
    }
    void completionIsCappedAtFiftyAndAcrossPlugins() {
        Rig r;
        const QByteArray lua = "hn.complete{id='c', trigger='/', items=function() local t = {} for i = 1, 500 do t[i] = {label='i'..i, insert='i'} end return t end}";
        QVERIFY(r.addLua("c1", {"editor.complete"}, lua, {}, nullptr, 2));
        QCOMPARE(r.mgr->complete("/", "", &r.note).size(), 50);
        QVERIFY(r.addLua("c2", {"editor.complete"}, lua, {}, nullptr, 2));
        const auto items = r.mgr->complete("/", "", &r.note);
        QCOMPARE(items.size(), 50);
        QCOMPARE(items[0].pluginId, QString("c1"));
    }
    void infiniteLoopInCompletionIsAborted() {
        Rig r;
        QVERIFY(r.addLua("cl", {"editor.complete"}, "hn.complete{id='c', trigger='[[', items=function() while true do end end}", {}, nullptr, 2));
        QElapsedTimer t;
        t.start();
        QVERIFY(r.mgr->complete("[[", "x", &r.note).isEmpty());
        const qint64 ms = t.elapsed();
        QVERIFY2(ms < 300, qPrintable(QString::number(ms)));
        QVERIFY(auditText(r).contains("time budget exceeded"));
        qInfo().noquote() << QString("v2: infinite loop in completion aborted after %1 ms (budget 20 ms)").arg(ms);
        for (int i = 0; i < 2; ++i) r.mgr->complete("[[", "x", &r.note);
        QVERIFY(r.mgr->info("cl").status != Status::Enabled);  // breaker
        QVERIFY(r.mgr->complete("[[", "x", &r.note).isEmpty());
    }
    void completionRegistrationRules() {
        for (const auto &c : QList<QPair<QString, QString>>{
                 {"hn.complete{id='c', trigger='', items=function() end}", "trigger"},
                 {"hn.complete{id='c', trigger='a b', items=function() end}", "trigger"},
                 {"hn.complete{id='c', trigger='123456789', items=function() end}", "trigger"},
                 {"hn.complete{id='c', trigger='[[', items=1}", "items"},
                 {"hn.complete{id='c', items=function() end}", "trigger"},
                 {"hn.complete{id='c', trigger='[[', items=function() end} hn.complete{id='c', trigger='/', items=function() end}", "duplicate"},
                 {"hn.complete('x')", "table"}}) {
            Rig r;
            QString err;
            QVERIFY2(!r.addLua("cr", {"editor.complete"}, c.first.toUtf8(), {}, &err, 2), qPrintable(c.first));
            QVERIFY2(err.contains(c.second), qPrintable(c.first + " -> " + err));
        }
    }

    // ---------------------------------------------------------------- link activation
    void linkActivationDeliversTheRef() {
        Rig r;
        QVERIFY(r.addLua("lnk", {"editor.links"}, R"LUA(
hn.link_handler{pattern = function(ref) return ref.kind == 'link' and ref.target:sub(1, 1) == 'x' end}
hn.on('link.activate', function(ref)
  hn.log(table.concat({ref.kind, ref.target, tostring(ref.alias), tostring(ref.anchor), tostring(ref.resolved)}, '|'))
  return ref.target == 'xyes'
end)
)LUA", {}, nullptr, 2));
        QCOMPARE(r.mgr->registry()->linkHandlers().size(), 1);
        QVERIFY(r.mgr->registry()->linkHandlers()[0].hasPattern);
        QCOMPARE(r.mgr->loadedStates(), 0);
        LinkActivation ref{"link", "xyes", "Yes", "sec", "x/yes.md"};
        QVERIFY(r.mgr->activateLink(ref, &r.note));
        QCOMPARE(r.logsOf("lnk").last(), QString("link|xyes|Yes|sec|x/yes.md"));
        ref = {"link", "xno", "", "", ""};
        QVERIFY(!r.mgr->activateLink(ref, &r.note));            // handler ran, returned false
        QCOMPARE(r.logsOf("lnk").last(), QString("link|xno|nil|nil|nil"));
        ref = {"link", "other", "", "", ""};
        const int n = r.logsOf("lnk").size();
        QVERIFY(!r.mgr->activateLink(ref, &r.note));            // pattern said no: the handler never ran
        QCOMPARE(r.logsOf("lnk").size(), n);
        ref = {"embed", "xpic", "", "", ""};
        QVERIFY(!r.mgr->activateLink(ref, &r.note));
        // reply path
        ref = {"link", "xyes", "", "", ""};
        r.mgr->requestLinkActivation(7, ref, &r.note);
        QCOMPARE(r.editor.links.size(), 1);
        QCOMPARE(r.editor.links[0].first, quint64(7));
        QVERIFY(r.editor.links[0].second);
    }
    void linkActivationWithoutHandlersCostsNothing() {
        Rig r;
        QVERIFY(r.addLua("plain", {"ui"}, "hn.command{id='t', title='T', run=function() end}", {}, nullptr, 2));
        QVERIFY(r.mgr->registry()->linkHandlers().isEmpty());
        QVERIFY(!r.mgr->activateLink({"link", "x", "", "", ""}, &r.note));
        QCOMPARE(r.mgr->loadedStates(), 0);
        // posting the event name directly never invokes handlers with a string
        QVERIFY(r.addLua("lh", {"editor.links"}, "hn.on('link.activate', function(ref) error('must not run') end)", {}, nullptr, 2));
        r.mgr->post("link.activate", "x", &r.note);
        QCOMPARE(r.mgr->trust()->record("lh").failures, 0);
    }
    void handlerLoopIsAbortedAndNeverConsumesTheLink() {
        Rig r;
        QVERIFY(r.addLua("lp", {"editor.links"}, "hn.on('link.activate', function() while true do end end)", {}, nullptr, 2));
        QElapsedTimer t;
        t.start();
        QVERIFY(!r.mgr->activateLink({"link", "x", "", "", ""}, &r.note));
        QVERIFY2(t.elapsed() < 300, qPrintable(QString::number(t.elapsed())));
    }

    // ---------------------------------------------------------------- registry cache, laziness
    void registryCacheRoundTrip() {
        Rig r;
        QVERIFY(r.addLua("all", {"ui.panel", "editor.complete", "editor.links", "note.read"}, R"LUA(
hn.panel{id = 'p', title = 'Panel', icon = 'star', refresh_on = {'note.saved'}, on_event = function() end, render = function() return {{type = 'text', text = 'hi'}} end}
hn.complete{id = 'c', trigger = '[[', items = function() return {{label = 'L', insert = 'L'}} end}
hn.link_handler{pattern = function() return true end}
hn.command{id = 'cmd', title = 'Cmd', run = function() end}
)LUA", {}, nullptr, 2));
        const PluginRegs *live = r.mgr->registry()->of("all");
        QVERIFY(live);
        const QJsonObject js = live->toJson();
        QVERIFY(js.contains("panels") && js.contains("completions") && js.contains("link_handlers"));
        QCOMPARE(PluginRegs::fromJson("all", js).toJson(), js);
        QVERIFY(PluginRegs::fromJson("all", js) == *live);
        // an api-1 style regs object does not grow v2 keys (cache stays byte-compatible)
        QVERIFY(!PluginRegs().toJson().contains("panels"));
        // a fresh manager over the same directories restores everything from plugins-registry.json without creating any state
        ManagerConfig c;
        c.bridges = {&r.lib, &r.ui, &r.net, &r.clip, &r.theme, &r.panel, &r.editor};
        FakePanel panel2;
        c.bridges.panel = &panel2;
        PluginManager m2(c);
        m2.scan();
        QCOMPARE(m2.loadedStates(), 0);
        QVERIFY(!m2.timerActive());
        const auto *re = m2.registry()->of("all");
        QVERIFY(re);
        QVERIFY(*re == *live);
        QCOMPARE(m2.registry()->panels().size(), 1);
        QCOMPARE(m2.registry()->panels()[0].refreshOn, QStringList{"note.saved"});
        QVERIFY(m2.registry()->panels()[0].onEvent);
        QCOMPARE(m2.registry()->completions("[[").size(), 1);
        QCOMPARE(m2.registry()->linkHandlers().size(), 1);
        QCOMPARE(panel2.shown, QStringList{"all:p|Panel|star"});  // the dock knows the panel before any Lua ran
        QCOMPARE(m2.loadedStates(), 0);
        // the first real request creates the state on demand
        QList<PanelBlock> b;
        QString err;
        QVERIFY2(m2.renderPanel("all", "p", &r.note, &b, &err), qPrintable(err));
        QCOMPARE(m2.loadedStates(), 1);
        QCOMPARE(b[0].text, QString("hi"));
        // completion on another cold start
        PluginManager m3(c);
        m3.scan();
        QCOMPARE(m3.loadedStates(), 0);
        QCOMPARE(m3.complete("[[", "", &r.note).size(), 1);
        QCOMPARE(m3.loadedStates(), 1);
        PluginManager m4(c);
        m4.scan();
        QVERIFY(!m4.activateLink({"link", "t", "", "", ""}, &r.note));  // pattern-only handler, no hn.on: nothing to run
        QCOMPARE(m4.loadedStates(), 0);
    }
    void idleCostsNothing() {
        Rig r;  // nothing installed
        QCOMPARE(r.mgr->loadedStates(), 0);
        QVERIFY(!r.mgr->timerActive());
        for (const char *e : {"note.opened", "note.changed", "note.saved", "selection.changed", "note.closed", "app.started"}) r.mgr->post(e, "a.md", &r.note);
        r.mgr->flushPanels();
        QVERIFY(r.mgr->complete("[[", "x", &r.note).isEmpty());
        QVERIFY(!r.mgr->activateLink({"link", "x", "", "", ""}, &r.note));
        QVERIFY(r.mgr->preSave("t", &r.note) == "t");
        QCOMPARE(r.mgr->loadedStates(), 0);
        QVERIFY(!r.mgr->timerActive());
        // an installed, enabled, cached API-2 plugin that nobody asks anything of: still nothing
        QVERIFY(r.addLua("quiet", {"ui.panel", "editor.complete", "editor.links", "notes.index"}, panelPlugin() + R"LUA(
hn.complete{id='c', trigger='[[', items=function() return {} end}
hn.on('link.activate', function() end)
)LUA", {}, nullptr, 2));
        QCOMPARE(r.mgr->loadedStates(), 0);
        for (const char *e : {"note.opened", "note.changed", "note.saved", "selection.changed"}) r.mgr->post(e, "a.md", &r.note);
        r.mgr->flushEvents();
        QCOMPARE(r.mgr->loadedStates(), 0);
        QVERIFY(!r.mgr->timerActive());
        QCOMPARE(r.panel.updates, 0);
        QCOMPARE(r.lib.indexCalls, 0);
        // unknown trigger and unrelated link: still no state
        QVERIFY(r.mgr->complete("/", "x", &r.note).isEmpty());
        QCOMPARE(r.mgr->loadedStates(), 0);
    }
    void api2ScaffoldInstallsAndRenders() {
        Rig r;
        const QString dir = r.env.tmp.path() + "/sample";
        QString err;
        QVERIFY(!createTemplate(dir + "x", "Sample", &err, 3));
        QVERIFY2(createTemplate(dir, "Sample", &err, 2), qPrintable(err));
        const auto c = checkDirectory(dir);
        QVERIFY2(c.ok, qPrintable(c.errors.value(0).message));
        QCOMPARE(c.manifest.api, 2);
        QVERIFY(c.manifest.permissions.contains("ui.panel") && c.manifest.permissions.contains("notes.index"));
        QVERIFY2(r.setup(dir, {}, true, &err), qPrintable(err));
        QList<PanelBlock> b;
        QVERIFY2(r.mgr->renderPanel("sample", "backlinks", &r.note, &b, &err), qPrintable(err));
        QCOMPARE(b[0].text, QString("2 backlinks"));
        QVERIFY2(r.mgr->panelClick("sample", "backlinks", b[1].items[0].click, &r.note, &err), qPrintable(err));
        QCOMPARE(r.lib.opened, QStringList{"b.md@organizer"});
        // the default scaffold is still the API-1 command sample
        const QString dir1 = r.env.tmp.path() + "/sample1";
        QVERIFY(createTemplate(dir1, "Sample One", &err));
        QCOMPARE(checkDirectory(dir1).manifest.api, 1);
    }
    void apiOnePluginsAreUnaffectedByTheRuntimeChanges() {
        Rig r;
        QString err;
        QVERIFY2(r.addLua("v1", {"note.read", "ui"}, "hn.command{id='t', title='T', run=function() hn.ui.notify(hn.note.title()) end}\nhn.on('note.saved', function() hn.log('saved') end)", {}, &err), qPrintable(err));
        QVERIFY(runT(r, "v1"));
        r.mgr->post("note.saved", "a.md", &r.note);
        QCOMPARE(r.logsOf("v1").last(), QString("saved"));
        QCOMPARE(r.ui.notes, QStringList{"A"});
        QVERIFY(r.mgr->registry()->of("v1")->panels.isEmpty());
        // a v1 cache entry (no v2 keys) loads fine
        QVERIFY(!r.mgr->registry()->of("v1")->toJson().contains("panels"));
    }
};

QTEST_MAIN(V2Test)
#include "plugins_v2_test.moc"
