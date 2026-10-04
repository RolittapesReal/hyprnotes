#include "app_test_util.h"
#include "ui_polish_test_util.h"
#include "consent_dialog.h"
#include "plugins_page.h"
#include "plugin_bridges.h"
#include "settings_dialog.h"
#include "link_rename.h"
#include "ui_common.h"
#include <hn/core/note_repository.h>
#include <QDialogButtonBox>
#include <QCheckBox>
#include <QElapsedTimer>
#include <QFontDatabase>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QScreen>
#include <QTimer>
#include <algorithm>

using namespace hn::app;

namespace {
void scheme(AppController &c, const QString &name, int size = 0) {
    auto s = c.settings(); s.colorScheme = name; s.fontSize = size;
    auto p = c.prefs(); p.fontSize = size;
    c.applySettings(s, p);
}
QString plugin(apptest::Lib &lib, const QString &id = "style-fixture", const QString &name = {},
               const QByteArray &lua = "hn.setting{ id='draft', type='string', default='saved', title='Draft value' }\n") {
    const QString dir = lib.dir.filePath("source/" + id);
    if (!QDir().mkpath(dir)) qFatal("Cannot create plugin fixture");
    QFile manifest(dir + "/plugin.json"), script(dir + "/main.lua");
    if (!manifest.open(QIODevice::WriteOnly) || !script.open(QIODevice::WriteOnly)) qFatal("Cannot write plugin fixture");
    manifest.write(QJsonDocument(QJsonObject{{"id", id}, {"name", name.isEmpty() ? (id == "style-fixture" ? "Style Fixture" : id) : name}, {"version", "1.0.0"},
        {"author", "tests"}, {"description", "test"}, {"api", 2}, {"tier", "script"}, {"entry", "main.lua"},
        {"permissions", QJsonArray{}}, {"min_app", "0.1.0"}}).toJson());
    script.write(lua);
    return dir;
}
QPushButton *button(QWidget *w, const QString &text) {
    for (auto *b : w->findChildren<QPushButton *>())
        if (QString(b->text()).remove('&') == text) return b;
    return nullptr;
}
QLabel *label(QWidget *w, const QString &text) {
    for (auto *l : w->findChildren<QLabel *>()) if (l->text() == text) return l;
    return nullptr;
}
// Every modal test releases the real dialog even when its inspection fails.
struct ModalProbe {
    QTimer poll, watchdog;
    bool seen = false, expired = false;
    explicit ModalProbe(std::function<bool(QDialog *)> inspect) {
        poll.setInterval(10);
        QObject::connect(&poll, &QTimer::timeout, &poll, [this, inspect] {
            if (auto *d = qobject_cast<QDialog *>(QApplication::activeModalWidget())) {
                seen = true;
                if (inspect(d)) poll.stop();
            }
        });
        watchdog.setSingleShot(true);
        QObject::connect(&watchdog, &QTimer::timeout, &poll, [this] {
            expired = true; poll.stop();
            if (auto *d = qobject_cast<QDialog *>(QApplication::activeModalWidget())) d->reject();
        });
        poll.start(); watchdog.start(4000);
    }
};
ConsentRequest request() {
    ConsentRequest r;
    r.id = "review-fixture"; r.name = "Review fixture"; r.version = "1.0.0"; r.author = "tests";
    r.source = "/isolated/plugin"; r.sha256 = QString(64, 'a'); r.permissions = {"notes.read", "network"};
    return r;
}
}

