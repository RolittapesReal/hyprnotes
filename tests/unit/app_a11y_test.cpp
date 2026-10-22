#include "app_test_util.h"
#include "ui_polish_test_util.h"
#include "consent_dialog.h"
#include "command_palette.h"
#include "link_rename.h"
#include "market_page.h"
#include "plugin_bridges.h"
#include "plugins_page.h"
#include "settings_dialog.h"
#include "ui_common.h"
#include <hn/theme/theme.h>
#include <hn/theme/theme_io.h>
#include <QAccessible>
#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAbstractScrollArea>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QLayout>
#include <QComboBox>
#include <QLineEdit>
#include <QListWidget>
#include <QInputDialog>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QMessageBox>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeySequenceEdit>
#include <QTimer>
#include <QSet>
#include <QTabWidget>
#include <QtGui/qpa/qwindowsysteminterface.h>

using namespace hn::app;

namespace {
struct ModalProbe {
    QTimer poll, watchdog;
    bool seen = false, expired = false;
    explicit ModalProbe(std::function<void(QDialog *)> inspect) {
        poll.setInterval(10);
        QObject::connect(&poll, &QTimer::timeout, &poll, [this, inspect] {
            if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) {
                seen = true;
                poll.stop();
                watchdog.stop();
                inspect(dialog);
            }
        });
        watchdog.setSingleShot(true);
        QObject::connect(&watchdog, &QTimer::timeout, &poll, [this] {
            expired = true;
            poll.stop();
            if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) dialog->reject();
        });
        poll.start();
        watchdog.start(5000);
    }
};

struct IsolatedScreenGeometry {
    QScreen *screen = QGuiApplication::primaryScreen();
    QRect geometry, available;
    IsolatedScreenGeometry() {
        if (!screen) qFatal("Offscreen QPA has no primary screen");
        geometry = screen->geometry();
        available = screen->availableGeometry();
        QWindowSystemInterface::handleScreenGeometryChange(screen, QRect(0, 0, 1366, 768), QRect(0, 0, 1366, 768));
        QCoreApplication::processEvents();
    }
    ~IsolatedScreenGeometry() {
        QWindowSystemInterface::handleScreenGeometryChange(screen, geometry, available);
        QCoreApplication::processEvents();
    }
};

QList<QWidget *> interactiveWidgets(QWidget &root) {
    QList<QWidget *> result;
    QWidget *widget = &root;
    do {
        widget = widget->nextInFocusChain();
        if (widget == &root) break;
        if (qobject_cast<QTabWidget *>(widget)) continue;
        bool spinEditor = false;
        for (QWidget *parent = widget->parentWidget(); parent && parent != &root; parent = parent->parentWidget())
            if (qobject_cast<QAbstractSpinBox *>(parent) || qobject_cast<QKeySequenceEdit *>(parent)) { spinEditor = true; break; }
        if (spinEditor) continue;
        if (!root.isAncestorOf(widget) || QString::fromLatin1(widget->metaObject()->className()) == "QComboBoxListView") continue;
        if (widget->isEnabled() && widget->focusPolicy() != Qt::NoFocus
            && (widget->focusPolicy() & Qt::TabFocus) && widget->isVisible()) result << widget;
    } while (widget != &root);
    if (root.isEnabled() && root.focusPolicy() != Qt::NoFocus
        && (root.focusPolicy() & Qt::TabFocus)) result.prepend(&root);
    return result;
}

QString accessibleName(QWidget *widget) {
    if (!widget->accessibleName().trimmed().isEmpty()) return widget->accessibleName().trimmed();
    if (auto *iface = QAccessible::queryAccessibleInterface(widget)) return iface->text(QAccessible::Name).trimmed();
    return {};
}

void verifyAccessibleNames(QWidget &root) {
    for (auto *control : root.findChildren<QWidget *>()) {
        if (!control->isVisible() || !control->isEnabled() || control->focusPolicy() == Qt::NoFocus
            || !(control->focusPolicy() & Qt::TabFocus)
            || QString::fromLatin1(control->metaObject()->className()) == "QComboBoxListView") continue;
        QVERIFY2(!accessibleName(control).isEmpty(), qPrintable(QString("unnamed %1 objectName=%2")
            .arg(control->metaObject()->className(), control->objectName())));
    }
}

void verifyNamesAndTabCycle(QWidget &root) {
    root.show(); root.activateWindow();
    QCoreApplication::processEvents();
    const auto controls = interactiveWidgets(root);
    QVERIFY(!controls.isEmpty());
    verifyAccessibleNames(root);

    controls.first()->setFocus(Qt::TabFocusReason);
    QTRY_VERIFY(controls.first()->hasFocus());
        QSet<QWidget *> reached;
    QWidget *start = QApplication::focusWidget();
    QVERIFY(start);
    for (int i = 0; i < controls.size(); ++i) {
        auto *focus = QApplication::focusWidget();
        QVERIFY(focus);
        QVERIFY2(controls.contains(focus), qPrintable(QString("unexpected focus %1 %2")
            .arg(focus->metaObject()->className(), focus->objectName())));
        QVERIFY2(!reached.contains(focus), "tab order repeated a widget before reaching all controls");
        reached.insert(focus);
        QTest::keyClick(focus, Qt::Key_Tab);
        if (QApplication::focusWidget() == start) break;
    }
    QStringList missed;
    for (auto *control : controls) if (!reached.contains(control))
        missed << QString("%1:%2").arg(control->metaObject()->className(), accessibleName(control));
    QVERIFY2(missed.isEmpty(), qPrintable(QString("tab cycle omitted: %1").arg(missed.join(", "))));
    QCOMPARE(QApplication::focusWidget(), start);
}

