#include "app_test_util.h"
#include "ui_polish_test_util.h"
#include "settings_dialog.h"
#include <QAbstractItemView>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QStyleHints>
#include <QFontMetricsF>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QProcess>
#include <QProcessEnvironment>
#include <QScrollBar>
#include <QSpinBox>
#include <QStyle>
#include <QStyleOptionComboBox>
#include <QStyleOptionButton>
#include <QStyleOptionSpinBox>
#include <QTextDocument>
#include <QTextEdit>
#include <QVBoxLayout>
#include <hn/theme/theme.h>
#include <hn/theme/theme_io.h>

namespace {
const QStringList kBuiltins{"modernist", "catppuccin-mocha", "tokyo-night", "dracula", "nord", "gruvbox-dark", "one-dark"};

void compareThemes(const hn::theme::Theme &actual, const hn::theme::Theme &expected) {
    QCOMPARE(actual.dark, expected.dark);
    QCOMPARE(actual.bg, expected.bg); QCOMPARE(actual.surface, expected.surface);
    QCOMPARE(actual.text, expected.text); QCOMPARE(actual.muted, expected.muted);
    QCOMPARE(actual.accent, expected.accent); QCOMPARE(actual.accentText, expected.accentText);
    QCOMPARE(actual.border, expected.border); QCOMPARE(actual.danger, expected.danger);
    QCOMPARE(actual.success, expected.success); QCOMPARE(actual.selection, expected.selection);
    for (int i = 0; i < 6; ++i) QCOMPARE(actual.noteAccent[i], expected.noteAccent[i]);
    QCOMPARE(actual.fontFamily, expected.fontFamily); QCOMPARE(actual.monoFamily, expected.monoFamily);
    QCOMPARE(actual.baseSize, expected.baseSize); QCOMPARE(actual.padding, expected.padding);
    QCOMPARE(actual.lineHeight, expected.lineHeight); QCOMPARE(actual.radius, expected.radius);
    QCOMPARE(actual.borderWidth, expected.borderWidth);
}

QFont fixedFont() {
    QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    // The offscreen platform can report a proportional system FixedFont.
    if (!QFontDatabase::isFixedPitch(font.family()))
        for (const QString &family : QFontDatabase::families())
            if (QFontDatabase::isFixedPitch(family)) { font.setFamily(family); break; }
    return font;
}

struct Combo : QComboBox {
    using QComboBox::QComboBox;
    QStyleOptionComboBox option() const { QStyleOptionComboBox o; initStyleOption(&o); return o; }
};
template<class Base> struct Spin : Base {
    using Base::Base;
    QStyleOptionSpinBox option() const { QStyleOptionSpinBox o; this->initStyleOption(&o); return o; }
};

// Only count glyph-colored ink in the center of the arrow subcontrol, away from borders/text.
int arrowInk(QWidget &widget, const QRect &subcontrol, const QColor &background, const QColor &tint) {
    const QImage image = widget.grab().toImage();
    const qreal dpr = image.devicePixelRatio();
    const QRect center(subcontrol.center() - QPoint(5, 4), QSize(10, 8));
    if (!subcontrol.contains(center) || !widget.rect().contains(subcontrol)) return 0;
    auto distance = [](const QColor &a, const QColor &b) {
        return qAbs(a.red() - b.red()) + qAbs(a.green() - b.green()) + qAbs(a.blue() - b.blue());
    };
    int ink = 0;
    for (int y = center.top(); y <= center.bottom(); ++y)
        for (int x = center.left(); x <= center.right(); ++x) {
            const QColor pixel = image.pixelColor(qRound(x * dpr), qRound(y * dpr));
            if (distance(pixel, background) > 60 && distance(pixel, tint) < 100) ++ink;
        }
    return ink;
}
}

class AppThemePolishTest : public QObject {
    Q_OBJECT
private slots:
    void themeFontSurvivesPlatformThemeFontReset() {
        // qt6ct re-applies its own app font (pt-sized) after startup; the theme font must win.
        QWidget host;
        host.show();
        const auto theme = hn::theme::loadTheme("modernist", true);
        hn::theme::applyTheme(theme);
        QTRY_COMPARE(qApp->font().pixelSize(), theme.baseSize);
        QFont platform(theme.fontFamily);
        platform.setPointSizeF(14);
        qApp->setFont(platform);
        QTRY_COMPARE(qApp->font().pixelSize(), theme.baseSize);
    }

