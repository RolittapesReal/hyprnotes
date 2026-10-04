// Plugin API v2 in the app: index-backed LibraryBridge, rename with link updates (incl. hostile cases), panel dock lifecycle,
// editor hooks, the three example plugins end to end, zero-cost-when-off, memory cost, screenshots (HN_SCREENSHOT_DIR).
#include "app_test_util.h"
#include "cli.h"
#include "panel_dock.h"
#include "plugins_page.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QPainter>
#include <QProcessEnvironment>
#include <QThread>
#include <QTextStream>
#include <sys/stat.h>

using namespace apptest;
using namespace hn::plugins;
using hn::editor::LinkState;

namespace {
QString example(const QString &id) { return QString(HN_SOURCE_DIR) + "/examples/plugins/" + id; }
QString mkPlugin(const QString &root, const QString &id, const QStringList &perms, const QByteArray &lua) {
    const QString dir = root + "/src-" + id;
    QDir().mkpath(dir);
    QJsonObject o{{"id", id}, {"name", id}, {"version", "1.0.0"}, {"author", "tests"}, {"description", "t"}, {"api", 2}, {"tier", "script"},
                  {"entry", "main.lua"}, {"permissions", QJsonArray::fromStringList(perms)}, {"min_app", "0.1.0"}};
    QFile a(dir + "/plugin.json"), b(dir + "/main.lua");
    if (!a.open(QIODevice::WriteOnly) || !b.open(QIODevice::WriteOnly)) qFatal("cannot write plugin");
    a.write(QJsonDocument(o).toJson());
    b.write(lua);
    return dir;
}
ControllerOptions accepting(Lib &l) {
    auto o = l.opts();
    o.pluginHooks.consent = [](const ConsentRequest &) { return true; };
    return o;
}
bool installOk(AppController &c, const QString &src) {
    QString msg;
    const auto r = c.plugins().install(src, nullptr, &msg);
    if (!r.ok) qWarning() << msg;
    return r.ok && c.plugins().manager()->info(r.id).status == Status::Enabled;
}
qint64 pssKb() {
    QFile f("/proc/self/smaps_rollup");
    if (!f.open(QIODevice::ReadOnly)) return -1;
    for (const auto &l : f.readAll().split('\n')) if (l.startsWith("Pss:")) return l.simplified().split(' ')[1].toLongLong();
    return -1;
}
void waitIndex(AppController &c, int n) {
    auto *idx = c.index();
    QTRY_COMPARE_WITH_TIMEOUT(idx->count(), n, 15000);
    QTest::qWait(150);
}
void seed(Lib &l) {
    l.write("Alpha.md", "---\ntitle: Alpha Note\ntags: [one, two]\naliases: [A]\ncustom: hello\n---\n# Alpha\n\nBody.\n");
    l.write("Beta.md", "# Beta\n\nSee [[Alpha]] and [[Missing]].\n\n- [ ] write tests\n- [x] done thing\n");
    l.write("Work/Gamma.md", "# Gamma\n\nAgain [[Alpha#Intro|the alpha]] and [[Beta]].\n\n- [ ] ship it\n");
    l.write("Work/Alpha.md", "# Other Alpha\n");   // makes [[Alpha]] ambiguous
}
}  // namespace

struct Rn {
    Lib l;
    std::unique_ptr<AppController> c;
    QStringList seenLines;
    LinkChoice choice = LinkChoice::Update;
    std::function<void()> beforeApply;
    int dialogs = 0;
    explicit Rn(bool withHooks = true) {
        l.write("Alpha.md", "# Alpha\n\nSelf [[Alpha]].\n");
        l.write("Beta.md", "# Beta\n\nSee [[Alpha]] and [[Alpha|the first]].\n");
        l.write("Work/Gamma.md", "# Gamma\n\nAgain [[Alpha#Intro]] and ![[Alpha]].\n");
        l.write("Solo.md", "# Solo\n\nNothing links here.\n");
        auto o = l.opts();
        if (withHooks)
            o.linkChoice = [this](const QString &, const QStringList &lines) { ++dialogs; seenLines = lines; if (beforeApply) beforeApply(); return choice; };
        c = std::make_unique<AppController>(o);
        waitIndex(*c, 4);
    }
};