void sweepResponsivePicker(QDialog *picker, const QString &flow, QStringList &failures, int &checked) {
    auto *fileDialog = qobject_cast<QFileDialog *>(picker);
    QVERIFY(fileDialog);
    verifyNamesAndTabCycle(*picker);
    auto *fileType = picker->findChild<QComboBox *>("fileTypeCombo");
    auto *lookIn = picker->findChild<QComboBox *>("lookInCombo");
    auto *fileNameLabel = picker->findChild<QLabel *>("fileNameLabel");
    auto *fileTypeLabel = picker->findChild<QLabel *>("fileTypeLabel");
    QVERIFY(fileType && lookIn);
    QVERIFY(fileNameLabel && fileTypeLabel);
    QCOMPARE(fileDialog->labelText(QFileDialog::FileName),
        fileDialog->fileMode() == QFileDialog::Directory ? QStringLiteral("&Folder:") : QStringLiteral("&Name:"));
    QCOMPARE(fileDialog->labelText(QFileDialog::FileType), QStringLiteral("&Type:"));
    QCOMPARE(fileType->sizeAdjustPolicy(), QComboBox::AdjustToMinimumContentsLengthWithIcon);
    QCOMPARE(fileType->minimumContentsLength(), 3);
    QCOMPARE(lookIn->sizeAdjustPolicy(), QComboBox::AdjustToMinimumContentsLengthWithIcon);
    QCOMPARE(lookIn->minimumContentsLength(), 10);
    for (auto *combo : {fileType, lookIn}) {
        QVERIFY(combo->count() > 0);
        auto *accessible = QAccessible::queryAccessibleInterface(combo);
        QVERIFY(accessible);
        QCOMPARE(accessible->text(QAccessible::Name), accessibleName(combo));
        QCOMPARE(accessible->text(QAccessible::Value), combo->currentText());
        QCOMPARE(combo->toolTip(), combo->currentText());
        int longestIndex = 0;
        for (int i = 0; i < combo->count(); ++i) {
            const QString fullItem = combo->itemText(i);
            QVERIFY(!fullItem.isEmpty());
            QCOMPARE(combo->model()->data(combo->model()->index(i, combo->modelColumn()), Qt::DisplayRole).toString(), fullItem);
            if (combo->fontMetrics().horizontalAdvance(fullItem) > combo->fontMetrics().horizontalAdvance(combo->itemText(longestIndex)))
                longestIndex = i;
        }
        QVERIFY(combo->view());
        QCOMPARE(combo->view()->model(), combo->model());
        QCOMPARE(combo->view()->model()->data(combo->view()->model()->index(longestIndex, combo->modelColumn()), Qt::DisplayRole).toString(),
            combo->itemText(longestIndex));
        const int previousIndex = combo->currentIndex();
        combo->setFocus(Qt::TabFocusReason);
        combo->setCurrentIndex(0);
        for (int i = 0; i < longestIndex; ++i)
            QTest::keyClick(combo, Qt::Key_Down);
        QCoreApplication::processEvents();
        QCOMPARE(combo->currentIndex(), longestIndex);
        QCOMPARE(combo->currentText(), combo->itemText(longestIndex));
        QCOMPARE(QAccessible::queryAccessibleInterface(combo)->text(QAccessible::Value), combo->currentText());
        QCOMPARE(combo->toolTip(), combo->currentText());
        combo->setCurrentIndex(previousIndex);
        QCOMPARE(combo->toolTip(), combo->currentText());
    }
    for (const auto *name : {"fileNameLabel", "fileTypeLabel"}) {
        auto *label = picker->findChild<QLabel *>(QLatin1String(name));
        QVERIFY(label && label->wordWrap());
        if (fileDialog->fileMode() != QFileDialog::Directory)
            QVERIFY(label->isVisible());
        QCOMPARE(label->text(), name == QLatin1String("fileNameLabel")
            ? fileDialog->labelText(QFileDialog::FileName) : fileDialog->labelText(QFileDialog::FileType));
    }
    auto *screen = QGuiApplication::primaryScreen();
    QVERIFY(screen);
    const qreal originalDpiX = screen->logicalDotsPerInchX();
    const qreal originalDpiY = screen->logicalDotsPerInchY();
    const QStringList palettes{"modernist", "catppuccin-mocha", "tokyo-night", "dracula", "nord", "gruvbox-dark", "one-dark"};
    const QList<qreal> dpis{originalDpiX, 120.0, 144.0};
    checked = 0;
    for (qreal dpi : dpis) {
        QWindowSystemInterface::handleScreenLogicalDotsPerInchChange(screen, dpi, dpi);
        QCoreApplication::processEvents();
        for (const QString &palette : palettes) for (bool dark : {false, true}) for (int px : {8, 14, 18, 32}) {
            auto theme = hn::theme::loadTheme(palette, dark); theme.baseSize = px;
            ui::setTheme(theme); hn::theme::applyTheme(theme);
            QCoreApplication::processEvents();
            QCOMPARE(QApplication::font().pixelSize(), px);
            picker->resize(520, 460);
            QCoreApplication::processEvents();
            int contained = 0, scrollable = 0, clipped = 0;
            for (auto *control : picker->findChildren<QWidget *>()) {
                if (!control->isVisible() || !control->isEnabled() || control->focusPolicy() == Qt::NoFocus
                    || !(control->focusPolicy() & Qt::TabFocus)) continue;
                if (picker->rect().contains(inWidget(control, picker))) { ++contained; continue; }
                bool hasScroll = false;
                for (QWidget *parent = control->parentWidget(); parent && parent != picker; parent = parent->parentWidget())
                    if (qobject_cast<QAbstractScrollArea *>(parent)) { hasScroll = true; break; }
                if (hasScroll) ++scrollable; else ++clipped;
            }
            for (const auto *name : {"fileNameLabel", "fileTypeLabel"}) {
                auto *label = picker->findChild<QLabel *>(QLatin1String(name));
                const bool requiredVisible = qobject_cast<QFileDialog *>(picker)->fileMode() != QFileDialog::Directory;
                if (!requiredVisible && label && !label->isVisible()) continue;
                if (!label || label->width() <= 0 || label->height() < label->fontMetrics().height()
                    || (label->heightForWidth(label->width()) > 0 && label->height() < label->heightForWidth(label->width()))
                    || !picker->rect().contains(inWidget(label, picker)))
                    failures << QString("%1/%2/%3px/%4DPI label=%5 clipped at %6x%7")
                        .arg(flow).arg(palette).arg(dark ? "dark" : "light").arg(px).arg(name)
                        .arg(label ? label->width() : -1).arg(label ? label->height() : -1);
            }
            const bool geometryFits = picker->minimumWidth() <= 520 && picker->minimumHeight() <= 460;
            const QSize minimumHint = picker->minimumSizeHint();
            const QSize layoutMinimum = picker->layout() ? picker->layout()->minimumSize() : QSize();
            qInfo().noquote() << QString("PICKER_EVIDENCE flow=%1 palette=%2 scheme=%3 px=%4 font=%5 fontPx=%6 screenDpi=%7 requested=520x460 actual=%8x%9 minimum=%10x%11 sizeHint=%12x%13 layoutMinimum=%14x%15 contained=%16 scrollable=%17 clipped=%18 criterion=%19")
                .arg(flow).arg(palette).arg(dark ? "dark" : "light").arg(px).arg(QApplication::font().family())
                .arg(QApplication::font().pixelSize()).arg(screen->logicalDotsPerInchX())
                .arg(picker->width()).arg(picker->height()).arg(picker->minimumWidth()).arg(picker->minimumHeight())
                .arg(minimumHint.width()).arg(minimumHint.height()).arg(layoutMinimum.width()).arg(layoutMinimum.height())
                .arg(contained).arg(scrollable).arg(clipped).arg(geometryFits ? "MET" : "UNMET");
            ++checked;
            if (picker->minimumWidth() > 520 || minimumHint.width() > 520 || layoutMinimum.width() > 520
                || picker->minimumHeight() > 460 || picker->width() > 520 || picker->height() > 460 || clipped)
                failures << QString("%1/%2/%3px/%4DPI min=%5 hint=%6 layout=%7 actual=%8x%9 clipped=%10")
                    .arg(flow).arg(palette).arg(dark ? "dark" : "light").arg(px).arg(screen->logicalDotsPerInchX())
                    .arg(picker->minimumWidth()).arg(minimumHint.width()).arg(layoutMinimum.width())
                    .arg(picker->width()).arg(picker->height()).arg(clipped);
        }
    }
    QWindowSystemInterface::handleScreenLogicalDotsPerInchChange(screen, originalDpiX, originalDpiY);
    QCoreApplication::processEvents();
    QCOMPARE(checked, dpis.size() * palettes.size() * 2 * 4);
}

