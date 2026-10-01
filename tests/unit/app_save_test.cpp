#include "app_test_util.h"
#include <QElapsedTimer>
#include <unistd.h>

using namespace apptest;
using hn::platform::Action;
using St = NoteSession::State;

static QString plain(NoteSession *s) { return s->editor()->visualEdit()->toPlainText(); }

class SaveTest : public QObject {
    Q_OBJECT
    // external edit while the note has unsaved local text => conflict
    NoteSession *conflict(AppController &c, Lib &l) {
        l.write("a.md", "# A\n\nbase\n");
        auto *s = c.openSticky("a.md");
        s->setTiming({300, 600});
        typeText(s, "LOCAL");
        l.write("a.md", "# A\n\nexternal\n");
        for (int i = 0; i < 100 && s->state() != St::Conflict; ++i) QTest::qWait(50);
        return s;
    }
private slots:
    void autosave_idle_750ms() {
        Lib l; l.write("a.md", "# A\n\nbase\n");
        AppController c(l.opts());
        auto *s = c.openSticky("a.md");
        QCOMPARE(s->timing().idleMs, 750);
        QCOMPARE(s->timing().maxMs, 5000);
        typeText(s, "x");
        QTest::qWait(500);
        QVERIFY(!l.read("a.md").contains("x"));      // not before the idle interval
        QCOMPARE(s->state(), St::Dirty);
        QTest::qWait(450);                            // ~950 ms after the edit
        QTRY_VERIFY_WITH_TIMEOUT(l.read("a.md").contains("x"), 1500);
        QTRY_COMPARE(s->state(), St::Clean);
        QCOMPARE(s->stateLabel(), QString("Saved"));
    }

    void autosave_max_5s_during_continuous_typing() {
        Lib l; l.write("a.md", "# A\n\nbase\n");
        AppController c(l.opts());
        auto *s = c.openSticky("a.md");
        QElapsedTimer t; t.start();
        bool savedEarly = false;
        qint64 savedAt = -1;
        while (t.elapsed() < 6500) {
            typeText(s, "k");
            QTest::qWait(300);                        // never idle for 750 ms
            if (savedAt < 0 && l.read("a.md").contains("k")) savedAt = t.elapsed();
            if (t.elapsed() < 4500 && savedAt >= 0) savedEarly = true;
        }
        QVERIFY2(!savedEarly, "saved before the 5 s maximum while typing continuously");
        QVERIFY2(savedAt >= 4800 && savedAt <= 6000, qPrintable(QString::number(savedAt)));
    }

