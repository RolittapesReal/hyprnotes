// The shipped example plugins (installed from folders and from .hnplugin archives, consented through the TrustStore API,
// run against fake bridges) and the deliberately malicious evil-demo fixture (must be fully contained).
#include "plugins_test_util.h"
#include <QElapsedTimer>
#include <QSignalSpy>
#include <ctime>

using namespace hn::plugins;
using namespace hn::plugins::test;

static QString example(const QString &id) { return QStringLiteral(HN_PLUGIN_EXAMPLES) + "/" + id; }
static const qint64 kClock = 1790863509;  // 2026-10-01 14:05:09 UTC

// Library where nothing exists until it is written.
struct EmptyLib : FakeLib {
    QMap<QString, QString> files;
    bool read(const QString &p, QString *t) override { calls << "read:" + p; if (!files.contains(p)) return false; *t = files[p]; return true; }
    bool write(const QString &p, const QString &t) override { calls << "write:" + p; files[p] = t; return true; }
};

struct ExRig {
    Rig r;
    ExRig() {
        qputenv("TZ", "UTC");
        tzset();
        r.mgr->host()->env().clock = [] { return kClock; };
    }
    // install the example from its folder, consent to exactly what it asks for through the TrustStore API, enable
    void use(const QString &id) {
        const auto res = r.mgr->install(example(id), true);
        QVERIFY2(res.ok, qPrintable(res.errors.value(0).message));
        const auto m = r.mgr->info(id).manifest;
        QString err;
        QVERIFY2(r.mgr->consent(id, m.permissions, &err), qPrintable(err));
        QCOMPARE(r.mgr->trust()->record(id).permissions, m.permissions);
        QVERIFY2(r.mgr->enable(id, &err), qPrintable(err));
        QCOMPARE(r.mgr->info(id).status, Status::Enabled);
    }
    QString audit() const { return QString::fromUtf8(readFile(r.env.state() + "/plugins-audit.jsonl")); }
};

static QString x_audit(Rig &r) { return QString::fromUtf8(readFile(r.env.state() + "/plugins-audit.jsonl")); }