void verifyModalLayoutAtSupportedSizes(QDialog *dialog) {
    const QSize requested(520, 460);
    // The consent dialog is allowed to grow past 460 px so the security warning stays fully visible at large
    // fonts, but never past 90% of the usable screen height.
    QSize limit = requested;
    const bool consent = qobject_cast<ConsentDialog *>(dialog) != nullptr;
    if (consent && dialog->screen()) limit.setHeight(dialog->screen()->availableGeometry().height() * 9 / 10);
    for (int px : {8, 14, 18, 32}) {
        auto theme = hn::theme::loadTheme("modernist", false);
        theme.baseSize = px;
        ui::setTheme(theme);
        hn::theme::applyTheme(theme);
        QCoreApplication::processEvents();
        dialog->resize(requested);
        QCoreApplication::processEvents();
        QVERIFY2(dialog->width() <= limit.width() && dialog->height() <= limit.height()
                 && dialog->minimumWidth() <= limit.width() && dialog->minimumHeight() <= limit.height(),
            qPrintable(QString("%1px dialog geometry %2x%3 (minimum %4x%5) exceeds %6x%7")
                .arg(px).arg(dialog->width()).arg(dialog->height()).arg(dialog->minimumWidth()).arg(dialog->minimumHeight())
                .arg(limit.width()).arg(limit.height())));
        if (consent) {
            // Fully visible whenever the screen has room (every size except the largest font with the native
            // panel on a ~800x600 screen); otherwise it must still show at least three lines and be keyboard-scrollable.
            auto *area = dialog->findChild<QScrollArea *>("hnConsentWarningDetails");
            QVERIFY(area);
            QVERIFY(area->focusPolicy() != Qt::NoFocus);
            const int lines = area->viewport()->height() / qMax(1, dialog->fontMetrics().height());
            QVERIFY2(lines >= 3, qPrintable(QString("%1px warning shows %2 lines").arg(px).arg(lines)));
            if (px <= 18)
                QVERIFY2(area->verticalScrollBar()->maximum() == 0,
                    qPrintable(QString("%1px security warning clipped (max %2)").arg(px).arg(area->verticalScrollBar()->maximum())));
        }
        for (auto *control : dialog->findChildren<QWidget *>()) {
            if (!control->isVisible() || !control->isEnabled() || control->focusPolicy() == Qt::NoFocus
                || !(control->focusPolicy() & Qt::TabFocus)) continue;
            QScrollArea *area = nullptr;
            for (QWidget *parent = control->parentWidget(); parent && parent != dialog; parent = parent->parentWidget())
                if (auto *scroll = qobject_cast<QScrollArea *>(parent)) { area = scroll; break; }
            if (area) {
                area->ensureWidgetVisible(control);
                QVERIFY2(area->viewport()->rect().intersects(inWidget(control, area->viewport())),
                    qPrintable(QString("%1px unreachable in scroll area: %2").arg(px).arg(accessibleName(control))));
            } else {
                QVERIFY2(dialog->rect().contains(inWidget(control, dialog)), qPrintable(QString("%1px clipped %2")
                    .arg(px).arg(accessibleName(control))));
            }
        }
    }
    auto theme = hn::theme::loadTheme("modernist", false);
    ui::setTheme(theme);
    hn::theme::applyTheme(theme);
}

