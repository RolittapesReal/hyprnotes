#include "app_test_util.h"
#include "tag_edit.h"
#include "hn/core/worker.h"
#include <QDateTime>
#include <QLineEdit>
#include <QListView>
#include <QSemaphore>
#include <QScrollBar>
#include <QTimeZone>
#include <memory>

using namespace apptest;
using hn::platform::Action;

static void fill(Lib &l, int n, const QString &prefix = "note") {
    for (int i = 0; i < n; ++i) l.write(QString("%1-%2.md").arg(prefix).arg(i, 4, 10, QChar('0')), QString("# %1 %2\n\nbody number %2 alpha\n").arg(prefix).arg(i).toUtf8());
}

static void writeAt(Lib &l, const QString &rel, qint64 mtimeMs) {
    l.write(rel, "# fixture\n\nbody\n");
    QFile f(l.notes + "/" + rel);
    QVERIFY(f.open(QIODevice::ReadWrite));
    QVERIFY(f.setFileTime(QDateTime::fromMSecsSinceEpoch(mtimeMs, QTimeZone::UTC), QFileDevice::FileModificationTime));
}

class IndexSearchDequeueGate {
    struct State { QSemaphore entered, release; };
    std::shared_ptr<State> m_state = std::make_shared<State>();
    bool m_enteredSeen = false;
    bool m_opened = false;
public:
    IndexSearchDequeueGate() {
        const auto state = m_state;
        hn::core::Worker::shared().setBeforeDequeueHookForTest(
            hn::core::Worker::Index, QStringLiteral("search:"), [state] {
                state->entered.release();
                state->release.acquire();
            });
    }
    ~IndexSearchDequeueGate() {
        open();
        hn::core::Worker::shared().clearBeforeDequeueHookForTest();
    }
    bool takeEntered() {
        if (m_enteredSeen) return true;
        if (m_state->entered.tryAcquire()) m_enteredSeen = true;
        return m_enteredSeen;
    }
    void open() {
        if (!m_opened) { m_opened = true; m_state->release.release(); }
    }
};

class UiTest : public QObject {
    Q_OBJECT
private slots:
    void organizer_defaults_and_empty_states() {
        Lib l;
        AppController c(l.opts());
        c.showOrganizer();
        auto *o = c.organizer();
        QCOMPARE(o->size(), QSize(900, 640));
        QTRY_VERIFY(o->model()->rowCount() == 0);
        QTest::qWait(300);
        QVERIFY(o->property("hn.role").toString() == "hyprnotes-organizer");
        QVERIFY(o->list()->viewport()->findChildren<QWidget *>().isEmpty());   // no per-row widgets
        QVERIFY(o->editorPane());
    }

    void search_is_debounced_and_superseded_results_are_ignored() {
        Lib l; fill(l, 30); l.write("zebra.md", "# Zebra\n\nstripes only\n");
        AppController c(l.opts());
        c.showOrganizer();
        auto *o = c.organizer();
        QTRY_COMPARE_WITH_TIMEOUT(o->model()->rowCount(), 31, 5000);
        QTest::qWait(600);                                    // let index sync/refresh settle
        const int base = o->searchCount();
        QTest::keyClicks(o->searchEdit(), "zebra");           // five keystrokes inside one 120 ms window
        QTest::qWait(50);
        QCOMPARE(o->searchCount(), base);                     // nothing issued while typing
        QTRY_COMPARE_WITH_TIMEOUT(o->searchCount(), base + 1, 1000);
        QTRY_COMPARE_WITH_TIMEOUT(o->model()->rowCount(), 1, 3000);
        // fire a query, then supersede it before it can be delivered: only the last one may show
        o->searchEdit()->setText("alpha");
        QTest::qWait(140);
        o->searchEdit()->setText("zebra");
        QTest::qWait(600);
        QCOMPARE(o->model()->rowCount(), 1);
        QCOMPARE(o->model()->index(0).data(NoteListModel::RelRole).toString(), QString("zebra.md"));
        QVERIFY(o->searchCount() <= base + 3);
        o->searchEdit()->clear();
        QTRY_COMPARE_WITH_TIMEOUT(o->model()->rowCount(), 31, 3000);
    }