class V2Test : public QObject {
    Q_OBJECT
private slots:
    // ---------------------------------------------------------------- bridges
    void bridgesOverRealLibrary() {
        Lib l;
        l.write("Alpha.md", "---\ntitle: Alpha Note\ntags: [one, two]\naliases: [A]\ncustom: hello\n---\n# Alpha\n");
        l.write("Beta.md", "# Beta\n\nSee [[Alpha]] and [[Missing]].\n\n- [ ] write tests\n- [x] done thing\n");
        l.write("Work/Gamma.md", "# Gamma\n\nAgain [[Alpha#Intro|the alpha]] and [[Beta]].\n\n- [ ] ship it\n");
        l.write("Delta.md", "# Delta\n");
        AppController c(l.opts());
        waitIndex(c, 4);
        PluginLibraryBridge lib(&c);
        QList<LinkRow> rows;
        QCOMPARE(lib.links("Beta.md", &rows), BridgeStatus::Ok);
        QCOMPARE(rows.size(), 2);
        QCOMPARE(rows[0].target, QString("Alpha"));
        QCOMPARE(rows[0].resolved, QString("Alpha.md"));
        QVERIFY(rows[1].resolved.isEmpty());
        QCOMPARE(lib.backlinks("Alpha.md", 50, 0, &rows), BridgeStatus::Ok);
        QStringList srcs;
        for (const auto &r : rows) srcs << r.src;
        srcs.sort();
        QCOMPARE(srcs, (QStringList{"Beta.md", "Work/Gamma.md"}));
        QVERIFY(!rows[0].context.isEmpty());
        QCOMPARE(lib.backlinks("Alpha.md", 1, 1, &rows), BridgeStatus::Ok);
        QCOMPARE(rows.size(), 1);
        QCOMPARE(lib.links("Nope.md", &rows), BridgeStatus::NotFound);
        QCOMPARE(lib.links("../x.md", &rows), BridgeStatus::Invalid);
        ResolveResult rr;
        QCOMPARE(lib.resolve("Alpha", &rr), BridgeStatus::Ok);
        QCOMPARE(rr.status, QString("resolved"));
        QCOMPARE(rr.path, QString("Alpha.md"));
        QCOMPARE(lib.resolve("Nothing", &rr), BridgeStatus::Ok);
        QCOMPARE(rr.status, QString("unresolved"));
        QJsonObject fm;
        QCOMPARE(lib.frontmatter("Alpha.md", &fm), BridgeStatus::Ok);
        QCOMPARE(fm["title"].toString(), QString("Alpha Note"));
        QCOMPARE(fm["tags"].toArray().size(), 2);
        QCOMPARE(fm["custom"].toString(), QString("hello"));
        fm = {};
        QCOMPARE(lib.frontmatter("Beta.md", &fm), BridgeStatus::Ok);
        QVERIFY(!fm["present"].toBool());
        QCOMPARE(lib.frontmatter("Nope.md", &fm), BridgeStatus::NotFound);
        QJsonArray out;
        const QJsonObject spec{{"from", "tasks"}, {"where", QJsonArray{QJsonObject{{"field", "done"}, {"op", "="}, {"value", false}}}},
                               {"order", QJsonArray{QJsonObject{{"field", "path"}, {"dir", "asc"}}}}, {"select", QJsonArray{"path", "text"}}, {"limit", 100}, {"offset", 0}};
        QCOMPARE(lib.query(spec, &out), BridgeStatus::Ok);
        QCOMPARE(out.size(), 2);
        QCOMPARE(out[0].toObject()["text"].toString(), QString("write tests"));
        QCOMPARE(lib.query(QJsonObject{{"from", "bogus"}}, &out), BridgeStatus::Invalid);
        // open note: unsaved text is merged into backlinks and read by frontmatter
        auto *s = c.openInOrganizer("Delta.md");
        QVERIFY(s);
        typeText(s, "see [[Alpha]] ");
        QVERIFY(s->editor()->isModified());
        QCOMPARE(lib.backlinks("Alpha.md", 50, 0, &rows), BridgeStatus::Ok);
        srcs.clear();
        for (const auto &r : rows) srcs << r.src;
        QVERIFY2(srcs.contains("Delta.md"), qPrintable(srcs.join(",")));
        // open() routes to the windows
        QCOMPARE(lib.open("Work/Gamma.md", "sticky"), BridgeStatus::Ok);
        QVERIFY(c.stickyOf("Work/Gamma.md"));
        QCOMPARE(lib.open("Nope.md", "sticky"), BridgeStatus::NotFound);
        QCOMPARE(lib.open("Beta.md", "sideways"), BridgeStatus::Invalid);
        QCOMPARE(lib.open("Beta.md", "organizer"), BridgeStatus::Ok);
        QCOMPARE(c.organizerNote(), QString("Beta.md"));
    }

    void rowsCappedAt500() {
        Lib l;
        l.write("Target.md", "# T\n");
        QByteArray many;
        for (int i = 0; i < 700; ++i) many += "[[Target]] line " + QByteArray::number(i) + "\n\n";
        l.write("Many.md", many);
        AppController c(l.opts());
        waitIndex(c, 2);
        PluginLibraryBridge lib(&c);
        QList<LinkRow> rows;
        QCOMPARE(lib.backlinks("Target.md", 5000, 0, &rows), BridgeStatus::Ok);
        QVERIFY2(rows.size() <= 500 && rows.size() > 100, qPrintable(QString::number(rows.size())));
        QCOMPARE(lib.links("Many.md", &rows), BridgeStatus::Ok);
        QVERIFY(rows.size() <= 500);
    }