    void menuItemsAreInsetFromThePopupEdge() {
        const auto sheet = hn::theme::styleSheetFor(hn::theme::loadTheme("modernist", true));
        QVERIFY(sheet.contains("QMenu { background:") && sheet.contains("padding: 6px; }"));
        QVERIFY(sheet.contains("QMenu::item { padding: 6px 28px 6px 14px;"));
    }

    void widgetFontSurvivesWindowPolish() {
        QWidget host;
        auto *layout = new QVBoxLayout(&host);
        auto *source = new QPlainTextEdit(&host);
        auto *hint = new QLabel("Theme-dependent hint", &host);
        hint->setProperty("hnRole", "hint");
        layout->addWidget(source);
        layout->addWidget(hint);
        QFont mono = fixedFont();
        mono.setPixelSize(18);
        QCOMPARE(QFontMetrics(mono).horizontalAdvance("iiii"), QFontMetrics(mono).horizontalAdvance("WWWW"));
        source->setFont(mono);
        source->document()->setDefaultFont(mono);
        source->setPlainText("iiii\nWWWW");
        host.show();
        for (bool dark : {true, false, true}) {
            const auto theme = hn::theme::loadTheme("modernist", dark);
            hn::theme::applyTheme(theme);
            QTRY_COMPARE(host.styleSheet(), hn::theme::styleSheetFor(theme));
            QTRY_COMPARE(source->fontMetrics().horizontalAdvance("iiii"), source->fontMetrics().horizontalAdvance("WWWW"));
            const QFontMetricsF metrics(source->document()->defaultFont());
            QVERIFY(qAbs(metrics.horizontalAdvance("iiii") - metrics.horizontalAdvance("WWWW")) < 0.01);
            QCOMPARE(source->font().pixelSize(), 18);
            QTRY_COMPARE(hint->palette().color(QPalette::WindowText), theme.muted);
            QCOMPARE(hn::theme::popupRadius(theme), 6);
        }
    }

    void hiddenAndNewWindowsKeepSpecializedFonts() {
        QWidget hidden;
        auto *layout = new QVBoxLayout(&hidden);
        auto *label = new QLabel("TRACKED LABEL", &hidden);
        auto custom = hn::theme::labelFont(hn::theme::loadTheme("modernist", false));
        custom.setFamily(fixedFont().family());
        custom.setPixelSize(21);
        label->setFont(custom);
        layout->addWidget(label);
        hn::theme::applyTheme(hn::theme::loadTheme("modernist", false));
        auto theme = hn::theme::loadTheme("modernist", true);
        theme.baseSize = 18;
        hn::theme::applyTheme(theme);
        hidden.show();
        QTRY_COMPARE(hidden.styleSheet(), hn::theme::styleSheetFor(theme));
        QCOMPARE(label->font().family(), custom.family());
        QCOMPARE(label->font().pixelSize(), 21);
        QCOMPARE(label->font().letterSpacing(), custom.letterSpacing());
        QCOMPARE(label->font().capitalization(), QFont::AllUppercase);
        QCOMPARE(label->font().weight(), QFont::Bold);
        QWidget fresh;
        auto *freshLayout = new QVBoxLayout(&fresh);
        auto *ordinary = new QLabel("UI default", &fresh);
        auto *special = new QLabel("Specialized", &fresh);
        special->setFont(custom);
        freshLayout->addWidget(ordinary);
        freshLayout->addWidget(special);
        fresh.show();
        QTRY_COMPARE(fresh.styleSheet(), hn::theme::styleSheetFor(theme));
        QCOMPARE(ordinary->font().pixelSize(), 18);
        QCOMPARE(special->font(), custom);
        QVERIFY(qApp->styleSheet().isEmpty());
    }