    void organizerRefreshRebasesBeforePostSyncFetch() {
        Lib l; fill(l, 240);
        constexpr qint64 oldMtime = 1700000000000LL;
        constexpr qint64 newMtime = oldMtime + 60000;
        for (int i = 0; i < 240; ++i)
            writeAt(l, QString("note-%1.md").arg(i, 4, 10, QChar('0')), oldMtime);

        AppController c(l.opts());
        auto *idx = c.index();
        QSignalSpy syncs(idx, &hn::core::LibraryIndex::synced);
        QSignalSpy pages(idx, &hn::core::LibraryIndex::searchFinished);
        c.showOrganizer();
        auto *o = c.organizer();
        QTRY_COMPARE_WITH_TIMEOUT(syncs.count(), 1, 15000);
        QTRY_VERIFY_WITH_TIMEOUT(pages.count() >= 2, 15000);
        QTRY_COMPARE_WITH_TIMEOUT(o->model()->rowCount(), 100, 5000);

        QSignalSpy fetches(o->model(), &NoteListModel::fetchRequested);
        auto *bar = o->list()->verticalScrollBar();
        bar->setValue(bar->maximum() - 1);
        bar->setValue(bar->maximum());
        QTRY_COMPARE_WITH_TIMEOUT(o->model()->rowCount(), 200, 5000);
        QCOMPARE(fetches.count(), 1);
        QCOMPARE(fetches.first().at(0).toInt(), 100);
        hn::core::Worker::shared().waitIdle();

        // This controlled sync is the only post-fixture change.
        QObject::disconnect(&c.repo(), &hn::core::NoteRepository::libraryChanged, idx, nullptr);
        for (int i = 0; i < 40; ++i)
            writeAt(l, QString("new-%1.md").arg(i, 3, 10, QChar('0')), newMtime);
        const int pagesBeforeGeneration = pages.count();
        const int searchesBeforeGeneration = o->searchCount();
        QSignalSpy generationSync(idx, &hn::core::LibraryIndex::synced);

        // Hold a queued search after synchronous rail work has completed, so another
        // same-key search can replace it before the worker dequeues it.
        IndexSearchDequeueGate searchGate;
        const quint64 generation = idx->sync();
        QTRY_COMPARE_WITH_TIMEOUT(generationSync.count(), 1, 5000);
        QCOMPARE(generationSync.first().at(0).toULongLong(), generation);
        QCOMPARE(generationSync.first().at(1).toInt(), 40);

        QTRY_VERIFY_WITH_TIMEOUT(searchGate.takeEntered(), 5000);
        QCOMPARE(o->searchCount(), searchesBeforeGeneration + 1);

        // A real scrollbar fetch supersedes the queued refresh through the same key.
        bar->setValue(bar->maximum() - 1);
        bar->setValue(bar->maximum());
        QTRY_COMPARE_WITH_TIMEOUT(fetches.count(), 2, 3000);
        QCOMPARE(fetches.last().at(0).toInt(), 200);
        QCOMPARE(o->searchCount(), searchesBeforeGeneration + 2);

        searchGate.open();
        QTRY_VERIFY_WITH_TIMEOUT(pages.count() > pagesBeforeGeneration, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(o->model()->rowCount(), 280, 5000);

        const auto canonical = idx->searchNow({.limit = 500, .offset = 0});
        QCOMPARE(canonical.rows.size(), 280);
        QStringList actual;
        for (int row = 0; row < o->model()->rowCount(); ++row)
            actual << o->model()->index(row).data(NoteListModel::RelRole).toString();
        QSet<QString> unique;
        for (const auto &path : actual) unique.insert(path);
        QStringList expectedPrefix;
        for (qsizetype i = 0; i < actual.size() && i < canonical.rows.size(); ++i)
            expectedPrefix << canonical.rows.at(i).relPath;
        const bool canonicalPrefix = actual == expectedPrefix && actual.size() <= canonical.rows.size();
        QVERIFY2(unique.size() == actual.size() && canonicalPrefix,
                 qPrintable(QString("loaded=%1 unique=%2 canonical-prefix=%3")
                                .arg(actual.size()).arg(unique.size())
                                .arg(canonicalPrefix ? QStringLiteral("true") : QStringLiteral("false"))));
    }

    void pagination_100_and_row_cache_cap_500() {
        Lib l; fill(l, 620);
        AppController c(l.opts());
        QSignalSpy syncs(c.index(), &hn::core::LibraryIndex::synced);
        QSignalSpy pages(c.index(), &hn::core::LibraryIndex::searchFinished);
        c.showOrganizer();
        auto *o = c.organizer();
        QTRY_COMPARE_WITH_TIMEOUT(o->model()->rowCount(), 100, 15000);
        QTRY_COMPARE_WITH_TIMEOUT(syncs.count(), 1, 15000);
        QTRY_VERIFY_WITH_TIMEOUT(o->searchCount() >= 2, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(pages.count() >= 2, 5000);
        QCOMPARE(o->model()->rowCount(), 100);
        for (int want = 200; want <= 500; want += 100) {
            // The real path: the user scrolls the list to its end and QListView itself asks the model for the next page (QAbstractItemView
            // fetches when the scroll bar sits at its maximum). A debounced refresh from the index sync that is still running can supersede
            // a fetch in flight and resets the model (scroll position back to the top), so the user scrolls to the end again, as here.
            QTRY_VERIFY_WITH_TIMEOUT((o->list()->verticalScrollBar()->setValue(o->list()->verticalScrollBar()->maximum()), o->model()->rowCount() >= want), 15000);
            QCOMPARE(o->model()->rowCount(), want);
        }
        QVERIFY(!o->model()->canFetchMore({}));               // never more than 500 rows held
        QVERIFY(o->model()->truncated());
        QVERIFY(c.index()->cachedRows() <= 500);

        const auto canonical = c.index()->searchNow({.limit = 500, .offset = 0});
        QStringList actual;
        for (int row = 0; row < o->model()->rowCount(); ++row)
            actual << o->model()->index(row).data(NoteListModel::RelRole).toString();
        QSet<QString> unique;
        for (const auto &path : actual) unique.insert(path);
        QStringList expectedPrefix;
        for (qsizetype i = 0; i < actual.size() && i < canonical.rows.size(); ++i)
            expectedPrefix << canonical.rows.at(i).relPath;
        const bool canonicalPrefix = actual == expectedPrefix && actual.size() <= canonical.rows.size();
        QVERIFY2(unique.size() == actual.size() && canonicalPrefix,
                 qPrintable(QString("loaded=%1 unique=%2 canonical-prefix=%3")
                                .arg(actual.size()).arg(unique.size())
                                .arg(canonicalPrefix ? QStringLiteral("true") : QStringLiteral("false"))));
    }

    void organizer_list_shows_titles_tags_and_opens_notes() {
        Lib l; l.write("Work/plan.md", "# Plan\n\nship it #roadmap\n"); l.write("idea.md", "# Idea\n\nsomething #roadmap #x\n");
        AppController c(l.opts());
        c.showOrganizer();
        auto *o = c.organizer();
        QTRY_COMPARE_WITH_TIMEOUT(o->model()->rowCount(), 2, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(o->rail()->model()->rowCount() >= 5, 3000);   // headers + all + folder + tags
        o->setFilter({}, "roadmap");
        QTRY_COMPARE_WITH_TIMEOUT(o->model()->rowCount(), 2, 3000);
        o->setFilter("Work", {});
        QTRY_COMPARE_WITH_TIMEOUT(o->model()->rowCount(), 1, 3000);
        QCOMPARE(o->model()->index(0).data(NoteListModel::TitleRole).toString(), QString("Plan"));
        o->list()->setCurrentIndex(o->model()->index(0));
        QCOMPARE(c.organizerNote(), QString("Work/plan.md"));
    }

    void ten_stickies_and_organizer_smoke() {
        Lib l; fill(l, 12);
        AppController c(l.opts());
        c.showOrganizer();
        for (int i = 0; i < 10; ++i) QVERIFY(c.openSticky(QString("note-%1.md").arg(i, 4, 10, QChar('0')), false));
        c.openInOrganizer("note-0010.md");
        QCOMPARE(c.stickies().size(), 10);
        QCOMPARE(c.windowCount(), 11);
        for (auto *w : c.stickies()) { QVERIFY(w->isVisible()); typeText(w->session(), "!"); }
        QTRY_VERIFY_WITH_TIMEOUT(c.stickies().first()->session()->state() == NoteSession::State::Clean, 4000);
        QSet<QString> tokens;
        for (auto *w : c.stickies()) tokens.insert(w->token());
        QCOMPARE(tokens.size(), 10);                           // unique identity per window
        for (const auto &r : c.openNotes()) c.closeNote(r);
        QTRY_VERIFY_WITH_TIMEOUT(c.openNotes().isEmpty(), 6000);
        QVERIFY(c.stickies().isEmpty());
    }

    void sticky_toolbar_toggle_does_not_reflow() {
        Lib l; l.write("a.md", "# A\n\none two three four five six seven eight nine ten eleven twelve thirteen\n");
        AppController c(l.opts());
        auto *s = c.openSticky("a.md");
        auto *w = c.stickyOf("a.md");
        QTest::qWait(100);
        auto *ed = s->editor();
        const QPoint topBefore = ed->mapTo(w, QPoint(0, 0));
        const int width = ed->width();
        const QSizeF docBefore = ed->visualEdit()->document()->size();
        const QRect cursorBefore = ed->visualEdit()->cursorRect();
        QVERIFY(!w->toolbarShown());
        w->setToolbarShown(true);
        QTest::qWait(250);
        QVERIFY(w->toolbarShown());
        QCOMPARE(ed->mapTo(w, QPoint(0, 0)), topBefore);
        QCOMPARE(ed->width(), width);
        QCOMPARE(ed->visualEdit()->document()->size().width(), docBefore.width());
        QCOMPARE(ed->visualEdit()->cursorRect().topLeft(), cursorBefore.topLeft());
        QCOMPARE(w->header()->height(), 32);
    }

    void sticky_chip_toggles_workspace_mode() {
        Lib l; l.write("a.md", "# A\n");
        AppController c(l.opts());
        c.openSticky("a.md");
        auto *w = c.stickyOf("a.md");
        QCOMPARE(w->workspaceMode(), hn::platform::WorkspaceMode::ThisWorkspace);
        w->chip()->click();
        QCOMPARE(w->workspaceMode(), hn::platform::WorkspaceMode::AllWorkspaces);
        QCOMPARE(c.sessionState().windows.size(), 1);
        QCOMPARE(c.sessionState().windows[0].mode, hn::platform::WorkspaceMode::AllWorkspaces);
        w->chip()->click();
        QCOMPARE(w->workspaceMode(), hn::platform::WorkspaceMode::ThisWorkspace);
    }

    void sticky_buttons_pop_in_and_close() {
        Lib l; l.write("a.md", "# A\n"); l.write("b.md", "# B\n");
        AppController c(l.opts());
        c.openSticky("a.md");
        c.stickyOf("a.md")->popInButton()->click();
        QTRY_VERIFY(!c.stickyOf("a.md"));
        QCOMPARE(c.organizerNote(), QString("a.md"));
        c.openSticky("b.md");
        c.stickyOf("b.md")->closeButton()->click();
        QTRY_VERIFY(!c.session("b.md"));
    }

    void status_strip_states_and_source_reason() {
        Lib l; l.write("t.md", "| a | b |\n|---|---|\n| 1 | 2 |\n");
        AppController c(l.opts());
        auto *s = c.openSticky("t.md");
        auto *st = c.stickyOf("t.md")->status();
        QCOMPARE(st->labelText(), QString("Saved"));
        QVERIFY(!st->detailText().isEmpty());                  // source-mode reason is shown
        QVERIFY(!st->actionsVisible());
        typeText(s, "x");
        QCOMPARE(st->labelText(), QString("Unsaved"));
    }

    void reduce_motion_disables_fades() {
        Lib l; l.write("a.md", "# A\n");
        AppController c(l.opts());
        auto s = c.settings(); s.reduceMotion = true;
        c.applySettings(s, c.prefs());
        QCOMPARE(hn::app::ui::animation().duration(120), 0);
        s.reduceMotion = false;
        c.applySettings(s, c.prefs());
        QCOMPARE(hn::app::ui::animation().duration(120), 120);
    }

    void theme_switch_applies_live() {
        Lib l;
        AppController c(l.opts());
        auto s = c.settings(); s.colorScheme = "dark";
        c.applySettings(s, c.prefs());
        QVERIFY(c.theme().dark);
        QCOMPARE(hn::app::ui::theme().bg, QColor("#111111"));
        s.colorScheme = "light";
        c.applySettings(s, c.prefs());
        QVERIFY(!c.theme().dark);
        hn::theme::Config reread(l.cfg);
        QCOMPARE(reread.settings().colorScheme, QString("light"));
    }

    void tag_parse() {
        QCOMPARE(parseTagInput("#Work, ideas  #x_y bad!tag"), (QStringList{"work", "ideas", "x_y", "badtag"}));
    }
};

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    UiTest t;
    return QTest::qExec(&t, argc, argv);
}
#include "app_ui_test.moc"