    void slowIndexTimesOutAndLateResultIsDiscarded() {
        Lib l;
        seed(l);
        AppController c(l.opts());
        waitIndex(c, 4);
        PluginLibraryBridge lib(&c);
        lib.indexDelayForTests = [] { QThread::msleep(400); };
        QElapsedTimer t;
        t.start();
        QList<LinkRow> rows{LinkRow{}};
        QCOMPARE(lib.backlinks("Beta.md", 10, 0, &rows), BridgeStatus::Timeout);
        QVERIFY2(t.elapsed() < 300, qPrintable(QString::number(t.elapsed())));
        QCOMPARE(rows.size(), 1);   // the late result never reached the caller
        ResolveResult rr;
        QCOMPARE(lib.resolve("Beta", &rr), BridgeStatus::Timeout);
        QJsonArray out;
        QCOMPARE(lib.query(QJsonObject{{"from", "notes"}, {"limit", 5}}, &out), BridgeStatus::Timeout);
        QVERIFY(lib.lateCalls() >= 1);
        lib.indexDelayForTests = nullptr;
        lib.waitLate();
        QCOMPARE(lib.lateCalls(), 0);
        QCOMPARE(lib.resolve("Beta", &rr), BridgeStatus::Ok);
        QCOMPARE(rr.status, QString("resolved"));
    }

    // ---------------------------------------------------------------- rename with link updates
    void renameUpdatesClosedNotesAndSelfLink() {
        Rn r;
        QString err, np;
        QVERIFY2(r.c->renameNoteWithLinks("Alpha.md", "Omega", LinkMode::Ask, &err, &np), qPrintable(err));
        QCOMPARE(np, QString("Omega.md"));
        QCOMPARE(r.dialogs, 1);
        QCOMPARE(r.seenLines.size(), 3);   // Beta, Gamma and the renamed note's own self link
        QVERIFY(QFile::exists(r.l.notes + "/Omega.md") && !QFile::exists(r.l.notes + "/Alpha.md"));
        QCOMPARE(r.l.read("Beta.md"), QByteArray("# Beta\n\nSee [[Omega]] and [[Omega|the first]].\n"));
        QVERIFY(r.l.read("Work/Gamma.md").contains("[[Omega#Intro]] and ![[Omega]]"));
        QVERIFY(r.l.read("Omega.md").contains("Self [[Omega]]"));   // old-path keyed edit landed on the NEW path
        QCOMPARE(r.c->lastLinkReport().updatedFiles, 3);
        QVERIFY(r.c->lastLinkReport().failed.isEmpty());
    }

    void renameOnlyCancelAndNoBacklinks() {
        {
            Rn r;
            r.choice = LinkChoice::RenameOnly;
            QString err;
            QVERIFY(r.c->renameNoteWithLinks("Alpha.md", "Omega", LinkMode::Ask, &err));
            QVERIFY(r.l.read("Beta.md").contains("[[Alpha]]"));
            QVERIFY(QFile::exists(r.l.notes + "/Omega.md"));
        }
        {
            Rn r;
            r.choice = LinkChoice::Cancel;
            QString err = "x";
            QVERIFY(!r.c->renameNoteWithLinks("Alpha.md", "Omega", LinkMode::Ask, &err));
            QVERIFY(err.isEmpty());   // cancel is not an error
            QVERIFY(QFile::exists(r.l.notes + "/Alpha.md") && !QFile::exists(r.l.notes + "/Omega.md"));
            QVERIFY(r.l.read("Beta.md").contains("[[Alpha]]"));
        }
        {
            Rn r;   // nothing links to Solo: no dialog at all
            QVERIFY(r.c->renameNoteWithLinks("Solo.md", "Single", LinkMode::Ask));
            QCOMPARE(r.dialogs, 0);
            Rn r2;   // plugin without update_links: plain rename, no dialog, links untouched
            QVERIFY(r2.c->renameNoteWithLinks("Alpha.md", "Omega", LinkMode::None));
            QCOMPARE(r2.dialogs, 0);
            QVERIFY(r2.l.read("Beta.md").contains("[[Alpha]]"));
        }
    }

    void renameOpenDirtyAffectedNote() {
        Rn r;
        auto *s = r.c->openInOrganizer("Beta.md");
        QVERIFY(s);
        s->editor()->setMode(hn::editor::Mode::Source);
        QTest::keyClick(s->editor()->sourceEdit(), Qt::Key_End, Qt::ControlModifier);
        QTest::keyClick(s->editor()->sourceEdit(), Qt::Key_Return);
        typeText(s, "unsaved line [[Alpha]]");
        QTest::keyClick(s->editor()->sourceEdit(), Qt::Key_Return);
        QVERIFY(s->dirty());
        QString err;
        QVERIFY2(r.c->renameNoteWithLinks("Alpha.md", "Omega", LinkMode::Ask, &err), qPrintable(err));
        QTRY_VERIFY(s->settled());
        const QByteArray b = r.l.read("Beta.md");
        QVERIFY2(b.contains("unsaved line [[Omega]]"), b.constData());   // the user's unsaved text survived AND was updated
        QVERIFY(!b.contains("[[Alpha"));
        QVERIFY(r.c->lastLinkReport().failed.isEmpty());
        const QString before = s->editor()->sourceEdit()->toPlainText();
        QVERIFY(s->editor()->undo());   // the whole link update is one undo step
        QVERIFY(s->editor()->sourceEdit()->toPlainText().contains("unsaved line [[Alpha]]"));
        QVERIFY(!before.isEmpty());
    }