class DialogPolishTest : public QObject {
    Q_OBJECT
private slots:
    void pluginRestylesInPlaceWithoutReloadOrLostEditingState() {
        PolishProfile profile;
        apptest::Lib lib;
        auto options = lib.opts(); options.pluginHooks.consent = [](const ConsentRequest &) { return true; };
        AppController controller(options);
        scheme(controller, "dark");
        QVERIFY(controller.plugins().install(plugin(lib), nullptr, nullptr).ok);
        for (int i = 0; i < 8; ++i) QVERIFY(controller.plugins().install(plugin(lib, QString("other-%1").arg(i)), nullptr, nullptr).ok);
        PluginsPage page(&controller);
        page.resize(840, 360); page.show(); page.select("style-fixture");
        QVERIFY(QTest::qWaitForWindowExposed(&page));
        page.activateWindow();
        QLineEdit *draft = nullptr;
        for (auto *e : page.settingEditors()) if (e->property("settingId").toString() == "draft") draft = qobject_cast<QLineEdit *>(e);
        QVERIFY(draft);
        QPointer<QLineEdit> sameEditor(draft);
        draft->setFocus();
        QTRY_VERIFY(draft->hasFocus());
        draft->setText("uncommitted text"); draft->setSelection(2, 5);
        auto *warning = page.findChild<QWidget *>("hnPluginsWarning");
        QVERIFY(warning);
        QSignalSpy changed(&controller.plugins(), &PluginService::changed);
        QMap<QScrollBar *, int> positions;
        for (auto *scroll : page.findChildren<QAbstractScrollArea *>()) {
            scroll->verticalScrollBar()->setValue(qMin(20, scroll->verticalScrollBar()->maximum()));
            positions.insert(scroll->verticalScrollBar(), scroll->verticalScrollBar()->value());
        }
        QVERIFY(std::any_of(positions.cbegin(), positions.cend(), [](int value) { return value > 0; }));
        QVERIFY(page.list()->verticalScrollBar()->value() > 0);
        for (const QString &s : {QString("dark"), QString("light"), QString("dark")}) {
            scheme(controller, s);
            QTRY_VERIFY(sameEditor);
            QCOMPARE(draft->text(), QString("uncommitted text"));
            QCOMPARE(draft->selectionStart(), 2); QCOMPARE(draft->selectedText(), QString("commi"));
            QCOMPARE(draft->cursorPosition(), 7);
            QVERIFY(draft->hasFocus());
            QCOMPARE(page.selectedId(), QString("style-fixture"));
            QTRY_COMPARE(warning->palette().color(QPalette::Window), controller.theme().surface);
            for (auto *l : page.findChildren<QLabel *>()) if (l->property("hnRole").toString() == "hint")
                QTRY_COMPARE(l->palette().color(QPalette::WindowText), controller.theme().muted);
            for (auto it = positions.cbegin(); it != positions.cend(); ++it) QCOMPARE(it.key()->value(), it.value());
        }
        QCOMPARE(changed.count(), 0);
        QCOMPARE(controller.plugins().manager()->host()->setting("style-fixture", "draft").toString(), QString("saved"));
        for (int px : {8, 18, 32}) {
            scheme(controller, "dark", px);
            QTRY_COMPARE(draft->font().pixelSize(), px);
            QCOMPARE(sameEditor.data(), draft);
            QCOMPARE(draft->text(), QString("uncommitted text"));
            QCOMPARE(draft->selectionStart(), 2); QCOMPARE(draft->cursorPosition(), 7);
            QVERIFY(draft->hasFocus());
        }
        QCOMPARE(changed.count(), 0);
    }

    void pluginNarrowLayoutAndFontChangesKeepControlsReachable() {
        PolishProfile profile;
        apptest::Lib lib;
        auto options = lib.opts(); options.pluginHooks.consent = [](const ConsentRequest &) { return true; };
        AppController controller(options);
        QVERIFY(controller.plugins().install(plugin(lib), nullptr, nullptr).ok);
        PluginsPage page(&controller);
        page.resize(520, 460); page.show(); page.select("style-fixture");
        QPointer<QWidget> editor(page.settingEditors().value(0));
        QVERIFY(editor);
        int lastChipHeight = 0;
        for (int px : {8, 18, 32}) {
            scheme(controller, "dark", px);
            QTRY_COMPARE(editor->font().pixelSize(), px);
            QTRY_COMPARE(page.size(), QSize(520, 460));
            QVERIFY(editor);
            QTRY_VERIFY(inWidget(page.list(), &page).bottom() < inWidget(editor, &page).top());
            QVERIFY(page.list()->height() <= 180);
            auto *outer = page.findChild<QScrollArea *>("hnPluginsScroll");
            QVERIFY(outer);
            QTRY_COMPARE(outer->horizontalScrollBar()->maximum(), 0);
            for (auto *b : {page.button("install"), page.button("disable"), page.button("reload"),
                            page.button("folder"), page.button("audit"), page.button("remove")}) {
                QVERIFY(b);
                // Scroll nested viewports from the innermost one outwards.
                for (QWidget *p = b->parentWidget(); p; p = p->parentWidget())
                    if (auto *scroll = qobject_cast<QScrollArea *>(p)) scroll->ensureWidgetVisible(b, 0, 0);
                QTRY_VERIFY(page.rect().contains(inWidget(b, &page)));
                QVERIFY(b->height() >= b->fontMetrics().height() + 8);
            }
            auto *chips = page.findChild<QLabel *>("hnPluginChips");
            QVERIFY(chips);
            QVERIFY(chips->pixmap().width() > 0);
            QTRY_VERIFY2(chips->pixmap().width() <= chips->width(), qPrintable(QString("px=%1 pixmap=%2 label=%3 detail=%4")
                .arg(px).arg(chips->pixmap().width()).arg(chips->width()).arg(chips->parentWidget()->width())));
            QTRY_VERIFY(chips->pixmap().height() > lastChipHeight);
            lastChipHeight = chips->pixmap().height();
        }
        page.resize(1000, 700);
        QTRY_VERIFY(inWidget(page.list(), &page).right() < inWidget(editor, &page).left());
    }