class ExamplesTest : public QObject {
    Q_OBJECT
    bool ok_ = false;
    // runs one evil-demo command; returns attempt name -> "blocked|reason" / "SUCCEEDED|..." ; sets ok_
    QMap<QString, QString> outcomes(Rig &r, const QString &cmdId) {
        r.logs.clear();
        QString e;
        ok_ = r.mgr->runCommand("evil-demo:" + cmdId, &r.note, &e);
        if (!ok_) qWarning().noquote() << cmdId << "failed:" << e;
        QMap<QString, QString> m;
        for (const auto &l : r.logsOf("evil-demo")) { const auto p = l.split('|'); m[p[0]] = p.value(1) + "|" + p.value(2); }
        return m;
    }
private slots:
    void everyExampleValidatesAndHasAReadme() {
        const QStringList ids{"word-count", "insert-date", "sort-lines", "markdown-toc", "title-case-selection", "daily-note", "snippets"};
        QCOMPARE(QDir(QStringLiteral(HN_PLUGIN_EXAMPLES)).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size(), ids.size());
        for (const auto &id : ids) {
            const auto c = checkDirectory(example(id));
            QVERIFY2(c.ok, qPrintable(id + ": " + c.errors.value(0).message));
            for (const auto &w : c.warnings) QVERIFY2(w.startsWith("requests dangerous permission"), qPrintable(id + ": " + w));  // the only acceptable warning
            QCOMPARE(c.manifest.id, id);
            QVERIFY2(QFileInfo::exists(example(id) + "/README.md"), qPrintable(id));
            QVERIFY(readFile(example(id) + "/README.md").contains("Permissions"));
        }
    }
    void everyExampleInstallsFromAnArchiveToo() {
        for (const QString &id : {"word-count", "insert-date", "sort-lines", "markdown-toc", "title-case-selection", "daily-note", "snippets"}) {
            Rig r;
            const QString pkg = r.env.tmp.path() + "/" + id + ".hnplugin";
            QList<PluginError> errs;
            QVERIFY2(packDirectory(example(id), pkg, &errs), qPrintable(errs.value(0).message));
            const auto res = r.mgr->install(pkg);
            QVERIFY2(res.ok, qPrintable(id + ": " + res.errors.value(0).message));
            QCOMPARE(r.mgr->info(id).status, Status::NeedsConsent);  // never enabled without consent
            QString err;
            QVERIFY(!r.mgr->enable(id, &err));
            QVERIFY(r.mgr->consent(id, r.mgr->info(id).manifest.permissions));
            QVERIFY(r.mgr->enable(id, &err));
            QVERIFY2(!r.mgr->registry()->commands().isEmpty() || !r.mgr->registry()->triggers().isEmpty(), qPrintable(id));
        }
    }
    void wordCount() {
        ExRig x;
        x.use("word-count");
        auto &r = x.r;
        QString err;
        r.note.sel = "world";
        QVERIFY2(r.mgr->runCommand("word-count:count", &r.note, &err), qPrintable(err));
        r.note.sel.clear();
        r.note.t = "one two\nthree\n";
        QVERIFY2(r.mgr->runCommand("word-count:count", &r.note, &err), qPrintable(err));
        QCOMPARE(r.ui.notes, (QStringList{"Selection: 1 words, 5 characters, 1 lines", "Note: 3 words, 14 characters, 2 lines"}));
        r.ui.notes.clear();
        r.mgr->post("note.saved", "a.md", &r.note);
        QVERIFY(r.ui.notes.isEmpty());  // setting is off by default
        QVERIFY(r.mgr->host()->setSetting("word-count", "notify_on_save", true));
        r.mgr->post("note.saved", "a.md", &r.note);
        QCOMPARE(r.ui.notes, QStringList{"Saved: 3 words, 14 characters, 2 lines"});
        QVERIFY(r.note.ops.isEmpty());  // never edits
    }
    void insertDate() {
        ExRig x;
        x.use("insert-date");
        auto &r = x.r;
        auto m = r.mgr->matchTrigger("today is ::date", &r.note);
        QVERIFY(m && m->replacement == "2026-10-01");
        m = r.mgr->matchTrigger("at ::time", &r.note);
        QVERIFY(m && m->replacement == "14:05");
        QVERIFY(r.mgr->host()->setSetting("insert-date", "date_format", "%A, %d %B %Y"));
        m = r.mgr->matchTrigger("::date", &r.note);
        QVERIFY(m && m->replacement == "Thursday, 01 October 2026");
        QVERIFY(r.mgr->host()->setSetting("insert-date", "time_format", QString(80, QLatin1Char('x'))));  // invalid: falls back
        m = r.mgr->matchTrigger("::time", &r.note);
        QVERIFY(m && m->replacement == "14:05");
        QVERIFY(!r.mgr->matchTrigger("::dat", &r.note));
        QCOMPARE(r.mgr->info("insert-date").manifest.permissions, QStringList{});
    }
    void sortLines() {
        ExRig x;
        x.use("sort-lines");
        auto &r = x.r;
        QString err;
        r.note.t = "pear\nApple\nbanana\napple\n";
        r.note.sel = r.note.t;
        QVERIFY2(r.mgr->runCommand("sort-lines:asc", &r.note, &err), qPrintable(err));
        QCOMPARE(r.note.ops, QStringList{"replace:Apple\napple\nbanana\npear\n"});
        r.note.ops.clear();
        r.note.sel = "pear\nApple\nbanana\napple\n";
        QVERIFY2(r.mgr->runCommand("sort-lines:desc", &r.note, &err), qPrintable(err));
        QCOMPARE(r.note.ops, QStringList{"replace:pear\nbanana\nApple\napple\n"});
        r.note.sel.clear();
        r.note.ops.clear();
        r.ui.notes.clear();
        QVERIFY(r.mgr->runCommand("sort-lines:asc", &r.note, &err));
        QVERIFY(r.note.ops.isEmpty());
        QCOMPARE(r.ui.notes, QStringList{"Select the lines to sort first"});
        QCOMPARE(r.note.begins, r.note.ends);  // every edit was one balanced undo step
    }
    void markdownToc() {
        ExRig x;
        x.use("markdown-toc");
        auto &r = x.r;
        QString err;
        r.note.t = "# Title\n\nintro\n\n## Setup\n\n```\n# not a heading\n```\n\n### Deep dive!\n\n#### too deep\n\n## Setup\n";
        QVERIFY2(r.mgr->runCommand("markdown-toc:insert", &r.note, &err), qPrintable(err));
        QCOMPARE(r.note.ops.value(0), QString("insert:- [Title](#title)\n  - [Setup](#setup)\n    - [Deep dive!](#deep-dive)\n  - [Setup](#setup-1)\n"));
        QVERIFY(r.mgr->host()->setSetting("markdown-toc", "max_depth", 1));
        QVERIFY(r.mgr->host()->setSetting("markdown-toc", "bullet", "*"));
        QVERIFY(r.mgr->runCommand("markdown-toc:insert", &r.note, &err));
        QCOMPARE(r.note.ops.value(1), QString("insert:* [Title](#title)\n"));
        r.note.t = "no headings here";
        r.ui.notes.clear();
        QVERIFY(r.mgr->runCommand("markdown-toc:insert", &r.note, &err));
        QCOMPARE(r.ui.notes.last(), QString("No headings found"));
        QCOMPARE(r.note.ops.size(), 2);
    }
    void titleCase() {
        ExRig x;
        x.use("title-case-selection");
        auto &r = x.r;
        QString err;
        r.note.sel = "the lord of the rings: a well-known story";
        QVERIFY2(r.mgr->runCommand("title-case-selection:title-case", &r.note, &err), qPrintable(err));
        QCOMPARE(r.note.ops, QStringList{"replace:The Lord of the Rings: A Well-Known Story"});
        r.note.ops.clear();
        r.note.sel = "  UP  and   down  ";
        QVERIFY(r.mgr->runCommand("title-case-selection:title-case", &r.note, &err));
        QCOMPARE(r.note.ops, QStringList{"replace:  Up  and   Down  "});
    }
    void dailyNote() {
        ExRig x;
        EmptyLib lib;
        x.r.mgr->host()->env().bridges.library = &lib;
        x.use("daily-note");
        auto &r = x.r;
        QString err;
        QVERIFY2(r.mgr->runCommand("daily-note:today", &r.note, &err), qPrintable(err));
        QCOMPARE(lib.files.value("daily/2026-10-01.md"), QString("# 2026-10-01\n\n## Plan\n\n## Notes\n"));
        QCOMPARE(r.ui.notes, QStringList{"Created daily/2026-10-01.md"});
        QVERIFY(r.mgr->runCommand("daily-note:today", &r.note, &err));  // second run: found, not overwritten
        QCOMPARE(r.ui.notes.last(), QString("Today's note already exists: daily/2026-10-01.md"));
        QCOMPARE(lib.calls.filter("write:").size(), 1);
        QVERIFY(r.mgr->host()->setSetting("daily-note", "folder", "/journal/2026/"));
        QVERIFY(r.mgr->host()->setSetting("daily-note", "template", "Day {{date}}\\n- "));
        QVERIFY(r.mgr->runCommand("daily-note:today", &r.note, &err));
        QCOMPARE(lib.files.value("journal/2026/2026-10-01.md"), QString("Day 2026-10-01\n- "));
        QVERIFY(x.audit().contains("notes.write"));  // writes are audited
        QVERIFY(!x.audit().contains("\"denied\""));
    }
    void snippets() {
        ExRig x;
        x.use("snippets");
        auto &r = x.r;
        QCOMPARE(r.logsOf("snippets").last(), QString("snippets loaded: 3"));
        auto m = r.mgr->matchTrigger("regards ::sig", &r.note);
        QVERIFY(m && m->replacement == "Best regards,\nYour Name");
        m = r.mgr->matchTrigger("::todo", &r.note);
        QVERIFY(m && m->replacement == "- [ ] ");
        m = r.mgr->matchTrigger("::shrug", &r.note);
        QVERIFY(m && m->replacement == QString::fromUtf8("\xC2\xAF\\_(\xE3\x83\x84)_/\xC2\xAF"));
        // user-defined table: duplicates and garbage are skipped, the rest still loads after Reload
        QVERIFY(r.mgr->host()->setSetting("snippets", "snippets", "::a1=one\n::a1=dup\nnot a snippet\n::b-2=two\\nlines\n::x=short"));
        r.logs.clear();
        QString err;
        QVERIFY2(r.mgr->reload("snippets", false, &err), qPrintable(err));
        m = r.mgr->matchTrigger("::b-2", &r.note);
        QVERIFY(m && m->replacement == "two\nlines");
        m = r.mgr->matchTrigger("::a1", &r.note);
        QVERIFY(m && m->replacement == "one");
        QVERIFY(!r.mgr->matchTrigger("::sig", &r.note));
        const auto logs = r.logsOf("snippets");
        QVERIFY(logs.contains("skipped duplicate or invalid trigger ::a1"));
        QVERIFY(logs.contains("snippets loaded: 3"));
    }