    void renameHostileConflictAndReadOnly() {
        {   // a closed affected note changes on disk between the plan and the apply
            Rn r;
            r.beforeApply = [&r] { r.l.write("Work/Gamma.md", "# Gamma\n\nSomeone else edited this [[Alpha]].\n"); };
            QString err;
            QVERIFY(r.c->renameNoteWithLinks("Alpha.md", "Omega", LinkMode::Ask, &err));
            QCOMPARE(r.c->lastLinkReport().failed.size(), 1);
            QVERIFY(r.c->lastLinkReport().failed[0].startsWith("Work/Gamma.md"));
            QCOMPARE(r.l.read("Work/Gamma.md"), QByteArray("# Gamma\n\nSomeone else edited this [[Alpha]].\n"));   // never overwritten
            QVERIFY(r.l.read("Beta.md").contains("[[Omega]]"));   // the others were updated
        }
        {   // read-only folder: flagged in the dialog, left alone, rename still happens
            Rn r;
            const QString dir = r.l.notes + "/Work";
            chmod(dir.toUtf8().constData(), 0555);
            QString err;
            const bool ok = r.c->renameNoteWithLinks("Alpha.md", "Omega", LinkMode::Ask, &err);
            chmod(dir.toUtf8().constData(), 0755);
            QVERIFY2(ok, qPrintable(err));
            QVERIFY(r.seenLines.join("\n").contains("read-only"));
            QCOMPARE(r.c->lastLinkReport().failed.size(), 1);
            QVERIFY(r.l.read("Work/Gamma.md").contains("[[Alpha#Intro]]"));
            QVERIFY(r.l.read("Beta.md").contains("[[Omega]]"));
        }
        {   // the note itself lives in a read-only folder: rename fails, nothing was touched
            Rn r;
            const QString dir = r.l.notes + "/Work";
            r.l.write("Work/Inner.md", "# Inner\n");
            r.c->index()->updatePath("Work/Inner.md");
            chmod(dir.toUtf8().constData(), 0555);
            QString err;
            const bool ok = r.c->renameNoteWithLinks("Work/Inner.md", "Renamed", LinkMode::Ask, &err);
            chmod(dir.toUtf8().constData(), 0755);
            QVERIFY(!ok);
            QVERIFY(!err.isEmpty());
            QVERIFY(QFile::exists(r.l.notes + "/Work/Inner.md"));
        }
        {   // open affected note in a conflict state is skipped, its text kept
            Rn r;
            auto *s = r.c->openInOrganizer("Beta.md");
            s->editor()->setMode(hn::editor::Mode::Source);
            typeText(s, "local edit ");
            r.l.write("Beta.md", "# Beta\n\nChanged outside [[Alpha]].\n");
            r.c->repo().reconcileNow();
            QTRY_VERIFY_WITH_TIMEOUT(s->state() == NoteSession::State::Conflict, 8000);
            QString err;
            QVERIFY(r.c->renameNoteWithLinks("Alpha.md", "Omega", LinkMode::Ask, &err));
            QVERIFY(r.seenLines.join("\n").contains("Beta.md"));
            QVERIFY(r.c->lastLinkReport().failed.join("\n").contains("Beta.md"));
            QVERIFY(s->editor()->sourceEdit()->toPlainText().contains("local edit"));
            QCOMPARE(r.l.read("Beta.md"), QByteArray("# Beta\n\nChanged outside [[Alpha]].\n"));
        }
        {   // target exists
            Rn r;
            QString err;
            QVERIFY(!r.c->renameNoteWithLinks("Alpha.md", "Beta", LinkMode::Ask, &err));
            QVERIFY(err.contains("already exists"));
            QCOMPARE(r.dialogs, 0);
        }
    }

    void pluginRenameThroughBridge() {
        Rn r;
        PluginLibraryBridge lib(r.c.get());
        QString np;
        QCOMPARE(lib.rename("Alpha.md", "Omega.md", true, &np), BridgeStatus::Ok);
        QCOMPARE(np, QString("Omega.md"));
        QVERIFY(r.l.read("Beta.md").contains("[[Omega]]"));
        r.choice = LinkChoice::Cancel;
        QCOMPARE(lib.rename("Omega.md", "Zeta", true, &np), BridgeStatus::Invalid);
        QVERIFY(QFile::exists(r.l.notes + "/Omega.md"));
        QCOMPARE(lib.rename("Omega.md", "a/b", false, &np), BridgeStatus::Invalid);
        QCOMPARE(lib.rename("Nope.md", "x", false, &np), BridgeStatus::NotFound);
    }

