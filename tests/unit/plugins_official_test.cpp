// The official plugins in plugins/: installed from their folders, consented to exactly what their manifests ask for,
// run against fake bridges (same rig as plugins_examples_test).
#include "plugins_test_util.h"
#include <QJsonDocument>
#include <ctime>

using namespace hn::plugins;
using namespace hn::plugins::test;

static QString official(const QString &id) { return QStringLiteral(HN_PLUGIN_OFFICIAL) + "/" + id; }
static const qint64 kClock = 1790863509;  // 2026-10-01 14:05:09 UTC (a Thursday)

// In-memory library on top of the fake index: files exist only when written (or seeded).
struct MemLib : FakeIndexLib {
    QMap<QString, QString> files;
    QSet<QString> unindexed;                             // on disk (files) but the index has no row yet
    BridgeStatus frontmatterStatus = BridgeStatus::Ok;   // forced answer of frontmatter() only (the index query keeps working)
    QMap<QString, QStringList> tagsOf;   // note path -> tags, for the tasks plugin's tag queries
    QList<NoteInfo> list(const QString &q) override {
        calls << "list:" + q;
        QList<NoteInfo> out;
        for (auto it = files.cbegin(); it != files.cend(); ++it) {
            if (!q.isEmpty() && !it.key().contains(q, Qt::CaseInsensitive)) continue;
            out.append({it.key(), it.key().section('/', -1).section('.', 0, -2)});
        }
        return out;
    }
    bool read(const QString &p, QString *t) override { calls << "read:" + p; if (!files.contains(p)) return false; *t = files[p]; return true; }
    bool write(const QString &p, const QString &t) override { calls << "write:" + p; files[p] = t; return true; }
    BridgeStatus frontmatter(const QString &p, QJsonObject *o) override {
        calls << "frontmatter:" + p;
        if (frontmatterStatus != BridgeStatus::Ok) return frontmatterStatus;
        if (auto st = gate(); st != BridgeStatus::Ok) return st;
        if (fm.contains(p)) { *o = fm[p]; return BridgeStatus::Ok; }
        if (!files.contains(p)) return BridgeStatus::NotFound;
        *o = QJsonObject{{"present", false}};      // like the real bridge: a note without frontmatter is a table, not an error
        return BridgeStatus::Ok;
    }
    QString create(const QString &ti, const QString &t) override { calls << "create:" + ti; const QString p = ti + ".md"; files[p] = t; return p; }
    // from="notes" is answered from `files` when every clause is path = / folder = / meta.aliases contains (aliases come from `fm`);
    // anything else (tasks, tag or title queries of the tasks plugin) keeps the canned rows of the base class.
    BridgeStatus query(const QJsonObject &spec, QJsonArray *rows) override {
        const auto where = spec.value("where").toArray();
        bool ours = spec.value("from").toString() == "notes";
        for (const auto &w : where) {
            const auto f = w.toObject().value("field").toString(), op = w.toObject().value("op").toString();
            ours = ours && (((f == "path" || f == "folder") && op == "=") || (f == "meta.aliases" && op == "contains") || (f == "tag" && op == "="));
        }
        if (!ours) return FakeIndexLib::query(spec, rows);
        calls << "query";
        querySpecs << spec;
        if (auto s = gate(); s != BridgeStatus::Ok) return s;
        const int limit = spec.contains("limit") ? spec.value("limit").toInt() : 1000;
        for (auto it = files.cbegin(); it != files.cend() && rows->size() < limit; ++it) {
            if (unindexed.contains(it.key())) continue;
            bool ok = true;
            for (const auto &w : where) {
                const auto o = w.toObject();
                const auto f = o.value("field").toString(), v = o.value("value").toString();
                if (f == "path") ok = ok && it.key() == v;
                else if (f == "folder") ok = ok && (it.key().contains('/') ? it.key().left(it.key().lastIndexOf('/')) : QString()) == v;
                else if (f == "tag") ok = ok && tagsOf.value(it.key()).contains(v);
                else {
                    bool any = false;
                    for (const auto &a : fm.value(it.key()).value("aliases").toArray()) any = any || a.toString().contains(v, Qt::CaseInsensitive);
                    ok = ok && any;
                }
            }
            if (!ok) continue;
            const QJsonObject all{{"path", it.key()}, {"title", it.key().section('/', -1).section('.', 0, -2)},
                                  {"tag", QJsonArray::fromStringList(tagsOf.value(it.key()))}};   // the index returns a note's tags as one list
            QJsonObject row;
            for (const auto &c : spec.value("select").toArray()) row[c.toString()] = all.value(c.toString());
            rows->append(row);
        }
        return BridgeStatus::Ok;
    }
};

struct OfficialRig {
    Rig r;
    MemLib lib;
    OfficialRig() {
        qputenv("TZ", "UTC");
        tzset();
        r.mgr->host()->env().clock = [] { return kClock; };
        r.mgr->host()->env().bridges.library = &lib;
    }
    void use(const QString &id) {
        const auto res = r.mgr->install(official(id), true);
        QVERIFY2(res.ok, qPrintable(res.errors.value(0).message));
        const auto m = r.mgr->info(id).manifest;
        QString err;
        QVERIFY2(r.mgr->consent(id, m.permissions, &err), qPrintable(err));
        QVERIFY2(r.mgr->enable(id, &err), qPrintable(err));
        QCOMPARE(r.mgr->info(id).status, Status::Enabled);
    }
    QString audit() const { return QString::fromUtf8(readFile(r.env.state() + "/plugins-audit.jsonl")); }
};

class OfficialTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        if (!QFileInfo::exists(official("slash-menu") + "/plugin.json"))
            QSKIP("hyprnotes-plugins repository not found (configure with -DHN_PLUGINS_DIR=...)");
    }
    // ------------------------------------------------------------ manifests (all four)
    void manifestsAreValidApi2ScriptPlugins_data() {
        QTest::addColumn<QString>("id");
        for (const char *id : {"slash-menu", "journal", "tasks", "wiki-links"}) QTest::newRow(id) << QString(id);
    }
    void manifestsAreValidApi2ScriptPlugins() {
        QFETCH(QString, id);
        if (!QFileInfo::exists(official(id) + "/plugin.json")) QSKIP("plugin not written yet");
        OfficialRig x;
        x.use(id);
        const auto m = x.r.mgr->info(id).manifest;
        QCOMPARE(m.api, 2);
        QCOMPARE(m.version, QString("1.0.0"));
        QVERIFY(!m.permissions.contains("net"));
        QVERIFY(QFileInfo::exists(official(id) + "/README.md"));
    }

    // ------------------------------------------------------------ slash-menu
    void slashMenuListsBlocksOnlyAtLineStart() {
        OfficialRig x;
        x.use("slash-menu");
        auto &r = x.r;
        QCOMPARE(r.mgr->registry()->completions("/").size(), 1);
        const auto all = r.mgr->complete("/", "", &r.note, true);
        QVERIFY(all.size() >= 10);
        for (const auto &i : all) QVERIFY2(i.markdown || i.label == "Link to note", qPrintable(i.label));
        QStringList labels;
        for (const auto &i : all) labels << i.label;
        for (const char *want : {"Heading 1", "Heading 2", "Heading 3", "Bulleted list", "Numbered list", "Task", "Quote", "Code block", "Divider", "Table", "Date", "Time", "Link to note"})
            QVERIFY2(labels.contains(want), want);
        // not at the start of a line (mid-sentence, a/b, https://x): no items at all
        QVERIFY(r.mgr->complete("/", "", &r.note, false).isEmpty());
        QVERIFY(r.mgr->complete("/", "he", &r.note, false).isEmpty());
        QCOMPARE(r.mgr->trust()->record("slash-menu").failures, 0);
        QVERIFY(r.note.ops.isEmpty());   // a completion source never edits the note
    }
    void slashMenuFiltersAndInsertsMarkdown() {
        OfficialRig x;
        x.use("slash-menu");
        auto &r = x.r;
        auto items = r.mgr->complete("/", "head", &r.note, true);
        QCOMPARE(items.size(), 3);
        QCOMPARE(items[0].insert, QString("# "));
        QCOMPARE(items[1].insert, QString("## "));
        QCOMPARE(items[2].insert, QString("### "));
        items = r.mgr->complete("/", "TASK", &r.note, true);                  // case-insensitive
        QCOMPARE(items.size(), 1);
        QCOMPARE(items[0].insert, QString("- [ ] "));
        items = r.mgr->complete("/", "uote", &r.note, true);                  // substring (not prefix) matches "Quote"; "ote" would also hit "Link to note"
        QCOMPARE(items.size(), 1);
        QVERIFY(r.mgr->complete("/", "zzzz", &r.note, true).isEmpty());
        items = r.mgr->complete("/", "date", &r.note, true);
        QCOMPARE(items.size(), 1);
        QCOMPARE(items[0].insert, QString("2026-10-01"));
        items = r.mgr->complete("/", "time", &r.note, true);
        QCOMPARE(items.size(), 1);
        QCOMPARE(items[0].insert, QString("14:05"));
        items = r.mgr->complete("/", "link", &r.note, true);
        QCOMPARE(items.size(), 1);
        QCOMPARE(items[0].insert, QString("[["));
        QVERIFY(!items[0].markdown);                                          // plain, so the [[ completion follows
        items = r.mgr->complete("/", "code", &r.note, true);
        QCOMPARE(items[0].insert, QString("```\n\n```"));
        items = r.mgr->complete("/", "divider", &r.note, true);
        QCOMPARE(items[0].insert, QString("---\n"));
    }
    // ------------------------------------------------------------ journal
    void journalTodayCreatesFromDefaultTemplateOpensAndNeverOverwrites() {
        OfficialRig x;
        x.use("journal");
        auto &r = x.r;
        QString err;
        QVERIFY2(r.mgr->runCommand("journal:today", &r.note, &err), qPrintable(err));
        QCOMPARE(x.lib.files.value("daily/2026-10-01.md"), QString("# 2026-10-01\n\n## Plan\n\n## Notes\n"));
        QCOMPARE(x.lib.opened, QStringList{"daily/2026-10-01.md@organizer"});
        x.lib.files["daily/2026-10-01.md"] = "my edits";
        QVERIFY(r.mgr->runCommand("journal:today", &r.note, &err));
        QCOMPARE(x.lib.files.value("daily/2026-10-01.md"), QString("my edits"));   // never overwritten
        QCOMPARE(x.lib.calls.filter("write:daily").size(), 1);
        QCOMPARE(x.lib.opened.last(), QString("daily/2026-10-01.md@organizer"));
    }
    void journalDailyTemplateFileAndSettings() {
        OfficialRig x;
        x.lib.files["templates/day.md"] = "# {{title}} ({{weekday}}) {{date}} {{time}} {{unknown}}\n";
        x.use("journal");
        auto &r = x.r;
        QVERIFY(r.mgr->host()->setSetting("journal", "daily_folder", "/log/2026/"));
        QVERIFY(r.mgr->host()->setSetting("journal", "date_format", "%d-%m-%Y"));
        QVERIFY(r.mgr->host()->setSetting("journal", "daily_template", "day.md"));
        QString err;
        QVERIFY2(r.mgr->runCommand("journal:today", &r.note, &err), qPrintable(err));
        QCOMPARE(x.lib.files.value("log/2026/01-10-2026.md"), QString("# 01-10-2026 (Thursday) 2026-10-01 14:05 {{unknown}}\n"));
    }
    void journalPreviousAndNextDayFollowTheOpenDailyNote() {
        OfficialRig x;
        x.use("journal");
        auto &r = x.r;
        QString err;
        r.note.p = "daily/2026-03-01.md";
        QVERIFY2(r.mgr->runCommand("journal:previous-day", &r.note, &err), qPrintable(err));
        QCOMPARE(x.lib.opened.last(), QString("daily/2026-02-28.md@organizer"));   // month boundary, non-leap year
        r.note.p = "daily/2028-02-28.md";
        QVERIFY(r.mgr->runCommand("journal:next-day", &r.note, &err));
        QCOMPARE(x.lib.opened.last(), QString("daily/2028-02-29.md@organizer"));   // leap day
        r.note.p = "notes/not-a-date.md";                                          // not a daily note: relative to today
        QVERIFY(r.mgr->runCommand("journal:previous-day", &r.note, &err));
        QCOMPARE(x.lib.opened.last(), QString("daily/2026-09-30.md@organizer"));
        QVERIFY(x.lib.files.contains("daily/2026-09-30.md"));
    }
    void journalSteppedDayIsFilledWithThatDayNotToday() {
        OfficialRig x;
        x.lib.files["templates/day.md"] = "# {{date}} {{weekday}} {{title}} {{time}}\n";
        x.use("journal");
        auto &r = x.r;
        QVERIFY(r.mgr->host()->setSetting("journal", "daily_template", "day.md"));
        QString err;
        r.note.p = "daily/2026-03-01.md";
        QVERIFY2(r.mgr->runCommand("journal:previous-day", &r.note, &err), qPrintable(err));
        QCOMPARE(x.lib.files.value("daily/2026-02-28.md"), QString("# 2026-02-28 Saturday 2026-02-28 14:05\n"));
    }
    void journalUnrecognisedDateFormatStepsFromToday() {
        OfficialRig x;
        x.use("journal");
        auto &r = x.r;
        QVERIFY(r.mgr->host()->setSetting("journal", "date_format", "%d-%m-%Y"));
        QString err;
        r.note.p = "daily/01-03-2026.md";
        QVERIFY2(r.mgr->runCommand("journal:previous-day", &r.note, &err), qPrintable(err));
        QCOMPARE(x.lib.opened.last(), QString("daily/30-09-2026.md@organizer"));
        QCOMPARE(x.lib.files.keys(), QStringList{"daily/30-09-2026.md"});
    }
    void journalTemplatesCommands() {
        OfficialRig x;
        x.use("journal");
        auto &r = x.r;
        QString err;
        // empty templates folder: helpful message, nothing written
        QVERIFY(r.mgr->runCommand("journal:new-from-template", &r.note, &err));
        QVERIFY2(r.ui.notes.last().contains("templates"), qPrintable(r.ui.notes.last()));
        QVERIFY(x.lib.calls.filter("create:").isEmpty() && x.lib.calls.filter("write:").isEmpty());
        x.lib.files["templates/meeting.md"] = "# {{title}}\n\nDate: {{date}}\n";
        x.lib.files["templates/readme.txt"] = "not a template";                 // only .md files count
        x.lib.files["templates/sub/deep.md"] = "sub-folders are not used";
        x.lib.files["templates/a-first.md"] = "first\n";                          // FakeUi::pick answers index 1 (0-based), so a second template must exist; sorted, meeting.md is it
        QVERIFY(r.mgr->runCommand("journal:new-from-template", &r.note, &err));   // FakeUi: pick -> 1, prompt -> default + "!"
        QCOMPARE(r.ui.lastPickItems, (QStringList{"a-first", "meeting"}));
        QCOMPARE(x.lib.calls.filter("create:").size(), 1);                       // hn.notes.create goes through MemLib::create, never write
        QVERIFY(x.lib.calls.filter("write:").isEmpty());
        QCOMPARE(x.lib.files.value("!.md"), QString("# !\n\nDate: 2026-10-01\n"));          // prompt answers default + "!" = "!"
        QVERIFY(!x.lib.opened.isEmpty());
        r.note.ops.clear();
        QVERIFY(r.mgr->runCommand("journal:insert-template", &r.note, &err));
        QCOMPARE(r.note.ops.size(), 1);
        QVERIFY(r.note.ops[0].startsWith("insert:# A"));                         // {{title}} is the open note's title ("A")
    }
    void journalRegistersToolbarButtonAndSettings() {
        OfficialRig x;
        x.use("journal");
        const auto info = x.r.mgr->info("journal");
        QCOMPARE(info.manifest.permissions, (QStringList{"notes.read", "notes.write", "notes.index", "note.read", "note.edit", "ui"}));
        auto *reg = x.r.mgr->registry();
        QCOMPARE(reg->toolbarButtons().size(), 1);
        QCOMPARE(reg->toolbarButtons().first().pluginId, QString("journal"));
        QCOMPARE(reg->toolbarButtons().first().icon, QString("calendar"));
        QStringList ids;
        for (const auto &s : reg->settings("journal")) ids << s.id;
        QCOMPARE(ids, (QStringList{"daily_folder", "date_format", "templates_folder", "daily_template"}));
        QVERIFY(!x.audit().contains("\"failure\""));
    }
    void journalIndexUnavailableCreatesNothingAndNotifiesOnce() {
        OfficialRig x;
        x.use("journal");
        auto &r = x.r;
        x.lib.forced = BridgeStatus::Timeout;
        QString err;
        QVERIFY2(r.mgr->runCommand("journal:today", &r.note, &err), qPrintable(err));
        QVERIFY(x.lib.files.isEmpty());
        QVERIFY(x.lib.calls.filter("write:").isEmpty());
        QCOMPARE(r.ui.notes.size(), 1);
        QVERIFY2(r.ui.notes.last().contains("index is not available"), qPrintable(r.ui.notes.last()));
        QCOMPARE(r.mgr->trust()->record("journal").failures, 0);
    }
    void journalExistingNoteFoundByIndexIsOpenedNotRewritten() {
        OfficialRig x;
        x.lib.files["daily/2026-10-01.md"] = "mine";
        x.use("journal");
        auto &r = x.r;
        QString err;
        QVERIFY2(r.mgr->runCommand("journal:today", &r.note, &err), qPrintable(err));
        QCOMPARE(x.lib.files.value("daily/2026-10-01.md"), QString("mine"));
        QVERIFY(x.lib.calls.filter("write:").isEmpty());
        QVERIFY(x.lib.calls.filter("read:").isEmpty());                         // existence is never decided by a read
        QCOMPARE(x.lib.opened, QStringList{"daily/2026-10-01.md@organizer"});
    }
    void journalNoteOnDiskButNotYetIndexedIsNotOverwritten() {
        OfficialRig x;
        x.lib.files["daily/2026-10-01.md"] = "synced from another device";
        x.lib.unindexed.insert("daily/2026-10-01.md");
        x.use("journal");
        auto &r = x.r;
        QString err;
        QVERIFY2(r.mgr->runCommand("journal:today", &r.note, &err), qPrintable(err));
        QVERIFY(x.lib.calls.filter("write:").isEmpty());
        QCOMPARE(x.lib.files.value("daily/2026-10-01.md"), QString("synced from another device"));
        QCOMPARE(x.lib.opened, QStringList{"daily/2026-10-01.md@organizer"});
    }
    void journalDiskCheckErrorRefusesToWrite() {
        OfficialRig x;
        x.use("journal");
        auto &r = x.r;
        x.lib.frontmatterStatus = BridgeStatus::Timeout;
        QString err;
        QVERIFY2(r.mgr->runCommand("journal:today", &r.note, &err), qPrintable(err));
        QVERIFY(x.lib.calls.filter("write:").isEmpty());
        QCOMPARE(r.ui.notes.size(), 1);
        QVERIFY(r.ui.notes.last().contains("not available"));
        QCOMPARE(r.mgr->trust()->record("journal").failures, 0);
    }
    void journalNonAsciiDateFormatNotifiesNotFailure() {
        OfficialRig x;
        x.use("journal");
        auto &r = x.r;
        QVERIFY(r.mgr->host()->setSetting("journal", "date_format", QString::fromUtf8("\xc3\xa9%Y")));
        QString err;
        QVERIFY2(r.mgr->runCommand("journal:today", &r.note, &err), qPrintable(err));
        QVERIFY(x.lib.files.isEmpty());
        QCOMPARE(r.ui.notes.size(), 1);
        QVERIFY2(r.ui.notes.last().contains("is not valid"), qPrintable(r.ui.notes.last()));
        QCOMPARE(r.mgr->trust()->record("journal").failures, 0);
    }
    void wikiLinksNoteOnDiskButNotIndexedIsOpenedNotOverwritten() {
        OfficialRig x;
        x.lib.files = {{"notes/a.md", "# A"}, {"notes/Here.md", "mine"}};
        x.lib.unindexed.insert("notes/Here.md");
        x.use("wiki-links");
        auto &r = x.r;
        r.note.p = "notes/a.md";
        QVERIFY(r.mgr->activateLink(LinkActivation{"link", "Here", "", "", ""}, &r.note));
        QVERIFY(x.lib.calls.filter("write:").isEmpty());
        QCOMPARE(x.lib.files.value("notes/Here.md"), QString("mine"));
        QCOMPARE(x.lib.opened.last(), QString("notes/Here.md@organizer"));
        x.lib.frontmatterStatus = BridgeStatus::Unsupported;                     // cannot tell: refuse
        r.ui.notes.clear();
        QVERIFY(r.mgr->activateLink(LinkActivation{"link", "Other", "", "", ""}, &r.note));
        QVERIFY(x.lib.calls.filter("write:").isEmpty());
        QCOMPARE(r.ui.notes.size(), 1);
        QCOMPARE(r.mgr->trust()->record("wiki-links").failures, 0);
    }
    void journalTemplatesFoundAmongManyNotes() {
        OfficialRig x;
        for (int i = 0; i < 250; ++i) x.lib.files[QString("misc/n%1.md").arg(i, 3, 10, QChar('0'))] = "x";
        x.lib.files["templates/one.md"] = "1";
        x.lib.files["templates/two.md"] = "2";
        x.use("journal");
        auto &r = x.r;
        r.ui.pickIndex = -1;
        QString err;
        QVERIFY2(r.mgr->runCommand("journal:new-from-template", &r.note, &err), qPrintable(err));
        QCOMPARE(r.ui.lastPickItems, (QStringList{"one", "two"}));
        QVERIFY(x.lib.calls.filter("list:").isEmpty());
    }
    void journalTemplatesIndexUnavailableNotifiesOnce() {
        OfficialRig x;
        x.lib.files["templates/one.md"] = "1";
        x.use("journal");
        auto &r = x.r;
        x.lib.forced = BridgeStatus::Timeout;
        QString err;
        QVERIFY2(r.mgr->runCommand("journal:new-from-template", &r.note, &err), qPrintable(err));
        QCOMPARE(r.ui.notes.size(), 1);
        QVERIFY(r.ui.notes.last().contains("not available"));
        QCOMPARE(r.mgr->trust()->record("journal").failures, 0);
    }
    void journalInvalidDailyPathNotifiesAndIsNotAFailure_data() {
        QTest::addColumn<QString>("key");
        QTest::addColumn<QString>("value");
        QTest::newRow("dotdot format") << "date_format" << "../%Y";
        QTest::newRow("slash format") << "date_format" << "/%Y";
        QTest::newRow("backslash format") << "date_format" << "a\\%Y";
        QTest::newRow("dotdot folder") << "daily_folder" << "a/../../b";
    }
    void journalInvalidDailyPathNotifiesAndIsNotAFailure() {
        QFETCH(QString, key);
        QFETCH(QString, value);
        OfficialRig x;
        x.use("journal");
        auto &r = x.r;
        QVERIFY(r.mgr->host()->setSetting("journal", key, value));
        QString err;
        QVERIFY2(r.mgr->runCommand("journal:today", &r.note, &err), qPrintable(err));
        QVERIFY(x.lib.files.isEmpty());
        QCOMPARE(r.ui.notes.size(), 1);
        QVERIFY2(r.ui.notes.last().contains("is not valid"), qPrintable(r.ui.notes.last()));
        QCOMPARE(r.mgr->trust()->record("journal").failures, 0);
    }
    // ------------------------------------------------------------ tasks
    void tasksPanelBuiltInViews() {
        OfficialRig x;
        x.use("tasks");
        auto &r = x.r;
        QList<PanelBlock> b;
        QString err;
        QVERIFY2(r.mgr->renderPanel("tasks", "tasks", &r.note, &b, &err), qPrintable(err));
        QVERIFY(!b.isEmpty());
        QCOMPARE(x.lib.querySpecs.last().value("from").toString(), QString("tasks"));           // default view: open tasks
        QCOMPARE(x.lib.querySpecs.last().value("where").toArray().at(0).toObject().value("value").toBool(), false);
        // rows become clickable items that open the note
        bool item = false;
        for (const auto &blk : b) for (const auto &i : blk.items) if (i.title == "buy milk" && i.path == "inbox/todo.md" && i.line == 3) item = true;
        QVERIFY(item);
        QCOMPARE(r.mgr->trust()->record("tasks").failures, 0);
    }
    void tasksFilterCompilation_data() {
        QTest::addColumn<QString>("filter");
        QTest::addColumn<QString>("from");
        QTest::addColumn<int>("clauses");
        QTest::newRow("open") << "open" << "tasks" << 1;
        QTest::newRow("open+text") << "open text:milk" << "tasks" << 2;
        QTest::newRow("done") << "done" << "tasks" << 1;
        QTest::newRow("tag") << "tag:work" << "notes" << 1;
        QTest::newRow("tag+folder+recent") << "tag:work in:projects recent:7" << "notes" << 3;
        QTest::newRow("title") << "title:Plan" << "notes" << 1;
        QTest::newRow("case") << "OPEN Text:Milk" << "tasks" << 2;
        QTest::newRow("non-ascii") << "text:ça" << "tasks" << 1;
    }
    void tasksFilterCompilation() {
        QFETCH(QString, filter);
        QFETCH(QString, from);
        QFETCH(int, clauses);
        OfficialRig x;
        x.use("tasks");
        auto &r = x.r;
        r.ui.nextPrompts = {"My view", filter};                    // scripted answers for the two prompts
        QString err;
        QVERIFY2(r.mgr->runCommand("tasks:save-query", &r.note, &err), qPrintable(err));
        QVERIFY(r.ui.notes.last().contains("Saved"));
        r.ui.pickIndex = 0;                                        // first saved query
        QVERIFY2(r.mgr->runCommand("tasks:run-query", &r.note, &err), qPrintable(err));
        QList<PanelBlock> b;
        QVERIFY(r.mgr->renderPanel("tasks", "tasks", &r.note, &b, &err));
        const auto spec = x.lib.querySpecs.last();
        QCOMPARE(spec.value("from").toString(), from);
        QCOMPARE(spec.value("where").toArray().size(), clauses);
        if (filter.contains("recent:")) {
            const auto mt = spec.value("where").toArray().last().toObject();
            QCOMPARE(mt.value("field").toString(), QString("mtime"));
            QCOMPARE(qint64(mt.value("value").toDouble()), (kClock - 7 * 86400) * 1000);       // milliseconds
        }
    }
    void tasksRejectsBadFiltersAndLeavesStorageAlone_data() {
        QTest::addColumn<QString>("filter");
        QTest::newRow("empty") << "";
        QTest::newRow("blank") << "   ";
        QTest::newRow("unknown term") << "colour:red";
        QTest::newRow("mixed groups") << "open tag:work";
        QTest::newRow("bad days") << "recent:abc";
        QTest::newRow("empty value") << "tag:";
        QTest::newRow("too long") << QString(600, 'a');
        QTest::newRow("just over the limit") << QString(301, 'a');
        QTest::newRow("empty tag after #") << "tag:#";
        QTest::newRow("empty folder after slash") << "in:/";
    }
    void tasksRejectsBadFiltersAndLeavesStorageAlone() {
        QFETCH(QString, filter);
        OfficialRig x;
        x.use("tasks");
        auto &r = x.r;
        r.ui.nextPrompts = {"Name", filter};
        QString err;
        QVERIFY(r.mgr->runCommand("tasks:save-query", &r.note, &err));
        QVERIFY2(!r.ui.notes.isEmpty() && !r.ui.notes.last().contains("Saved"), qPrintable(r.ui.notes.value(0)));
        r.ui.notes.clear();
        QVERIFY(r.mgr->runCommand("tasks:run-query", &r.note, &err));      // nothing stored -> friendly message
        QVERIFY(r.ui.notes.last().contains("No saved"));
        QCOMPARE(r.mgr->trust()->record("tasks").failures, 0);
    }
    void tasksCapsSavedQueriesAndDeletes() {
        OfficialRig x;
        x.use("tasks");
        auto &r = x.r;
        QString err;
        for (int i = 0; i < 32; ++i) {
            r.ui.nextPrompts = {QString("q%1").arg(i), "open"};
            QVERIFY(r.mgr->runCommand("tasks:save-query", &r.note, &err));
        }
        // The host drops notifications beyond 10 per 10 s, so restart the plugin (storage persists) to get a fresh budget.
        QVERIFY(r.mgr->disable("tasks"));
        QVERIFY2(r.mgr->enable("tasks", &err), qPrintable(err));
        r.ui.notes.clear();
        r.ui.nextPrompts = {"q32", "open"};
        QVERIFY(r.mgr->runCommand("tasks:save-query", &r.note, &err));
        QVERIFY2(r.ui.notes.last().contains("maximum"), qPrintable(r.ui.notes.last()));
        r.ui.pickIndex = -1;                                         // cancel; only look at the list size
        QVERIFY(r.mgr->runCommand("tasks:run-query", &r.note, &err));
        QCOMPARE(r.ui.lastPickItems.size(), 32);                     // the refused 33rd save stored nothing
        r.ui.pickIndex = 0;
        QVERIFY(r.mgr->runCommand("tasks:delete-query", &r.note, &err));
        QVERIFY(r.ui.notes.last().contains("Deleted"));
        r.ui.nextPrompts = {"again", "done"};
        QVERIFY(r.mgr->runCommand("tasks:save-query", &r.note, &err));      // room again after a delete
        QVERIFY(r.ui.notes.last().contains("Saved"));
    }
    void tasksFolderFilterEscapesLikeWildcards() {
        OfficialRig x;
        x.use("tasks");
        auto &r = x.r;
        r.ui.nextPrompts = {"Folder", "in:foo_bar/"};
        QString err;
        QVERIFY2(r.mgr->runCommand("tasks:save-query", &r.note, &err), qPrintable(err));
        r.ui.pickIndex = 0;
        QVERIFY2(r.mgr->runCommand("tasks:run-query", &r.note, &err), qPrintable(err));
        QList<PanelBlock> b;
        QVERIFY2(r.mgr->renderPanel("tasks", "tasks", &r.note, &b, &err), qPrintable(err));
        const auto w = x.lib.querySpecs.last().value("where").toArray().at(0).toObject();
        QCOMPARE(w.value("op").toString(), QString("like"));
        QCOMPARE(w.value("value").toString(), QString("foo\\_bar%"));
    }
    void tasksLongTaskTextAndNoteTitleStillRender() {
        OfficialRig x;
        x.use("tasks");
        auto &r = x.r;
        x.lib.queryRows = QJsonArray{QJsonObject{{"path", "a.md"}, {"line", 1}, {"text", QString(600, 'x')}, {"done", false}}};
        QList<PanelBlock> b;
        QString err;
        QVERIFY2(r.mgr->renderPanel("tasks", "tasks", &r.note, &b, &err), qPrintable(err));
        r.ui.nextPrompts = {"Notes", "title:a"};
        QVERIFY(r.mgr->runCommand("tasks:save-query", &r.note, &err));
        r.ui.pickIndex = 0;
        x.lib.queryRows = QJsonArray{QJsonObject{{"path", "b.md"}, {"title", QString(150, QChar(0x00e7))}, {"mtime", 1}}};   // 300 bytes
        QVERIFY(r.mgr->runCommand("tasks:run-query", &r.note, &err));
        QList<PanelBlock> b2;
        QVERIFY2(r.mgr->renderPanel("tasks", "tasks", &r.note, &b2, &err), qPrintable(err));
        int items = 0;
        for (const auto &list : {b, b2}) for (const auto &blk : list) for (const auto &i : blk.items) {
            ++items;
            QVERIFY2(i.title.toUtf8().size() <= 200 && !i.title.isEmpty(), qPrintable(QString::number(i.title.toUtf8().size())));
        }
        QCOMPARE(items, 2);
        QCOMPARE(r.mgr->trust()->record("tasks").failures, 0);
    }
    void tasksIndexUnavailableIsFriendlyNotAFailure() {
        OfficialRig x;
        x.use("tasks");
        auto &r = x.r;
        x.lib.forced = BridgeStatus::Timeout;
        QList<PanelBlock> b;
        QString err;
        QVERIFY2(r.mgr->renderPanel("tasks", "tasks", &r.note, &b, &err), qPrintable(err));
        QString all;
        for (const auto &blk : b) all += blk.text + "|";
        QVERIFY2(all.contains("not available"), qPrintable(all));
        QCOMPARE(r.mgr->trust()->record("tasks").failures, 0);
    }
    // ------------------------------------------------------------ tasks: By tag, "N more", stale view
    static PanelBlock findButton(const QList<PanelBlock> &b, const QString &label) {
        for (const auto &blk : b) if (blk.type == "button" && blk.text == label) return blk;
        return {};
    }
    static QList<PanelBlock> items(const QList<PanelBlock> &b) {
        QList<PanelBlock> out;
        for (const auto &blk : b) if (blk.type == "list") out += blk.items;
        return out;
    }
    void tasksByTagListsTagsAndOpensOne() {
        OfficialRig x;
        x.use("tasks");
        auto &r = x.r;
        x.lib.files = {{"a.md", ""}, {"b.md", ""}, {"c.md", ""}};
        x.lib.tagsOf = {{"a.md", {"work", "idea"}}, {"b.md", {"work"}}, {"c.md", {}}};
        QList<PanelBlock> b;
        QString err;
        QVERIFY2(r.mgr->renderPanel("tasks", "tasks", &r.note, &b, &err), qPrintable(err));
        const auto btn = findButton(b, "By tag");
        QVERIFY(btn.click != 0);
        QVERIFY2(r.mgr->panelClick("tasks", "tasks", btn.click, &r.note, &err), qPrintable(err));
        QVERIFY2(r.mgr->renderPanel("tasks", "tasks", &r.note, &b, &err), qPrintable(err));
        const auto tags = items(b);
        QCOMPARE(tags.size(), 2);                                   // sorted by name: idea, work
        QCOMPARE(tags[0].title, QString("idea"));
        QCOMPARE(tags[0].subtitle, QString("1 note"));
        QCOMPARE(tags[1].title, QString("work"));
        QCOMPARE(tags[1].subtitle, QString("2 notes"));
        QVERIFY2(r.mgr->panelClick("tasks", "tasks", tags[1].click, &r.note, &err), qPrintable(err));
        QVERIFY2(r.mgr->renderPanel("tasks", "tasks", &r.note, &b, &err), qPrintable(err));
        const auto w = x.lib.querySpecs.last().value("where").toArray().at(0).toObject();   // same as the saved filter tag:work
        QCOMPARE(w.value("field").toString(), QString("tag"));
        QCOMPARE(w.value("value").toString(), QString("work"));
        QCOMPARE(items(b).size(), 2);                               // a.md and b.md
        QCOMPARE(r.mgr->trust()->record("tasks").failures, 0);
    }
    void tasksByTagCapsTheTagList() {
        OfficialRig x;
        x.use("tasks");
        auto &r = x.r;
        QStringList many;
        for (int i = 0; i < 130; ++i) many << QString("t%1").arg(i, 3, 10, QChar('0'));
        x.lib.files = {{"a.md", ""}};
        x.lib.tagsOf = {{"a.md", many}};
        QList<PanelBlock> b;
        QString err;
        QVERIFY(r.mgr->renderPanel("tasks", "tasks", &r.note, &b, &err));
        QVERIFY(r.mgr->panelClick("tasks", "tasks", findButton(b, "By tag").click, &r.note, &err));
        QVERIFY2(r.mgr->renderPanel("tasks", "tasks", &r.note, &b, &err), qPrintable(err));
        QCOMPARE(items(b).size(), 100);
        QCOMPARE(b.last().type, QString("empty"));
        QVERIFY2(b.last().text.contains("30 more"), qPrintable(b.last().text));
        QVERIFY(b.size() < 100);
    }
    void tasksShowsAtMost100WithMoreLine() {
        OfficialRig x;
        x.use("tasks");
        auto &r = x.r;
        auto fill = [&](int n) {
            QJsonArray rows;
            for (int i = 0; i < n; ++i) rows.append(QJsonObject{{"path", "a.md"}, {"line", i + 1}, {"text", QString("t%1").arg(i)}, {"done", false}});
            x.lib.queryRows = rows;
        };
        QList<PanelBlock> b;
        QString err;
        fill(340);
        QVERIFY2(r.mgr->renderPanel("tasks", "tasks", &r.note, &b, &err), qPrintable(err));
        QCOMPARE(x.lib.querySpecs.last().value("limit").toInt(), 500);
        QCOMPARE(items(b).size(), 100);
        QCOMPARE(b.last().type, QString("empty"));
        QCOMPARE(b.last().text, QString("240 more not shown"));
        bool heading = false;
        for (const auto &blk : b) if (blk.type == "heading") heading = blk.text == "Open tasks (340)";
        QVERIFY(heading);
        fill(500);
        QVERIFY(r.mgr->renderPanel("tasks", "tasks", &r.note, &b, &err));
        QCOMPARE(b.last().text, QString("400+ more not shown"));
        heading = false;
        for (const auto &blk : b) if (blk.type == "heading") heading = blk.text == "Open tasks (500+)";
        QVERIFY(heading);
        fill(100);
        QVERIFY(r.mgr->renderPanel("tasks", "tasks", &r.note, &b, &err));
        QCOMPARE(b.last().type, QString("list"));                  // exactly 100: nothing hidden
    }
    void tasksDeletingTheActiveQueryResetsTheView() {
        OfficialRig x;
        x.use("tasks");
        auto &r = x.r;
        r.ui.nextPrompts = {"Done ones", "done"};
        QString err;
        QVERIFY(r.mgr->runCommand("tasks:save-query", &r.note, &err));
        r.ui.pickIndex = 0;
        QVERIFY(r.mgr->runCommand("tasks:run-query", &r.note, &err));
        QList<PanelBlock> b;
        QVERIFY(r.mgr->renderPanel("tasks", "tasks", &r.note, &b, &err));
        bool shown = false;
        for (const auto &blk : b) if (blk.type == "heading") shown = blk.text.startsWith("Done ones");
        QVERIFY(shown);
        QVERIFY(r.mgr->runCommand("tasks:delete-query", &r.note, &err));
        QVERIFY(r.mgr->renderPanel("tasks", "tasks", &r.note, &b, &err));
        for (const auto &blk : b) if (blk.type == "heading") QVERIFY2(blk.text.startsWith("Open tasks"), qPrintable(blk.text));
    }
    // ------------------------------------------------------------ wiki-links
    void wikiLinksCompletionMatchesTitlesAndDisambiguates() {
        OfficialRig x;
        x.lib.files = {{"alpha.md", "# Alpha"}, {"x/dup.md", "# Dup"}, {"y/dup.md", "# Dup"}, {"notes/a.md", "# A\n## Intro\n## Setup\n### Deep"}};
        x.use("wiki-links");
        auto &r = x.r;
        auto items = r.mgr->complete("[[", "alp", &r.note);
        QCOMPARE(items.size(), 1);
        QCOMPARE(items[0].insert, QString("alpha]]"));
        items = r.mgr->complete("[[", "dup", &r.note);
        QCOMPARE(items.size(), 2);                                   // ambiguous name (FakeIndexLib: resolves["dup"] is ambiguous)
        QCOMPARE(items[0].insert, QString("x/dup]]"));
        QCOMPARE(items[1].insert, QString("y/dup]]"));
        // headings after '#': the name must resolve (FakeIndexLib: resolves["b"] -> b.md)
        x.lib.files["b.md"] = "# B\n## First part\n```\n# not a heading\n```\n## Second part\n";
        items = r.mgr->complete("[[", "b#", &r.note);
        QStringList inserts;
        for (const auto &i : items) inserts << i.insert;
        QCOMPARE(inserts, (QStringList{"b#B]]", "b#First part]]", "b#Second part]]"}));   // fenced code is skipped
        items = r.mgr->complete("[[", "b#sec", &r.note);
        QCOMPARE(items.size(), 1);
        QVERIFY(r.mgr->complete("[[", "nope#x", &r.note).isEmpty());                    // unresolved name: nothing, no error
        // a very long heading and long accented names stay inside the host's item limits
        x.lib.files["b.md"] = "# " + QString(300, QChar(0xE9)) + "\n";
        items = r.mgr->complete("[[", "b#", &r.note);
        QCOMPARE(items.size(), 1);
        QVERIFY(items[0].label.toUtf8().size() <= 200);
        QCOMPARE(r.mgr->trust()->record("wiki-links").failures, 0);
        QVERIFY(r.note.ops.isEmpty());
    }
    void wikiLinksActivateResolvedOpensAndUnresolvedCreatesSafely() {
        OfficialRig x;
        x.lib.files = {{"notes/a.md", "# A"}};
        x.use("wiki-links");
        auto &r = x.r;
        r.note.p = "notes/a.md";
        LinkActivation resolved{"link", "b", "", "", "b.md"};
        QVERIFY(r.mgr->activateLink(resolved, &r.note));                                // handled
        QCOMPARE(x.lib.opened.last(), QString("b.md@organizer"));
        // unresolved: confirm (fake: yes) -> created next to the current note -> opened
        LinkActivation fresh{"link", "Fresh idea", "", "", ""};
        QVERIFY(r.mgr->activateLink(fresh, &r.note));
        QCOMPARE(x.lib.files.value("notes/Fresh idea.md"), QString("# Fresh idea\n\n"));
        QCOMPARE(x.lib.opened.last(), QString("notes/Fresh idea.md@organizer"));
        // nested target keeps its folder under the current note's folder
        LinkActivation nested{"link", "area/Idea", "", "", ""};
        QVERIFY(r.mgr->activateLink(nested, &r.note));
        QVERIFY(x.lib.files.contains("notes/area/Idea.md"));
        // declined: nothing written
        r.ui.confirmAnswer = false;
        const auto before = x.lib.files;
        LinkActivation declined{"link", "Declined", "", "", ""};
        r.mgr->activateLink(declined, &r.note);
        QCOMPARE(x.lib.files, before);
        r.ui.confirmAnswer = true;
        // hostile targets are refused (nothing written, plugin not penalised)
        for (const QString &bad : {QString("../escape"), QString("/abs"), QString("a\\b"), QString("x/../../y"), QString("ctl\x01z"), QString("."), QString(".."), QString(""), QString("a/"), QString(" lead"), QString("trail ")}) {
            const auto snap = x.lib.files;
            r.mgr->activateLink(LinkActivation{"link", bad, "", "", ""}, &r.note);
            QCOMPARE(x.lib.files, snap);
        }
        QCOMPARE(r.mgr->trust()->record("wiki-links").failures, 0);
        // the setting skips the confirmation
        QVERIFY(r.mgr->host()->setSetting("wiki-links", "confirm_create", false));
        r.ui.confirmAnswer = false;
        QVERIFY(r.mgr->activateLink(LinkActivation{"link", "No ask", "", "", ""}, &r.note));
        QVERIFY(x.lib.files.contains("notes/No ask.md"));
    }
    void wikiLinksCreateUsesTheIndexNotAReadToDecide() {
        OfficialRig x;
        x.lib.files = {{"notes/a.md", "# A"}, {"notes/Here.md", "mine"}};
        x.use("wiki-links");
        auto &r = x.r;
        r.note.p = "notes/a.md";
        QVERIFY(r.mgr->activateLink(LinkActivation{"link", "Here", "", "", ""}, &r.note));
        QCOMPARE(x.lib.files.value("notes/Here.md"), QString("mine"));
        QVERIFY(x.lib.calls.filter("write:").isEmpty());
        QCOMPARE(x.lib.opened.last(), QString("notes/Here.md@organizer"));
        x.lib.forced = BridgeStatus::Timeout;
        r.ui.notes.clear();
        const auto before = x.lib.files;
        QVERIFY(r.mgr->activateLink(LinkActivation{"link", "New one", "", "", ""}, &r.note));
        QCOMPARE(x.lib.files, before);
        QVERIFY(x.lib.calls.filter("write:").isEmpty());
        QCOMPARE(r.ui.notes.size(), 1);
        QVERIFY(r.ui.notes.last().contains("index is not available"));
        QCOMPARE(r.mgr->trust()->record("wiki-links").failures, 0);
    }
    void wikiLinksPanelStaysUnderTheBlockLimitWithHugeLists() {
        OfficialRig x;
        x.use("wiki-links");
        auto &r = x.r;
        QList<LinkRow> out, back;
        for (int i = 0; i < 250; ++i) out.append({"link", QString("t%1").arg(i), "", "", "", "", "ctx", 1});
        for (int i = 0; i < 80; ++i) back.append({"link", "a", "", "", "notes/a.md", QString("s%1.md").arg(i), "ctx", 1});
        x.lib.outgoing["notes/a.md"] = out;
        x.lib.incoming["notes/a.md"] = back;
        QList<PanelBlock> b;
        QString err;
        QVERIFY2(r.mgr->renderPanel("wiki-links", "links", &r.note, &b, &err), qPrintable(err));
        int total = 0;
        QStringList headings, empties;
        for (const auto &blk : b) {
            total += 1 + int(blk.items.size());
            if (blk.type == "heading") headings << blk.text;
            if (blk.type == "empty") empties << blk.text;
        }
        QVERIFY2(total <= 200, qPrintable(QString::number(total)));
        QVERIFY2(headings.join("|").contains("Unresolved (250)"), qPrintable(headings.join("|")));
        QVERIFY2(empties.contains("190 more"), qPrintable(empties.join("|")));
        QCOMPARE(r.mgr->trust()->record("wiki-links").failures, 0);
    }
    void wikiLinksCompletionOffersAliases() {
        OfficialRig x;
        x.lib.files = {{"people/robert.md", "# Robert"}, {"alpha.md", "# Alpha"}, {"x/dup.md", "# Dup"}, {"y/dup.md", "# Dup"}};
        x.lib.fm["people/robert.md"] = QJsonObject{{"aliases", QJsonArray{"Bob", "Bobby"}}};
        x.lib.fm["x/dup.md"] = QJsonObject{{"aliases", QJsonArray{"Duplicate"}}};
        x.use("wiki-links");
        auto &r = x.r;
        auto items = r.mgr->complete("[[", "bob", &r.note);
        QCOMPARE(items.size(), 2);
        QCOMPARE(items[0].label, QString("Bob"));
        QCOMPARE(items[0].detail, QString("alias of robert"));
        QCOMPARE(items[0].insert, QString("robert|Bob]]"));
        QCOMPARE(items[1].insert, QString("robert|Bobby]]"));
        items = r.mgr->complete("[[", "duplic", &r.note);                     // ambiguous stem: folder-qualified
        QCOMPARE(items.size(), 1);
        QCOMPARE(items[0].insert, QString("x/dup|Duplicate]]"));
        // a name match and an alias on other notes are both offered, within the cap
        items = r.mgr->complete("[[", "alp", &r.note);
        QCOMPARE(items.size(), 1);
        x.lib.forced = BridgeStatus::Timeout;                                   // index down: aliases silently skipped, names still work
        items = r.mgr->complete("[[", "alp", &r.note);
        QCOMPARE(items.size(), 1);
        QCOMPARE(r.mgr->trust()->record("wiki-links").failures, 0);
    }
    void wikiLinksPanelSections() {
        OfficialRig x;
        x.use("wiki-links");
        auto &r = x.r;
        QList<PanelBlock> b;
        QString err;
        QVERIFY2(r.mgr->renderPanel("wiki-links", "links", &r.note, &b, &err), qPrintable(err));   // note notes/a.md from FakeIndexLib
        QStringList headings;
        for (const auto &blk : b) if (blk.type == "heading") headings << blk.text;
        QVERIFY2(headings.join("|").contains("Backlinks (2)"), qPrintable(headings.join("|")));
        QVERIFY2(headings.join("|").contains("Outgoing (1)"), qPrintable(headings.join("|")));     // the fake has one resolved outgoing row
        QVERIFY2(headings.join("|").contains("Unresolved (1)"), qPrintable(headings.join("|")));   // the embed row has no 'resolved'
        // the embed is listed but not clickable
        bool embedRow = false;
        for (const auto &blk : b) for (const auto &i : blk.items) if (i.title == "pic") { embedRow = i.subtitle == "Embed" && i.click == 0; }
        QVERIFY(embedRow);
        // clicking a backlink opens it
        bool clicked = false;
        for (const auto &blk : b) if (blk.type == "list") for (const auto &i : blk.items) if (i.path == "c.md") {
            QVERIFY(r.mgr->panelClick("wiki-links", "links", i.click, &r.note, &err));
            QCOMPARE(x.lib.opened.last(), QString("c.md@organizer"));
            clicked = true;
        }
        QVERIFY(clicked);
        // no note open / index unavailable: friendly, not a failure
        QVERIFY(r.mgr->renderPanel("wiki-links", "links", nullptr, &b, &err));
        QVERIFY(b[0].text.contains("Open a note"));
        x.lib.forced = BridgeStatus::Timeout;
        QVERIFY(r.mgr->renderPanel("wiki-links", "links", &r.note, &b, &err));
        QString all;
        for (const auto &blk : b) all += blk.text + "|";
        QVERIFY2(all.contains("not available"), qPrintable(all));
        QCOMPARE(r.mgr->trust()->record("wiki-links").failures, 0);
        QVERIFY(r.note.ops.isEmpty());
    }
};
QTEST_MAIN(OfficialTest)
#include "plugins_official_test.moc"