    void appearanceThemeChangesDoNotActivatePluginManager() {
        PolishProfile profile;
        apptest::Lib lib;
        AppController controller(lib.opts());
        SettingsDialog dialog(&controller); dialog.show();
        for (const QString &s : {QString("dark"), QString("light"), QString("dark")}) scheme(controller, s, 18);
        QVERIFY(!controller.plugins().active());
    }

    void longPluginBooleanLabelWrapsAndRemainsOperable() {
        PolishProfile profile;
        apptest::Lib lib;
        auto options = lib.opts(); options.pluginHooks.consent = [](const ConsentRequest &) { return true; };
        AppController controller(options);
        const QString title = "Include automatically generated headings when collecting references";
        const QString path = plugin(lib, "long-label", "Long label",
            "hn.setting{ id='long', type='bool', default=false, title='Include automatically generated headings when collecting references' }");
        QVERIFY(controller.plugins().install(path, nullptr, nullptr).ok);
        scheme(controller, "dark", 32);
        PluginsPage page(&controller); page.resize(520, 460); page.show(); page.select("long-label");
        QVERIFY(QTest::qWaitForWindowExposed(&page));
        auto *scroll = page.findChild<QScrollArea *>("hnPluginDetailScroll");
        QVERIFY(scroll);
        QTRY_VERIFY(scroll->widget()->width() <= scroll->viewport()->width());
        auto *cb = qobject_cast<QCheckBox *>(page.settingEditors().value(0));
        QVERIFY(cb);
        QCOMPARE(cb->accessibleName(), title);
        auto *caption = label(&page, title);
        QTRY_VERIFY((!cb->text().isEmpty() && cb->fontMetrics().horizontalAdvance(cb->text()) + 24 <= cb->width())
                || (caption && caption->isVisible() && caption->wordWrap()
                    && caption->height() >= caption->heightForWidth(caption->width())));
        QVERIFY(caption);
        QCOMPARE(caption->buddy(), cb);
        for (QWidget *p = caption->parentWidget(); p; p = p->parentWidget())
            if (auto *area = qobject_cast<QScrollArea *>(p)) area->ensureWidgetVisible(caption, 0, 0);
        QTRY_VERIFY(page.rect().contains(inWidget(caption, &page)));
        QTest::mouseClick(caption, Qt::LeftButton, Qt::NoModifier, caption->rect().bottomRight() - QPoint(2, 2));
        QVERIFY(controller.plugins().manager()->host()->setting("long-label", "long").toBool());
        QTRY_VERIFY(cb->hasFocus());
        QTest::keyClick(cb, Qt::Key_Space);
        QVERIFY(!controller.plugins().manager()->host()->setting("long-label", "long").toBool());
        QTest::mouseClick(caption, Qt::RightButton);
        QVERIFY(!controller.plugins().manager()->host()->setting("long-label", "long").toBool());
        QTest::mousePress(caption, Qt::LeftButton);
        QTest::mouseRelease(caption, Qt::LeftButton, Qt::NoModifier, QPoint(-2, -2));
        QVERIFY(!controller.plugins().manager()->host()->setting("long-label", "long").toBool());
        cb->setEnabled(false);
        QTest::mouseClick(caption, Qt::LeftButton);
        QVERIFY(!controller.plugins().manager()->host()->setting("long-label", "long").toBool());
    }