    // ---------------------------------------------------------------- nothing enabled = nothing exists
    void nothingEnabledCostsNothing() {
        Lib l;
        seed(l);
        AppController c(l.opts());
        c.showOrganizer();
        c.openInOrganizer("Beta.md");
        QTest::qWait(300);
        QVERIFY(!c.plugins().active());
        QVERIFY(!c.plugins().hub());
        QVERIFY(!c.plugins().editorHooks());
        QVERIFY(!c.plugins().networkManagerCreated());
        QCOMPARE(PluginNetBridge::managersCreated(), 0);
        QVERIFY(!c.organizer()->dock());
        QVERIFY(!c.organizer()->findChild<PanelDock *>());
        auto *ed = c.session("Beta.md")->editor();
        QVERIFY(!ed->wikiLinksEnabled());
        QVERIFY(!c.plugins().managerIfActive());
        c.openSticky("Alpha.md");
        QVERIFY(!c.stickyOf("Alpha.md")->panelButton());
        // visual-mode edit keeps [[links]] verbatim even though no plugin is enabled
        auto *s = c.session("Beta.md");
        typeText(s, "x");
        QVERIFY2(s->editor()->toMarkdownBytes().contains("[[Alpha]] and [[Missing]]"), s->editor()->toMarkdownBytes().constData());
    }

    void editorKeepsLinksVerbatimAndThemeBeforeLoad() {
        Lib l;
        l.write("N.md", "# N\n\nSee [[Alpha]] and ![[pic.png]] here.\n");
        AppController c(l.opts());
        auto *s = c.openSticky("N.md")->editor() ? c.session("N.md") : nullptr;
        QVERIFY(s);
        QVERIFY(!s->dirty());                 // setTheme ran before load: a freshly opened note is clean
        QVERIFY(!s->editor()->canUndo());
        typeText(s, "z");
        QVERIFY(s->editor()->toMarkdownBytes().contains("[[Alpha]]"));
        QVERIFY(s->editor()->toMarkdownBytes().contains("![[pic.png]]"));
        QVERIFY(!s->editor()->toMarkdownBytes().contains("\\["));
    }

