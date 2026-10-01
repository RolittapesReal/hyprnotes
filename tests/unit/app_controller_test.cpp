#include "app_test_util.h"
#include "policy.h"
#include <QJsonDocument>
#include <QJsonObject>

using namespace apptest;
using hn::platform::Action;

class ControllerTest : public QObject {
    Q_OBJECT
private slots:
    void tray_policy_logic() {
        QCOMPARE(organizerCloseAction(true, true), OrganizerClose::HideToTray);
        QCOMPARE(organizerCloseAction(true, false), OrganizerClose::CloseNormally);
        QCOMPARE(organizerCloseAction(false, true), OrganizerClose::CloseNormally);
        QVERIFY(exitAfterWindowClosed(false, 0));
        QVERIFY(!exitAfterWindowClosed(false, 1));   // hidden/other-workspace stickies still count
        QVERIFY(!exitAfterWindowClosed(true, 0));    // usable tray keeps the idle instance
    }

    void open_popout_popin_close_release() {
        Lib l; l.write("a.md", "# A\n\nalpha\n"); l.write("b.md", "# B\n\nbeta\n");
        AppController c(l.opts());
        auto *sa = c.openSticky("a.md");
        QVERIFY(sa);
        QVERIFY(c.stickyOf("a.md"));
        QCOMPARE(c.registry().noteFor(c.stickyOf("a.md")->token()), QString("a.md"));
        // duplicate open focuses the existing editor: same session, one window
        QCOMPARE(c.openSticky("a.md"), sa);
        QCOMPARE(c.stickies().size(), 1);
        QCOMPARE(c.openNotes().size(), 1);

        // organizer open of a note that lives in a sticky: no second editor
        c.openInOrganizer("a.md");
        QCOMPARE(c.openNotes().size(), 1);
        QVERIFY(c.organizer()->currentSession() == nullptr);

        auto *sb = c.openInOrganizer("b.md");
        QCOMPARE(c.organizerNote(), QString("b.md"));
        typeText(sb, "XY");
        auto *ed = sb->editor();
        QVERIFY(ed->canUndo());
        const int cursor = sb->cursorPosition();
        QVERIFY(c.popOut("b.md"));
        QVERIFY(c.stickyOf("b.md"));
        QVERIFY(!c.organizer()->currentSession());
        QCOMPARE(c.session("b.md")->editor(), ed);                      // same editor object: content, cursor, history move with it
        QCOMPARE(sb->cursorPosition(), cursor);
        QVERIFY(ed->canUndo());
        QVERIFY(ed->parentWidget() && ed->window() == c.stickyOf("b.md"));
        QVERIFY(c.popIn("b.md"));
        QTRY_VERIFY(!c.stickyOf("b.md"));
        QCOMPARE(c.organizerNote(), QString("b.md"));
        QCOMPARE(c.session("b.md")->editor(), ed);
        QVERIFY(ed->canUndo());
        QCOMPARE(sb->cursorPosition(), cursor);

        c.closeNote("b.md");
        QTRY_VERIFY(!c.session("b.md"));                                 // successful close releases editor
        QVERIFY(l.read("b.md").contains("XY"));
        c.closeNote("a.md");
        QTRY_VERIFY(!c.session("a.md"));
        QVERIFY(c.stickies().isEmpty());
        QVERIFY(sessionsMatchWindows(c));
    }

    // D15: opening (in the organizer) a note that already lives in a sticky used to drop the organizer's current
    // note from the window without closing it: a live editor no window showed, kept until quit.
    void organizer_target_in_sticky_does_not_orphan_current_note() {
        Lib l; l.write("a.md", "# A\n"); l.write("b.md", "# B\n");
        AppController c(l.opts());
        c.openSticky("a.md");
        c.openInOrganizer("b.md");
        QVERIFY(sessionsMatchWindows(c));
        c.openInOrganizer("a.md");                       // a is in a sticky: organizer keeps no editor...
        QTRY_VERIFY(!c.session("b.md"));                 // ...and b must be closed, not leaked
        QVERIFY(sessionsMatchWindows(c));
        QCOMPARE(c.openNotes().size(), 1);
        // the same invariant across a mix of re-attach operations
        c.openInOrganizer("b.md"); c.popOut("b.md"); c.popIn("b.md"); c.openInOrganizer("a.md"); c.openSticky("b.md");
        c.newNote(); c.newNote({}, true); c.popOut(c.organizerNote());
        QTest::qWait(50);
        QVERIFY(sessionsMatchWindows(c));
        for (const QString &r : c.openNotes()) c.closeNote(r);
        QTRY_VERIFY(c.openNotes().isEmpty());
    }

