// Keeps docs/plugins.md and docs/plugin-api.md honest: every Lua code block runs as a real plugin against fake bridges,
// every manifest block parses, every hn.* function the sandbox exposes is documented (and vice versa), the permission
// table and the warning texts match the implementation, and every link/anchor resolves.
#include "plugins_test_util.h"
#include <QRegularExpression>
#include <QSet>

using namespace hn::plugins;
using namespace hn::plugins::test;

struct Fence { QString file, lang, attrs, body; int line = 0; };

static QString docPath(const QString &name) { return QStringLiteral(HN_PLUGIN_DOCS) + "/" + name; }
static QString readText(const QString &p) { return QString::fromUtf8(readFile(p)); }

static QList<Fence> fences(const QString &name) {
    QList<Fence> out;
    const QStringList lines = readText(docPath(name)).split('\n');
    static const QRegularExpression open(QStringLiteral("^\\s*```(\\w*)\\s*(.*)$"));
    for (int i = 0; i < lines.size(); ++i) {
        const auto m = open.match(lines[i]);
        if (!m.hasMatch() || m.captured(1).isEmpty()) {
            if (m.hasMatch()) { /* a bare closing fence outside a block: ignore */ }
            continue;
        }
        Fence f{name, m.captured(1), m.captured(2).trimmed(), {}, i + 1};
        const QString indent = lines[i].left(lines[i].indexOf('`'));
        int j = i + 1;
        for (; j < lines.size() && !lines[j].trimmed().startsWith("```"); ++j) f.body += (lines[j].startsWith(indent) ? lines[j].mid(indent.size()) : lines[j]) + "\n";
        out << f;
        i = j;
    }
    return out;
}
static QString attr(const QString &attrs, const QString &key, bool *present = nullptr) {
    for (const auto &tok : attrs.split(' ', Qt::SkipEmptyParts))
        if (tok.startsWith(key + "=")) { if (present) *present = true; return tok.mid(key.size() + 1); }
    if (present) *present = false;
    return {};
}
static QString slug(QString h) {
    h = h.toLower();
    QString o;
    for (QChar c : h) {
        if (c.isLetterOrNumber() || c == '-' || c == '_') o += c;
        else if (c == ' ') o += '-';
    }
    return o;
}