void verifyModalNamesAndCycle(QDialog *dialog) {
    verifyNamesAndTabCycle(*dialog);
    verifyModalLayoutAtSupportedSizes(dialog);
}

void verifyFilePickerFlow(const QString &flow, const std::function<void()> &launch) {
    bool inspected = false;
    QStringList failures;
    int checked = 0;
    ModalProbe probe([&](QDialog *picker) {
        inspected = true;
        sweepResponsivePicker(picker, flow, failures, checked);
        QTest::keyClick(picker, Qt::Key_Escape);
    });
    launch();
    QVERIFY(probe.seen && !probe.expired && inspected);
    QCOMPARE(checked, 168);
    QVERIFY2(failures.isEmpty(), qPrintable(failures.join("\n")));
}

double contrast(const QColor &a, const QColor &b) { return hn::theme::contrastRatio(a, b); }
}

class AppA11yTest : public QObject {
    Q_OBJECT
private slots:
    void init() {
        const auto theme = hn::theme::loadTheme("modernist", false);
        ui::setTheme(theme);
        hn::theme::applyTheme(theme);
    }

    void settingsControlsHaveNamesAndCompleteTabCycle() {
        PolishProfile profile;
        apptest::Lib lib;
        AppController controller(lib.opts());
        SettingsDialog dialog(&controller);
        dialog.resize(520, 460);
        dialog.show();
        for (int index = 0; index < dialog.tabs()->count(); ++index) {
            dialog.showTab(index);
            QCoreApplication::processEvents();
            verifyAccessibleNames(dialog);
            verifyNamesAndTabCycle(dialog);
        }
        dialog.showTab(0);
        verifyNamesAndTabCycle(dialog);
    }

    void settingsPagesStayWithinNarrowLayoutAtEverySupportedFontSize() {
        PolishProfile profile;
        apptest::Lib lib;
        AppController controller(lib.opts());
        SettingsDialog dialog(&controller);
        dialog.resize(520, 460);
        dialog.show();
        for (int px : {8, 14, 18, 32}) {
            auto settings = controller.settings();
            settings.fontSize = px;
            auto prefs = controller.prefs(); prefs.fontSize = px;
            controller.applySettings(settings, prefs);
            for (int index = 0; index < dialog.tabs()->count(); ++index) {
                dialog.showTab(index);
                QCoreApplication::processEvents();
                auto *scroll = qobject_cast<QScrollArea *>(dialog.tabs()->widget(index));
                if (!scroll) continue;
                QCOMPARE(scroll->horizontalScrollBar()->maximum(), 0);
                QVERIFY2(scroll->widget()->width() <= scroll->viewport()->width(),
                    qPrintable(QString("font %1 tab %2 content exceeds viewport").arg(px).arg(index)));
            }
            QCOMPARE(dialog.size(), QSize(520, 460));
        }
    }

    void pluginPagesExposeNamesAndCompleteTabCycle() {
        PolishProfile profile;
        apptest::Lib lib;
        AppController controller(lib.opts());
        PluginsPage page(&controller);
        page.resize(520, 460);
        page.show();
        verifyNamesAndTabCycle(page);
        page.showBrowse();
        QCoreApplication::processEvents();
        auto *browse = page.browse();
        QVERIFY(browse);
        verifyAccessibleNames(page);
        verifyNamesAndTabCycle(page);
        for (int px : {8, 14, 18, 32}) {
            auto settings = controller.settings(); settings.fontSize = px;
            auto prefs = controller.prefs(); prefs.fontSize = px;
            controller.applySettings(settings, prefs);
            QCoreApplication::processEvents();
            verifyAccessibleNames(page);
            QVERIFY(page.width() <= 520 && page.height() <= 460);
            QWidget *browseContainer = browse->parentWidget();
            QVERIFY(browseContainer);
            QVERIFY2(browseContainer->rect().contains(inWidget(browse, browseContainer)),
                qPrintable(QString("Browse child escaped %1 container").arg(browseContainer->metaObject()->className())));
        }
    }

    void consentControlsHaveNamesSafeEscapeAndDefault() {
        PolishProfile profile;
        ConsentRequest request;
        request.id = "a11y-fixture"; request.name = "A11y fixture";
        request.version = "1"; request.author = "tests"; request.sha256 = QString(64, 'a');
        request.permissions = {"notes.read", "network"};
        ConsentDialog dialog(request, nullptr, 60000);
        verifyNamesAndTabCycle(dialog);
        QVERIFY(dialog.cancelButton()->isDefault());
        dialog.show();
        QTest::keyClick(&dialog, Qt::Key_Escape);
        QTRY_VERIFY(!dialog.isVisible());
    }