    void pluginRemovalQuestionShowsLiteralIdentityAndNoKeepsPlugin() {
        PolishProfile profile;
        apptest::Lib lib;
        auto options = lib.opts(); options.pluginHooks.consent = [](const ConsentRequest &) { return true; };
        AppController controller(options);
        const QString name = "<b>Literal plugin</b>";
        QVERIFY(controller.plugins().install(plugin(lib, "literal-name", name), nullptr, nullptr).ok);
        PluginsPage page(&controller); page.show(); page.select("literal-name");
        bool plain = false, visible = false, declined = false;
        ModalProbe probe([&](QDialog *d) {
            auto *box = qobject_cast<QMessageBox *>(d);
            if (!box) { d->reject(); return true; }
            plain = box->textFormat() == Qt::PlainText && box->informativeText().contains(name);
            visible = box->text() == "Remove plugin";
            if (auto *no = button(box, "No")) { declined = true; no->click(); } else box->reject();
            return true;
        });
        QVERIFY(page.button("remove"));
        page.button("remove")->click();
        QVERIFY(probe.seen && !probe.expired && plain && visible && declined);
        QVERIFY(QFile::exists(lib.dir.filePath("plugins/literal-name/plugin.json")));
    }

    void consentRethemesWithoutChangingApprovalState() {
        PolishProfile profile;
        apptest::Lib lib;
        AppController controller(lib.opts()); scheme(controller, "dark");
        const auto r = request();
        ConsentDialog dialog(r, nullptr, 60000); dialog.show();
        QSignalSpy accepted(&dialog, &QDialog::accepted);
        QVERIFY(!dialog.approveButton()->isEnabled());
        for (const QString &s : {QString("light"), QString("dark")}) {
            scheme(controller, s);
            QTRY_COMPARE(dialog.warningPanel()->palette().color(QPalette::Window), controller.theme().surface);
            QVERIFY(!dialog.approveButton()->isEnabled());
            QVERIFY(dialog.cancelButton()->isDefault());
            QCOMPARE(dialog.hashLabel()->text(), r.sha256);
            QCOMPARE(dialog.permissionLabels().size(), r.permissions.size());
            QCOMPARE(dialog.permissionLabels().first()->property("permission").toString(), QString("network"));
            QTRY_COMPARE(dialog.permissionLabels().first()->palette().color(QPalette::WindowText), controller.theme().danger);
        }
        QCOMPARE(accepted.count(), 0);
        QTest::mouseClick(dialog.cancelButton(), Qt::LeftButton);
        QCOMPARE(accepted.count(), 0);
    }

    void consentRestyleDoesNotRestartTheTwoSecondGuard() {
        PolishProfile profile;
        apptest::Lib lib;
        AppController controller(lib.opts()); scheme(controller, "dark");
        ConsentDialog dialog(request()); dialog.show();
        QSignalSpy accepted(&dialog, &QDialog::accepted);
        QTest::qWait(1200);
        scheme(controller, "light");
        QVERIFY(!dialog.approveButton()->isEnabled());
        QTRY_VERIFY_WITH_TIMEOUT(dialog.approveButton()->isEnabled(), 1500);
        QTest::keyClick(&dialog, Qt::Key_Return);
        QCOMPARE(accepted.count(), 0);
    }