    void semanticRoles_data() {
        QTest::addColumn<bool>("dark");
        QTest::addColumn<int>("size");
        for (bool dark : {false, true}) for (int size : {8, 14, 32})
            QTest::newRow(qPrintable(QString("%1-%2").arg(dark ? "dark" : "light").arg(size))) << dark << size;
    }
    void semanticRoles() {
        QFETCH(bool, dark);
        QFETCH(int, size);
        QWidget host;
        auto *layout = new QVBoxLayout(&host);
        QMap<QString, QLabel *> labels;
        for (const QString &role : {QString("hint"), QString("section"), QString("heading"), QString("danger"), QString("accent")}) {
            auto *label = new QLabel(role, &host);
            label->setProperty("hnRole", role);
            labels.insert(role, label);
            layout->addWidget(label);
        }
        auto *primary = new QPushButton("Primary action", &host);
        primary->setProperty("hnRole", "primary");
        layout->addWidget(primary);
        host.show();
        auto theme = hn::theme::loadTheme("modernist", dark);
        theme.baseSize = size;
        hn::theme::applyTheme(theme);
        QTRY_COMPARE(host.styleSheet(), hn::theme::styleSheetFor(theme));
        QCOMPARE(labels["hint"]->palette().color(QPalette::WindowText), theme.muted);
        QCOMPARE(labels["hint"]->font().pixelSize(), qMax(11, size - 1));
        QCOMPARE(labels["section"]->palette().color(QPalette::WindowText), theme.muted);
        QCOMPARE(labels["section"]->font().pixelSize(), qMax(10, size - 3));
        QCOMPARE(labels["section"]->font().weight(), QFont::Bold);
        QCOMPARE(labels["heading"]->palette().color(QPalette::WindowText), theme.text);
        QCOMPARE(labels["heading"]->font().pixelSize(), size + 6);
        QCOMPARE(labels["heading"]->font().weight(), QFont::Bold);
        QCOMPARE(labels["danger"]->palette().color(QPalette::WindowText), theme.danger);
        QCOMPARE(labels["danger"]->font().weight(), QFont::Bold);
        QCOMPARE(labels["accent"]->palette().color(QPalette::WindowText), theme.accent);
        QCOMPARE(primary->palette().color(QPalette::Button), theme.accent);
        QCOMPARE(primary->palette().color(QPalette::ButtonText), theme.accentText);
        // Changing a semantic role requires an explicit local repolish.
        labels["hint"]->setProperty("hnRole", "danger");
        labels["hint"]->style()->unpolish(labels["hint"]);
        labels["hint"]->style()->polish(labels["hint"]);
        labels["hint"]->update();
        QCOMPARE(labels["hint"]->palette().color(QPalette::WindowText), theme.danger);
    }

    void primaryStatesPaintLegibleText_data() { arrowsAreVisibleAndControlsWork_data(); }
    void primaryStatesPaintLegibleText() {
        QFETCH(bool, dark);
        QPushButton button("Primary action");
        button.setProperty("hnRole", "primary");
        button.resize(200, 40);
        button.show();
        const auto theme = hn::theme::loadTheme("modernist", dark);
        hn::theme::applyTheme(theme);
        QTRY_COMPARE(button.styleSheet(), hn::theme::styleSheetFor(theme));
        // Exercise the polished style with the same state flags QPushButton paints with.
        const QList<QStyle::State> states{QStyle::State_Enabled, QStyle::State_Enabled | QStyle::State_MouseOver,
            QStyle::State_Enabled | QStyle::State_HasFocus,
            QStyle::State_Enabled | QStyle::State_MouseOver | QStyle::State_HasFocus};
        for (const auto state : states) {
            QStyleOptionButton option;
            option.initFrom(&button);
            option.state = state;
            option.text = button.text();
            QImage image(button.size(), QImage::Format_ARGB32_Premultiplied);
            image.fill(theme.bg);
            QPainter painter(&image);
            painter.setFont(button.font());
            button.style()->drawControl(QStyle::CE_PushButton, &option, &painter, &button);
            painter.end();
            const bool hover = state.testFlag(QStyle::State_MouseOver);
            const QColor bg = hover ? theme.text : theme.accent;
            const QColor fg = hover ? theme.bg : theme.accentText;
            QCOMPARE(image.pixelColor(10, image.height() / 2), bg);
            int ink = 0;
            const QRect text = button.style()->subElementRect(QStyle::SE_PushButtonContents, &option, &button).adjusted(3, 3, -3, -3);
            for (int y = text.top(); y <= text.bottom(); ++y) for (int x = text.left(); x <= text.right(); ++x)
                if (image.pixelColor(x, y) == fg) ++ink;
            QVERIFY(ink >= 4);
            QVERIFY(hn::theme::contrastRatio(fg, bg) >= 4.5);
            if (state.testFlag(QStyle::State_HasFocus))
                QCOMPARE(image.pixelColor(image.width() / 2, 0), theme.text);
        }
    }