    void save_failure_keeps_editor_and_offers_retry() {
        if (geteuid() == 0) QSKIP("root ignores directory permissions");
        Lib l; l.write("a.md", "# A\n\nbase\n");
        AppController c(l.opts());
        auto *s = c.openSticky("a.md");
        s->setTiming({100, 300});
        QFile::setPermissions(l.notes, QFileDevice::ReadOwner | QFileDevice::ExeOwner);
        typeText(s, "F");
        QTRY_COMPARE_WITH_TIMEOUT(s->state(), St::Failed, 4000);
        auto *w = c.stickyOf("a.md");
        QVERIFY(w->status()->actionsVisible());
        QVERIFY(w->status()->button("retry")->isVisibleTo(w->status()));
        QVERIFY(!s->detail().isEmpty());
        c.closeNote("a.md");                          // refused: failure never discards a dirty editor
        QTest::qWait(300);
        QVERIFY(c.session("a.md"));
        QVERIFY(w->isVisible());
        QVERIFY(plain(s).contains("F"));
        QFile::setPermissions(l.notes, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        w->status()->button("retry")->click();
        QTRY_COMPARE(s->state(), St::Clean);
        QVERIFY(l.read("a.md").contains("F"));
    }

    void conflict_replace_disk() {
        Lib l; AppController c(l.opts());
        auto *s = conflict(c, l);
        QCOMPARE(s->state(), St::Conflict);
        QVERIFY(c.stickyOf("a.md")->status()->button("replace")->isVisibleTo(c.stickyOf("a.md")->status()));
        QVERIFY(l.read("a.md").contains("external"));   // never silently overwritten
        s->replaceDisk();
        QTRY_COMPARE(s->state(), St::Clean);
        QVERIFY(l.read("a.md").contains("LOCAL"));
    }

    void conflict_reload() {
        Lib l; AppController c(l.opts());
        auto *s = conflict(c, l);
        QCOMPARE(s->state(), St::Conflict);
        s->reload();
        QCOMPARE(s->state(), St::Clean);
        QVERIFY(plain(s).contains("external") && !plain(s).contains("LOCAL"));
        QVERIFY(l.read("a.md").contains("external"));
    }

    void conflict_save_local_as_copy() {
        Lib l; AppController c(l.opts());
        auto *s = conflict(c, l);
        QCOMPARE(s->state(), St::Conflict);
        QString copy;
        QVERIFY(s->saveLocalCopy(&copy));
        QVERIFY(copy.contains("local copy"));
        QVERIFY(l.read(copy).contains("LOCAL"));
        QVERIFY(l.read("a.md").contains("external"));
        QCOMPARE(s->state(), St::Clean);
        QVERIFY(plain(s).contains("external"));
    }

    void external_change_reloads_clean_document() {
        Lib l; l.write("a.md", "# A\n\nbase\n");
        AppController c(l.opts());
        auto *s = c.openSticky("a.md");
        l.write("a.md", "# A\n\nchanged elsewhere\n");
        QTRY_VERIFY_WITH_TIMEOUT(plain(s).contains("changed elsewhere"), 4000);
        QCOMPARE(s->state(), St::Clean);
    }

    void deleted_dirty_note_is_not_silently_recreated() {
        Lib l; l.write("a.md", "# A\n\nbase\n"); l.write("c.md", "# C\n");
        AppController c(l.opts());
        auto *s = c.openSticky("a.md");
        s->setTiming({200, 500});
        typeText(s, "D");
        QFile::remove(l.notes + "/a.md");
        QTRY_COMPARE_WITH_TIMEOUT(s->state(), St::Removed, 4000);
        QTest::qWait(900);
        QVERIFY(!QFile::exists(l.notes + "/a.md"));
        QVERIFY(c.session("a.md"));
        s->recreate();
        QTRY_COMPARE(s->state(), St::Clean);
        QVERIFY(l.read("a.md").contains("D"));
        // a clean deleted note just closes
        auto *cs = c.openSticky("c.md");
        Q_UNUSED(cs);
        QFile::remove(l.notes + "/c.md");
        QTRY_VERIFY_WITH_TIMEOUT(!c.session("c.md"), 4000);
    }

    void quit_finishes_pending_saves() {
        Lib l; l.write("a.md", "# A\n\nbase\n"); l.write("b.md", "# B\n");
        AppController c(l.opts());
        auto *a = c.openSticky("a.md"); auto *b = c.openInOrganizer("b.md");
        typeText(a, "QA"); typeText(b, "QB");
        QSignalSpy ex(&c, &AppController::exitRequested);
        c.requestQuit();
        QTRY_COMPARE(ex.count(), 1);
        QVERIFY(l.read("a.md").contains("QA") && l.read("b.md").contains("QB"));
        QVERIFY(c.session("a.md"));                  // windows stay: session file keeps them for next start
    }

    void quit_with_failing_save_aborts_and_shows_error() {
        if (geteuid() == 0) QSKIP("root ignores directory permissions");
        Lib l; l.write("a.md", "# A\n\nbase\n");
        AppController c(l.opts());
        auto *a = c.openSticky("a.md");
        QFile::setPermissions(l.notes, QFileDevice::ReadOwner | QFileDevice::ExeOwner);
        typeText(a, "NO");
        QSignalSpy ex(&c, &AppController::exitRequested), ab(&c, &AppController::quitAborted);
        c.requestQuit();
        QTRY_COMPARE_WITH_TIMEOUT(ab.count(), 1, 4000);
        QCOMPARE(ex.count(), 0);
        QVERIFY(!c.quitting());
        QVERIFY(c.stickyOf("a.md")->isVisible());
        QCOMPARE(a->state(), St::Failed);
        QFile::setPermissions(l.notes, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    }

    void mod_command_is_one_undo_step_and_autosaves() {
        Lib l;
        const QString modDir = l.dir.filePath("mods/uppercase-selection");
        QDir().mkpath(modDir);
        const QString so = QString(HN_EXAMPLE_MOD_DIR) + "/libuppercase_selection.so";
        if (!QFile::exists(so)) QSKIP("example mod not built");
        QVERIFY(QFile::copy(so, modDir + "/libuppercase_selection.so"));
        QVERIFY(QFile::copy(QString(HN_EXAMPLE_MOD_SRC) + "/mod.json", modDir + "/mod.json"));
        QDir().mkpath(l.dir.filePath("cfg"));
        QFile en(l.dir.filePath("cfg/mods-enabled.json"));
        QVERIFY(en.open(QIODevice::WriteOnly));
        en.write(R"({"version":1,"enabled":["uppercase-selection"]})");
        en.close();
        l.write("a.md", "# T\n\nhello world\n");
        QFile fb(l.dir.filePath("cfg/mods-enabled.json"));
        QVERIFY(fb.open(QIODevice::ReadOnly));
        const QByteArray enabledBefore = fb.readAll();
        fb.close();
        auto opt = l.opts();
        opt.pluginHooks.consent = [](const ConsentRequest &r) { return r.native && r.permissions == QStringList{"native"}; };   // the user approves the native warning
        AppController c(opt);
        auto *s = c.openSticky("a.md");
        s->setTiming({200, 500});
        auto *ed = s->editor();
        QTextEdit *v = ed->visualEdit();
        QTextCursor cur = v->document()->find("hello");
        QVERIFY(!cur.isNull());
        v->setTextCursor(cur);
        const int before = ed->history().position();
        // Legacy mods are migrated to native plugins and no longer run on the old enable list alone: consent first.
        QVERIFY(!c.runModCommand("a.md", "uppercase-selection"));
        QVERIFY(c.plugins().manager()->info("uppercase-selection").status == hn::plugins::Status::NeedsConsent);
        QVERIFY(c.plugins().enable("uppercase-selection", nullptr));
        QCOMPARE(c.plugins().manager()->info("uppercase-selection").status, hn::plugins::Status::Enabled);
        QFile f2(l.dir.filePath("cfg/mods-enabled.json"));   // the legacy enable list is left exactly as it was
        QVERIFY(f2.open(QIODevice::ReadOnly));
        QCOMPARE(f2.readAll(), enabledBefore);
        QVERIFY(c.runModCommand("a.md", "uppercase-selection"));
        QVERIFY(plain(s).contains("HELLO world"));
        QCOMPARE(ed->history().position(), before + 1);   // one begin/end pair = one undo step
        QTRY_VERIFY_WITH_TIMEOUT(l.read("a.md").contains("HELLO"), 3000);   // goes through autosave
        QVERIFY(ed->undo());
        QVERIFY(plain(s).contains("hello world") && !plain(s).contains("HELLO"));
        QTRY_VERIFY_WITH_TIMEOUT(l.read("a.md").contains("hello world"), 3000);
        QVERIFY(ed->redo());
        QVERIFY(plain(s).contains("HELLO world"));
    }
};

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    SaveTest t;
    return QTest::qExec(&t, argc, argv);
}
#include "app_save_test.moc"
