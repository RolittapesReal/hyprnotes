#include "app_test_util.h"
#include "ui_polish_test_util.h"
#include "ui_common.h"
#include "settings_dialog.h"
#include <QCheckBox>
#include <QTabWidget>
#include <QElapsedTimer>
#include <QEvent>
#include <QSignalSpy>

namespace ui = hn::app::ui;
struct PaintCounter : QObject {
    int paints = 0;
    bool eventFilter(QObject *, QEvent *event) override {
        if (event->type() == QEvent::Paint) ++paints;
        return false;
    }
};

class AppMotionPolishTest : public QObject {
    Q_OBJECT
private slots:
    void cleanup() { ui::setReducedMotion(false); }
    void snapshotDissolvesAndResizeCancels() {
        QWidget target; target.resize(200, 100);
        target.setAutoFillBackground(true);
        auto palette = target.palette();
        palette.setColor(QPalette::Window, Qt::red); target.setPalette(palette);
        target.show();
        ui::FadeOverlay fade(&target);
        const auto previous = fade.snapshot();
        QVERIFY(!previous.isNull());
        palette.setColor(QPalette::Window, Qt::blue); target.setPalette(palette);
        fade.play(previous);
        QCOMPARE(target.grab().toImage().pixelColor(100, 50), QColor(Qt::red));
        QTest::qWait(40);
        const auto middle = target.grab().toImage().pixelColor(100, 50);
        QVERIFY(middle.red() > 0 && middle.blue() > 0);
        QTRY_VERIFY_WITH_TIMEOUT(!fade.running(), 600);
        QCOMPARE(target.grab().toImage().pixelColor(100, 50), QColor(Qt::blue));
        fade.play(previous);
        QVERIFY(fade.running());
        target.resize(240, 120);
        QVERIFY(!fade.running());
        QVERIFY(!fade.isVisible());
        ui::setReducedMotion(true);
        QVERIFY(fade.snapshot().isNull());
    }
    void settingsPagesFadeWithoutDelayingNavigation() {
        apptest::Lib lib;
        hn::app::AppController controller(lib.opts());
        hn::app::SettingsDialog dialog(&controller);
        dialog.show();
        QCoreApplication::processEvents();
        auto *tabs = dialog.tabs();
        auto *page = tabs->currentWidget();
        const QImage previous = page->grab().toImage();
        tabs->setCurrentIndex(1);
        QCOMPARE(tabs->currentIndex(), 1);
        auto *fade = dialog.findChild<ui::FadeOverlay *>();
        QVERIFY(fade);
        QVERIFY(fade->running());
        QVERIFY(fade->testAttribute(Qt::WA_TransparentForMouseEvents));
        QCOMPARE(fade->grab().toImage(), previous);
        tabs->setCurrentIndex(2); // rapid switches immediately select the latest page
        tabs->setCurrentIndex(0);
        QCOMPARE(tabs->currentWidget(), page);
        QVERIFY(fade->running());
        QTRY_VERIFY_WITH_TIMEOUT(!fade->running(), 600);
        QVERIFY(!fade->isVisible());
        tabs->setCurrentIndex(1);
        QVERIFY(fade->running());
        dialog.hide();
        QVERIFY(!fade->running());
        dialog.show();
        QVERIFY(!fade->isVisible());
        ui::setReducedMotion(true);
        tabs->setCurrentIndex(2);
        QVERIFY(!fade->running());
        QVERIFY(!fade->isVisible());
    }
    void noteSwitchDissolvesPreviousViewAndKeepsEditorLive() {
        apptest::Lib lib;
        lib.write("a.md", "# Alpha\n\nFirst note\n");
        lib.write("b.md", "# Beta\n\nSecond note\n");
        hn::app::AppController controller(lib.opts());
        controller.showOrganizer();
        controller.openInOrganizer("a.md");
        auto *organizer = controller.organizer();
        auto *fade = organizer->findChild<ui::FadeOverlay *>();
        QVERIFY(fade);
        QTRY_VERIFY_WITH_TIMEOUT(!fade->running(), 600);
        QCoreApplication::processEvents();
        const QImage previous = organizer->editorPane()->grab().toImage();
        auto *next = controller.openInOrganizer("b.md");
        QVERIFY(next);
        QCOMPARE(organizer->currentSession(), next);
        QVERIFY(fade->running());
        QCOMPARE(fade->grab().toImage(), previous);
        apptest::typeText(next, "Typed during transition");
        QVERIFY(next->editor()->toMarkdownBytes().contains("Typed during transition"));
        controller.openInOrganizer("a.md");
        QCOMPARE(organizer->currentSession()->rel(), QString("a.md"));
        QTRY_VERIFY_WITH_TIMEOUT(!fade->running(), 600);
        QVERIFY(!fade->isVisible());
        QTRY_VERIFY(lib.read("b.md").contains("Typed during transition"));
        auto settings = controller.settings(); settings.reduceMotion = true;
        controller.applySettings(settings, controller.prefs());
        controller.openInOrganizer("b.md");
        QVERIFY(!fade->running());
        QVERIFY(!fade->isVisible());
    }
    void reducedMotionCancelsAnExistingFade() {
        QWidget target; target.resize(200, 100); target.show();
        ui::FadeOverlay fade(&target);
        PaintCounter counter; fade.installEventFilter(&counter);
        ui::setReducedMotion(false);
        fade.play();
        QVERIFY(fade.running());
        QVERIFY(fade.isVisible());
        ui::setReducedMotion(true);
        QVERIFY(!fade.running());
        QVERIFY(!fade.isVisible());
        fade.play();
        QVERIFY(!fade.running());
        QTest::qWait(180);
        const int settled = counter.paints;
        QTest::qWait(240);
        QCOMPARE(counter.paints, settled);
    }
    void controllerPolicyCancelsWithoutAnotherPlay() {
        apptest::Lib lib;
        hn::app::AppController controller(lib.opts());
        QWidget target; target.resize(200, 100); target.show();
        ui::FadeOverlay fade(&target);
        fade.play();
        QVERIFY(fade.running());
        auto settings = controller.settings(); settings.reduceMotion = true;
        controller.applySettings(settings, controller.prefs());
        QVERIFY(!fade.running());
        QVERIFY(!fade.isVisible());
    }
    void targetHideCancelsAndDoesNotResurface() {
        QWidget target; target.resize(200, 100); target.show();
        ui::FadeOverlay fade(&target);
        fade.play();
        QVERIFY(fade.running());
        target.hide();
        QVERIFY(!fade.running());
        target.show();
        QVERIFY(!fade.isVisible());
    }
    void notifierPublishesCurrentTokensAndDisconnectsDestroyedListeners() {
        QSignalSpy changed(ui::themeNotifier(), &ui::ThemeNotifier::changed);
        int calls = 0;
        {
            QObject listener;
            connect(ui::themeNotifier(), &ui::ThemeNotifier::changed, &listener, [&] {
                ++calls;
                QCOMPARE(ui::theme().accent, QColor("#123456"));
            });
            auto theme = hn::theme::loadTheme("modernist", false); theme.accent = QColor("#123456");
            ui::setTheme(theme);
            QCOMPARE(changed.count(), 1);
            QCOMPARE(calls, 1);
        }
        ui::setTheme(hn::theme::loadTheme("modernist", true));
        QCOMPARE(changed.count(), 2);
        QCOMPARE(calls, 1);
    }
    void normalFadeFinishesAndStopsPainting() {
        QWidget target; target.resize(200, 100); target.show();
        ui::FadeOverlay fade(&target);
        PaintCounter counter; fade.installEventFilter(&counter);
        QElapsedTimer elapsed; elapsed.start();
        fade.play();
        QVERIFY(fade.running());
        QTRY_VERIFY_WITH_TIMEOUT(!fade.running(), 600);
        QVERIFY(!fade.isVisible());
        QTest::qWait(80);
        const int settled = counter.paints;
        QTest::qWait(240);
        QCOMPARE(counter.paints, settled);
        qInfo() << "Offscreen fade settled within" << elapsed.elapsed() - 320 << "ms; paints" << settled;
    }
};
int main(int argc, char **argv) {
    PolishProfile profile;
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    AppMotionPolishTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "app_motion_polish_test.moc"