    void consentFitsAvailableHeightWithKeyboardReachableDetails_data() {
        QTest::addColumn<bool>("native");
        QTest::newRow("script") << false;
        QTest::newRow("native") << true;
    }
    void consentFitsAvailableHeightWithKeyboardReachableDetails() {
        QFETCH(bool, native);
        PolishProfile profile;
        apptest::Lib lib;
        AppController controller(lib.opts()); scheme(controller, "dark", 32);
        auto r = request(); r.native = native;
        r.description = QString("A plugin description with enough detail to require scrolling.\n").repeated(8);
        r.reconsentReason = "Review the changed package before enabling it again.";
        r.permissions = {"notes.read", "network", "notes.write"};
        ConsentDialog dialog(r, nullptr, 60000);
        const QSize available = dialog.screen()->availableGeometry().size();
        const QSize requested(520, qMin(1080, available.height() - 16));
        dialog.resize(requested); dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        QTRY_COMPARE(dialog.width(), requested.width());
        QTRY_VERIFY2(dialog.height() <= requested.height(), qPrintable(QString("requested %1x%2, actual %3x%4, minimum height %5")
            .arg(requested.width()).arg(requested.height()).arg(dialog.width()).arg(dialog.height()).arg(dialog.minimumHeight())));
        QVERIFY(dialog.minimumHeight() <= requested.height());
        QCOMPARE(dialog.size(), requested);
        QVERIFY(dialog.frameGeometry().height() <= available.height());
        auto *warning = dialog.findChild<QLabel *>("hnConsentWarningText");
        QVERIFY(warning);
        QTRY_VERIFY(warning->height() >= warning->heightForWidth(warning->width()));
        QVERIFY(dialog.rect().contains(inWidget(dialog.warningPanel(), &dialog)));
        for (auto *b : {dialog.cancelButton(), dialog.approveButton()}) {
            QVERIFY(b->isVisible());
            QVERIFY(dialog.rect().contains(inWidget(b, &dialog)));
            QVERIFY(b->height() >= b->heightForWidth(b->width()));
        }
        QVERIFY(dialog.cancelButton()->isDefault() && !dialog.approveButton()->isEnabled());
        QCOMPARE(dialog.permissionLabels().first()->property("permission").toString(), QString("network"));
        auto *details = dialog.findChild<QScrollArea *>("hnConsentDetails");
        QVERIFY(details);
        QTRY_VERIFY(details->viewport()->height() >= dialog.fontMetrics().height());
        QTRY_VERIFY(details->verticalScrollBar()->maximum() > 0);
        QCOMPARE(details->horizontalScrollBar()->maximum(), 0);
        const QRect warningRect = inWidget(dialog.warningPanel(), &dialog);
        details->setFocus();
        QTest::keyClick(details, Qt::Key_PageDown);
        QTRY_VERIFY(details->verticalScrollBar()->value() > 0);
        for (int i = 0; i < 64 && details->verticalScrollBar()->value() < details->verticalScrollBar()->maximum(); ++i)
            QTest::keyClick(details, Qt::Key_PageDown);
        QCOMPARE(details->verticalScrollBar()->value(), details->verticalScrollBar()->maximum());
        QCOMPARE(inWidget(dialog.warningPanel(), &dialog), warningRect);
        QSet<QWidget *> reached;
        dialog.cancelButton()->setFocus();
        for (int i = 0; i < 64; ++i) {
            QTest::keyClick(QApplication::focusWidget(), Qt::Key_Tab);
            auto *focus = QApplication::focusWidget();
            reached.insert(focus);
            if (focus && details->widget()->isAncestorOf(focus))
                QTRY_VERIFY2(details->viewport()->rect().intersects(inWidget(focus, details->viewport())),
                    qPrintable(QString("focus %1 %2 at y=%3, scroll=%4, viewport=%5")
                        .arg(focus->metaObject()->className(), focus->objectName()).arg(inWidget(focus, details->viewport()).y())
                        .arg(details->verticalScrollBar()->value()).arg(details->viewport()->height())));
        }
        QVERIFY(reached.contains(dialog.hashLabel()));
        QVERIFY(reached.contains(dialog.findChild<QLabel *>("hnConsentSource")));
        for (auto *permission : dialog.permissionLabels()) QVERIFY(reached.contains(permission));
        scheme(controller, "light", 32);
        QTRY_COMPARE(dialog.warningPanel()->palette().color(QPalette::Window), controller.theme().surface);
        QVERIFY(dialog.height() <= requested.height());
        QVERIFY(dialog.cancelButton()->isDefault() && !dialog.approveButton()->isEnabled());
        dialog.resize(600, available.height() * 2);
        QTRY_VERIFY(dialog.frameGeometry().height() <= available.height());
        dialog.resize(requested);
        QTRY_COMPARE(dialog.size(), requested);
        QSignalSpy accepted(&dialog, &QDialog::accepted);
        dialog.cancelButton()->setFocus();
        QTest::keyClick(&dialog, Qt::Key_Return);
        QCOMPARE(accepted.count(), 0);
        QVERIFY(!dialog.isVisible());
    }

