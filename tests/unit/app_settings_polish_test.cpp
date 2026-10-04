#include "app_test_util.h"
#include "ui_polish_test_util.h"
#include "settings_dialog.h"
#include <hn/theme/theme_io.h>
#include <QCheckBox>
#include <QComboBox>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSpinBox>
#include <QTableWidget>
#include <QTabWidget>

using namespace hn::app;

namespace {
QString sizeTheme(PolishProfile &profile) {
    const QString path = profile.dir.filePath("size.json");
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) qFatal("Cannot write theme fixture");
    file.write(QJsonDocument(QJsonObject{{"version", 1}, {"name", "size-fixture"},
        {"light", QJsonObject{{"baseSize", 15}}}, {"dark", QJsonObject{{"baseSize", 19}}}}).toJson());
    return path;
}
}

class SettingsPolishTest : public QObject {
    Q_OBJECT
private slots:
    // A stale displayed size must never become an override through an unrelated edit.
    void importedDefaultSizeIsShownWithoutCreatingAnOverride() {
        PolishProfile profile;
        const QString path = sizeTheme(profile);
        apptest::Lib lib;
        AppController controller(lib.opts());
        auto settings = controller.settings();
        settings.colorScheme = "light";
        settings.fontSize = 0;
        auto prefs = controller.prefs(); prefs.fontSize = 0;
        controller.applySettings(settings, prefs);
        SettingsDialog dialog(&controller);
        dialog.show();
        QVERIFY(dialog.importThemeFile(path));
        const QString imported = dialog.note();
        QCOMPARE(controller.theme().baseSize, 15);
        QCOMPARE(dialog.fontSpin()->value(), 15);
        QCOMPARE(controller.settings().fontSize, 0);
        dialog.motionBox()->setChecked(!dialog.motionBox()->isChecked());
        QCOMPARE(controller.settings().fontSize, 0);
        QCOMPARE(dialog.note(), imported);
        dialog.schemeBox()->setCurrentIndex(2);
        QCOMPARE(controller.theme().baseSize, 19);
        QCOMPARE(dialog.fontSpin()->value(), 19);
        QCOMPARE(controller.settings().fontSize, 0);
        QCOMPARE(dialog.note(), imported);
        dialog.fontSpin()->setValue(18);
        QCOMPARE(controller.settings().fontSize, 18);
        dialog.themeBox()->setCurrentIndex(dialog.themeBox()->findText("nord"));
        QCOMPARE(dialog.fontSpin()->value(), 18);
        QCOMPARE(controller.settings().fontSize, 18);
    }

    void externalChangesAndDefaultResetPreserveMessagesWithoutApplyLoops() {
        PolishProfile profile;
        apptest::Lib lib;
        AppController controller(lib.opts());
        SettingsDialog dialog(&controller);
        dialog.show();
        QVERIFY(dialog.importThemeFile(sizeTheme(profile)));
        const QString message = dialog.note();
        QVERIFY(!message.isEmpty());
        QSignalSpy changed(&controller, &AppController::themeChanged);
        for (int px : {8, 18, 32}) {
            auto settings = controller.settings();
            settings.fontSize = px;
            settings.colorScheme = "dark";
            controller.applySettings(settings, controller.prefs());
            QCOMPARE(dialog.fontSpin()->value(), px);
            QCOMPARE(dialog.schemeBox()->currentIndex(), 2);
            QCOMPARE(dialog.note(), message);
        }
        QCOMPARE(changed.size(), 3);
        auto *reset = dialog.findChild<QPushButton *>("hnThemeDefaultSize");
        QVERIFY(reset);
        reset->click();
        QCOMPARE(controller.settings().fontSize, 0);
        QCOMPARE(controller.prefs().fontSize, 0);
        QCOMPARE(dialog.fontSpin()->value(), 19);
        dialog.trayBox()->setChecked(!dialog.trayBox()->isChecked());
        QCOMPARE(controller.settings().fontSize, 0);

        // The real config watcher also updates an already open dialog.
        hn::theme::Config external(lib.cfg);
        auto settings = controller.settings();
        settings.theme = "dracula"; settings.colorScheme = "light"; settings.fontSize = 8;
        QVERIFY(external.save(settings));
        QTRY_COMPARE(controller.settings().theme, QString("dracula"));
        QTRY_COMPARE(dialog.fontSpin()->value(), 8);
        QCOMPARE(dialog.themeBox()->currentText(), QString("dracula"));
        QCOMPARE(dialog.schemeBox()->currentIndex(), 1);
        dialog.motionBox()->setChecked(!dialog.motionBox()->isChecked());
        QCOMPARE(controller.settings().fontSize, 8);
    }