    void nativeConsentControlsHaveNamesAndSafeEscape() {
        PolishProfile profile;
        ConsentRequest request;
        request.id = "native-a11y"; request.name = "Native fixture";
        request.version = "1"; request.author = "tests"; request.sha256 = QString(64, 'b');
        request.permissions = {"native"}; request.native = true;
        ConsentDialog dialog(request, nullptr, 60000);
        verifyModalNamesAndCycle(&dialog);
        QVERIFY(dialog.cancelButton()->isDefault());
        QVERIFY(dialog.nativePanel());
        QTest::keyClick(&dialog, Qt::Key_Escape);
        QTRY_VERIFY(!dialog.isVisible());
    }

    void consentUsesNinetyPercentOf1366By768OffscreenScreen() {
        PolishProfile profile;
        IsolatedScreenGeometry screen;
        QCOMPARE(screen.screen->availableGeometry(), QRect(0, 0, 1366, 768));
        ConsentRequest request;
        request.id = "large-screen-a11y"; request.name = "Long review fixture";
        request.version = "1"; request.author = "tests"; request.sha256 = QString(64, 'c');
        request.description = QString("Detailed plugin review content.\n").repeated(30);
        request.permissions = {"notes.read", "network", "notes.write", "filesystem.write"};
        request.reconsentReason = "Review updated permissions and package content.";
        ConsentDialog dialog(request, nullptr, 60000);
        dialog.resize(520, 1080);
        dialog.show();
        QTRY_VERIFY(dialog.isVisible());
        const int cap = 768 * 9 / 10;
        QVERIFY2(dialog.height() <= cap, qPrintable(QString("consent %1px exceeds 90%% cap %2")
            .arg(dialog.height()).arg(cap)));
        auto *details = dialog.findChild<QScrollArea *>("hnConsentDetails");
        QVERIFY(details);
        QTRY_VERIFY(details->verticalScrollBar()->maximum() > 0);
        const QRect cancelRect = inWidget(dialog.cancelButton(), &dialog);
        const QRect approveRect = inWidget(dialog.approveButton(), &dialog);
        QVERIFY2(dialog.rect().contains(cancelRect), qPrintable(QString("cancel at %1,%2 %3x%4 outside %5x%6")
            .arg(cancelRect.x()).arg(cancelRect.y()).arg(cancelRect.width()).arg(cancelRect.height()).arg(dialog.width()).arg(dialog.height())));
        QVERIFY2(dialog.rect().contains(approveRect), qPrintable(QString("approve at %1,%2 %3x%4 outside %5x%6")
            .arg(approveRect.x()).arg(approveRect.y()).arg(approveRect.width()).arg(approveRect.height()).arg(dialog.width()).arg(dialog.height())));
        QVERIFY(dialog.cancelButton()->isDefault() && !dialog.approveButton()->isDefault());
        dialog.reject();
    }

    void commandPaletteSearchHasNameAndEscapeDismisses() {
        PolishProfile profile;
        QWidget host;
        host.resize(520, 460); host.show();
        QLineEdit previous(&host); previous.show(); previous.setFocus();
        QList<CommandPalette::Item> items;
        for (int i = 0; i < 12; ++i) items << CommandPalette::Item{QString::number(i), QString("Fixture command %1").arg(i), "Fixture", {}};
        auto *palette = CommandPalette::toggle(&host, items, [](const QString &) {});
        QVERIFY(palette);
        QCOMPARE(accessibleName(palette->input()), QString("Search plugin commands"));
        QCOMPARE(accessibleName(palette), QString("Command palette"));
        for (int px : {8, 14, 18, 32}) {
            auto theme = hn::theme::loadTheme("modernist", false); theme.baseSize = px;
            hn::theme::applyTheme(theme);
            QCoreApplication::processEvents();
            QVERIFY(host.rect().contains(inWidget(palette, &host)));
            QVERIFY(palette->rect().contains(inWidget(palette->input(), palette)));
            QVERIFY(palette->height() <= 460 && palette->width() <= 520);
        }
        QTest::keyClick(palette->input(), Qt::Key_Escape);
        QTRY_VERIFY(!palette->isVisible());
        QTRY_VERIFY(previous.hasFocus());
    }

    void firstRunFolderDialogHasNamedControlsAndEscapeCancels() {
        PolishProfile profile;
        bool inspected = false;
        ModalProbe probe([&](QDialog *dialog) {
            inspected = true;
            verifyModalNamesAndCycle(dialog);
            QTest::keyClick(dialog, Qt::Key_Escape);
        });
        QCOMPARE(runFirstRunDialog(profile.dir.filePath("notes")), QString());
        QVERIFY(probe.seen && !probe.expired && inspected);
    }

    void themeImportPickerExposesKeyboardControlsAndGeometry() {
        PolishProfile profile;
        apptest::Lib lib;
        AppController controller(lib.opts());
        SettingsDialog settings(&controller);
        settings.resize(520, 460); settings.show();
        QPushButton *import = nullptr;
        for (auto *button : settings.findChildren<QPushButton *>()) {
            if (button->text().contains("Import theme")) import = button;
        }
        QVERIFY(import);
        const auto beforeSettings = controller.settings();
        verifyFilePickerFlow("theme-import", [&] { import->click(); });
        QCOMPARE(controller.settings(), beforeSettings);
    }

    void themeExportPickerExposesKeyboardControlsAndGeometry() {
        PolishProfile profile;
        apptest::Lib lib;
        AppController controller(lib.opts());
        SettingsDialog settings(&controller);
        settings.resize(520, 460); settings.show();
        QPushButton *exportTheme = nullptr;
        for (auto *button : settings.findChildren<QPushButton *>())
            if (button->text().contains("Export current theme")) exportTheme = button;
        QVERIFY(exportTheme);
        verifyFilePickerFlow("theme-export", [&] { exportTheme->click(); });
        QVERIFY(!QFileInfo::exists(profile.dir.filePath("modernist.json")));
    }

