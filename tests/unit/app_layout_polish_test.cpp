#include "app_test_util.h"
#include "ui_polish_test_util.h"
#include "status_strip.h"
#include "action_row.h"
#include "command_palette.h"
#include <QScreen>
#include <QFontMetrics>
#include <QLineEdit>
#include <QScrollArea>
#include <QScrollBar>
#include <QToolButton>
#include <unistd.h>

using namespace apptest;
namespace ui = hn::app::ui;

class AppLayoutPolishTest : public QObject {
    Q_OBJECT
    hn::theme::Theme m_savedTheme;
    void checkActions(StickyWindow *window, const QStringList &names) {
        auto *status = window->status();
        auto *scroll = status->findChild<QScrollArea *>();
        QList<QRect> occupied;
        for (const auto &name : names) {
            auto *button = status->button(name);
            QVERIFY(button && button->isVisible());
            const QRect r = inWidget(button, button->parentWidget());
            for (const QRect &other : occupied) QVERIFY(!r.intersects(other));
            occupied << r;
            const int needed = QFontMetrics(button->font()).boundingRect(
                QRect(0, 0, button->width() - 24, 10000), Qt::TextWordWrap, button->text()).height();
            QVERIFY2(button->height() >= needed + 8, qPrintable(name + " clipped text"));
            button->setFocus(Qt::TabFocusReason);
            if (scroll) QTRY_VERIFY(scroll->viewport()->rect().contains(inWidget(button, scroll->viewport())));
            QTRY_VERIFY(status->rect().contains(inWidget(button, status)));
            QTRY_VERIFY(window->rect().contains(inWidget(button, window)));
        }
        QVERIFY(window->rect().contains(inWidget(status, window)));
        status->button(names.first())->setFocus(Qt::TabFocusReason);
        for (int i = 1; i < names.size(); ++i) {
            QTest::keyClick(status->button(names[i - 1]), Qt::Key_Tab);
            QTRY_VERIFY(status->button(names[i])->hasFocus());
            if (scroll) QTRY_VERIFY(scroll->viewport()->rect().contains(inWidget(status->button(names[i]), scroll->viewport())));
        }
    }
private slots:
    void initTestCase() {
        const auto theme = hn::theme::loadTheme("modernist", false);
        ui::setTheme(theme); hn::theme::applyTheme(theme);
    }
    void init() { m_savedTheme = ui::theme(); }
    void cleanup() { ui::setTheme(m_savedTheme); hn::theme::applyTheme(m_savedTheme); }
    void shortcutSheetSeparatesLabelsAndKeycaps_data() {
        QTest::addColumn<int>("fontSize"); QTest::addColumn<QSize>("size");
        for (int font : {14, 32}) for (QSize size : {QSize(260, 180), QSize(360, 300), QSize(720, 480)})
            QTest::newRow(qPrintable(QString("%1-%2").arg(font).arg(size.width()))) << font << size;
    }
    void shortcutSheetSeparatesLabelsAndKeycaps() {
        QFETCH(int, fontSize); QFETCH(QSize, size);
        auto theme = hn::theme::loadTheme("modernist", false); theme.baseSize = fontSize;
        ui::setTheme(theme); hn::theme::applyTheme(theme);
        QWidget host; host.resize(size);
        QLineEdit previous(&host); previous.setGeometry(4, 4, 120, 32);
        host.show(); host.activateWindow(); previous.setFocus(); QTRY_VERIFY(previous.hasFocus());
        ui::ShortcutSheet::toggle(&host,
            {{"toggle-organizer", QKeySequence("Ctrl+Shift+O")}, {"command-palette", QKeySequence("Ctrl+Shift+P")}},
            {{"Run a plugin operation with a deliberately long descriptive action name", QKeySequence("Ctrl+Alt+Shift+Meta+P, Ctrl+Alt+Shift+Meta+O")}});
        auto *sheet = ui::ShortcutSheet::openOn(&host); QVERIFY(sheet);
        QVERIFY(!sheet->titleRect().intersects(sheet->closeHintRect()));
        QCOMPARE(sheet->rowGeometries().size(), sheet->rows().size());
        QList<QRect> occupied;
        int rowIndex = 0;
        for (const auto &row : sheet->rowGeometries()) {
            const int needed = QFontMetrics(sheet->font()).boundingRect(QRect(0, 0, row.label.width(), 10000),
                Qt::TextWordWrap, sheet->rows()[rowIndex].label).height();
            QVERIFY(row.label.height() >= needed);
            int keyIndex = 0;
            for (const QRect &key : row.keys) {
                const QString text = sheet->rows()[rowIndex].keys[keyIndex++];
                if (QStringList{"Shift", "F1", "F2", "Esc", "Space"}.contains(text))
                    QVERIFY2(key.height() <= QFontMetrics(ui::uiFont(qMax(11, fontSize - 8), QFont::DemiBold)).height() + 8,
                             qPrintable(text + " must not wrap when its natural width fits"));
                QVERIFY(!row.label.intersects(key));
                QVERIFY(key.left() >= 0 && key.right() < sheet->width());
                for (const QRect &other : occupied) QVERIFY(!key.intersects(other));
                occupied << key;
            }
            for (const QRect &other : occupied) QVERIFY(!row.label.intersects(other));
            occupied << row.label;
            QVERIFY(row.label.left() >= 0 && row.label.right() < sheet->width());
            ++rowIndex;
        }
        if (size.width() < 720) QVERIFY(sheet->maximumScroll() > 0);
        QTest::keyClick(sheet, Qt::Key_End); QCOMPARE(sheet->scrollOffset(), sheet->maximumScroll());
        const auto last = sheet->rowGeometries().last();
        QVERIFY2(sheet->rect().contains(last.label), qPrintable(QString("last %1,%2 %3x%4; scroll %5; sheet %6x%7")
            .arg(last.label.x()).arg(last.label.y()).arg(last.label.width()).arg(last.label.height()).arg(sheet->scrollOffset()).arg(sheet->width()).arg(sheet->height())));
        for (const auto &key : last.keys) QVERIFY(sheet->rect().contains(key));
        QTest::keyClick(sheet, Qt::Key_Home); QCOMPARE(sheet->scrollOffset(), 0);
        if (const auto dir = qEnvironmentVariable("HN_SCREENSHOT_DIR"); !dir.isEmpty())
            QVERIFY(sheet->grab().save(dir + QString("/help-%1-%2.png").arg(fontSize).arg(size.width())));
        QTest::keyClick(sheet, Qt::Key_Tab); QVERIFY(ui::ShortcutSheet::isOpen(&host));
        QTest::keyClick(sheet, Qt::Key_A, Qt::ControlModifier); QVERIFY(ui::ShortcutSheet::isOpen(&host));
        QPointer<QWidget> guard(sheet); QTest::keyClick(sheet, Qt::Key_Escape); QTRY_VERIFY(guard.isNull());
        QTRY_VERIFY(previous.hasFocus());
        ui::ShortcutSheet::toggle(&host, {});
        guard = ui::ShortcutSheet::openOn(&host);
        QTest::mouseClick(guard, Qt::LeftButton); QTRY_VERIFY(guard.isNull());
    }
    void commandPaletteFitsTinyHostAndRethemesSearch() {
        auto theme = hn::theme::loadTheme("modernist", false); ui::setTheme(theme); hn::theme::applyTheme(theme);
        QWidget host; host.resize(260, 180); host.show();
        QList<hn::app::CommandPalette::Item> items;
        for (int i = 0; i < 20; ++i) items << hn::app::CommandPalette::Item{QString::number(i), QString("Command %1").arg(i), "Plugin", "Ctrl+P"};
        QString selected;
        auto *palette = hn::app::CommandPalette::toggle(&host, items, [&](const QString &id) { selected = id; });
        QTRY_VERIFY(host.rect().contains(palette->geometry()));
        QTest::keyClick(palette->input(), Qt::Key_PageDown);
        QVERIFY(palette->currentId() != "0");
        theme = hn::theme::loadTheme("dracula", true); theme.baseSize = 32;
        ui::setTheme(theme); hn::theme::applyTheme(theme);
        QTRY_COMPARE(host.styleSheet(), hn::theme::styleSheetFor(theme));
        QTRY_COMPARE(palette->input()->palette().color(QPalette::Text), theme.text);
        QTRY_VERIFY(host.rect().contains(palette->geometry()));
        QVERIFY(palette->input()->height() >= palette->input()->fontMetrics().height() + 8);
        if (const auto dir = qEnvironmentVariable("HN_SCREENSHOT_DIR"); !dir.isEmpty()) QVERIFY(host.grab().save(dir + "/palette-260-32.png"));
        const auto expected = palette->currentId();
        QTest::keyClick(palette->input(), Qt::Key_Return); QCOMPARE(selected, expected);
    }
    void swatchPopupClampsToScreenAndKeepsKeyboardChoice() {
        QWidget host; host.show();
        auto *popup = new ui::SwatchPopover(0, &host); QSignalSpy picked(popup, &ui::SwatchPopover::picked);
        const QRect screen = host.screen()->availableGeometry();
        popup->popup(screen.bottomRight() - QPoint(4, 4));
        QVERIFY(screen.contains(popup->geometry()));
        QTest::keyClick(popup, Qt::Key_Right); QTest::keyClick(popup, Qt::Key_Return);
        QCOMPARE(picked.count(), 1); QCOMPARE(picked.first().first().toInt(), 1);
    }
    void wrappingActionsInvalidateAndKeepStyleStatesReadable() {
        auto theme = hn::theme::loadTheme("modernist", false);
        hn::theme::applyTheme(theme); ui::setTheme(theme);
        ui::ActionRow row;
        auto *first = new ui::WrappingButton("Save mine as copy");
        auto *second = new ui::WrappingButton("Replace disk");
        row.addButton(first); row.addButton(second);
        row.resize(220, 240); row.show();
        QTRY_COMPARE(row.styleSheet(), hn::theme::styleSheetFor(theme));
        QTRY_VERIFY(!first->geometry().intersects(second->geometry()));
        second->hide();
        QTRY_COMPARE(row.heightForWidth(220), first->heightForWidth(220));
        first->setText("Save my considerably longer unsaved note as a separate local copy");
        QFont large = first->font(); large.setPixelSize(32); first->setFont(large);
        QTRY_COMPARE(first->height(), first->heightForWidth(first->width()));
        QVERIFY(first->height() > 80);
        first->setText("Readable");
        row.resize(220, 240);
        QTRY_COMPARE(first->height(), first->heightForWidth(first->width()));
        QTest::mouseMove(first, first->rect().center());
        const QImage hovered = first->grab().toImage();
        int lightInk = 0;
        for (int y = 8; y < hovered.height() - 8; ++y) for (int x = 12; x < hovered.width() - 12; ++x)
            if (hovered.pixelColor(x, y).lightness() > 220) ++lightInk;
        QVERIFY2(lightInk > 25, "Hovered wrapped button must retain contrasting text");
        second->show();
        QTRY_VERIFY(!first->geometry().intersects(second->geometry()));
    }
    void failedActionsRemainReachableAndRetrySaves() {
        if (geteuid() == 0) QSKIP("root ignores directory permissions");
        Lib lib; lib.write("a.md", "original\n");
        AppController c(lib.opts()); auto *s = c.openSticky("a.md"); s->setTiming({100, 300});
        QVERIFY(QFile::setPermissions(lib.notes, QFileDevice::ReadOwner | QFileDevice::ExeOwner));
        typeText(s, "local"); QTRY_COMPARE(s->state(), NoteSession::State::Failed);
        auto *w = c.stickyOf("a.md"); w->resize(260, 180);
        checkActions(w, {"retry", "discard"});
        QVERIFY(QFile::setPermissions(lib.notes, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        w->status()->button("retry")->click(); QTRY_COMPARE(s->state(), NoteSession::State::Clean);
        QVERIFY(lib.read("a.md").contains("local"));
        QTRY_VERIFY(w->status()->height() <= 40);
    }
    void conflictChoicesFitAtNarrowWidths_data() {
        QTest::addColumn<int>("fontSize");
        QTest::addColumn<QSize>("size");
        for (int font : {14, 18, 32}) for (QSize size : {QSize(260, 180), QSize(260, 300), QSize(360, 300), QSize(560, 480)})
            QTest::newRow(qPrintable(QString("%1-%2x%3").arg(font).arg(size.width()).arg(size.height()))) << font << size;
    }
    void conflictChoicesFitAtNarrowWidths() {
        QFETCH(int, fontSize); QFETCH(QSize, size);
        Lib lib; lib.write("a.md", "original\n");
        AppController controller(lib.opts());
        auto settings = controller.settings(); settings.fontSize = fontSize;
        controller.applySettings(settings, controller.prefs());
        auto *session = controller.openSticky("a.md");
        QVERIFY(session);
        session->setTiming({60000, 60000});
        session->editor()->setMode(hn::editor::Mode::Source);
        QTest::keyClicks(session->editor()->sourceEdit(), "local ");
        lib.write("a.md", "external\n"); controller.repo().reconcileNow();
        QTRY_COMPARE_WITH_TIMEOUT(session->state(), NoteSession::State::Conflict, 8000);
        auto *window = controller.stickyOf("a.md");
        window->resize(size);
        QTRY_COMPARE(window->size(), size);
        QTRY_COMPARE(window->styleSheet(), hn::theme::styleSheetFor(controller.theme()));
        checkActions(window, {"reload", "copy", "replace"});
        if (const auto dir = qEnvironmentVariable("HN_SCREENSHOT_DIR"); !dir.isEmpty())
            QVERIFY(window->grab().save(dir + QString("/conflict-%1-%2x%3.png").arg(fontSize).arg(size.width()).arg(size.height())));
        for (auto *a : window->status()->findChildren<QLabel *>())
            for (auto *b : window->status()->findChildren<QLabel *>())
                if (a != b && a->isVisible() && b->isVisible()) QVERIFY(!a->geometry().intersects(b->geometry()));
        auto *status = window->status();
        QCOMPARE(status->button("reload")->text(), QString("Reload from disk"));
        QCOMPARE(status->button("copy")->text(), QString("Save mine as copy"));
        QCOMPARE(status->button("replace")->text(), QString("Replace disk"));
        status->button("copy")->setFocus(Qt::TabFocusReason);
        QTest::keyClick(status->button("copy"), Qt::Key_Space);
        QTRY_COMPARE(session->state(), NoteSession::State::Clean);
        QCOMPARE(lib.read("a.md"), QByteArray("external\n"));
        QVERIFY(lib.read("a (local copy).md").contains("local"));
        QTRY_VERIFY(!status->actionsVisible());
        QVERIFY(status->height() < 100);
    }
    void removedActionsFitAndCollapseAfterRecreate() {
        Lib lib; lib.write("a.md", "original\n");
        AppController controller(lib.opts());
        auto *s = controller.openSticky("a.md"); s->setTiming({60000, 60000});
        typeText(s, "local");
        QVERIFY(QFile::remove(lib.notes + "/a.md")); controller.repo().reconcileNow();
        QTRY_COMPARE(s->state(), NoteSession::State::Removed);
        auto *w = controller.stickyOf("a.md"); w->resize(260, 180);
        QTRY_COMPARE(w->size(), QSize(260, 180));
        checkActions(w, {"recreate", "discard"});
        w->status()->button("recreate")->click();
        QTRY_COMPARE(s->state(), NoteSession::State::Clean);
        QTRY_VERIFY(w->status()->height() <= 40);
    }
    void organizerColumnsHonorRequestedWidth_data() {
        QTest::addColumn<QSize>("size");
        for (QSize s : {QSize(720, 480), QSize(900, 640), QSize(1100, 740)})
            QTest::newRow(qPrintable(QString::number(s.width()))) << s;
    }
    void organizerColumnsHonorRequestedWidth() {
        QFETCH(QSize, size);
        Lib lib; lib.write("a.md", "alpha\n"); lib.write("b.md", "beta\n");
        AppController c(lib.opts()); c.showOrganizer(); auto *o = c.organizer();
        for (bool selected : {false, true}) {
            if (selected) c.openInOrganizer("a.md");
            o->resize(size); QTRY_COMPARE(o->size(), size);
            auto *rail = o->findChild<QWidget *>("hnRailColumn");
            auto *list = o->findChild<QWidget *>("hnListColumn");
            auto *editor = o->findChild<QWidget *>("hnEditorColumn");
            QVERIFY(rail && list && editor);
            for (auto *column : {rail, list, editor}) QVERIFY(o->rect().contains(inWidget(column, o)));
            QVERIFY(!inWidget(rail, o).intersects(inWidget(list, o)));
            QVERIFY(!inWidget(list, o).intersects(inWidget(editor, o)));
            QTRY_VERIFY(editor->width() >= 280);
            o->searchEdit()->setText("beta");
            QTRY_COMPARE(o->model()->rowCount(), 1);
            QVERIFY(list->rect().contains(inWidget(o->searchEdit(), list)));
            auto *clear = o->searchEdit()->findChild<QAbstractButton *>();
            QVERIFY(clear && clear->isVisible());
            QVERIFY(o->searchEdit()->rect().contains(clear->geometry()));
            QTest::mouseClick(clear, Qt::LeftButton);
            QTRY_VERIFY(o->searchEdit()->text().isEmpty());
            QTRY_COMPARE(o->model()->rowCount(), 2);
        }
    }
    void compactEmptyStateKeepsTextAndActionsInside_data() {
        QTest::addColumn<int>("fontSize");
        QTest::newRow("14") << 14;
        QTest::newRow("32") << 32;
    }
    void compactEmptyStateKeepsTextAndActionsInside() {
        QFETCH(int, fontSize);
        auto theme = hn::theme::loadTheme("modernist", false); theme.baseSize = fontSize;
        ui::setTheme(theme); hn::theme::applyTheme(theme);
        ui::EmptyState empty;
        empty.setContent(ui::Art::Note, "A long title for a small pane", "Keep this useful explanation readable.", "Create a new note", "Choose another folder");
        empty.resize(184, 400); empty.show(); empty.activateWindow();
        QTRY_COMPARE(empty.size(), QSize(184, 400));
        QTRY_COMPARE(empty.styleSheet(), hn::theme::styleSheetFor(theme));
        auto *scroll = empty.findChild<QScrollArea *>();
        QWidget *content = scroll ? scroll->widget() : &empty;
        QWidget *viewport = scroll ? scroll->viewport() : &empty;
        for (auto *child : content->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly))
            if (child->isVisible()) QVERIFY2(content->rect().contains(child->geometry()), qPrintable(child->metaObject()->className()));
        const auto labels = empty.findChildren<QLabel *>();
        QCOMPARE(labels.size(), 2);
        QCOMPARE(labels[0]->text(), QString("A long title for a small pane"));
        QCOMPARE(labels[1]->text(), QString("Keep this useful explanation readable."));
        QCOMPARE(labels[0]->font().pixelSize(), fontSize + 4);
        QCOMPARE(labels[1]->font().pixelSize(), qMax(13, fontSize - 1));
        for (auto *label : labels) {
            QVERIFY(label->wordWrap());
            QVERIFY(label->height() >= label->heightForWidth(label->width()));
        }
        const auto buttons = empty.findChildren<QPushButton *>();
        QCOMPARE(buttons.size(), 2);
        for (auto *button : buttons) {
            QVERIFY(button->isVisible());
            QCOMPARE(button->font().pixelSize(), fontSize);
            QVERIFY(button->height() >= button->heightForWidth(button->width()));
        }
        if (fontSize == 32) {
            QVERIFY(scroll);
            QVERIFY(empty.rect().contains(inWidget(scroll, &empty)));
            QTRY_VERIFY(scroll->verticalScrollBar()->maximum() > 0);
            QCOMPARE(scroll->horizontalScrollBar()->maximum(), 0);
            scroll->setFocus(Qt::TabFocusReason);
            QTRY_VERIFY(scroll->hasFocus());
            QTest::keyClick(scroll, Qt::Key_PageDown);
            QCOMPARE(scroll->verticalScrollBar()->value(), scroll->verticalScrollBar()->maximum());
            QTest::keyClick(scroll, Qt::Key_PageUp);
            QCOMPARE(scroll->verticalScrollBar()->value(), 0);
            QVERIFY(viewport->rect().contains(inWidget(labels.first(), viewport)));
        } else if (scroll) QCOMPARE(scroll->verticalScrollBar()->maximum(), 0);
        buttons.first()->setFocus(Qt::TabFocusReason);
        QTRY_VERIFY(buttons.first()->hasFocus());
        QTRY_VERIFY(viewport->rect().contains(inWidget(buttons.first(), viewport)));
        QTest::keyClick(buttons.first(), Qt::Key_Tab);
        QTRY_VERIFY(buttons.last()->hasFocus());
        QTRY_VERIFY(viewport->rect().contains(inWidget(buttons.last(), viewport)));
        QSignalSpy primary(&empty, &ui::EmptyState::primaryClicked), secondary(&empty, &ui::EmptyState::secondaryClicked);
        QTest::keyClick(buttons.last(), Qt::Key_Space); QCOMPARE(secondary.count(), 1);
        QTest::keyClick(buttons.last(), Qt::Key_Backtab);
        QTRY_VERIFY(buttons.first()->hasFocus());
        QTRY_VERIFY(viewport->rect().contains(inWidget(buttons.first(), viewport)));
        QTest::keyClick(buttons.first(), Qt::Key_Space); QCOMPARE(primary.count(), 1);
        if (scroll) {
            empty.resize(184, 1200); QTRY_COMPARE(scroll->verticalScrollBar()->maximum(), 0);
            empty.resize(184, 400);
            if (fontSize == 32) QTRY_VERIFY(scroll->verticalScrollBar()->maximum() > 0);
            theme.baseSize = 14; ui::setTheme(theme); hn::theme::applyTheme(theme);
            QTRY_COMPARE(scroll->verticalScrollBar()->maximum(), 0);
        }
    }
    void stickyKeyboardFormattingRevealsAndFocusesToolbar() {
        Lib lib; lib.write("a.md", "hello world\n");
        AppController c(lib.opts()); auto *s = c.openSticky("a.md"); auto *w = c.stickyOf("a.md");
        ui::IconButton *format = nullptr;
        for (auto *button : w->findChildren<ui::IconButton *>())
            if (button->accessibleName() == "Formatting toolbar") format = button;
        QVERIFY(format);
        QTRY_VERIFY(w->isActiveWindow());
        format->setFocus(Qt::TabFocusReason); QTRY_VERIFY(format->hasFocus());
        QTest::keyClick(format, Qt::Key_Space);
        QVERIFY(w->toolbarShown());
        QVERIFY(s->toolbar()->isAncestorOf(QApplication::focusWidget()));
    }
};
int main(int argc, char **argv) {
    PolishProfile profile;
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv); app.setQuitOnLastWindowClosed(false);
    AppLayoutPolishTest test; return QTest::qExec(&test, argc, argv);
}
#include "app_layout_polish_test.moc"
