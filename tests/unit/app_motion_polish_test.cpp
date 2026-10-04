#include "app_test_util.h"
#include "ui_polish_test_util.h"
#include "ui_common.h"
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
