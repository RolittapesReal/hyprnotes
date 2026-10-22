#include "app_test_util.h"
#include "ui_polish_test_util.h"
#include "settings_dialog.h"
#include <QAbstractItemView>
#include <QApplication>
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
#include <QPalette>
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
#include <limits>
#include <vector>
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

QRect paintedTextBounds(const QImage &image, const QColor &foreground, const QColor &background,
                        const QRect &clip, int *pixels = nullptr) {
    QRect bounds;
    int count = 0;
    for (int y = qMax(0, clip.top()); y <= qMin(image.height() - 1, clip.bottom()); ++y)
        for (int x = qMax(0, clip.left()); x <= qMin(image.width() - 1, clip.right()); ++x) {
            const QColor pixel = image.pixelColor(x, y);
            const int fgDistance = qAbs(pixel.red() - foreground.red()) + qAbs(pixel.green() - foreground.green())
                + qAbs(pixel.blue() - foreground.blue());
            const int bgDistance = qAbs(pixel.red() - background.red()) + qAbs(pixel.green() - background.green())
                + qAbs(pixel.blue() - background.blue());
            if (fgDistance < 75 && bgDistance > 60) {
                bounds |= QRect(x, y, 1, 1);
                ++count;
            }
        }
    if (pixels) *pixels = count;
    return bounds;
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
                    const auto *edit = widget.template findChild<QLineEdit *>();
                    QVERIFY(edit);
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

    void spinboxTextAndBoundaryClicksRemainUsable_data() {
        QTest::addColumn<bool>("dark");
        QTest::addColumn<bool>("real");
        for (bool dark : {false, true}) for (bool real : {false, true})
            QTest::newRow(qPrintable(QString("%1-%2").arg(dark ? "dark" : "light").arg(real ? "double" : "int")))
                << dark << real;
    }
    void spinboxTextAndBoundaryClicksRemainUsable() {
        QFETCH(bool, dark);
        QFETCH(bool, real);
        QWidget host;
        Spin<QSpinBox> integer;
        Spin<QDoubleSpinBox> decimal;
        QWidget *widget = real ? static_cast<QWidget *>(&decimal) : static_cast<QWidget *>(&integer);
        QLineEdit *edit = nullptr;
        host.resize(320, 90);
        widget->setParent(&host);
        widget->setGeometry(10, 10, 298, 32);
        auto theme = hn::theme::loadTheme("modernist", dark);
        hn::theme::applyTheme(theme);
        if (real) {
            decimal.setRange(-1.0e100, 1.0e100);
            decimal.setDecimals(100);
        } else {
            integer.setRange(0, std::numeric_limits<int>::max());
            integer.setValue(integer.minimum());
        }
        if (real) decimal.setValue(decimal.minimum());
        const auto spinOption = [&]() {
            return real ? static_cast<Spin<QDoubleSpinBox> *>(widget)->option()
                        : static_cast<Spin<QSpinBox> *>(widget)->option();
        };
        host.show();
        host.activateWindow();
        QTRY_VERIFY(host.isActiveWindow());
        QTRY_COMPARE(host.styleSheet(), hn::theme::styleSheetFor(theme));
        edit = widget->findChild<QLineEdit *>();
        QVERIFY(edit);
        const QString minimum = edit->text();
        QVERIFY(minimum.size() >= (real ? 30 : 1));
        const QImage minimumRender = widget->grab().toImage();
        QVERIFY(qAbs(minimumRender.devicePixelRatio() - 1.0) < 0.01);
        int minimumInk = 0;
        const QRect valueEditor(edit->geometry().topLeft(), edit->size());
        const QRect minimumInkBounds = paintedTextBounds(minimumRender, edit->palette().color(QPalette::Text),
            edit->palette().color(QPalette::Base), valueEditor.adjusted(2, 2, -2, -2), &minimumInk);
        QVERIFY2(minimumInk >= 4, "minimum numeric value must produce visible glyph ink in widget render");
        for (auto sc : {QStyle::SC_SpinBoxUp, QStyle::SC_SpinBoxDown}) {
            const auto option = spinOption();
            const QRect arrow = widget->style()->subControlRect(QStyle::CC_SpinBox, &option, sc, widget);
            QVERIFY2(!minimumInkBounds.intersects(arrow), "minimum-value glyph ink must not paint in arrow hit area");
        }
        if (real) decimal.setValue(decimal.maximum());
        else integer.setValue(integer.maximum());
        const QString maximal = edit->text();
        QVERIFY(maximal.size() >= (real ? 30 : 10));
        const QImage maximumRender = widget->grab().toImage();
        const qreal maximumDpr = maximumRender.devicePixelRatio();
        QVERIFY(qAbs(maximumDpr - 1.0) < 0.01);
        int maximumInk = 0;
        const QRect maximumEditor(edit->geometry().topLeft(), edit->size());
        const QRect maximumInkBounds = paintedTextBounds(maximumRender, edit->palette().color(QPalette::Text),
            edit->palette().color(QPalette::Base), maximumEditor.adjusted(2, 2, -2, -2), &maximumInk);
        QVERIFY2(maximumInk >= 8, "maximum numeric value must produce visible glyph ink in widget render");
        for (auto sc : {QStyle::SC_SpinBoxUp, QStyle::SC_SpinBoxDown}) {
            const auto option = spinOption();
            const QRect arrow = widget->style()->subControlRect(QStyle::CC_SpinBox, &option, sc, widget);
            QVERIFY2(!maximumInkBounds.intersects(arrow), "maximum-value glyph ink must not paint in arrow hit area");
        }

        // Use an overlong editable draft to force QLineEdit's real horizontal-scroll path.
        edit->setFocus();
        edit->selectAll();
        QTest::keyClicks(edit, QString(48, QLatin1Char('0')));
        QCOMPARE(edit->text().size(), 48);
        edit->setFocus();
        edit->setCursorPosition(edit->text().size());
        QTRY_VERIFY(edit->hasFocus());
        const QRect editRect = edit->geometry();
        const QImage rendered = widget->grab().toImage();
        const qreal dpr = rendered.devicePixelRatio();
        QVERIFY(qAbs(dpr - 1.0) < 0.01);
        const QPoint offset = editRect.topLeft();
        const QColor textColor = edit->palette().color(QPalette::Text);
        const QColor baseColor = edit->palette().color(QPalette::Base);
        const QRect editorPixels(qRound(offset.x() * dpr), qRound(offset.y() * dpr),
                                 qRound(editRect.width() * dpr), qRound(editRect.height() * dpr));
        int ink = 0;
        const QRect draftInk = paintedTextBounds(rendered, textColor, baseColor, editorPixels.adjusted(2, 2, -2, -2), &ink);
        QVERIFY2(ink >= 8, "long editable spinbox text must paint glyph ink in actual widget rendering");
        QVERIFY2(draftInk.right() >= editorPixels.right() - 12, "scrolled long text must remain visible at editor far edge");
        for (auto sc : {QStyle::SC_SpinBoxUp, QStyle::SC_SpinBoxDown}) {
            const auto option = spinOption();
            const QRect arrow = widget->style()->subControlRect(QStyle::CC_SpinBox, &option, sc, widget);
            const QRect arrowPixels(qRound(arrow.x() * dpr), qRound(arrow.y() * dpr),
                                    qRound(arrow.width() * dpr), qRound(arrow.height() * dpr));
            QVERIFY2(!draftInk.intersects(arrowPixels), "rendered editor glyph ink must not enter spinbox arrow hit area");
        }

        // Selection reaching the end and the insertion caret must both remain visibly rendered.
        edit->selectAll();
        const QImage selected = widget->grab().toImage();
        const QColor selection = edit->palette().color(QPalette::Highlight);
        int selectedInk = 0;
        const QRect selectedInkBounds = paintedTextBounds(selected,
            edit->palette().color(QPalette::HighlightedText), selection,
            editorPixels.adjusted(2, 2, -2, -2), &selectedInk);
        QVERIFY2(selectedInk >= 8, "selected glyphs must remain visibly rendered");
        QVERIFY2(selectedInkBounds.right() >= editorPixels.right() - 12,
                 "selection reaching the end must remain visible at editor far edge");
        for (auto sc : {QStyle::SC_SpinBoxUp, QStyle::SC_SpinBoxDown}) {
            const auto option = spinOption();
            const QRect arrow = widget->style()->subControlRect(QStyle::CC_SpinBox, &option, sc, widget);
            QVERIFY2(!selectedInkBounds.intersects(arrow), "selected glyph ink must not paint in arrow hit area");
        }
        int selectedPixels = 0;
        for (int y = editorPixels.top(); y <= editorPixels.bottom(); ++y)
            for (int x = editorPixels.left(); x <= editorPixels.right(); ++x) {
                const QColor pixel = selected.pixelColor(x, y);
                if (qAbs(pixel.red() - selection.red()) + qAbs(pixel.green() - selection.green())
                    + qAbs(pixel.blue() - selection.blue()) < 75) ++selectedPixels;
            }
        QVERIFY(selectedPixels >= 20);
        edit->deselect();
        edit->setCursorPosition(edit->text().size());
        struct CursorFlashTimeRestore {
            int value;
            bool active = true;
            void restore() {
                if (active) QApplication::setCursorFlashTime(value);
                active = false;
            }
            ~CursorFlashTimeRestore() { restore(); }
        } restoreCursorFlashTime{QApplication::cursorFlashTime()};
        QApplication::setCursorFlashTime(200);
        QCoreApplication::processEvents();
        const QRect caretGeometry = edit->geometry();
        const QFont caretFont = edit->font();
        const QPalette caretPalette = edit->palette();
        const QString caretText = edit->text();
        const int caretPosition = edit->cursorPosition();
        QVERIFY(edit->hasFocus());
        QVERIFY(!edit->hasSelectedText());
        struct BlinkRun { QImage image; int frames; };
        std::vector<BlinkRun> blinkRuns;
        for (int frame = 0; frame <= 50; ++frame) {
            QCOMPARE(edit->geometry(), caretGeometry);
            QCOMPARE(edit->font(), caretFont);
            QCOMPARE(edit->palette(), caretPalette);
            QCOMPARE(edit->text(), caretText);
            QCOMPARE(edit->cursorPosition(), caretPosition);
            QVERIFY(edit->hasFocus());
            QVERIFY(!edit->hasSelectedText());
            const QImage image = widget->grab().toImage();
            if (!blinkRuns.empty() && blinkRuns.back().image == image)
                ++blinkRuns.back().frames;
            else
                blinkRuns.push_back({image, 1});
            if (frame < 50) QTest::qWait(20);
        }
        restoreCursorFlashTime.restore();
        int repeatedCycle = -1;
        for (int i = 0; i + 3 < static_cast<int>(blinkRuns.size()); ++i) {
            const auto &a = blinkRuns[i];
            const auto &b = blinkRuns[i + 1];
            const auto &againA = blinkRuns[i + 2];
            const auto &againB = blinkRuns[i + 3];
            if (a.frames >= 2 && b.frames >= 2 && againA.frames >= 2 && againB.frames >= 2
                && !(a.image == b.image) && a.image == againA.image && b.image == againB.image) {
                repeatedCycle = i;
                break;
            }
        }
        QVERIFY2(repeatedCycle >= 0,
                 "same-position caret must produce repeated stable ON/OFF image states within bounded blink window");
        const QImage caretOn = blinkRuns[repeatedCycle].image;
        const QImage caretOff = blinkRuns[repeatedCycle + 1].image;
        QCOMPARE(caretOn.size(), caretOff.size());
        QCOMPARE(edit->geometry(), caretGeometry);
        QCOMPARE(edit->font(), caretFont);
        QCOMPARE(edit->palette(), caretPalette);
        QCOMPARE(edit->text(), caretText);
        QVERIFY(edit->hasFocus());
        QCOMPARE(edit->cursorPosition(), caretPosition);
        QCOMPARE(caretText.size(), 48);
        int changedPixels = 0;
        int rightmostColumnPixels = 0;
        QRect caretInk;
        for (int y = 0; y < caretOn.height(); ++y) {
            for (int x = 0; x < caretOn.width(); ++x) {
                const QColor on = caretOn.pixelColor(x, y);
                const QColor off = caretOff.pixelColor(x, y);
                if (on != off) {
                    ++changedPixels;
                    if (x >= editorPixels.right() - 3) ++rightmostColumnPixels;
                    const QRect pixel(x, y, 1, 1);
                    caretInk = caretInk.isNull() ? pixel : caretInk.united(pixel);
                }
            }
        }
        QVERIFY2(changedPixels >= edit->fontMetrics().height() * 2 / 3,
                 "same-position blink-state difference must contain actual caret paint");
        QVERIFY2(changedPixels <= edit->fontMetrics().height() * 6,
                 "same-position blink-state difference must exclude glyph or full-widget changes");
        QVERIFY2(caretInk.width() <= edit->fontMetrics().horizontalAdvance(QLatin1Char('0')) + 4
                     && caretInk.height() >= edit->fontMetrics().height() * 2 / 3,
                 "same-position blink-state difference must have narrow vertical caret shape");
        const QRect editorContents = edit->contentsRect().translated(edit->geometry().topLeft());
        QVERIFY2(editorContents.contains(caretInk), "caret-only paint must stay inside actual editor contents");
        QVERIFY2(rightmostColumnPixels >= edit->fontMetrics().height() * 2 / 3,
                 "caret at scrolled draft end must be present at editor far edge");
        for (auto sc : {QStyle::SC_SpinBoxUp, QStyle::SC_SpinBoxDown}) {
            const auto option = spinOption();
            const QRect arrow = widget->style()->subControlRect(QStyle::CC_SpinBox, &option, sc, widget);
            QVERIFY2(!caretInk.intersects(arrow), "caret-only paint must not enter either spinbox arrow hit area");
        }

        if (real) decimal.setValue(50.0);
        else integer.setValue(50);
        const double before = real ? decimal.value() : integer.value();
        const auto click = [&](int x, bool upDirection) {
            QTest::mouseClick(widget, Qt::LeftButton, Qt::NoModifier, QPoint(x, upDirection ? 8 : 24));
        };
        const auto option = spinOption();
        const int seam = widget->style()->subControlRect(QStyle::CC_SpinBox, &option, QStyle::SC_SpinBoxUp, widget).left();
        click(seam - 1, true);
        QVERIFY(edit->hasFocus());
        QCOMPARE(real ? decimal.value() : integer.value(), before);
        click(seam, true);
        QCOMPARE(real ? decimal.value() : integer.value(), before + 1);
        click(seam + 1, true);
        QCOMPARE(real ? decimal.value() : integer.value(), before + 2);
        click(seam, false);
        QCOMPARE(real ? decimal.value() : integer.value(), before + 1);
        click(seam + 1, false);
        QCOMPARE(real ? decimal.value() : integer.value(), before);
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
        QTest::newRow("catppuccin-mocha") << "catppuccin-mocha" << QString("#1E1E2E #181825 #CDD6F4 #BAC2DE #CBA6F7 #11111B #74768A #F38BA8 #A6E3A1 #313244 #F9E2AF #89B4FA").split(' ');
        QTest::newRow("tokyo-night") << "tokyo-night" << QString("#1A1B26 #24283B #C0CAF5 #A9B1D6 #7AA2F7 #1A1B26 #777F9B #F7768E #9ECE6A #292E42 #E0AF68 #7AA2F7").split(' ');
        QTest::newRow("dracula") << "dracula" << QString("#282A36 #21222C #F8F8F2 #BFBFD3 #BD93F9 #282A36 #777B8F #FF6E6E #50FA7B #44475A #F1FA8C #8BE9FD").split(' ');
        QTest::newRow("nord") << "nord" << QString("#2E3440 #3B4252 #ECEFF4 #D8DEE9 #88C0D0 #2E3440 #8992A5 #E7A2AA #A3BE8C #434C5E #EBCB8B #81A1C1").split(' ');
        QTest::newRow("gruvbox-dark") << "gruvbox-dark" << QString("#282828 #3C3836 #EBDBB2 #BDAE93 #FABD2F #282828 #8A8178 #FB7C6D #B8BB26 #504945 #FABD2F #83A598").split(' ');
        QTest::newRow("one-dark") << "one-dark" << QString("#282C34 #21252B #ABB2BF #ABB2BF #61AFEF #21252B #777E8A #E99AA2 #98C379 #3E4451 #E5C07B #61AFEF").split(' ');
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
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
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