    void consentFontsIconsAndWarningsStayReadable_data() {
        QTest::addColumn<int>("px"); QTest::addColumn<bool>("native");
        for (int px : {8, 18, 32}) for (bool native : {false, true})
            QTest::newRow(qPrintable(QString::number(px) + (native ? "-native" : "-script"))) << px << native;
    }
    void consentFontsIconsAndWarningsStayReadable() {
        QFETCH(int, px); QFETCH(bool, native);
        PolishProfile profile;
        apptest::Lib lib;
        AppController controller(lib.opts()); scheme(controller, "dark");
        auto r = request(); r.native = native; r.name = "<b>Literal plugin</b>";
        ConsentDialog dialog(r, nullptr, 60000); dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        QImage oldIcon;
        for (auto *l : dialog.warningPanel()->findChildren<QLabel *>()) if (!l->pixmap().isNull()) oldIcon = l->pixmap().toImage();
        QVERIFY(!oldIcon.isNull());
        scheme(controller, "light", px);
        QTRY_COMPARE(dialog.hashLabel()->font().pixelSize(), px);
        QTRY_COMPARE(dialog.warningPanel()->palette().color(QPalette::Window), controller.theme().surface);
        auto *warning = dialog.findChild<QLabel *>("hnConsentWarningText");
        QVERIFY(warning);
        QTRY_COMPARE(warning->font().pixelSize(), px);
        QTRY_VERIFY(warning->height() >= warning->heightForWidth(warning->width()));
        QTRY_VERIFY(dialog.rect().contains(inWidget(dialog.warningPanel(), &dialog)));
        QTRY_VERIFY(dialog.rect().contains(inWidget(dialog.approveButton(), &dialog)));
        QVERIFY(dialog.cancelButton()->isDefault() && !dialog.approveButton()->isEnabled());
        QVERIFY(label(&dialog, r.name)->textFormat() == Qt::PlainText);
        QImage icon;
        for (auto *l : dialog.warningPanel()->findChildren<QLabel *>()) if (!l->pixmap().isNull()) icon = l->pixmap().toImage();
        QVERIFY(icon != oldIcon);
        if (native) {
            QTRY_COMPARE(dialog.nativePanel()->palette().color(QPalette::Window), controller.theme().danger);
            auto *text = dialog.findChild<QLabel *>("hnConsentNativeText");
            QTRY_COMPARE(text->font().pixelSize(), px);
            QVERIFY(text->height() >= text->heightForWidth(text->width()));
        }
        // A second dialog opened after the switch uses the same current tokens.
        ConsentDialog later(r, nullptr, 60000); later.show();
        QTRY_COMPARE(later.warningPanel()->palette().color(QPalette::Window), controller.theme().surface);
        QTRY_COMPARE(later.hashLabel()->font().pixelSize(), px);
        QVERIFY(later.cancelButton()->isDefault() && !later.approveButton()->isEnabled());
    }

    void auditHasVisibleCloseAndLiveMonospaceWithoutLosingSelection() {
        PolishProfile profile;
        apptest::Lib lib;
        AppController controller(lib.opts());
        PluginsPage page(&controller); page.showAudit({});
        QPointer<QDialog> dialog(page.findChild<QDialog *>("hnAuditDialog"));
        QVERIFY(dialog);
        auto *ed = dialog->findChild<QPlainTextEdit *>("hnAuditText");
        QVERIFY(ed);
        const QString before = ed->toPlainText(); ed->selectAll();
        const auto selection = ed->textCursor().selectedText();
        auto theme = ui::theme();
        QString family;
        for (const auto &f : QFontDatabase::families())
            if (f != ed->font().family() && QFontDatabase::isFixedPitch(f)) { family = f; break; }
        QVERIFY(!family.isEmpty());
        theme.monoFamily = family; theme.baseSize = 18;
        ui::setTheme(theme); hn::theme::applyTheme(theme);
        QTRY_COMPARE(ed->font().family(), family);
        QTRY_COMPARE(ed->font().pixelSize(), 18);
        QCOMPARE(ed->toPlainText(), before); QCOMPARE(ed->textCursor().selectedText(), selection);
        QCOMPARE(ed->document()->defaultFont().family(), family);
        QCOMPARE(ed->fontMetrics().horizontalAdvance("iiii"), ed->fontMetrics().horizontalAdvance("WWWW"));
        auto *title = label(dialog, "Plugin audit log");
        QVERIFY(title && title->isVisible());
        auto *close = button(dialog, "Close");
        QVERIFY(close && close->isVisible());
        QTest::mouseClick(close, Qt::LeftButton);
        QTRY_VERIFY(!dialog);
    }