    void roundedControlsKeepDocumentSurfacesSquare() {
        QWidget host;
        auto *layout = new QVBoxLayout(&host);
        QLineEdit line;
        QPlainTextEdit plain;
        QTextEdit rich;
        QMenu menu;
        menu.addAction("Popup action");
        layout->addWidget(&line); layout->addWidget(&plain); layout->addWidget(&rich);
        host.show(); menu.show();
        for (int radius : {0, 4, 10}) {
            auto theme = hn::theme::loadTheme("modernist", false);
            theme.radius = radius;
            hn::theme::applyTheme(theme);
            QTRY_COMPARE(host.styleSheet(), hn::theme::styleSheetFor(theme));
            QTRY_COMPARE(menu.styleSheet(), hn::theme::styleSheetFor(theme));
            for (QWidget *document : {static_cast<QWidget *>(&plain), static_cast<QWidget *>(&rich)}) {
                document->clearFocus();
                QCOMPARE(document->grab().toImage().pixelColor(0, 0), theme.border);
            }
            line.clearFocus();
            const QColor controlCorner = line.grab().toImage().pixelColor(0, 0);
            const QColor popupCorner = menu.grab().toImage().pixelColor(0, 0);
            if (radius == 0) { QCOMPARE(controlCorner, theme.border); QCOMPARE(popupCorner, theme.border); }
            else { QVERIFY(controlCorner != theme.border); QVERIFY(popupCorner != theme.border); }
        }
    }

    void arrowsAreVisibleAndControlsWork_data() {
        QTest::addColumn<bool>("dark");
        QTest::newRow("light") << false;
        QTest::newRow("dark") << true;
    }
    void arrowsAreVisibleAndControlsWork() {
        QFETCH(bool, dark);
        QWidget host;
        auto *layout = new QVBoxLayout(&host);
        Combo combo;
        Spin<QSpinBox> spin;
        Spin<QDoubleSpinBox> real;
        combo.addItems({"System", "Light", "Dark"});
        spin.setRange(8, 32); spin.setValue(14);
        real.setRange(1, 3); real.setValue(2);
        layout->addWidget(&combo); layout->addWidget(&spin); layout->addWidget(&real);
        host.resize(320, 200);
        host.show();
        host.activateWindow();
        const auto theme = hn::theme::loadTheme("modernist", dark);
        hn::theme::applyTheme(theme);
        QTRY_COMPARE(host.styleSheet(), hn::theme::styleSheetFor(theme));
        QTest::keyClick(&combo, Qt::Key_Down);
        QCOMPARE(combo.currentText(), QString("Light"));
        QTest::keyClick(&spin, Qt::Key_Up);
        QCOMPARE(spin.value(), 15);
        QTest::keyClick(&spin, Qt::Key_Down);
        QCOMPARE(spin.value(), 14);
        QTest::keyClick(&real, Qt::Key_Up);
        QCOMPARE(real.value(), 3.0);
        QTest::keyClick(&real, Qt::Key_Down);
        QCOMPARE(real.value(), 2.0);
        for (bool enabled : {true, false}) {
            combo.setEnabled(enabled); spin.setEnabled(enabled); real.setEnabled(enabled);
            const QColor tint = enabled ? theme.text : theme.muted;
            const auto co = combo.option();
            const QRect arrow = combo.style()->subControlRect(QStyle::CC_ComboBox, &co, QStyle::SC_ComboBoxArrow, &combo);
            auto checkSpin = [&](auto &widget) {
                const auto o = widget.option();
                for (auto sc : {QStyle::SC_SpinBoxUp, QStyle::SC_SpinBoxDown}) {
                    const QRect r = widget.style()->subControlRect(QStyle::CC_SpinBox, &o, sc, &widget);
                    QVERIFY2(arrowInk(widget, r, theme.surface, tint) >= 4, "spin arrow ink");
                    const QRect text = widget.style()->subControlRect(QStyle::CC_SpinBox, &o, QStyle::SC_SpinBoxEditField, &widget);
                    QVERIFY(!text.intersects(r));
                }
            };
            checkSpin(spin); checkSpin(real);
            QVERIFY2(arrowInk(combo, arrow, theme.surface, tint) >= 4, "combo down-arrow ink");
        }
        combo.setEnabled(true); spin.setEnabled(true);
        for (const auto [value, subcontrol] : {std::pair{32, QStyle::SC_SpinBoxUp}, std::pair{8, QStyle::SC_SpinBoxDown}}) {
            spin.setValue(value);
            const auto option = spin.option();
            const QRect rect = spin.style()->subControlRect(QStyle::CC_SpinBox, &option, subcontrol, &spin);
            QVERIFY(arrowInk(spin, rect, theme.surface, theme.muted) >= 4);
        }
        combo.setFocus();
        QTRY_VERIFY(combo.hasFocus());
        const auto focused = combo.option();
        const QRect text = combo.style()->subControlRect(QStyle::CC_ComboBox, &focused, QStyle::SC_ComboBoxEditField, &combo);
        const QRect arrow = combo.style()->subControlRect(QStyle::CC_ComboBox, &focused, QStyle::SC_ComboBoxArrow, &combo);
        spin.setFocus();
        QTRY_VERIFY(spin.hasFocus());
        const auto spinFocused = spin.option();
        const auto unfocused = combo.option();
        QCOMPARE(combo.style()->subControlRect(QStyle::CC_ComboBox, &unfocused, QStyle::SC_ComboBoxEditField, &combo), text);
        QCOMPARE(combo.style()->subControlRect(QStyle::CC_ComboBox, &unfocused, QStyle::SC_ComboBoxArrow, &combo), arrow);
        QVERIFY(!text.intersects(arrow));
        combo.setFocus();
        QTRY_VERIFY(combo.hasFocus());
        const auto spinUnfocused = spin.option();
        for (auto sc : {QStyle::SC_SpinBoxUp, QStyle::SC_SpinBoxDown, QStyle::SC_SpinBoxEditField})
            QCOMPARE(spin.style()->subControlRect(QStyle::CC_SpinBox, &spinFocused, sc, &spin),
                     spin.style()->subControlRect(QStyle::CC_SpinBox, &spinUnfocused, sc, &spin));
    }

