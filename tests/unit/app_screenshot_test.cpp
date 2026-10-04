// Opt-in: set HN_SCREENSHOT_DIR to write app-{organizer,sticky}-{light,dark}.png (QWidget::grab, offscreen).
#include "app_test_util.h"
#include <QImage>
#include <QLineEdit>
#include <QMenu>
#include "settings_dialog.h"
#include "builtin_themes.h"

using namespace apptest;

static void populate(Lib &l) {
    l.write("Meeting notes.md", "# Meeting notes\n\nQuarterly planning with the **design** team.\n\n- [x] Agree on the roadmap\n- [ ] Send the summary\n- [ ] Book the next review\n\n#work #planning\n");
    l.write("Work/Launch plan.md", "# Launch plan\n\nShip the beta on Friday. Watch the **crash rate** and keep the changelog short.\n\n1. Freeze the branch\n2. Tag the release\n3. Announce\n\n#work #roadmap\n");
    l.write("Work/Retro.md", "# Retro\n\nWhat went well, what did not, and what we change next sprint.\n\n#work\n");
    l.write("Reading list.md", "# Reading list\n\nThe Design of Everyday Things, Norman.\nA Pattern Language, Alexander.\n\n#ideas #books\n");
    l.write("Groceries.md", "# Groceries\n\n- [ ] Oat milk\n- [ ] Coffee beans\n- [x] Lemons\n");
    l.write("Personal/Trip to Lisbon.md", "# Trip to Lisbon\n\nFlights booked. Find a quiet cafe near the river and walk the old town.\n\n#travel #ideas\n");
    l.write("Scratch.md", "Just a quick thought about keyboard shortcuts.\n");
}

static QString suffix() { return qEnvironmentVariable("HN_SCREENSHOT_SUFFIX"); }