    void firstRunCloseReturnsEmptyAndControllerPersistsDefault() {
        PolishProfile profile;
        qunsetenv("HN_NOTES_DIR");
        apptest::Lib lib;
        auto options = lib.opts(); options.notesDir.clear(); options.chooseNotesFolder = {};
        AppController controller(options);
        const QString expected = QDir::homePath() + "/Notes/Hyprnotes";
        for (bool throughController : {false, true}) {
            bool explanatory = false, titleVisible = false, clicked = false, live = false, contained = false;
            int stage = 0;
            ModalProbe probe([&](QDialog *d) {
                if (stage++ == 0) { scheme(controller, "dark", 32); d->resize(520, d->height()); return false; }
                auto *title = label(d, "Where should your notes live?");
                titleVisible = title && title->isVisible() && title->property("hnRole") == "heading";
                auto *hint = label(d, "Closing this dialog uses the default notes folder.");
                explanatory = hint && hint->isVisible();
                live = hint && hint->palette().color(QPalette::WindowText) == controller.theme().muted;
                auto *ok = button(d, "Use this folder");
                contained = ok && d->rect().contains(inWidget(ok, d)) && title
                    && title->height() >= title->heightForWidth(title->width());
                if (auto *close = button(d, "Close")) { clicked = close->isVisible(); close->click(); }
                else d->reject();
                return true;
            });
            if (throughController) QVERIFY(controller.handleAction(hn::platform::Action::ShowOrganizer));
            else QCOMPARE(runFirstRunDialog(expected), QString());
            QVERIFY(probe.seen && !probe.expired && explanatory && titleVisible && clicked && live && contained);
        }
        QCOMPARE(controller.notesRoot(), expected);
        QCOMPARE(hn::theme::Config(lib.cfg).settings().notesFolder, expected);
    }

    void linkUpdateCancelShowsLiteralSummaryAndLiveHeading() {
        PolishProfile profile;
        apptest::Lib lib;
        AppController controller(lib.opts()); scheme(controller, "dark");
        const QString summary = "Rename <b>literal</b> & keep [[links]]";
        bool visible = false, plain = false, contained = false;
        int stage = 0;
        ModalProbe probe([&](QDialog *d) {
            if (stage++ == 0) { scheme(controller, "light", 32); d->resize(520, d->height()); return false; }
            auto *title = label(d, "Update links to this note?");
            visible = title && title->isVisible() && title->property("hnRole") == "heading";
            auto *sum = label(d, summary); plain = sum && sum->textFormat() == Qt::PlainText;
            auto *cancel = button(d, "Cancel"), *only = button(d, "Rename only"), *update = button(d, "Update links");
            contained = cancel && only && update && d->rect().contains(inWidget(cancel, d))
                && d->rect().contains(inWidget(only, d)) && d->rect().contains(inWidget(update, d));
            if (cancel) cancel->click(); else d->reject();
            return true;
        });
        QCOMPARE(askLinkUpdate(nullptr, summary, {"<b>literal</b>.md - 1 link"}), LinkChoice::Cancel);
        QVERIFY(probe.seen && !probe.expired && visible && plain && contained);
    }

    void pluginModalFlows_data() {
        QTest::addColumn<QString>("flow"); QTest::addColumn<bool>("accept");
        for (const char *flow : {"prompt", "confirm", "pick"}) for (bool accept : {false, true})
            QTest::newRow(qPrintable(QString(flow) + (accept ? "-accept" : "-cancel"))) << QString(flow) << accept;
    }
    void pluginModalFlows() {
        QFETCH(QString, flow); QFETCH(bool, accept);
        PolishProfile profile;
        apptest::Lib lib;
        AppController controller(lib.opts()); scheme(controller, "dark");
        PluginUiBridge bridge(&controller.plugins());
        bool literal = false, heading = false, live = false, actions = false, safeDefault = true;
        int stage = 0;
        ModalProbe probe([&](QDialog *d) {
            if (stage++ == 0) { scheme(controller, "light", 32); d->resize(520, d->height()); return false; }
            auto *h = label(d, flow == "confirm" ? "Confirm" : "<b>Title</b>");
            heading = h && h->isVisible() && h->property("hnRole") == "heading";
            literal = h && h->textFormat() == Qt::PlainText;
            if (flow != "pick") {
                auto *body = label(d, "<b>Body</b>"); literal = literal && body && body->textFormat() == Qt::PlainText;
            }
            auto *kicker = label(d, "PLUGIN  -  STYLE-FIXTURE");
            live = kicker && kicker->palette().color(QPalette::WindowText) == controller.theme().muted;
            auto *cancel = button(d, "Cancel");
            auto *ok = button(d, flow == "prompt" ? "OK" : flow == "pick" ? "Select" : "Continue");
            actions = cancel && ok && d->rect().contains(inWidget(cancel, d)) && d->rect().contains(inWidget(ok, d));
            if (flow == "confirm") safeDefault = cancel && cancel->isDefault();
            if (flow == "prompt") { if (auto *edit = d->findChild<QLineEdit *>()) edit->setText("typed <literal>"); }
            if (flow == "pick") { if (auto *list = d->findChild<QListWidget *>()) list->setCurrentRow(1); }
            if (auto *b = accept ? ok : cancel) b->click(); else d->reject();
            return true;
        });
        if (flow == "prompt") QCOMPARE(bridge.prompt("style-fixture", "<b>Title</b>", "<b>Body</b>", "saved"),
                                      accept ? std::optional<QString>("typed <literal>") : std::nullopt);
        else if (flow == "confirm") QCOMPARE(bridge.confirm("style-fixture", "<b>Body</b>"), accept);
        else QCOMPARE(bridge.pick("style-fixture", "<b>Title</b>", {"one", "two"}), accept ? 1 : -1);
        QVERIFY(probe.seen && !probe.expired && literal && heading && live && actions && safeDefault);
    }