    // Closing a popped-out sticky releases its note; 'Pop in here' on the stale organizer panel must still open it.
    void pop_in_after_sticky_closed_opens_note_in_organizer() {
        Lib l; l.write("a.md", "# A\n");
        AppController c(l.opts());
        c.openInOrganizer("a.md");
        QVERIFY(c.popOut("a.md"));
        c.closeNote("a.md");
        QTRY_VERIFY(!c.session("a.md"));
        QVERIFY(c.popIn("a.md"));
        QVERIFY(c.session("a.md"));
        QCOMPARE(c.organizerNote(), QString("a.md"));
        QVERIFY(sessionsMatchWindows(c));
        c.closeNote("a.md");
        QTRY_VERIFY(c.openNotes().isEmpty());
    }

    // Context menu backing action: duplicate works for closed and for open (unsaved-edit) notes.
    void duplicate_note_copies_closed_and_open_notes() {
        Lib l; l.write("a.md", "# A\nbody\n");
        AppController c(l.opts());
        QString err;
        const QString copy = c.duplicateNote("a.md", &err);
        QVERIFY2(!copy.isEmpty(), qPrintable(err));
        QVERIFY(copy != "a.md");
        QFile f(l.notes + "/" + copy);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), QByteArray("# A\nbody\n"));
        QVERIFY(!c.session(copy));                       // duplicating never opens windows
        c.openSticky("a.md");
        const QString copy2 = c.duplicateNote("a.md", &err);
        QVERIFY(!copy2.isEmpty() && copy2 != copy);
        for (const QString &r : c.openNotes()) c.closeNote(r);
        QTRY_VERIFY(c.openNotes().isEmpty());
    }

    // D13: a session file listing many notes restores at most 30 windows, in batches, rest stay in the file.
    void session_restore_is_capped_and_staggered() {
        Lib l;
        hn::platform::SessionState st;
        for (int i = 0; i < 80; ++i) {
            l.write(QString("n%1.md").arg(i), "# N\n");
            hn::platform::WindowState w; w.noteKey = QString("n%1.md").arg(i); w.geometry = QRect(0, 0, 360, 300); st.windows.push_back(w);
        }
        QDir().mkpath(l.state);
        QString err;
        QVERIFY(hn::platform::saveSession(l.state + "/session.json", st, &err));
        AppController c(l.opts());
        QStringList notices;
        QObject::connect(&c, &AppController::errorOccurred, &c, [&](const QString &m) { notices << m; });
        QVERIFY(c.handleAction(Action::ShowOrganizer));
        QVERIFY(c.stickies().size() < 30);               // first batch only: the event loop gets control back
        QTRY_COMPARE(c.stickies().size(), 30);
        QTest::qWait(100);
        QCOMPARE(c.stickies().size(), 30);               // capped
        QCOMPARE(notices.size(), 1);
        QCOMPARE(c.sessionState().windows.size(), 80);   // the rest remain listed (closed but restorable)
        QVERIFY(sessionsMatchWindows(c));
    }

    void background_loads_nothing() {
        Lib l; l.write("a.md", "# A\n");
        auto o = l.opts(); o.background = true;
        AppController c(o);
        c.start(Action::Background);
        QVERIFY(c.openNotes().isEmpty());
        QVERIFY(!c.hasIndex());          // no body indexing
        QVERIFY(!c.organizer());         // organizer is lazy
        QVERIFY(c.stickies().isEmpty());
        QVERIFY(c.handleAction(Action::Background));
    }

    void session_restore_is_passive() {
        Lib l; l.write("a.md", "# A\n"); l.write("b.md", "# B\n"); l.write("gone.md", "x");
        {
            AppController c(l.opts());
            c.openSticky("a.md"); c.openSticky("b.md"); c.openSticky("gone.md");
            c.flushSessionFile();
        }
        QFile::remove(l.notes + "/gone.md");
        AppController c2(l.opts());
        QVERIFY(c2.stickies().isEmpty());
        QVERIFY(c2.handleAction(Action::ShowOrganizer));
        QCOMPARE(c2.stickies().size(), 2);
        for (auto *w : c2.stickies()) QVERIFY(w->testAttribute(Qt::WA_ShowWithoutActivating));
        QVERIFY(c2.session("a.md") && c2.session("b.md") && !c2.session("gone.md"));
        // background start must not rewrite the saved session
        c2.closeNote("a.md");
        QTRY_VERIFY(!c2.session("a.md"));
    }

    void first_run_folder_choice() {
        QTemporaryDir d;
        ControllerOptions o;
        o.stateDir = d.filePath("s"); o.cacheDir = d.filePath("c"); o.configPath = d.filePath("cfg/config.json");
        o.modsDir = d.filePath("m"); o.useTray = false; o.useHyprland = false;
        const QString chosen = d.filePath("MyNotes");
        QString suggestedSeen;
        o.chooseNotesFolder = [&](const QString &s) { suggestedSeen = s; return chosen; };
        AppController c(o);
        QVERIFY(!QFile::exists(o.configPath));                // never written at load
        QVERIFY(c.handleAction(Action::ShowOrganizer));
        QVERIFY(suggestedSeen.endsWith("Notes/Hyprnotes"));
        QCOMPARE(c.notesRoot(), chosen);
        QVERIFY(QFile::exists(o.configPath));
        hn::theme::Config cfg(o.configPath);
        QCOMPARE(cfg.settings().notesFolder, chosen);
        QVERIFY(cfg.settings().trayEnabled);                  // tray defaults to enabled (desktop design 4)
    }

    void crash_recovery_offer() {
        Lib l; l.write("r.md", "old\n"); l.write("d.md", "keep\n");
        {
            hn::core::NoteRepository r(l.notes, l.state + "/recovery");
            QByteArray disk = "old\n";
            QString err;
            QVERIFY(r.recovery().writePending(r.root(), "r.md", "recovered\n", &disk, 1, &err));
            QByteArray disk2 = "keep\n";
            QVERIFY(r.recovery().writePending(r.root(), "d.md", "dropme\n", &disk2, 1, &err));
        }
        auto o = l.opts();
        int offered = 0;
        o.recoveryPrompt = [&](const QList<hn::core::RecoveryEntry> &e) { offered = int(e.size()); return RecoveryChoice::Restore; };
        {
            AppController c(o);
            QVERIFY(c.handleAction(Action::ShowOrganizer));
            QCOMPARE(offered, 2);
            QVERIFY(c.session("r.md"));
            QTRY_COMPARE(l.read("r.md"), QByteArray("recovered\n"));
            QTRY_VERIFY(c.repo().recoverableDrafts().isEmpty());
        }
        Lib l2; l2.write("d.md", "keep\n");
        {
            hn::core::NoteRepository r(l2.notes, l2.state + "/recovery");
            QByteArray disk = "keep\n"; QString err;
            r.recovery().writePending(r.root(), "d.md", "dropme\n", &disk, 1, &err);
        }
        auto o2 = l2.opts();
        o2.recoveryPrompt = [](const QList<hn::core::RecoveryEntry> &) { return RecoveryChoice::Discard; };
        AppController c2(o2);
        c2.handleAction(Action::ShowOrganizer);
        QCOMPARE(l2.read("d.md"), QByteArray("keep\n"));
        QVERIFY(c2.repo().recoverableDrafts().isEmpty());
    }

    void residency_without_usable_tray_exits_on_last_close() {
        Lib l; l.write("a.md", "# A\n");
        AppController c(l.opts());
        c.setTrayUsableOverride(false);
        QSignalSpy exitSpy(&c, &AppController::exitRequested);
        c.showOrganizer();
        c.openSticky("a.md");
        c.organizer()->close();                        // organizer closes normally, sticky survives
        QTRY_VERIFY(!c.organizer());
        QCOMPARE(exitSpy.count(), 0);
        QVERIFY(c.session("a.md"));
        c.stickyOf("a.md")->close();                   // last window
        QTRY_COMPARE(exitSpy.count(), 1);
    }

    void residency_with_tray_hides_organizer_and_keeps_editor() {
        Lib l; l.write("a.md", "# A\n");
        AppController c(l.opts());
        c.setTrayUsableOverride(true);
        QSignalSpy exitSpy(&c, &AppController::exitRequested);
        c.showOrganizer();
        auto *s = c.openInOrganizer("a.md");
        typeText(s, "kept");
        const int cur = s->cursorPosition();
        c.organizer()->close();
        QVERIFY(c.organizer());
        QVERIFY(!c.organizer()->isVisible());
        QCOMPARE(c.organizer()->currentSession(), s);   // hidden organizer keeps its editor, cursor and history
        QCOMPARE(s->cursorPosition(), cur);
        QVERIFY(s->editor()->canUndo());
        QCOMPARE(exitSpy.count(), 0);
        c.showOrganizer();
        QVERIFY(c.organizer()->isVisible());
    }

    void switching_organizer_notes_saves_and_releases_previous() {
        Lib l; l.write("a.md", "# A\n"); l.write("b.md", "# B\n");
        AppController c(l.opts());
        auto *a = c.openInOrganizer("a.md");
        typeText(a, "Z");
        c.openInOrganizer("b.md");
        QTRY_VERIFY(!c.session("a.md"));
        QVERIFY(l.read("a.md").contains("Z"));
        QCOMPARE(c.organizerNote(), QString("b.md"));
    }

    void rename_delete_color_tags() {
        Lib l; l.write("a.md", "# A\n\nhello #old\n");
        AppController c(l.opts());
        auto *s = c.openInOrganizer("a.md");
        QString err;
        QVERIFY2(c.renameNote("a.md", "Alpha", &err), qPrintable(err));
        QCOMPARE(s->rel(), QString("Alpha.md"));
        QVERIFY(QFile::exists(l.notes + "/Alpha.md"));
        c.setNoteColor("Alpha.md", 3);
        QCOMPARE(c.noteColor("Alpha.md"), 3);
        QVERIFY(c.editTags("Alpha.md", {"new", "other"}));       // old removed, two added
        const QString md = QString::fromUtf8(s->editor()->toMarkdownBytes());
        QVERIFY(md.contains("#new") && md.contains("#other") && !md.contains("#old"));
        QVERIFY(s->editor()->undo());                            // one step restores the tag edit
        QVERIFY(QString::fromUtf8(s->editor()->toMarkdownBytes()).contains("#old"));
        QVERIFY(c.deleteNote("Alpha.md", &err));
        QVERIFY(!QFile::exists(l.notes + "/Alpha.md"));
        QVERIFY(!c.session("Alpha.md"));
    }

    void sticky_identity_is_set_before_show() {
        Lib l; l.write("a.md", "# A\n");
        AppController c(l.opts());
        c.openSticky("a.md");
        auto *w = c.stickyOf("a.md");
        QCOMPARE(w->property("hn.role").toString(), QString("hyprnotes-sticky"));
        QCOMPARE(w->objectName(), w->token());
        QCOMPARE(w->minimumSize(), QSize(260, 180));
        QVERIFY(w->windowFlags() & Qt::FramelessWindowHint);
        QCOMPARE(w->header()->height(), 28);
        QVERIFY(!c.quitting());
    }
};

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    ControllerTest t;
    return QTest::qExec(&t, argc, argv);
}
#include "app_controller_test.moc"