class DocsTest : public QObject {
    Q_OBJECT
private slots:
    void everyLuaBlockRunsAsAPlugin() {
        int n = 0, commands = 0, triggers = 0, events = 0, apiTwo = 0, panels = 0, completions = 0, links = 0;
        for (const QString &doc : {"plugins.md", "plugin-api.md"}) {
            for (const auto &f : fences(doc)) {
                if (f.lang != "lua") continue;
                const QString where = QString("%1:%2").arg(f.file).arg(f.line);
                QVERIFY2(f.attrs.split(' ').contains("test"), qPrintable(where + ": every lua block must be marked 'lua test perms=...'"));
                bool hasPerms;
                const QString permAttr = attr(f.attrs, "perms", &hasPerms);
                QVERIFY2(hasPerms, qPrintable(where + ": missing perms="));
                const QStringList perms = permAttr.isEmpty() ? QStringList{} : permAttr.split(',');
                const QString hostAttr = attr(f.attrs, "hosts");
                const QStringList hosts = hostAttr.isEmpty() ? QStringList{} : hostAttr.split(',');
                Rig r;
                const QString id = QString("doc-%1").arg(++n);
                QString err;
                const int api = attr(f.attrs, "api") == "2" ? 2 : 1;
                QVERIFY2(r.addLua(id, perms, f.body.toUtf8(), hosts, &err, api), qPrintable(where + ": load failed: " + err));
                if (api == 2) ++apiTwo;
                const auto *regs = r.mgr->registry()->of(id);
                QVERIFY2(regs, qPrintable(where));
                for (const auto &c : regs->commands) { QVERIFY2(r.mgr->runCommand(c.qualifiedId(), &r.note, &err), qPrintable(where + ": command " + c.id + ": " + err)); ++commands; }
                for (const auto &t : regs->toolbars) QVERIFY2(r.mgr->runToolbarButton(id + ":" + t.id, &r.note, &err), qPrintable(where + ": " + err));
                for (const auto &m : regs->menus) QVERIFY2(r.mgr->runMenuItem(id + ":" + m.id, &r.note, &err), qPrintable(where + ": " + err));
                for (const auto &t : regs->triggers) {
                    const auto m = r.mgr->matchTrigger("text " + t.pattern, &r.note);
                    QVERIFY2(m && m->pluginId == id, qPrintable(where + ": trigger " + t.pattern + " produced nothing"));
                    ++triggers;
                }
                for (const auto &pr : regs->panels) {  // API 2: render, then click everything clickable
                    QList<PanelBlock> blocks;
                    QVERIFY2(r.mgr->renderPanel(id, pr.id, &r.note, &blocks, &err), qPrintable(where + ": panel " + pr.id + ": " + err));
                    QList<int> tokens;
                    for (const auto &b : blocks) { if (b.click) tokens << b.click; for (const auto &it : b.items) if (it.click) tokens << it.click; }
                    for (int t : tokens) QVERIFY2(r.mgr->panelClick(id, pr.id, t, &r.note, &err), qPrintable(where + ": click: " + err));
                    ++panels;
                }
                for (const auto &c : regs->completions) {
                    QString msg;
                    QVERIFY2(!r.mgr->complete(c.trigger, "a", &r.note).isEmpty(), qPrintable(where + ": completion " + c.id + " returned nothing"));
                    ++completions;
                }
                if (!regs->linkHandlers.isEmpty()) {
                    QVERIFY2(r.mgr->activateLink({"link", "b", "", "", "b.md"}, &r.note), qPrintable(where + ": link handler did not consume the link"));
                    QVERIFY2(!r.lib.opened.isEmpty(), qPrintable(where));
                    ++links;
                }
                for (const auto &e : regs->events) {
                    if (e == "link.activate") continue;  // delivered with a table, covered above
                    if (e == "note.pre_save") { r.mgr->preSave("some text with trailing spaces  \n", &r.note); }
                    else if (e == "app.started") r.mgr->appStarted();
                    else { r.mgr->post(e, "notes/a.md", &r.note); r.mgr->flushEvents(); }
                    ++events;
                }
                QVERIFY2(r.mgr->trust()->record(id).failures == 0, qPrintable(where + ": a handler failed"));
                const QString audit = QString::fromUtf8(readFile(r.env.state() + "/plugins-audit.jsonl"));
                QVERIFY2(!audit.contains("\"failure\"") && !audit.contains("\"denied\""), qPrintable(where + ": failure/denied in audit log:\n" + audit));
                QVERIFY2(r.mgr->info(id).status == Status::Enabled, qPrintable(where));
            }
        }
        qInfo().noquote() << QString("docs: %1 Lua code blocks ran as plugins (%2 commands, %3 triggers, %4 event handlers invoked), no failures, no permission denials").arg(n).arg(commands).arg(triggers).arg(events);
        qInfo().noquote() << QString("docs: %1 of them are API 2 blocks (%2 panels rendered+clicked, %3 completions requested, %4 link handlers activated)").arg(apiTwo).arg(panels).arg(completions).arg(links);
        QVERIFY(n >= 45 && apiTwo >= 12 && panels >= 3 && completions >= 1 && links >= 1);
    }