    // ================= evil-demo: every escape must fail, every bomb must be contained =================
    void evilDemoIsContained() {
        Rig r;
        QFile::remove("/tmp/hn-evil-demo-should-not-exist");
        QFile::remove("/tmp/hn-evil-demo-exec");
        const auto res = r.mgr->install(fixture("evil-demo"), true);
        QVERIFY2(res.ok, qPrintable(res.errors.value(0).message));
        // The user consents to a subset: note.read note.edit ui storage network. NOT notes.read/notes.write/clipboard/theme.
        const QStringList granted{"note.read", "note.edit", "ui", "storage", "network"};
        QVERIFY(r.mgr->consent("evil-demo", granted));
        QString err;
        QVERIFY2(r.mgr->enable("evil-demo", &err), qPrintable(err));
        r.note.t = "my secret diary";

        // 1. language-level escapes
        auto esc = outcomes(r, "escape"); QVERIFY(ok_);
        int blocked = 0;
        for (auto it = esc.begin(); it != esc.end(); ++it) {
            QVERIFY2(it.value().startsWith("blocked|"), qPrintable(it.key() + " => " + it.value()));
            ++blocked;
        }
        QVERIFY(blocked >= 30);
        QVERIFY(!QFileInfo::exists("/tmp/hn-evil-demo-should-not-exist"));
        QVERIFY(!QFileInfo::exists("/tmp/hn-evil-demo-exec"));
        qInfo().noquote() << QString("evil-demo: %1 language-level escape attempts, all blocked; no file or process was created").arg(blocked);
        // 2. API calls without the permission
        auto perms = outcomes(r, "perms"); QVERIFY(ok_);
        for (auto it = perms.begin(); it != perms.end(); ++it) QVERIFY2(it.value().startsWith("blocked|"), qPrintable(it.key() + " => " + it.value()));
        QVERIFY(perms["notes.write"].contains("permission denied: notes.write"));
        QVERIFY(perms["clipboard.get"].contains("permission denied: clipboard"));
        QVERIFY(r.lib.calls.isEmpty());
        QCOMPARE(r.clip.v, QString("clip"));
        QVERIFY(r.theme.sets.isEmpty());
        // 3. network: only https + the declared host; header tricks refused; rate limited
        auto net = outcomes(r, "net"); QVERIFY(ok_);
        for (auto it = net.begin(); it != net.end(); ++it) {
            if (it.key() == "EXFIL to allowed host" || it.key() == "flood") continue;
            QVERIFY2(it.value().startsWith("blocked|"), qPrintable(it.key() + " => " + it.value()));
        }
        QVERIFY(net["http:// plain"].contains("only https"));
        QVERIFY(net["other host"].contains("not listed in net_hosts"));
        QVERIFY(net["suffix host"].contains("not listed in net_hosts"));
        QVERIFY(net["userinfo trick"].contains("credentials"));
        QVERIFY(net["odd port"].contains("default https port"));
        QVERIFY(net["crlf header"].contains("not allowed") && net["host header override"].contains("not allowed") && net["cookie header"].contains("not allowed"));
        for (const auto &q : r.net.reqs) QVERIFY2(q.url.startsWith("https://api.example.com/"), qPrintable(q.url));
        QVERIFY(r.net.reqs.size() <= 20);
        QCOMPARE(net["EXFIL to allowed host"].left(9), QString("SUCCEEDED"));  // honest limit: declared host + note.read = exfiltration
        bool exfil = false;
        for (const auto &q : r.net.reqs) exfil |= q.body == "my secret diary";
        QVERIFY(exfil);
        qInfo().noquote() << QString("evil-demo: %1 network attempts reached the bridge (all https://api.example.com, rate limit 20/min); %2 URL/header tricks refused before the bridge; the declared-host exfiltration SUCCEEDED, as documented")
                                 .arg(r.net.reqs.size()).arg(net.size() - 2);
        QVERIFY(!x_audit(r).contains("secret diary"));  // the audit log records the host only
        QVERIFY(x_audit(r).contains("\"denied\""));

        // 4. things that run inside its own permissions: honest limit, but still bounded
        r.ui.notes.clear();
        outcomes(r, "notify-flood"); QVERIFY(ok_);
        QVERIFY2(r.ui.notes.size() <= 10, qPrintable(QString::number(r.ui.notes.size())));
        outcomes(r, "wipe"); QVERIFY(ok_);
        QCOMPARE(r.note.t, QString());  // note.edit lets a plugin destroy the open note: undo is one step, not a backup

        // 5. bombs: aborted within budget, memory capped, and the failure counter is reset by a harmless success in between
        struct Bomb { const char *cmd, *errPart; qint64 maxMs; };
        const Bomb bombs[] = {{"loop", "time budget", 2400}, {"loop-pcall", "time budget", 2400}, {"memory", "not enough memory", 1500}, {"string-bomb", "not enough memory", 1500},
                              {"rep-bomb", "too large", 100}, {"pattern-bomb", "too complex", 250}, {"recursion", "depth", 500}, {"gc-trap", "__gc finalizers are not allowed", 100},
                              {"sort-bomb", "too large", 100}, {"move-bomb", "range too large", 100}, {"storage-flood", "quota", 1500}};
        for (const auto &b : bombs) {
            QElapsedTimer t;
            t.start();
            QString e;
            const bool ok = r.mgr->runCommand(QString("evil-demo:") + b.cmd, &r.note, &e);
            const qint64 ms = t.elapsed();
            qInfo().noquote() << QString("evil-demo %1: contained in %2 ms (%3), Lua heap %4 KiB").arg(b.cmd).arg(ms).arg(e.left(50)).arg(r.mgr->host()->memoryUsed("evil-demo") / 1024);
            QVERIFY2(!ok, b.cmd);
            QVERIFY2(e.contains(b.errPart), qPrintable(QString("%1: %2").arg(b.cmd, e)));
            QVERIFY2(ms < b.maxMs, qPrintable(QString("%1 took %2 ms").arg(b.cmd).arg(ms)));
            QVERIFY(r.mgr->host()->memoryUsed("evil-demo") <= 16u * 1024 * 1024);
            outcomes(r, "perms"); QVERIFY(ok_);  // harmless success resets the consecutive-failure counter
            QCOMPARE(r.mgr->info("evil-demo").status, Status::Enabled);
        }
        QVERIFY(QFileInfo(r.env.state() + "/plugin-data/evil-demo.json").size() <= 1100 * 1024);  // storage quota held on disk

        // 6. the note.opened handler spins: stopped by the 50 ms event budget
        QElapsedTimer t;
        t.start();
        r.mgr->post("note.opened", "a.md", &r.note);
        qInfo().noquote() << QString("evil-demo: spinning note.opened handler stopped after %1 ms").arg(t.elapsed());
        QVERIFY(t.elapsed() < 150);
        outcomes(r, "perms"); QVERIFY(ok_);

        // 7. repeated runaways trip the circuit breaker: plugin disabled, state closed, no more cost
        QSignalSpy spy(r.mgr.get(), &PluginManager::pluginAutoDisabled);
        for (int i = 0; i < 3; ++i) QVERIFY(!r.mgr->runCommand("evil-demo:loop-pcall", &r.note, &err));
        QCOMPARE(spy.count(), 1);
        QCOMPARE(r.mgr->info("evil-demo").status, Status::Failed);
        QCOMPARE(r.mgr->loadedStates(), 0);
        QElapsedTimer t2;
        t2.start();
        QVERIFY(!r.mgr->runCommand("evil-demo:loop", &r.note, &err));
        QVERIFY(t2.elapsed() < 20);
        QVERIFY(x_audit(r).contains("auto-disable"));
        QVERIFY(r.lib.calls.isEmpty() && r.clip.v == "clip" && r.theme.sets.isEmpty());
        QVERIFY(!QFileInfo::exists("/tmp/hn-evil-demo-should-not-exist"));
    }
    void evilDemoCannotTouchOtherPlugins() {
        Rig r;
        QVERIFY(r.addLua("victim", {"storage"}, "hn.command{id='c',title='C',run=function()\n"
                                                "  hn.log('os=' .. tostring(os) .. ' leak=' .. tostring(rawget(_G, 'leak')) .. ' upper=' .. ('x'):upper() .. ' http=' .. type(hn.http))\n"
                                                "  hn.log('stored=' .. tostring(hn.storage.get('k')))\n"
                                                "end}\nhn.storage.set('k', 'mine')\n"));
        QVERIFY(r.mgr->install(fixture("evil-demo"), true).ok);
        QVERIFY(r.mgr->consent("evil-demo", {"note.read", "storage", "ui"}));
        QVERIFY(r.mgr->enable("evil-demo"));
        QVERIFY(r.mgr->runCommand("evil-demo:tamper", &r.note));  // vandalises its own private environment
        QVERIFY(r.mgr->runCommand("victim:c", &r.note));
        QCOMPARE(r.logsOf("victim"), (QStringList{"os=nil leak=nil upper=X http=table", "stored=mine"}));
    }
};
QTEST_MAIN(ExamplesTest)
#include "plugins_examples_test.moc"