    void settingsMinimumSizeHasScrollableAppearanceAndClose() {
        PolishProfile profile;
        apptest::Lib lib;
        AppController controller(lib.opts());
        SettingsDialog dialog(&controller);
        dialog.resize(520, 460);
        dialog.show();
        QTRY_COMPARE(dialog.size(), QSize(520, 460));
        auto *scroll = qobject_cast<QScrollArea *>(dialog.tabs()->widget(0));
        QVERIFY(scroll);
        QVERIFY(scroll->widgetResizable());
        QTRY_VERIFY(scroll->verticalScrollBar()->maximum() > 0);
        QCOMPARE(scroll->horizontalScrollBar()->maximum(), 0);
        scroll->ensureWidgetVisible(dialog.motionBox());
        QTRY_VERIFY(scroll->viewport()->rect().contains(inWidget(dialog.motionBox(), scroll->viewport())));
        auto *close = dialog.findChild<QPushButton *>("hnSettingsClose");
        auto *title = dialog.findChild<QLabel *>("hnSettingsTitle");
        QVERIFY(close && title && close->isVisible() && title->isVisible());
        QVERIFY(dialog.rect().contains(inWidget(close, &dialog)));
        QTest::mouseClick(close, Qt::LeftButton);
        QTRY_VERIFY(!dialog.isVisible());
    }

    void largeTextPagesAndKeysRemainReachable() {
        PolishProfile profile;
        apptest::Lib lib;
        AppController controller(lib.opts());
        SettingsDialog dialog(&controller);
        dialog.resize(520, 460);
        dialog.show();
        for (int px : {8, 18, 32}) {
            auto settings = controller.settings(); settings.fontSize = px;
            controller.applySettings(settings, controller.prefs());
            QTRY_COMPARE(dialog.fontSpin()->font().pixelSize(), px);
            QTRY_COMPARE(dialog.size(), QSize(520, 460));
            for (int index : {0, 1, 2}) {
                dialog.showTab(index);
                auto *scroll = qobject_cast<QScrollArea *>(dialog.tabs()->widget(index));
                QVERIFY(scroll && scroll->widgetResizable());
                QTRY_COMPARE(scroll->horizontalScrollBar()->maximum(), 0);
                QTRY_VERIFY(scroll->widget()->width() <= scroll->viewport()->width());
            }
            dialog.showTab(4);
            auto *table = dialog.keysTable();
            QVERIFY(!qobject_cast<QScrollArea *>(dialog.tabs()->widget(4)));
            for (int row = 0; row < table->rowCount(); ++row) {
                auto *editor = qobject_cast<QKeySequenceEdit *>(table->cellWidget(row, 1));
                QVERIFY(editor);
                QTRY_VERIFY(table->rowHeight(row) >= editor->sizeHint().height());
                table->scrollToItem(table->item(row, 0));
                QTRY_VERIFY2(table->viewport()->rect().contains(inWidget(editor, table->viewport())),
                    qPrintable(QString("px=%1 row=%2 editor=%3,%4 %5x%6 viewport=%7x%8")
                        .arg(px).arg(row).arg(editor->x()).arg(editor->y()).arg(editor->width()).arg(editor->height())
                        .arg(table->viewport()->width()).arg(table->viewport()->height())));
            }
        }
        dialog.showTab(0);
        auto *scroll = qobject_cast<QScrollArea *>(dialog.tabs()->widget(0));
        const auto buttons = scroll->findChildren<QPushButton *>();
        QPushButton *import = nullptr, *exportButton = nullptr;
        for (auto *b : buttons) {
            if (b->text() == "Import theme…") import = b;
            if (b->text() == "Export current theme…") exportButton = b;
        }
        QVERIFY(import && exportButton);
        QTRY_VERIFY(inWidget(exportButton, scroll->widget()).top() > inWidget(import, scroll->widget()).top());
        for (auto *b : {import, exportButton, dialog.removeThemeButton()}) {
            scroll->ensureWidgetVisible(b, 0, 0);
            QTRY_VERIFY(scroll->viewport()->rect().contains(inWidget(b, scroll->viewport())));
            QVERIFY(b->height() >= b->fontMetrics().height() + 8);
        }
    }