    void jsonBlocksAreValidAndManifestsParse() {
        int manifests = 0, json = 0;
        for (const QString &doc : {"plugins.md", "plugin-api.md"})
            for (const auto &f : fences(doc)) {
                if (f.lang != "json") continue;
                const QString where = QString("%1:%2").arg(f.file).arg(f.line);
                if (f.attrs.split(' ').contains("manifest")) {
                    Manifest m;
                    QList<PluginError> errs;
                    QVERIFY2(parseManifest(f.body.toUtf8(), "/x/" + QJsonDocument::fromJson(f.body.toUtf8()).object()["id"].toString(), &m, &errs), qPrintable(where + ": " + errs.value(0).message));
                    ++manifests;
                } else {
                    QJsonParseError pe;
                    QJsonDocument::fromJson(f.body.toUtf8(), &pe);
                    QVERIFY2(pe.error == QJsonParseError::NoError, qPrintable(where + ": " + pe.errorString()));
                    ++json;
                }
            }
        QVERIFY(manifests >= 2);
        qInfo().noquote() << QString("docs: %1 plugin.json samples parse with the real manifest validator, %2 other JSON samples are valid").arg(manifests).arg(json);
    }

    void apiReferenceCoversExactlyTheRealApi() {
        const QByteArray walker = R"LUA(
hn.command{ id = 'c', title = 'C', run = function()
  local names = {}
  local function walk(prefix, t)
    for k, v in pairs(t) do
      local n = prefix .. '.' .. k
      if type(v) == 'table' then walk(n, v) else names[#names + 1] = n end
    end
  end
  walk('hn', hn)
  table.sort(names)
  hn.log(table.concat(names, ','))
end }
)LUA";
        auto realNames = [&](int api) {
            Rig r;
            const QString id = api == 2 ? "walk2" : "walk";
            [&] { QVERIFY(r.addLua(id, {}, walker, {}, nullptr, api)); }();
            [&] { QVERIFY(r.mgr->runCommand(id + ":c", &r.note)); }();
            const QStringList l = r.logsOf(id).value(0).split(',');
            return QSet<QString>(l.begin(), l.end());
        };
        const QSet<QString> real1 = realNames(1), real2 = realNames(2);
        QCOMPARE(int(real1.size()), 37);                 // an api-1 plugin sees exactly the original surface
        QCOMPARE(int(real2.size()), 37 + 11);            // api 2 adds: 7 hn.notes.*, panel, panel_refresh, complete, link_handler
        QVERIFY(real2.contains("hn.notes.query") && !real1.contains("hn.notes.query"));
        for (const auto &n : real1) QVERIFY2(real2.contains(n), qPrintable(n));
        QSet<QString> documented;
        static const QRegularExpression head(QStringLiteral("^### (hn\\.[A-Za-z_.]+)"));
        for (const auto &line : readText(docPath("plugin-api.md")).split('\n'))
            if (const auto m = head.match(line); m.hasMatch()) documented.insert(m.captured(1));
        QStringList missing, stale;
        for (const auto &n : real2) if (!documented.contains(n)) missing << n;
        for (const auto &n : documented) if (!real2.contains(n)) stale << n;
        missing.sort();
        stale.sort();
        QVERIFY2(missing.isEmpty(), qPrintable("not documented in plugin-api.md: " + missing.join(", ")));
        QVERIFY2(stale.isEmpty(), qPrintable("documented but not in the runtime: " + stale.join(", ")));
        qInfo().noquote() << QString("docs: plugin-api.md documents all %1 hn.* entries an API-2 plugin sees (%2 of them for API 1), and nothing else").arg(real2.size()).arg(real1.size());
    }