    void schemePopupFitsAllThreeRows() {
        QWidget host;
        auto *layout = new QVBoxLayout(&host);
        Combo combo;
        combo.addItems({"System", "Light", "Dark"});
        layout->addWidget(&combo);
        host.resize(320, 120); host.move(200, 200); host.show();
        hn::theme::applyTheme(hn::theme::loadTheme("modernist", false));
        for (int selected : {0, 1, 2}) {
            combo.setCurrentIndex(selected);
            combo.showPopup();
            QTRY_VERIFY(combo.view()->isVisible());
            for (int row = 0; row < 3; ++row)
                QTRY_VERIFY(combo.view()->viewport()->rect().contains(combo.view()->visualRect(combo.model()->index(row, 0))));
            QCOMPARE(combo.view()->verticalScrollBar()->maximum(), 0);
            combo.hidePopup();
        }
    }

    void builtinsLoadWithoutConfigAndExportEditableCopies() {
        PolishProfile profile;
        QCOMPARE(hn::theme::listThemes(), kBuiltins);
        const auto lightBase = hn::theme::loadTheme("modernist", false);
        for (const QString &id : kBuiltins) {
            QVERIFY(hn::theme::isBuiltinTheme(id));
            QVERIFY(hn::theme::isBuiltinTheme(id.toUpper()));
            const auto dark = hn::theme::loadTheme(id, true);
            QVERIFY2(hn::theme::lastThemeError().isEmpty(), qPrintable(id));
            QVERIFY(dark.dark);
            for (const QColor &bg : {dark.bg, dark.surface}) {
                QVERIFY(hn::theme::contrastRatio(dark.text, bg) >= 4.5);
                QVERIFY(hn::theme::contrastRatio(dark.muted, bg) >= 4.5);
                QVERIFY(hn::theme::contrastRatio(dark.danger, bg) >= 4.5);
                QVERIFY(hn::theme::contrastRatio(dark.success, bg) >= 4.5);
            }
            QVERIFY(hn::theme::contrastRatio(dark.accentText, dark.accent) >= 4.5);
            compareThemes(hn::theme::loadTheme(id, false), lightBase);
            compareThemes(hn::theme::loadTheme(id.toUpper(), true), dark);
            QString error;
            QVERIFY(!hn::theme::removeTheme(id, &error));
            QVERIFY(error.contains("built-in"));
        }
        QVERIFY(!QFileInfo::exists(hn::theme::themesDir()));
        for (const char *key : {"XDG_CONFIG_HOME", "HN_CONFIG_DIR"})
            QVERIFY(QDir(qEnvironmentVariable(key)).isEmpty());
        QVERIFY(!hn::theme::isBuiltinTheme({}));
        QVERIFY(!hn::theme::isBuiltinTheme("modernist-copy"));
        for (const QString &id : kBuiltins) {
            QString error, imported;
            const QString path = profile.dir.filePath(id + ".json");
            QVERIFY2(hn::theme::exportTheme(id, path, &error), qPrintable(error));
            QVERIFY2(hn::theme::importTheme(path, &imported, &error), qPrintable(error));
            QCOMPARE(imported, id + "-copy");
            QVERIFY(!hn::theme::isBuiltinTheme(imported));
            for (bool dark : {false, true})
                compareThemes(hn::theme::loadTheme(imported, dark), hn::theme::loadTheme(id, dark));
            QVERIFY(hn::theme::removeTheme(imported, &error));
        }
    }