    void notesFolderPickerExposesKeyboardControlsAndGeometry() {
        PolishProfile profile;
        apptest::Lib lib;
        AppController controller(lib.opts());
        SettingsDialog settings(&controller);
        settings.resize(520, 460); settings.show();
        settings.showTab(1);
        QCoreApplication::processEvents();
        QPushButton *folderBrowse = nullptr;
        for (auto *button : settings.tabs()->widget(1)->findChildren<QPushButton *>())
            if (button->text() == "Browse…") folderBrowse = button;
        QVERIFY(folderBrowse);
        const QString originalFolder = controller.notesRoot();
        verifyFilePickerFlow("notes-folder-browse", [&] { folderBrowse->click(); });
        QCOMPARE(controller.notesRoot(), originalFolder);
    }

    void pluginFilePickerExposesKeyboardControlsAndGeometry() {
        PolishProfile profile;
        apptest::Lib lib;
        auto options = lib.opts();
        options.pluginHooks.consent = [](const ConsentRequest &) { return false; };
        AppController pluginController(options);
        PluginsPage plugins(&pluginController); plugins.resize(520, 460); plugins.show();
        auto *install = plugins.button("install");
        QVERIFY(install && install->menu());
        QAction *fileAction = nullptr;
        for (auto *action : install->menu()->actions()) {
            if (action->text().contains("Plugin file")) fileAction = action;
        }
        QVERIFY(fileAction);
        verifyFilePickerFlow("plugin-file-install", [&] { fileAction->trigger(); });
        QVERIFY(!QFileInfo::exists(lib.dir.filePath("plugins/hello/plugin.json")));
    }

    void pluginFolderPickerExposesKeyboardControlsAndGeometry() {
        PolishProfile profile;
        apptest::Lib lib;
        auto options = lib.opts();
        options.pluginHooks.consent = [](const ConsentRequest &) { return false; };
        AppController pluginController(options);
        PluginsPage plugins(&pluginController); plugins.resize(520, 460); plugins.show();
        auto *install = plugins.button("install");
        QVERIFY(install && install->menu());
        QAction *folderAction = nullptr;
        for (auto *action : install->menu()->actions())
            if (action->text().contains("Plugin folder")) folderAction = action;
        QVERIFY(folderAction);
        verifyFilePickerFlow("plugin-folder-install", [&] { folderAction->trigger(); });
        QVERIFY(!QFileInfo::exists(lib.dir.filePath("plugins/hello/plugin.json")));
    }

    void firstRunFolderPickerExposesKeyboardControlsAndGeometry() {
        PolishProfile profile;
        bool firstRunSeen = false, nestedPickerSeen = false, nestedPickerExpired = false, browseFound = false;
        QStringList firstRunPickerFailures;
        ModalProbe firstRunProbe([&](QDialog *dialog) {
            firstRunSeen = true;
            QPushButton *browse = nullptr;
            for (auto *button : dialog->findChildren<QPushButton *>())
                if (button->text() == "Browse…") browse = button;
            browseFound = browse != nullptr;
            if (browse) {
                int checked = 0;
                ModalProbe nestedPicker([&](QDialog *picker) {
                    nestedPickerSeen = true;
                    sweepResponsivePicker(picker, "first-run-folder-browse", firstRunPickerFailures, checked);
                    QTest::keyClick(picker, Qt::Key_Escape);
                });
                browse->click();
                nestedPickerExpired = nestedPicker.expired;
                if (!nestedPickerSeen) nestedPickerExpired = true;
            }
            QTest::keyClick(dialog, Qt::Key_Escape);
        });
        QCOMPARE(runFirstRunDialog(profile.dir.filePath("notes")), QString());
        QVERIFY(firstRunSeen && browseFound && nestedPickerSeen && !nestedPickerExpired);
        QVERIFY2(firstRunPickerFailures.isEmpty(), qPrintable(firstRunPickerFailures.join("\n")));
    }

    void linkRenameConfirmationDefaultsToCancelAndEscapeIsSafe() {
        PolishProfile profile;
        bool safeDefault = false, named = false;
        ModalProbe probe([&](QDialog *dialog) {
            verifyModalNamesAndCycle(dialog);
            for (auto *button : dialog->findChildren<QPushButton *>())
                if (button->text() == "Cancel") safeDefault = button->isDefault();
            named = dialog->findChild<QLabel *>(QString(), Qt::FindChildrenRecursively) != nullptr;
            QTest::keyClick(dialog, Qt::Key_Escape);
        });
        QCOMPARE(askLinkUpdate(nullptr, "Rename a note", {"linked.md - 1 link"}), LinkChoice::Cancel);
        QVERIFY(probe.seen && !probe.expired && named && safeDefault);
    }

    void pluginBridgePromptEscapeCancelsAndNamesInput() {
        PolishProfile profile;
        apptest::Lib lib;
        AppController controller(lib.opts());
        PluginUiBridge bridge(&controller.plugins());
        bool inputNamed = false;
        ModalProbe probe([&](QDialog *dialog) {
            verifyModalNamesAndCycle(dialog);
            if (auto *input = dialog->findChild<QLineEdit *>()) inputNamed = !accessibleName(input).isEmpty();
            QTest::keyClick(dialog, Qt::Key_Escape);
        });
        QVERIFY(!bridge.prompt("fixture", "Prompt title", "Prompt label", "default").has_value());
        QVERIFY(probe.seen && !probe.expired && inputNamed);
    }