    void pickingDarkPresetInLightModeSwitchesToDark() {
        PolishProfile profile;
        apptest::Lib lib;
        AppController controller(lib.opts());
        auto settings = controller.settings(); settings.colorScheme = "light"; settings.theme = "modernist";
        controller.applySettings(settings, controller.prefs());
        SettingsDialog dialog(&controller);
        dialog.show();
        dialog.themeBox()->setCurrentIndex(dialog.themeBox()->findText("catppuccin-mocha"));
        QCOMPARE(controller.settings().theme, QString("catppuccin-mocha"));
        QCOMPARE(controller.settings().colorScheme, QString("dark"));
        QCOMPARE(dialog.schemeBox()->currentIndex(), 2);
        QTRY_VERIFY(controller.theme().bg.lightness() < 80);
    }

    void liveRolesAndImportFailureKeepStateAndBuiltinProtection() {
        PolishProfile profile;
        apptest::Lib lib;
        AppController controller(lib.opts());
        SettingsDialog dialog(&controller);
        dialog.show();
        for (const QString &scheme : {QString("dark"), QString("light"), QString("dark")}) {
            auto settings = controller.settings(); settings.colorScheme = scheme;
            controller.applySettings(settings, controller.prefs());
            int labels = 0;
            for (auto *label : dialog.tabs()->widget(0)->findChildren<QLabel *>()) {
                const QString role = label->property("hnRole").toString();
                if (role != "hint" && role != "section") continue;
                ++labels;
                QTRY_COMPARE(label->palette().color(QPalette::WindowText), controller.theme().muted);
            }
            QVERIFY(labels >= 5);
        }
        for (const QString &id : {QString("modernist"), QString("catppuccin-mocha"), QString("tokyo-night"),
                                  QString("dracula"), QString("nord"), QString("gruvbox-dark"), QString("one-dark")}) {
            dialog.themeBox()->setCurrentIndex(dialog.themeBox()->findText(id));
            QVERIFY(!dialog.removeThemeButton()->isEnabled());
            QCOMPARE(controller.settings().colorScheme, QString("dark"));
            QCOMPARE(controller.settings().fontSize, 0);
        }
        const auto before = controller.settings();
        QVERIFY(!dialog.importThemeFile(profile.dir.filePath("missing.json")));
        QCOMPARE(controller.settings(), before);
        QCOMPARE(dialog.themeBox()->currentText(), before.theme);
        const QString failure = dialog.note();
        QVERIFY(failure.contains("not imported"));
        auto settings = before; settings.colorScheme = "light";
        controller.applySettings(settings, controller.prefs());
        QCOMPARE(dialog.note(), failure);
        const QString copy = profile.dir.filePath("copy.json");
        QVERIFY(dialog.exportThemeTo(copy));
        QVERIFY(dialog.importThemeFile(copy));
        QVERIFY(dialog.removeThemeButton()->isEnabled());
        QVERIFY(dialog.removeSelectedTheme());
        QCOMPARE(controller.settings().theme, QString("modernist"));
        QCOMPARE(controller.settings().colorScheme, QString("light"));
        QCOMPARE(controller.settings().fontSize, 0);
    }
};

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    SettingsPolishTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "app_settings_polish_test.moc"