    void namedPaletteTokens_data() {
        QTest::addColumn<QString>("id");
        QTest::addColumn<QStringList>("colors");
        QTest::newRow("catppuccin-mocha") << "catppuccin-mocha" << QString("#1E1E2E #181825 #CDD6F4 #BAC2DE #CBA6F7 #11111B #45475A #F38BA8 #A6E3A1 #313244 #F9E2AF #89B4FA").split(' ');
        QTest::newRow("tokyo-night") << "tokyo-night" << QString("#1A1B26 #24283B #C0CAF5 #A9B1D6 #7AA2F7 #1A1B26 #414868 #F7768E #9ECE6A #292E42 #E0AF68 #7AA2F7").split(' ');
        QTest::newRow("dracula") << "dracula" << QString("#282A36 #21222C #F8F8F2 #BFBFD3 #BD93F9 #282A36 #6272A4 #FF6E6E #50FA7B #44475A #F1FA8C #8BE9FD").split(' ');
        QTest::newRow("nord") << "nord" << QString("#2E3440 #3B4252 #ECEFF4 #D8DEE9 #88C0D0 #2E3440 #4C566A #E7A2AA #A3BE8C #434C5E #EBCB8B #81A1C1").split(' ');
        QTest::newRow("gruvbox-dark") << "gruvbox-dark" << QString("#282828 #3C3836 #EBDBB2 #BDAE93 #FABD2F #282828 #665C54 #FB7C6D #B8BB26 #504945 #FABD2F #83A598").split(' ');
        QTest::newRow("one-dark") << "one-dark" << QString("#282C34 #21252B #ABB2BF #ABB2BF #61AFEF #21252B #4B5263 #E99AA2 #98C379 #3E4451 #E5C07B #61AFEF").split(' ');
    }
    void namedPaletteTokens() {
        QFETCH(QString, id);
        QFETCH(QStringList, colors);
        PolishProfile profile;
        const auto theme = hn::theme::loadTheme(id, true);
        QVERIFY2(hn::theme::lastThemeError().isEmpty(), qPrintable(hn::theme::lastThemeError()));
        const QList<QColor> actual{theme.bg, theme.surface, theme.text, theme.muted, theme.accent,
            theme.accentText, theme.border, theme.danger, theme.success, theme.selection};
        for (int i = 0; i < actual.size(); ++i) QCOMPARE(actual[i], QColor(colors[i]));
        const QList<int> noteIndices{7, 11, 10, 8, 6, 2};
        for (int i = 0; i < 6; ++i) QCOMPARE(theme.noteAccent[i], QColor(colors[noteIndices[i]]));
        const auto base = hn::theme::loadTheme("modernist", true);
        QCOMPARE(theme.fontFamily, base.fontFamily); QCOMPARE(theme.monoFamily, base.monoFamily);
        QCOMPARE(theme.baseSize, base.baseSize); QCOMPARE(theme.padding, 16);
        QCOMPARE(theme.lineHeight, base.lineHeight); QCOMPARE(theme.radius, 4); QCOMPARE(theme.borderWidth, 1);
    }