    void pluginBridgeConfirmEscapeUsesSafeDefault() {
        PolishProfile profile;
        apptest::Lib lib;
        AppController controller(lib.opts());
        PluginUiBridge bridge(&controller.plugins());
        bool safeDefault = false;
        ModalProbe probe([&](QDialog *dialog) {
            verifyModalNamesAndCycle(dialog);
            for (auto *button : dialog->findChildren<QPushButton *>())
                if (button->text() == "Cancel") safeDefault = button->isDefault();
            QTest::keyClick(dialog, Qt::Key_Escape);
        });
        QVERIFY(!bridge.confirm("fixture", "Continue with operation?"));
        QVERIFY(probe.seen && !probe.expired && safeDefault);
    }

    void renameInputEscapeLeavesNoteUnchanged() {
        PolishProfile profile;
        apptest::Lib lib; lib.write("rename-target.md", "original\n");
        AppController controller(lib.opts());
        QWidget parent;
        bool inputNamed = false;
        ModalProbe probe([&](QDialog *dialog) {
            verifyModalNamesAndCycle(dialog);
            if (auto *input = dialog->findChild<QLineEdit *>()) inputNamed = !accessibleName(input).isEmpty();
            QTest::keyClick(dialog, Qt::Key_Escape);
        });
        controller.promptRename("rename-target.md", &parent);
        QVERIFY(probe.seen && !probe.expired && inputNamed);
        QCOMPARE(lib.read("rename-target.md"), QByteArray("original\n"));
    }

    void tagInputEscapeLeavesNoteUnchanged() {
        PolishProfile profile;
        apptest::Lib lib; lib.write("tag-target.md", "# Original\n\nBody\n");
        AppController controller(lib.opts());
        QWidget parent;
        bool inputNamed = false;
        ModalProbe probe([&](QDialog *dialog) {
            verifyModalNamesAndCycle(dialog);
            if (auto *input = dialog->findChild<QLineEdit *>()) inputNamed = !accessibleName(input).isEmpty();
            QTest::keyClick(dialog, Qt::Key_Escape);
        });
        controller.promptEditTags("tag-target.md", &parent);
        QVERIFY(probe.seen && !probe.expired && inputNamed);
        QCOMPARE(lib.read("tag-target.md"), QByteArray("# Original\n\nBody\n"));
    }

    void deleteConfirmationHasSafeDefaultAndEscapeKeepsFile() {
        PolishProfile profile;
        apptest::Lib lib; lib.write("keep.md", "disk\n");
        auto options = lib.opts(); options.confirm = {};
        AppController controller(options);
        QMenu menu; controller.populateNoteMenu(&menu, "keep.md", true);
        QAction *remove = nullptr;
        for (auto *action : menu.actions()) if (action->text().contains("Delete")) remove = action;
        QVERIFY(remove);
        bool safe = false;
        ModalProbe probe([&](QDialog *dialog) {
            verifyModalNamesAndCycle(dialog);
            auto *box = qobject_cast<QMessageBox *>(dialog);
            QVERIFY(box);
            QPushButton *cancel = nullptr;
            for (auto *button : box->findChildren<QPushButton *>()) if (button->text() == "Cancel") cancel = button;
            safe = cancel && box->defaultButton() == cancel && box->escapeButton() == cancel;
            QTest::keyClick(dialog, Qt::Key_Escape);
        });
        remove->trigger();
        QVERIFY(probe.seen && !probe.expired && safe);
        QCOMPARE(lib.read("keep.md"), QByteArray("disk\n"));
    }

    void pluginRemovalConfirmationHasSafeDefaultAndEscapeKeepsPlugin() {
        PolishProfile profile;
        apptest::Lib lib;
        auto options = lib.opts();
        options.pluginHooks.consent = [](const ConsentRequest &) { return true; };
        AppController controller(options);
        PluginsPage page(&controller);
        page.resize(520, 460); page.show();
        QVERIFY(page.installPath(QString::fromUtf8(HN_PLUGIN_FIXTURES) + "/hello"));
        page.select("hello");
        bool safe = false;
        ModalProbe probe([&](QDialog *dialog) {
            verifyModalNamesAndCycle(dialog);
            auto *box = qobject_cast<QMessageBox *>(dialog);
            QVERIFY(box);
            auto *no = box->button(QMessageBox::No);
            safe = no && box->defaultButton() == no && box->escapeButton() == no;
            QTest::keyClick(dialog, Qt::Key_Escape);
        });
        QVERIFY(page.button("remove"));
        page.button("remove")->click();
        QVERIFY(probe.seen && !probe.expired && safe);
        QVERIFY(QFile::exists(lib.dir.filePath("plugins/hello/plugin.json")));
    }

    void themeRemovalConfirmationDefaultsToNoAndEscapeKeepsTheme() {
        PolishProfile profile;
        apptest::Lib lib;
        AppController controller(lib.opts());
        SettingsDialog dialog(&controller);
        const QString exported = profile.dir.filePath("theme-copy.json");
        QVERIFY(dialog.exportThemeTo(exported));
        QVERIFY(dialog.importThemeFile(exported));
        const QString themeId = controller.settings().theme;
        bool safe = false;
        ModalProbe probe([&](QDialog *active) {
            verifyModalNamesAndCycle(active);
            auto *box = qobject_cast<QMessageBox *>(active);
            QVERIFY(box);
            auto *no = box->button(QMessageBox::No);
            safe = no && box->defaultButton() == no && box->escapeButton() == no;
            QTest::keyClick(active, Qt::Key_Escape);
        });
        dialog.removeThemeButton()->click();
        QVERIFY(probe.seen && !probe.expired && safe);
        QCOMPARE(controller.settings().theme, themeId);
    }