    // ---------------------------------------------------------------- panels + the three example plugins
    void dockLifecycleAndExamples() {
        Lib l;
        l.write("Alpha.md", "# Alpha\n");
        l.write("Beta.md", "# Beta\n\nSee [[Alpha]] today.\n\n- [ ] write tests\n");
        l.write("Gamma.md", "# Gamma\n\nAlso [[Alpha]].\n\n- [ ] ship it\n");
        AppController c(accepting(l));
        c.showOrganizer();
        auto *o = c.organizer();
        QVERIFY(!o->dock());
        waitIndex(c, 3);
        QVERIFY(installOk(c, example("backlinks-panel")));
        QVERIFY(installOk(c, example("open-tasks")));
        QVERIFY(installOk(c, example("link-completion")));
        QVERIFY(c.plugins().hasPanels());
        QVERIFY(o->dock());                                  // created lazily, now
        QVERIFY(o->dockVisible());
        QCOMPARE(o->dock()->panelIds().size(), 2);
        // no overlay yet? link-completion has editor.complete, so wiki links are on; backlinks alone would not
        c.openInOrganizer("Alpha.md");
        auto *dock = o->dock();
        const QString bl = "backlinks-panel:backlinks", tk = "open-tasks:tasks";
        dock->setCurrentPanel(bl);
        auto *hub = c.plugins().hub();
        QTRY_VERIFY_WITH_TIMEOUT(hub->find(bl) && hub->find(bl)->rendered, 5000);
        QStringList titles;
        for (const auto &b : hub->find(bl)->blocks) {
            if (b.type == "heading") titles << "H:" + b.text;
            for (const auto &it : b.items) titles << it.title;
        }
        QCOMPARE(titles.value(0), QString("H:2 backlinks"));
        titles.removeFirst();
        titles.sort();
        QCOMPARE(titles, (QStringList{"Beta", "Gamma"}));
        QVERIFY(dock->view()->actionableCount() == 2);
        // keyboard: Down, Down, Enter opens the note through the plugin's on_click
        dock->view()->setFocus();
        QTest::keyClick(dock->view(), Qt::Key_Down);
        QCOMPARE(dock->view()->currentIndex(), 0);
        QTest::keyClick(dock->view(), Qt::Key_Down);
        QCOMPARE(dock->view()->currentIndex(), 1);
        const QString target = dock->view()->currentTitle() + ".md";
        QTest::keyClick(dock->view(), Qt::Key_Return);
        QTRY_COMPARE(c.organizerNote(), target);
        // tasks panel
        dock->setCurrentPanel(tk);
        QTRY_VERIFY_WITH_TIMEOUT(hub->find(tk) && hub->find(tk)->rendered, 5000);
        int n = 0;
        QStringList tasks;
        for (const auto &b : hub->find(tk)->blocks) for (const auto &it : b.items) { ++n; tasks << it.title; }
        tasks.sort();
        QCOMPARE(tasks, (QStringList{"ship it", "write tests"}));
        QCOMPARE(dock->view()->actionableCount(), 3);   // Refresh button + 2 items
        // click -> callback: the click on the task opens its note in the organizer
        c.openInOrganizer("Alpha.md");
        QTRY_COMPARE(c.organizerNote(), QString("Alpha.md"));
        dock->view()->setCurrentIndex(1);
        dock->view()->activateCurrent();
        QTRY_VERIFY(c.organizerNote() == "Beta.md" || c.organizerNote() == "Gamma.md");
        // width + visibility persist
        o->setDockVisible(false);
        QVERIFY(!o->dockVisible());
        DockPrefs p = c.dockPrefs();
        p.width = 410;
        c.setDockPrefs(p);
        QVERIFY(!c.dockPrefs().visible);
        {
            AppController c2(accepting(l));
            QCOMPARE(c2.dockPrefs().width, 410);
            QVERIFY(!c2.dockPrefs().visible);
        }
        o->setDockVisible(true);
        o->resize(720, o->height());
        QTRY_COMPARE(o->width(), 720);
        QTRY_VERIFY(o->dock()->width() < 410); // Temporary allocation must not overwrite the user's preference.
        QCOMPARE(c.dockPrefs().width, 410);
        o->resize(1100, o->height());
        QTRY_COMPARE(o->dock()->width(), 410);
        QVERIFY(c.dockPrefs().visible);
        // sticky: a header button appears only now and opens the same panels as a popup
        auto *st = c.openSticky("Beta.md") ? c.stickyOf("Beta.md") : nullptr;
        if (!st) { c.popOut("Beta.md"); st = c.stickyOf("Beta.md"); }
        QVERIFY(st && st->panelButton());
        auto *pop = st->openPanels();
        QVERIFY(pop && pop->isVisible());
        QCOMPARE(pop->panelIds().size(), 2);
        pop->close();
        // disabling both panel plugins removes the dock completely
        auto *mgr = c.plugins().manager();
        mgr->disable("backlinks-panel");
        mgr->disable("open-tasks");
        QTRY_VERIFY(!o->dock());
        QVERIFY(!c.plugins().hasPanels());
        QVERIFY(!c.stickyOf("Beta.md")->panelButton());
        Q_UNUSED(n);
    }