    void compiledCatalogWorksFromUnrelatedDirectory() {
        PolishProfile profile;
        QVERIFY(!QFileInfo::exists(hn::theme::themesDir()));
        QProcess child;
        child.setWorkingDirectory(profile.dir.path());
        child.setProcessEnvironment(QProcessEnvironment::systemEnvironment());
        child.start(QCoreApplication::applicationFilePath(), {"--builtin-probe"});
        QVERIFY(child.waitForFinished(15000));
        QCOMPARE(child.exitStatus(), QProcess::NormalExit);
        QCOMPARE(child.exitCode(), 0);
        QVERIFY(!QFileInfo::exists(hn::theme::themesDir()));
    }

    void pickerProtectsBuiltinsAndPreservesScheme_data() {
        QTest::addColumn<QString>("scheme");
        for (const QString &scheme : {QString("system"), QString("light"), QString("dark")})
            QTest::newRow(qPrintable(scheme)) << scheme;
    }
    void initiallySelectedPresetCannotBeRemoved() {
        PolishProfile profile;
        apptest::Lib lib;
        hn::app::AppController controller(lib.opts());
        auto settings = controller.settings();
        settings.theme = "nord";
        controller.applySettings(settings, controller.prefs());
        hn::app::SettingsDialog dialog(&controller);
        QCOMPARE(dialog.themeBox()->currentText(), QString("nord"));
        QVERIFY(!dialog.removeThemeButton()->isEnabled());
    }
    void pickerProtectsBuiltinsAndPreservesScheme() {
        QFETCH(QString, scheme);
        PolishProfile profile;
        apptest::Lib lib;
        hn::app::AppController controller(lib.opts());
        auto settings = controller.settings();
        settings.colorScheme = scheme;
        controller.applySettings(settings, controller.prefs());
        hn::app::SettingsDialog dialog(&controller);
        const bool systemDark = QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
        QString expected = scheme;   // picking a dark-only preset while the UI is light switches to Dark (and stays)
        for (const QString &id : kBuiltins) {
            const int index = dialog.themeBox()->findText(id);
            QVERIFY(index >= 0);
            dialog.themeBox()->setCurrentIndex(index);
            if (id != "modernist" && !(expected == "dark" || (expected == "system" && systemDark))) expected = "dark";
            QCOMPARE(controller.settings().theme, id);
            QCOMPARE(controller.settings().colorScheme, expected);
            QVERIFY(!dialog.removeThemeButton()->isEnabled());
            QVERIFY(!dialog.removeSelectedTheme());
            QCOMPARE(controller.settings().theme, id);
            const QString path = profile.dir.filePath(id + ".json");
            QVERIFY(dialog.exportThemeTo(path));
            QVERIFY(dialog.importThemeFile(path));
            QCOMPARE(controller.settings().theme, id + "-copy");
            QVERIFY(dialog.removeThemeButton()->isEnabled());
            QCOMPARE(controller.settings().colorScheme, expected);
            controller.config().reload();
            QCOMPARE(controller.config().settings().colorScheme, expected);
            QVERIFY(dialog.removeSelectedTheme());
            QVERIFY(!dialog.removeThemeButton()->isEnabled());
            QCOMPARE(controller.settings().colorScheme, expected);
        }
    }
};

int main(int argc, char **argv) {
    PolishProfile profile;
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    if (app.arguments().contains("--builtin-probe")) {
        if (QFileInfo::exists(hn::theme::themesDir())) return 1;
        for (const QString &id : kBuiltins) for (bool dark : {false, true}) {
            const auto theme = hn::theme::loadTheme(id, dark);
            if (!hn::theme::lastThemeError().isEmpty() || theme.dark != dark) return 2;
        }
        if (hn::theme::listThemes() != kBuiltins || QFileInfo::exists(hn::theme::themesDir())) return 3;
        return 0;
    }
    AppThemePolishTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "app_theme_polish_test.moc"