    void recoveryConfirmationDefaultsToLaterAndEscapePreservesDraft() {
        PolishProfile profile;
        apptest::Lib lib; lib.write("recover.md", "disk\n");
        {
            hn::core::NoteRepository repository(lib.notes, lib.state + "/recovery");
            const QByteArray disk("disk\n");
            QString error;
            QVERIFY(repository.recovery().writePending(repository.root(), "recover.md", "local\n", &disk, 1, &error));
        }
        auto options = lib.opts(); options.recoveryPrompt = {};
        options.chooseNotesFolder = [&lib](const QString &) { return lib.notes; };
        AppController controller(options);
        bool safe = false;
        ModalProbe probe([&](QDialog *dialog) {
            verifyModalNamesAndCycle(dialog);
            auto *box = qobject_cast<QMessageBox *>(dialog);
            QVERIFY(box);
            QPushButton *later = nullptr;
            for (auto *button : box->findChildren<QPushButton *>()) if (button->text() == "Later") later = button;
            safe = later && box->defaultButton() == later && box->escapeButton() == later;
            QTest::keyClick(dialog, Qt::Key_Escape);
        });
        QVERIFY(controller.handleAction(hn::platform::Action::ShowOrganizer));
        QVERIFY(probe.seen && !probe.expired && safe);
        QCOMPARE(lib.read("recover.md"), QByteArray("disk\n"));
        QVERIFY(!controller.repo().recoverableDrafts().isEmpty());
    }

    void duplicatePluginReplacementDefaultsToNoAndEscapeKeepsInstalledPlugin() {
        PolishProfile profile;
        apptest::Lib lib;
        auto options = lib.opts();
        options.pluginHooks.consent = [](const ConsentRequest &) { return false; };
        AppController controller(options);
        const QString source = QString::fromUtf8(HN_PLUGIN_FIXTURES) + "/hello";
        const auto first = controller.plugins().install(source, nullptr, nullptr);
        QVERIFY(first.ok);
        bool safe = false;
        ModalProbe probe([&](QDialog *dialog) {
            verifyModalNamesAndCycle(dialog);
            auto *box = qobject_cast<QMessageBox *>(dialog);
            QVERIFY(box);
            auto *no = box->button(QMessageBox::No);
            safe = no && box->defaultButton() == no && box->escapeButton() == no;
            QTest::keyClick(dialog, Qt::Key_Escape);
        });
        const auto replacement = controller.plugins().install(source, nullptr, nullptr);
        QVERIFY(probe.seen && !probe.expired && safe);
        QVERIFY(!replacement.ok);
        QVERIFY(QFile::exists(lib.dir.filePath("plugins/hello/plugin.json")));
    }

    void pluginAuditDialogNamesControlsAndEscapeCloses() {
        PolishProfile profile;
        apptest::Lib lib;
        AppController controller(lib.opts());
        PluginsPage page(&controller);
        page.showAudit({});
        auto *dialog = page.findChild<QDialog *>("hnAuditDialog");
        QVERIFY(dialog);
        verifyModalNamesAndCycle(dialog);
        QTest::keyClick(dialog, Qt::Key_Escape);
        QTRY_VERIFY(!dialog->isVisible());
    }

    void pluginBridgePickEscapeCancelsAndNamesList() {
        PolishProfile profile;
        apptest::Lib lib;
        AppController controller(lib.opts());
        PluginUiBridge bridge(&controller.plugins());
        bool listNamed = false;
        ModalProbe probe([&](QDialog *dialog) {
            verifyModalNamesAndCycle(dialog);
            if (auto *list = dialog->findChild<QListWidget *>()) listNamed = !accessibleName(list).isEmpty();
            QTest::keyClick(dialog, Qt::Key_Escape);
        });
        QCOMPARE(bridge.pick("fixture", "Pick item", {"one", "two"}), -1);
        QVERIFY(probe.seen && !probe.expired && listNamed);
    }

    void builtInThemeTextAndIndicatorPairsMeetContrastThresholds() {
        PolishProfile profile;
        const QStringList builtins = hn::theme::listThemes();
        QVERIFY(!builtins.isEmpty());
        for (const QString &id : builtins) for (bool dark : {false, true}) {
            const auto theme = hn::theme::loadTheme(id, dark);
            QVERIFY2(hn::theme::lastThemeError().isEmpty(), qPrintable(id));
            const QString context = QString("%1/%2").arg(id, dark ? "dark" : "light");
            for (const QColor &background : {theme.bg, theme.surface}) {
                QVERIFY2(contrast(theme.text, background) >= 4.5, qPrintable(context + " text/background"));
                QVERIFY2(contrast(theme.muted, background) >= 4.5, qPrintable(context + " muted/background"));
                QVERIFY2(contrast(theme.danger, background) >= 4.5, qPrintable(context + " danger/background"));
                QVERIFY2(contrast(theme.success, background) >= 4.5, qPrintable(context + " success/background"));
            }
            QVERIFY2(contrast(theme.accentText, theme.accent) >= 4.5, qPrintable(context + " accent text/chip"));
            QVERIFY2(contrast(theme.text, theme.selection) >= 3.0, qPrintable(context + " selection indicator"));
            QVERIFY2(contrast(theme.border, theme.surface) >= 3.0, qPrintable(context + " border indicator"));
            QVERIFY2(contrast(theme.danger, theme.surface) >= 3.0, qPrintable(context + " danger indicator"));
            QVERIFY2(contrast(theme.accent, theme.bg) >= 3.0, qPrintable(context + " accent/focus indicator"));
            for (int i = 0; i < 6; ++i)
                QVERIFY2(contrast(theme.noteAccent[i], theme.bg) >= 3.0,
                    qPrintable(QString("%1 note chip %2").arg(context).arg(i)));
        }
    }
};

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    AppA11yTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "app_a11y_test.moc"