    void completionAndLinkHooks() {
        Lib l;
        l.write("Alpha.md", "# Alpha\n");
        l.write("Alphabet.md", "# Alphabet\n");
        l.write("Work/Twin.md", "# Twin 1\n");
        l.write("Other/Twin.md", "# Twin 2\n");
        l.write("Text.md", "Links: [[Alpha]] [[Nowhere]] [[Twin]]\n\n");
        AppController c(accepting(l));
        waitIndex(c, 5);
        auto *s = c.openSticky("Text.md") ? c.session("Text.md") : nullptr;
        QVERIFY(s);
        auto *ed = s->editor();
        // a panel-only plugin does not need the overlay
        QVERIFY(installOk(c, example("backlinks-panel")));
        QTRY_VERIFY(c.plugins().hub());
        QVERIFY(!ed->wikiLinksEnabled());
        QVERIFY(!c.plugins().editorHooks()->hooked(s));
        // completion plugin: overlay + resolver states + popup
        QVERIFY(installOk(c, example("link-completion")));
        QVERIFY(ed->wikiLinksEnabled());
        QVERIFY(c.plugins().editorHooks()->hooked(s));
        QTRY_COMPARE_WITH_TIMEOUT(ed->linkRangesInBlock(0).size(), 3, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(ed->linkRangesInBlock(0)[0].state == LinkState::Resolved && ed->linkRangesInBlock(0)[1].state == LinkState::Unresolved
                                     && ed->linkRangesInBlock(0)[2].state == LinkState::Ambiguous, 5000);
        const int calls = c.plugins().editorHooks()->resolverCalls();
        ed->linkRangesInBlock(0);
        ed->linkRangesInBlock(0);
        QCOMPARE(c.plugins().editorHooks()->resolverCalls(), calls);   // cached
        // typing [[Al shows the popup, items come from the plugin
        ed->focusEditor();
        QTextCursor cur(ed->visualEdit()->document());
        cur.movePosition(QTextCursor::End);
        ed->visualEdit()->setTextCursor(cur);
        typeText(s, "[[Alp");
        QTRY_VERIFY_WITH_TIMEOUT(ed->completionActive() && ed->completionPopup() && ed->completionPopup()->isVisible(), 5000);
        QCOMPARE(c.plugins().editorHooks()->pendingCompletions(), 0);   // the reply was consumed
        QVERIFY(ed->acceptCompletion(0));
        QVERIFY2(ed->toMarkdownBytes().contains("[[Alpha]]"), ed->toMarkdownBytes().constData());
        // stale reply: unknown token is dropped, nothing shown
        c.plugins().editorHooks()->completionReply(9999, {CompletionItem{"p", "x", "d", "y", -1}});
        QVERIFY(!ed->completionActive());
        // link activation: no handler -> not consumed, nothing destructive happens
        hn::editor::LinkRefInfo ref;
        ref.target = "Alpha";
        const int windows = c.windowCount();
        emit ed->wikiLinkActivated(ref);
        QCOMPARE(c.windowCount(), windows);
        QVERIFY(c.lastNotice().contains("No plugin handled"));
        QCOMPARE(c.plugins().editorHooks()->pendingActivations(), 0);
        // a handler that consumes: opens the resolved note in a sticky
        const QString h = mkPlugin(l.dir.path(), "link-opener", {"editor.links", "notes.read"},
            "hn.on('link.activate', function(ref) if ref.resolved then hn.notes.open(ref.resolved, {where='sticky'}) return true end return false end)\n");
        QVERIFY(installOk(c, h));
        emit ed->wikiLinkActivated(ref);
        QTRY_VERIFY(c.stickyOf("Alpha.md"));
        const int w2 = c.windowCount();
        ref.target = "Nowhere";
        emit ed->wikiLinkActivated(ref);   // unresolved: the handler returns false, the app does nothing
        QCOMPARE(c.windowCount(), w2);
        // disabling every editor plugin switches everything off again
        auto *mgr = c.plugins().manager();
        mgr->disable("link-completion");
        mgr->disable("link-opener");
        QTRY_VERIFY(!ed->wikiLinksEnabled());
        QVERIFY(!c.plugins().editorHooks()->hooked(s));
    }

    void pluginsCardListsRegistrations() {
        Lib l;
        l.write("A.md", "# A\n");
        AppController c(accepting(l));
        QVERIFY(installOk(c, example("backlinks-panel")));
        QVERIFY(installOk(c, example("link-completion")));
        PluginsPage page(&c);
        page.ensureLoaded();
        page.select("backlinks-panel");
        QStringList t;
        for (auto *lb : page.registrationLabels()) t << lb->text();
        QVERIFY2(t.join("|").contains("Panel: Backlinks"), qPrintable(t.join("|")));
        bool idx = false;
        for (auto *lb : page.permissionLabels()) idx |= lb->property("permission").toString() == "notes.index" && lb->text().contains(describePermission("notes.index"));
        QVERIFY(idx);
        page.select("link-completion");
        t.clear();
        for (auto *lb : page.registrationLabels()) t << lb->text();
        QVERIFY2(t.join("|").contains("[["), qPrintable(t.join("|")));
    }

    void cliNewPluginApi2() {
        QVERIFY(hn::app::parseCli({"--new-plugin", "x", "--api", "2"}).error.isEmpty());
        QCOMPARE(hn::app::parseCli({"--new-plugin", "x", "--api", "2"}).newApi, 2);
        QVERIFY(!hn::app::parseCli({"--new-plugin", "x", "--api", "3"}).error.isEmpty());
        QVERIFY(!hn::app::parseCli({"--api", "2"}).error.isEmpty());
        QTemporaryDir d;
        QDir::setCurrent(d.path());
        QString o, e;
        QTextStream out(&o), err(&e), in;
        hn::app::CliOptions opt;
        opt.newPlugin = "My Thing";
        opt.newApi = 2;
        QCOMPARE(hn::app::runPluginCli(opt, in, out, err), 0);
        QFile f(d.path() + "/my-thing/plugin.json");
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(QJsonDocument::fromJson(f.readAll()).object()["api"].toInt(), 2);
    }

    // ---------------------------------------------------------------- measured cost
    void measureCostOfThreeExamples() {
        Lib l;
        for (int i = 0; i < 1000; ++i)
            l.write(QString("Folder%1/Note %2.md").arg(i % 10).arg(i),
                    QString("# Note %1\n\nLinks to [[Note %2]] and [[Note %3]].\n\n- [ ] task %1\n- [x] done %1\n\nSome body text for note %1.\n").arg(i).arg((i + 1) % 1000).arg((i * 7) % 1000).toUtf8());
        AppController c(accepting(l));
        c.showOrganizer();
        waitIndex(c, 1000);
        c.openInOrganizer("Folder5/Note 5.md");
        QTest::qWait(500);
        const qint64 before = pssKb();
        QVERIFY(installOk(c, example("backlinks-panel")));
        QVERIFY(installOk(c, example("open-tasks")));
        QVERIFY(installOk(c, example("link-completion")));
        auto *s = c.session("Folder5/Note 5.md");
        QTextCursor cur(s->editor()->visualEdit()->document());
        cur.movePosition(QTextCursor::End);
        s->editor()->visualEdit()->setTextCursor(cur);
        typeText(s, "[[Note 12");
        QTRY_VERIFY_WITH_TIMEOUT(s->editor()->completionActive(), 8000);
        auto *hub = c.plugins().hub();
        c.organizer()->dock()->setCurrentPanel("open-tasks:tasks");
        QTRY_VERIFY_WITH_TIMEOUT(hub->find("open-tasks:tasks")->rendered, 8000);
        c.organizer()->dock()->setCurrentPanel("backlinks-panel:backlinks");
        QTRY_VERIFY_WITH_TIMEOUT(hub->find("backlinks-panel:backlinks")->rendered, 8000);
        QTest::qWait(500);
        const qint64 after = pssKb();
        qInfo().nospace() << "COST 1000-note library: PSS before enabling the three example plugins " << before << " kB, after (panels rendered, completion shown) "
                          << after << " kB, delta " << (after - before) << " kB; lua states " << c.plugins().manager()->loadedStates();
        QVERIFY(after - before < 40 * 1024);
    }

    // ---------------------------------------------------------------- screenshots (opt-in)
    void screenshots() {
        const QString out = qEnvironmentVariable("HN_SCREENSHOT_DIR");
        if (out.isEmpty()) QSKIP("HN_SCREENSHOT_DIR not set");
        QDir().mkpath(out);
        for (const char *scheme : {"light", "dark"}) {
            Lib l;
            l.write("Product vision.md", "# Product vision\n\nWhere the notes app is going, in three bets.\n\n- [ ] Draft the roadmap\n- [ ] Review with the team\n\n#planning\n");
            l.write("Roadmap.md", "# Roadmap\n\nBuilds on the [[Product vision]]. Q3 focus is plugins; see [[Plugin ideas]].\n\n#planning\n");
            l.write("Plugin ideas.md", "# Plugin ideas\n\nA daily journal, saved queries and a slash menu. Context in the [[Product vision]] and the [[Roadmap]].\n\n- [ ] Prototype the journal\n");
            l.write("Meeting 2026-09-30.md", "# Meeting\n\nAgreed to revisit the [[Product vision]] before the next review. Owner: Sam.\n");
            l.write("Reading list.md", "# Reading list\n\nThe Design of Everyday Things.\n\n#books\n");
            l.write("Weekly review.md", "# Weekly review\n\nLinks: [[Roadmap]], [[Plugin ideas]], [[Not written yet]], [[Plugin]].\n\nStart typing a link here: ");
            l.write("Plugin notes.md", "# Plugin notes\n");
            AppController c(accepting(l));
            auto s = c.settings(); s.colorScheme = scheme; c.applySettings(s, c.prefs());
            c.showOrganizer();
            auto *o = c.organizer();
            waitIndex(c, 7);
            QVERIFY(installOk(c, example("backlinks-panel")));
            QVERIFY(installOk(c, example("open-tasks")));
            QVERIFY(installOk(c, example("link-completion")));
            c.openInOrganizer("Product vision.md");
            QTRY_VERIFY(o->dock());
            o->dock()->setCurrentPanel("backlinks-panel:backlinks");
            QTRY_VERIFY_WITH_TIMEOUT(c.plugins().hub()->find("backlinks-panel:backlinks")->blocks.size() >= 2, 8000);
            QTest::qWait(500);
            QVERIFY(o->grab().save(QString("%1/app-organizer-dock-%2.png").arg(out, scheme)));
            o->dock()->setCurrentPanel("open-tasks:tasks");
            QTest::qWait(500);
            QVERIFY(o->grab().save(QString("%1/app-organizer-dock-tasks-%2.png").arg(out, scheme)));
            // note with resolved / unresolved / ambiguous links and the completion popup
            o->setDockVisible(false);
            c.openInOrganizer("Weekly review.md");
            auto *sess = c.session("Weekly review.md");
            auto *ed = sess->editor();
            QTest::qWait(300);
            QTextCursor cur(ed->visualEdit()->document());
            cur.movePosition(QTextCursor::End);
            ed->visualEdit()->setTextCursor(cur);
            ed->focusEditor();
            typeText(sess, " [[Pl");
            QTRY_VERIFY_WITH_TIMEOUT(ed->completionActive() && ed->completionPopup() && ed->completionPopup()->isVisible(), 8000);
            QTest::qWait(500);
            QPixmap shot = o->grab();   // the popup is its own top-level window: composite it onto the window grab
            {
                QPainter pp(&shot);
                pp.drawPixmap(o->mapFromGlobal(ed->completionPopup()->mapToGlobal(QPoint(0, 0))), ed->completionPopup()->grab());
            }
            QVERIFY(shot.save(QString("%1/app-links-completion-%2.png").arg(out, scheme)));
            ed->dismissCompletions();
        }
    }
};

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    V2Test t;
    return QTest::qExec(&t, argc, argv);
}
#include "app_v2_test.moc"