    void permissionTableAndWarningsMatchTheImplementation() {
        const QString guide = readText(docPath("plugins.md"));
        QVERIFY(guide.contains(QString::fromLatin1(kThirdPartyWarning)));
        QVERIFY(guide.contains(QString::fromLatin1(kNativeWarning)));
        static const QRegularExpression row(QStringLiteral("^\\| `([a-z.]+)` \\| (.+?) \\| (low|medium|high|critical) \\| (yes|no) \\|$"));
        QMap<QString, PermissionInfo> docRows;
        for (const auto &line : guide.split('\n'))
            if (const auto m = row.match(line); m.hasMatch()) docRows[m.captured(1)] = {m.captured(1), m.captured(2), m.captured(3), m.captured(4) == "yes"};
        QCOMPARE(docRows.size(), permissionTable().size());
        for (const auto &p : permissionTable()) {
            QVERIFY2(docRows.contains(p.name), qPrintable(p.name));
            QCOMPARE(docRows[p.name].description, p.description);
            QCOMPARE(docRows[p.name].risk, p.risk);
            QCOMPARE(docRows[p.name].dangerous, p.dangerous);
        }
        // numbers the docs promise
        const QString api = readText(docPath("plugin-api.md"));
        HostEnv env;
        for (const auto &s : {QString("events %1 ms").arg(env.eventMs), QString("commands %1 s").arg(env.commandMs / 1000), QString("`note.pre_save` %1 ms").arg(env.preSaveMs),
                              QString("triggers %1 ms").arg(env.triggerMs), QString("top-level load %1 ms").arg(env.loadMs),
                              QString("panel render %1 ms").arg(env.renderMs), QString("completion %1 ms").arg(env.completeMs),
                              QString("**%1 ms** wall budget per call").arg(env.indexCallMs), QString("**%1 ms** for all index calls").arg(env.indexCallbackMs),
                              QString("**%1 calls per second**").arg(env.indexPerSecond)})
            QVERIFY2(api.contains(s), qPrintable(s));
        QVERIFY(api.contains("16 MiB Lua heap"));
        QVERIFY(guide.contains("4 000 000 matcher steps") && api.contains("4 000 000-step"));
        QVERIFY(guide.contains("subject 1 MiB, pattern 256 bytes, 24 quantifiers"));
        QVERIFY(api.contains("at most 262 144 elements") && api.contains("at most 100 000 elements"));
    }

    void linksAndAnchorsResolve() {
        QMap<QString, QSet<QString>> anchors;
        for (const QString &doc : {"plugins.md", "plugin-api.md"})
            for (const auto &line : readText(docPath(doc)).split('\n'))
                if (line.startsWith('#')) { QString h = line; while (h.startsWith('#')) h.remove(0, 1); anchors[doc].insert(slug(h.trimmed().remove('`'))); }
        static const QRegularExpression link(QStringLiteral("\\]\\(([^)\\s]+)\\)"));
        int checked = 0;
        for (const QString &doc : {"plugins.md", "plugin-api.md"}) {
            auto it = link.globalMatch(readText(docPath(doc)));
            while (it.hasNext()) {
                const QString t = it.next().captured(1);
                if (t.startsWith("http")) continue;
                const QString file = t.section('#', 0, 0), anchor = t.contains('#') ? t.section('#', 1) : QString();
                const QString target = file.isEmpty() ? doc : file;
                QVERIFY2(QFileInfo::exists(QDir::cleanPath(docPath(target))), qPrintable(doc + " links to missing " + t));
                if (!anchor.isEmpty()) QVERIFY2(anchors.value(target).contains(anchor), qPrintable(doc + " has a dead anchor " + t));
                ++checked;
            }
        }
        QVERIFY(checked >= 5);
    }

    void scaffoldFromTheQuickstartInstallsAndRuns() {
        Rig r;
        const QString dir = r.env.tmp.path() + "/shout";
        QString err;
        QVERIFY2(createTemplate(dir, "Shout", &err), qPrintable(err));
        const auto c = checkDirectory(dir);
        QVERIFY2(c.ok, qPrintable(c.errors.value(0).message));
        QCOMPARE(c.manifest.id, QString("shout"));
        QVERIFY2(r.setup(dir, {}, true, &err), qPrintable(err));
        r.note.sel = "world";
        QVERIFY2(r.mgr->runCommand("shout:shout", &r.note, &err), qPrintable(err));
        QCOMPARE(r.note.ops, QStringList{"replace:WORLD"});
        // the guide's own copy of the command behaves the same (it is also executed by everyLuaBlockRunsAsAPlugin)
        const auto first = fences("plugins.md").value(1);
        QVERIFY(first.body.contains("sel:upper()"));
    }
};
QTEST_MAIN(DocsTest)
#include "plugins_docs_test.moc"