class ShotTest : public QObject {
    Q_OBJECT
private slots:
    void states() {
        const QString out = qEnvironmentVariable("HN_SCREENSHOT_DIR");
        if (out.isEmpty()) QSKIP("HN_SCREENSHOT_DIR not set");
        QDir().mkpath(out);
        const QString sfx = suffix();
        for (const char *scheme : {"light", "dark"}) {
            {   // empty library
                Lib e;
                AppController c(e.opts());
                auto s = c.settings(); s.colorScheme = scheme; c.applySettings(s, c.prefs());
                c.showOrganizer();
                auto *o = c.organizer();
                o->resize(900, 640);
                QTest::qWait(500);
                QVERIFY(o->grab().save(QString("%1/app-organizer-nonotes-%2%3.png").arg(out, scheme, sfx)));
            }
            Lib l;
            populate(l);
            AppController c(l.opts());
            auto s = c.settings(); s.colorScheme = scheme; c.applySettings(s, c.prefs());
            c.setNoteColor("Work/Launch plan.md", 1);
            c.showOrganizer();
            auto *o = c.organizer();
            o->resize(900, 640);
            QTRY_VERIFY_WITH_TIMEOUT(o->model()->rowCount() >= 7, 8000);
            QTest::qWait(400);
            QVERIFY(o->grab().save(QString("%1/app-organizer-noselection-%2%3.png").arg(out, scheme, sfx)));
            hn::app::ui::ShortcutSheet::toggle(o, c.settings().keybindings);
            QTest::qWait(100);
            QVERIFY(o->grab().save(QString("%1/app-organizer-keys-%2%3.png").arg(out, scheme, sfx)));
            hn::app::ui::ShortcutSheet::toggle(o, c.settings().keybindings);
            QTRY_VERIFY(!hn::app::ui::ShortcutSheet::isOpen(o));
            auto *pop = new hn::app::ui::SwatchPopover(1);
            pop->popup(QPoint(40, 40));
            QTest::qWait(100);
            QVERIFY(pop->grab().save(QString("%1/app-color-popover-%2%3.png").arg(out, scheme, sfx)));
            pop->close();
            c.openSticky("Work/Launch plan.md");
            auto *w = c.stickyOf("Work/Launch plan.md");
            w->resize(360, 300);
            QTest::qWait(300);
            hn::app::ui::ShortcutSheet::toggle(w, c.settings().keybindings);
            QTest::qWait(100);
            QVERIFY(w->grab().save(QString("%1/app-sticky-keys-%2%3.png").arg(out, scheme, sfx)));
            hn::app::ui::ShortcutSheet::toggle(w, c.settings().keybindings);
        }
    }
    // Opt-in review sweep: every built-in theme (dark; plus Modernist light) -> organizer, menu, Settings tabs, sticky.
    void themeSweep() {
        const QString out = qEnvironmentVariable("HN_SCREENSHOT_DIR");
        if (out.isEmpty()) QSKIP("HN_SCREENSHOT_DIR not set");
        QDir().mkpath(out);
        Lib l;
        populate(l);
        AppController c(l.opts());
        c.setNoteColor("Work/Launch plan.md", 1);
        QList<QPair<QString, QString>> combos;
        combos << qMakePair(QString("modernist"), QString("light"));
        for (const auto &id : hn::theme::detail::builtinThemeIds()) combos << qMakePair(id, QString("dark"));
        for (const auto &[id, scheme] : combos) {
            auto st = c.settings(); st.theme = id; st.colorScheme = scheme;
            c.applySettings(st, c.prefs());
            const QString tag = id + "-" + scheme;
            c.showOrganizer();
            auto *o = c.organizer();
            o->resize(900, 640);
            QTRY_VERIFY_WITH_TIMEOUT(o->model()->rowCount() >= 7, 8000);
            c.openInOrganizer("Meeting notes.md");
            QTest::qWait(500);
            QVERIFY(o->grab().save(QString("%1/sweep-%2-organizer.png").arg(out, tag)));
            QMenu m(o);
            m.addAction("Open in sticky");
            m.addAction("Rename...");
            m.addAction("Tags...");
            auto *sub = m.addMenu("Color");
            sub->addAction("Red");
            m.addSeparator();
            auto *dis = m.addAction("Disabled item"); dis->setEnabled(false);
            m.addAction("Delete...");
            m.ensurePolished();
            m.resize(m.sizeHint());
            QVERIFY(m.grab().save(QString("%1/sweep-%2-menu.png").arg(out, tag)));
            hn::app::SettingsDialog dlg(&c);
            dlg.resize(700, 600);
            dlg.show();
            for (int t = 0; t < dlg.tabs()->count(); ++t) {
                dlg.showTab(t);
                QTest::qWait(150);
                QVERIFY(dlg.grab().save(QString("%1/sweep-%2-settings%3.png").arg(out, tag).arg(t)));
            }
            dlg.close();
            c.openSticky("Work/Launch plan.md");
            auto *w = c.stickyOf("Work/Launch plan.md");
            w->resize(360, 300);
            w->setToolbarShown(true);
            QTest::qWait(300);
            QVERIFY(w->grab().save(QString("%1/sweep-%2-sticky.png").arg(out, tag)));
            c.closeNote("Work/Launch plan.md");
            QTRY_VERIFY(!c.session("Work/Launch plan.md"));
        }
    }
    void screenshots() {
        const QString out = qEnvironmentVariable("HN_SCREENSHOT_DIR");
        if (out.isEmpty()) QSKIP("HN_SCREENSHOT_DIR not set");
        QDir().mkpath(out);
        Lib l;
        populate(l);
        AppController c(l.opts());
        c.setNoteColor("Meeting notes.md", 0);
        c.setNoteColor("Work/Launch plan.md", 1);
        c.setNoteColor("Work/Retro.md", 2);
        c.setNoteColor("Reading list.md", 3);
        c.setNoteColor("Groceries.md", 2);
        c.setNoteColor("Personal/Trip to Lisbon.md", 1);
        c.setNoteColor("Scratch.md", 0);
        for (const char *scheme : {"light", "dark"}) {
            auto s = c.settings();
            s.colorScheme = scheme;
            c.applySettings(s, c.prefs());
            c.showOrganizer();
            auto *o = c.organizer();
            o->resize(900, 640);
            c.openInOrganizer("Meeting notes.md");
            QTRY_VERIFY_WITH_TIMEOUT(o->model()->rowCount() >= 7, 8000);
            QTest::qWait(500);
            c.openInOrganizer("Meeting notes.md");
            QTest::qWait(300);
            QVERIFY(o->grab().save(QString("%1/app-organizer-%2%3.png").arg(out, scheme, suffix())));

            auto *st = c.openSticky("Work/Launch plan.md");
            auto *w = c.stickyOf("Work/Launch plan.md");
            w->resize(360, 300);
            QTest::qWait(400);
            QVERIFY(w->grab().save(QString("%1/app-sticky-%2%3.png").arg(out, scheme, suffix())));
            // extra states for review
            w->setToolbarShown(true);
            w->setWorkspaceMode(hn::platform::WorkspaceMode::AllWorkspaces);
            QTest::qWait(300);
            QVERIFY(w->grab().save(QString("%1/app-sticky-toolbar-%2%3.png").arg(out, scheme, suffix())));
            c.closeNote("Work/Launch plan.md");
            QTRY_VERIFY(!c.session("Work/Launch plan.md"));
            Q_UNUSED(st);
            o->searchEdit()->setText("zzzz");
            QTest::qWait(600);
            QVERIFY(o->grab().save(QString("%1/app-organizer-empty-%2%3.png").arg(out, scheme, suffix())));
            o->searchEdit()->clear();
            QTest::qWait(400);
        }
    }
};

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    ShotTest t;
    return QTest::qExec(&t, argc, argv);
}
#include "app_screenshot_test.moc"