    void recoveryDialogHasVisibleHeadingAndLaterPreservesDraft() {
        PolishProfile profile;
        apptest::Lib lib; lib.write("r.md", "disk\n");
        {
            hn::core::NoteRepository repo(lib.notes, lib.state + "/recovery");
            const QByteArray disk("disk\n"); QString error;
            QVERIFY(repo.recovery().writePending(repo.root(), "r.md", "local\n", &disk, 1, &error));
        }
        auto options = lib.opts(); options.recoveryPrompt = {};
        options.chooseNotesFolder = [&lib](const QString &) { return lib.notes; };
        AppController controller(options);
        bool heading = false, detail = false, safe = false, clicked = false, live = false;
        int stage = 0;
        ModalProbe probe([&](QDialog *d) {
            auto *box = qobject_cast<QMessageBox *>(d);
            if (!box) { d->reject(); return true; }
            if (stage++ == 0) { scheme(controller, "dark", 18); return false; }
            heading = box->text() == "Recover unsaved changes?";
            detail = box->informativeText().contains("1 note");
            auto *title = label(box, "Recover unsaved changes?");
            live = title && title->isVisible() && title->font().pixelSize() == 24
                && title->palette().color(QPalette::WindowText) == controller.theme().text;
            if (auto *later = button(box, "Later")) {
                safe = box->defaultButton() == later && box->escapeButton() == later;
                clicked = true; later->click();
            } else box->reject();
            return true;
        });
        QVERIFY(controller.handleAction(hn::platform::Action::ShowOrganizer));
        QVERIFY(probe.seen && !probe.expired && heading && detail && safe && clicked && live);
        QCOMPARE(lib.read("r.md"), QByteArray("disk\n"));
        QVERIFY(!controller.repo().recoverableDrafts().isEmpty());
    }

    void destructiveQuestionHasVisibleTitleAndCancelKeepsFile() {
        PolishProfile profile;
        apptest::Lib lib; lib.write("<b>literal</b>.md", "disk\n");
        auto options = lib.opts(); options.confirm = {};
        AppController controller(options);
        QMenu menu; controller.populateNoteMenu(&menu, "<b>literal</b>.md", true);
        QAction *remove = nullptr;
        for (auto *a : menu.actions()) if (a->text().contains("Delete")) remove = a;
        QVERIFY(remove);
        bool heading = false, plain = false, safe = false;
        ModalProbe probe([&](QDialog *d) {
            auto *box = qobject_cast<QMessageBox *>(d);
            if (!box) { d->reject(); return true; }
            heading = box->text() == "Delete note?";
            plain = box->textFormat() == Qt::PlainText && box->informativeText().contains("permanently deleted");
            auto *cancel = button(box, "Cancel"); safe = cancel && box->defaultButton() == cancel;
            if (cancel) cancel->click(); else box->reject();
            return true;
        });
        remove->trigger();
        QVERIFY(probe.seen && !probe.expired && heading && plain && safe);
        QCOMPARE(lib.read("<b>literal</b>.md"), QByteArray("disk\n"));
    }
};

int main(int argc, char **argv) {
    QApplication app(argc, argv); app.setQuitOnLastWindowClosed(false);
    DialogPolishTest test; return QTest::qExec(&test, argc, argv);
}
#include "app_dialog_polish_test.moc"
