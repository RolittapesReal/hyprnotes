#include "app_test_util.h"
#include "ui_polish_test_util.h"
#include "panel_dock.h"
#include <QMenu>
#include <QScreen>
#include <QScrollBar>
#include <QToolButton>
#include <QLineEdit>

using namespace apptest;
namespace ui = hn::app::ui;

class AppPanelGeometryTest : public QObject {
    Q_OBJECT
private slots:
    void searchShortcutRevealsSearchFromCompactRail() {
        Lib lib; lib.write("a.md", "alpha\n"); lib.write("b.md", "beta\n");
        AppController c(lib.opts()); c.plugins().manager();
        c.plugins().hub()->showPanel("fixture:one", "One panel", {});
        auto pref = c.dockPrefs(); pref.width = 410; pref.visible = true; c.setDockPrefs(pref);
        c.showOrganizer(); auto *o = c.organizer();
        o->resize(720, 480); QTRY_COMPARE(o->size(), QSize(720, 480));
        QTRY_VERIFY(o->isActiveWindow());
        QTRY_COMPARE(o->model()->rowCount(), 2);
        o->searchEdit()->setText("alpha"); QTRY_COMPARE(o->model()->rowCount(), 1);
        auto *toggle = o->findChild<QAbstractButton *>("hnRailToggle"); QVERIFY(toggle && toggle->isVisible());
        toggle->setFocus(Qt::TabFocusReason); QTest::keyClick(toggle, Qt::Key_Space);
        QTRY_VERIFY(o->rail()->hasFocus());
        QVERIFY(o->rail()->isVisible()); QVERIFY(!o->searchEdit()->isVisible());
        const int dockWidth = o->dock()->width();
        QTest::keyClick(o->rail(), Qt::Key_F, Qt::ControlModifier);
        QTRY_VERIFY(o->searchEdit()->isVisible());
        QTRY_VERIFY(o->searchEdit()->hasFocus());
        QVERIFY(!o->rail()->isVisible()); QVERIFY(!toggle->isChecked());
        QCOMPARE(o->searchEdit()->selectedText(), QString("alpha"));
        QTest::keyClicks(o->searchEdit(), "beta");
        QCOMPARE(o->searchEdit()->text(), QString("beta"));
        QTRY_COMPARE(o->model()->rowCount(), 1);
        QTRY_COMPARE(o->model()->index(0).data(NoteListModel::RelRole).toString(), QString("b.md"));
        QVERIFY(o->list()->isVisible());
        QCOMPARE(o->size(), QSize(720, 480)); QCOMPARE(o->dock()->width(), dockWidth);
        QCOMPARE(c.dockPrefs().width, 410); QVERIFY(c.dockPrefs().visible);
    }
    void dockCloseAndTabsStayInsideOrganizer() {
        Lib lib; lib.write("A.md", "# A\n");
        auto options = lib.opts(); options.pluginHooks.consent = [](const ConsentRequest &) { return true; };
        AppController controller(options); controller.showOrganizer(); controller.openInOrganizer("A.md");
        auto *o = controller.organizer(); const QSize before = o->size();
        QString message;
        QVERIFY2(controller.plugins().install(QString(HN_SOURCE_DIR) + "/examples/plugins/backlinks-panel", nullptr, &message).ok, qPrintable(message));
        QTRY_VERIFY(o->dock());
        QCOMPARE(o->size(), before);
        for (int width : {720, 1100, 1920}) {
            o->resize(width, 740); QTRY_COMPARE(o->width(), width);
            auto *dock = o->dock(); QVERIFY(dock->isVisible());
            QTRY_VERIFY(o->rect().contains(inWidget(dock, o)));
            auto *editor = o->findChild<QWidget *>("hnEditorColumn"); QVERIFY(editor);
            QTRY_VERIFY(editor->width() >= 280);
            for (const QString &name : {QString("hnRailColumn"), QString("hnListColumn")}) {
                auto *column = o->findChild<QWidget *>(name);
                QVERIFY(column);
                if (column->isVisible()) {
                    QVERIFY(o->rect().contains(inWidget(column, o)));
                    QVERIFY(!inWidget(column, o).intersects(inWidget(editor, o)));
                }
            }
            QVERIFY(dock->rect().contains(dock->closeButton()->geometry()));
            QVERIFY(o->rect().contains(inWidget(dock->closeButton(), o)));
            QVERIFY(dock->rect().contains(inWidget(dock->view(), dock)));
            QVERIFY(dock->tabRect(0).isValid());
            QVERIFY(!dock->tabRect(0).intersects(dock->closeButton()->geometry()));
            auto tabFont = ui::labelFont(10); tabFont.setBold(true);
            QCOMPARE(QFontMetrics(tabFont).elidedText("BACKLINKS", Qt::ElideRight, dock->tabRect(0).width() - 24), QString("BACKLINKS"));
            if (width >= 1100) QVERIFY(dock->tabRect(0).width() >= QFontMetrics(ui::labelFont(10)).horizontalAdvance("BACKLINKS") + 24);
            if (const auto dir = qEnvironmentVariable("HN_SCREENSHOT_DIR"); !dir.isEmpty()) QVERIFY(o->grab().save(dir + QString("/dock-%1.png").arg(width)));
        }
        const auto size = o->size(); QTest::mouseClick(o->dock()->closeButton(), Qt::LeftButton);
        QTRY_VERIFY(!o->dockVisible()); QCOMPARE(o->size(), size);
    }
    void temporaryDockWidthPreservesPreferenceAndCompactRailAccess() {
        Lib lib; AppController c(lib.opts());
        c.plugins().manager();
        c.plugins().hub()->showPanel("fixture:one", "One panel", {});
        auto pref = c.dockPrefs(); pref.width = 410; pref.visible = true; c.setDockPrefs(pref);
        c.showOrganizer(); auto *o = c.organizer();
        o->resize(720, 480); QTRY_COMPARE(o->size(), QSize(720, 480));
        QTRY_VERIFY(o->dock()->width() < 410);
        QCOMPARE(c.dockPrefs().width, 410);
        auto *toggle = o->findChild<QAbstractButton *>("hnRailToggle"); QVERIFY(toggle && toggle->isVisible());
        const QImage image = toggle->grab().toImage(); int ink = 0;
        for (int y = 8; y < 24; ++y) for (int x = 8; x < 24; ++x) if (image.pixelColor(x, y).lightness() < 128) ++ink;
        QVERIFY2(ink > 10, "Compact rail toggle must paint a visible navigation icon");
        toggle->setFocus(Qt::TabFocusReason); QTest::keyClick(toggle, Qt::Key_Space);
        QTRY_VERIFY(o->rail()->isVisible());
        QVERIFY(!o->list()->isVisible());
        QTRY_VERIFY(o->rail()->hasFocus());
        QTest::keyClick(o->rail(), Qt::Key_Escape); QTRY_VERIFY(!o->rail()->isVisible());
        QTRY_VERIFY(toggle->hasFocus());
        for (bool show : {false, true}) { o->setDockVisible(show); QCOMPARE(o->width(), 720); QCOMPARE(c.dockPrefs().width, 410); }
        o->resize(1100, 740); QTRY_COMPARE(o->dock()->width(), 410);
        QVERIFY(!toggle->isVisible());
        AppController restored(lib.opts()); QCOMPARE(restored.dockPrefs().width, 410); QVERIFY(restored.dockPrefs().visible);
    }
    void manyLongTabsUseKeyboardAccessibleOverflow_data() {
        QTest::addColumn<int>("fontSize"); QTest::addColumn<int>("width");
        for (int font : {14, 32}) for (int width : {260, 360, 640})
            QTest::newRow(qPrintable(QString("%1-%2").arg(font).arg(width))) << font << width;
    }
    void manyLongTabsUseKeyboardAccessibleOverflow() {
        QFETCH(int, fontSize); QFETCH(int, width);
        Lib lib; AppController c(lib.opts()); c.plugins().manager(); auto *hub = c.plugins().hub();
        auto settings = c.settings(); settings.fontSize = fontSize; c.applySettings(settings, c.prefs());
        for (int i = 0; i < 6; ++i) hub->showPanel(QString("fixture:%1").arg(i), QString("A long descriptive panel title %1").arg(i), {});
        PanelDock dock(&c); dock.resize(width, 300); dock.show();
        QTRY_COMPARE(dock.styleSheet(), hn::theme::styleSheetFor(c.theme()));
        for (int i = 0; i < 6; ++i) {
            dock.setCurrentPanel(QString("fixture:%1").arg(i));
            const QRect current = dock.tabRect(i);
            QVERIFY2(current.isValid(), "Selected panel must remain visible in overflow");
            QVERIFY(dock.rect().contains(current));
            QVERIFY(!current.intersects(dock.closeButton()->geometry()));
            for (int j = 0; j < 6; ++j) if (j != i && dock.tabRect(j).isValid()) QVERIFY(!current.intersects(dock.tabRect(j)));
        }
        auto *overflow = dock.findChild<QToolButton *>("hnPanelOverflow");
        QVERIFY(overflow && overflow->isVisible() && overflow->focusPolicy() != Qt::NoFocus);
        QVERIFY(dock.rect().contains(overflow->geometry()));
        QVERIFY(!overflow->geometry().intersects(dock.closeButton()->geometry()));
        QVERIFY(!overflow->accessibleName().isEmpty());
        QVERIFY(overflow->menu()); QCOMPARE(overflow->menu()->actions().size(), 6);
        overflow->menu()->actions().first()->trigger(); QCOMPARE(dock.currentPanel(), QString("fixture:0"));
        dock.view()->setFocus(); QTest::keyClick(dock.view(), Qt::Key_Right); QCOMPARE(dock.currentPanel(), QString("fixture:1"));
    }
    void panelRowsAndMarkdownRethemeWithoutNewContent() {
        auto theme = hn::theme::loadTheme("modernist", false); theme.accent = QColor("#eb0000"); ui::setTheme(theme);
        PanelView view; view.resize(280, 400); view.show();
        hn::plugins::PanelBlock text; text.type = "text"; text.text = "Several words wrap across the panel and must relayout after a live font change.";
        hn::plugins::PanelBlock markdown; markdown.type = "markdown"; markdown.text = "[Live link](https://example.org)\n\nMore markdown text for line wrapping.";
        view.setContent({text, markdown}, {});
        const int before = view.rowRect(0).height(), markdownBefore = view.rowRect(1).height();
        theme.baseSize = 32; theme.accent = QColor("#008000"); ui::setTheme(theme);
        QTRY_VERIFY(view.rowRect(0).height() > before);
        QTRY_VERIFY(view.rowRect(1).height() > markdownBefore);
        view.verticalScrollBar()->setValue(view.rowRect(0).height());
        const QImage image = view.viewport()->grab().toImage(); int green = 0, red = 0;
        for (int y = 0; y < image.height(); ++y) for (int x = 0; x < image.width(); ++x) {
            const auto color = image.pixelColor(x, y);
            if (color.green() > color.red() + 40 && color.green() > color.blue() + 40) ++green;
            if (color.red() > color.green() + 80 && color.red() > color.blue() + 80) ++red;
        }
        QVERIFY2(green > 10, "Markdown links must paint with current accent"); QCOMPARE(red, 0);
        theme.accent = QColor("#0000e0"); ui::setTheme(theme); // Same font and width: color alone must invalidate cached documents.
        const QImage changed = view.viewport()->grab().toImage(); int blue = 0;
        for (int y = 0; y < changed.height(); ++y) for (int x = 0; x < changed.width(); ++x) {
            const auto color = changed.pixelColor(x, y);
            if (color.blue() > color.red() + 80 && color.blue() > color.green() + 80) ++blue;
        }
        QVERIFY(blue > 10);
    }
    void stickyPanelPopupStaysOnScreen() {
        Lib lib; lib.write("A.md", "# A\n"); AppController c(lib.opts());
        c.plugins().manager();
        c.plugins().hub()->showPanel("fixture:one", "Panel", {});
        c.openSticky("A.md"); auto *w = c.stickyOf("A.md"); w->resize(260, 180);
        const QRect bounds = w->screen()->availableGeometry(); w->move(bounds.right() - 60, bounds.bottom() - 60);
        auto *popup = w->openPanels(); QVERIFY(popup);
        QVERIFY(bounds.contains(popup->geometry()));
        QVERIFY(popup->rect().contains(popup->closeButton()->geometry()));
        QVERIFY(popup->rect().contains(popup->view()->geometry())); popup->close();
    }
};
int main(int argc, char **argv) {
    PolishProfile profile; qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv); app.setQuitOnLastWindowClosed(false);
    AppPanelGeometryTest test; return QTest::qExec(&test, argc, argv);
}
#include "app_panel_geometry_test.moc"
