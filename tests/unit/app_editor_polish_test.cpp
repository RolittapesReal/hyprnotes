#include "app_test_util.h"
#include "ui_polish_test_util.h"
#include "hn/editor/types.h"
#include "hn/theme/theme_io.h"
#include "../../src/editor/completion_popup.h"
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QInputDialog>
#include <QMenu>
#include <QPainter>
#include <QScreen>
#include <QTimer>
#include <QToolButton>
#include <QTextBlock>
#include <QTextLayout>
#include <QVBoxLayout>
#include <cmath>

using hn::editor::Mode;

namespace {
void selectText(QTextEdit *edit, int start, int end) {
    QTextCursor c(edit->document());
    c.setPosition(start);
    c.setPosition(end, QTextCursor::KeepAnchor);
    edit->setTextCursor(c);
}

void checkMono(const QFont &font) {
    const QFontMetricsF fm(font);
    QVERIFY2(qAbs(fm.horizontalAdvance("iiii") - fm.horizontalAdvance("WWWW")) < 0.01, qPrintable(font.toString()));
}

QString customMonoFamily() {
    for (const QString &family : QFontDatabase::families())
        if (QFontDatabase::isFixedPitch(family) && !family.contains("Emoji") && !family.contains("Symbols")) return family;
    return {};
}

struct ToolbarFixture {
    QWidget host;
    hn::editor::NoteEditor *editor = new hn::editor::NoteEditor(&host);
    hn::editor::FormattingToolbar *toolbar = new hn::editor::FormattingToolbar(editor, &host);
    ToolbarFixture(int width = 640) {
        auto *layout = new QVBoxLayout(&host);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(toolbar);
        layout->addWidget(editor);
        editor->load("hello world\n");
        host.resize(width, 400);
        host.show();
        host.activateWindow();
        editor->focusEditor();
        selectText(editor->visualEdit(), 0, 5);
    }
    QToolButton *button(const char *name) { return toolbar->findChild<QToolButton *>(name); }
};

QAction *actionNamed(QMenu *menu, const QString &text) {
    for (auto *a : menu->actions()) if (a->text() == text) return a;
    return nullptr;
}
}

class AppEditorPolishTest : public QObject {
    Q_OBJECT
private slots:
    void sourceRemainsMonospaceAcrossLiveThemeChanges() {
        apptest::Lib lib;
        lib.write("font.md", "iiii\nWWWW\n");
        hn::app::AppController controller(lib.opts());
        auto *session = controller.openSticky("font.md");
        QVERIFY(session);
        session->setTiming({60000, 60000});
        auto *editor = session->editor();
        QVERIFY(editor->setMode(Mode::Source));
        editor->focusEditor();
        QTest::keyClicks(editor->sourceEdit(), "edited ");
        const QByteArray bytes = editor->toMarkdownBytes();
        const int revision = editor->revision();
        const int cursor = editor->sourceEdit()->textCursor().position();
        for (const QString &id : {QString("modernist"), QString("nord"), QString("dracula")}) {
            auto settings = controller.settings();
            settings.theme = id;
            settings.colorScheme = "dark";
            controller.applySettings(settings, controller.prefs());
            QTRY_COMPARE(editor->window()->styleSheet(), hn::theme::styleSheetFor(controller.theme()));
            QTRY_COMPARE(editor->theme().bg, controller.theme().bg);
            const auto widgetMetrics = editor->sourceEdit()->fontMetrics();
            QCOMPARE(widgetMetrics.horizontalAdvance("iiii"), widgetMetrics.horizontalAdvance("WWWW"));
            checkMono(editor->sourceEdit()->document()->defaultFont());
            QCOMPARE(editor->toMarkdownBytes(), bytes);
            QCOMPARE(editor->revision(), revision);
            QCOMPARE(editor->sourceEdit()->textCursor().position(), cursor);
            QVERIFY(editor->canUndo());
        }
        QVERIFY(editor->undo());
        QCOMPARE(editor->toMarkdownBytes(), QByteArray("iiii\nWWWW\n"));
    }

    void sourceThemeMatrix_data() {
        QTest::addColumn<bool>("dark");
        QTest::addColumn<int>("size");
        QTest::addColumn<bool>("hidden");
        QTest::addColumn<bool>("reparent");
        for (bool dark : {false, true}) for (int size : {8, 14, 18, 32})
            for (bool hidden : {false, true}) for (bool reparent : {false, true})
                QTest::newRow(qPrintable(QString("%1-%2-hidden%3-reparent%4").arg(dark).arg(size).arg(hidden).arg(reparent)))
                    << dark << size << hidden << reparent;
    }

    void sourceThemeMatrix() {
        QFETCH(bool, dark);
        QFETCH(int, size);
        QFETCH(bool, hidden);
        QFETCH(bool, reparent);
        apptest::Lib lib;
        // A table opens directly in source mode, with no mode-switch history.
        const QByteArray original = "| iiii | WWWW |\n| --- | --- |\n| one | two |\n";
        lib.write("source.md", original);
        hn::app::AppController controller(lib.opts());
        auto *session = controller.openSticky("source.md");
        QVERIFY(session);
        session->setTiming({60000, 60000});
        auto *editor = session->editor();
        QCOMPARE(editor->mode(), Mode::Source);
        const int unrecorded = editor->recorder().unrecorded();
        auto *source = editor->sourceEdit();
        QTextCursor c(source->document());
        c.setPosition(2);
        c.setPosition(6, QTextCursor::KeepAnchor);
        source->setTextCursor(c);
        if (hidden) editor->window()->hide();
        for (const QString &id : {QString("nord"), QString("dracula")}) {
            auto settings = controller.settings();
            settings.theme = id;
            settings.colorScheme = dark ? "dark" : "light";
            settings.fontSize = size;
            controller.applySettings(settings, controller.prefs());
            QTRY_COMPARE(editor->theme().baseSize, size);
            QTRY_COMPARE(editor->theme().bg, controller.theme().bg);
            if (hidden) editor->window()->show();
            if (reparent) {
                QVERIFY(controller.popIn("source.md"));
                QVERIFY(controller.popOut("source.md"));
                QCOMPARE(session->editor(), editor);
            }
            QTRY_COMPARE(editor->window()->styleSheet(), hn::theme::styleSheetFor(controller.theme()));
            QCOMPARE(source->font().pixelSize(), size);
            QCOMPARE(source->document()->defaultFont().pixelSize(), size);
            QCOMPARE(source->fontMetrics().horizontalAdvance("iiii"), source->fontMetrics().horizontalAdvance("WWWW"));
            checkMono(source->document()->defaultFont());
            // Measure laid-out glyph positions, not just the requested font.
            (void)source->cursorRect(QTextCursor(source->document()->begin()));
            QTRY_VERIFY(source->document()->begin().layout()->lineCount() > 0);
            const auto line = source->document()->begin().layout()->lineAt(0);
            QVERIFY(line.isValid());
            QVERIFY(qAbs((line.cursorToX(6) - line.cursorToX(2)) - (line.cursorToX(13) - line.cursorToX(9))) < 0.01);
            QCOMPARE(editor->toMarkdownBytes(), original);
            QCOMPARE(editor->revision(), 0);
            QCOMPARE(source->textCursor().anchor(), 2);
            QCOMPARE(source->textCursor().position(), 6);
            QVERIFY(!editor->canUndo());
            QVERIFY(!session->dirty());
            QCOMPARE(session->state(), hn::app::NoteSession::State::Clean);
            if (hidden) editor->window()->hide();
        }
        editor->window()->show();
        source->moveCursor(QTextCursor::End);
        QTest::keyClicks(source, "edited");
        c = source->textCursor();
        c.movePosition(QTextCursor::PreviousCharacter, QTextCursor::KeepAnchor, 3);
        source->setTextCursor(c);
        const QByteArray edited = editor->toMarkdownBytes();
        const int rev = editor->revision();
        auto t = controller.theme();
        t.monoFamily = customMonoFamily();
        QVERIFY(!t.monoFamily.isEmpty());
        t.baseSize = size == 32 ? 8 : 32;
        session->applyTheme(t);
        QCOMPARE(source->font().family(), t.monoFamily);
        checkMono(source->font());
        checkMono(source->document()->defaultFont());
        QCOMPARE(editor->toMarkdownBytes(), edited);
        QCOMPARE(editor->revision(), rev);
        QCOMPARE(source->textCursor().position(), c.position());
        QCOMPARE(source->textCursor().anchor(), c.anchor());
        QVERIFY(session->dirty());
        QVERIFY(editor->undo());
        QCOMPARE(editor->toMarkdownBytes(), original);
        QVERIFY(editor->redo());
        QCOMPARE(editor->toMarkdownBytes(), edited);
        QCOMPARE(editor->recorder().unrecorded(), unrecorded);
    }

    void visualCodeFontsPreserveHistory() {
        apptest::Lib lib;
        lib.write("code.md", "Body `iiii WWWW`\n\n```\niiii WWWW\n```\n");
        hn::app::AppController controller(lib.opts());
        auto *session = controller.openSticky("code.md");
        QVERIFY(session);
        session->setTiming({60000, 60000});
        auto *editor = session->editor();
        QCOMPARE(editor->mode(), Mode::Visual);
        const int unrecorded = editor->recorder().unrecorded();
        auto *visual = editor->visualEdit();
        QTest::keyClicks(visual, "edited ");
        selectText(visual, 0, 6);
        const auto bytes = editor->toMarkdownBytes();
        const int revision = editor->revision();
        const QString fixed = customMonoFamily();
        QVERIFY(!fixed.isEmpty());
        for (bool dark : {false, true}) for (int size : {8, 14, 18, 32}) {
            auto t = hn::theme::loadTheme("dracula", dark);
            t.baseSize = size;
            t.monoFamily = fixed;
            hn::theme::applyTheme(t);
            session->applyTheme(t);
            QTRY_COMPARE(editor->window()->styleSheet(), hn::theme::styleSheetFor(t));
            QCOMPARE(visual->font().pixelSize(), size);
            QCOMPARE(visual->document()->defaultFont().family(), t.fontFamily);
            QVERIFY(!editor->sourceEdit()->isVisible());
            QCOMPARE(editor->sourceEdit()->font().family(), fixed);
            checkMono(editor->sourceEdit()->font());
            checkMono(editor->sourceEdit()->document()->defaultFont());
            int codeRuns = 0;
            for (auto b = visual->document()->begin(); b.isValid(); b = b.next()) {
                for (auto it = b.begin(); !it.atEnd(); ++it) {
                    const auto fragment = it.fragment();
                    if (!fragment.charFormat().fontFixedPitch()) continue;
                    ++codeRuns;
                    QFont f = fragment.charFormat().font().resolve(visual->document()->defaultFont());
                    QCOMPARE(f.family(), fixed);
                    QCOMPARE(f.pixelSize(), size);
                    checkMono(f);
                }
            }
            QVERIFY(codeRuns >= 2);
            QCOMPARE(editor->toMarkdownBytes(), bytes);
            QCOMPARE(editor->revision(), revision);
            QCOMPARE(visual->textCursor().selectedText(), QString("edited"));
            QCOMPARE(visual->textCursor().anchor(), 0);
            QVERIFY(editor->canUndo());
            QVERIFY(session->dirty());
            QCOMPARE(editor->recorder().unrecorded(), unrecorded);
        }
        QVERIFY(editor->undo());
        QCOMPARE(editor->toMarkdownBytes(), lib.read("code.md"));
        QVERIFY(editor->redo());
        QCOMPARE(editor->toMarkdownBytes(), bytes);
    }

    void toolbarKeyboardTraversalAndMouseSelection() {
        ToolbarFixture f;
        auto *bold = f.button("bold");
        auto *italic = f.button("italic");
        QTRY_VERIFY(f.editor->visualEdit()->hasFocus());
        QTest::mouseClick(bold, Qt::LeftButton);
        QCOMPARE(f.editor->visualEdit()->textCursor().selectedText(), QString("hello"));
        QVERIFY(f.editor->visualEdit()->hasFocus());
        QVERIFY(f.editor->undo());
        QTest::keyClick(f.editor->activeEdit(), Qt::Key_F10, Qt::AltModifier);
        QTRY_VERIFY(bold->hasFocus());
        QCOMPARE(bold->focusPolicy(), Qt::TabFocus);
        QTest::keyClick(bold, Qt::Key_Tab);
        QTRY_VERIFY(italic->hasFocus());
        QTest::keyClick(italic, Qt::Key_Space);
        QVERIFY(f.editor->toMarkdownBytes().contains("*hello*"));
        QVERIFY(italic->hasFocus());
        QTest::keyClick(italic, Qt::Key_Backtab);
        QTRY_VERIFY(bold->hasFocus());
        QTest::keyClick(bold, Qt::Key_Backtab);
        QTRY_VERIFY(f.button("more")->hasFocus());
        QTest::keyClick(f.button("more"), Qt::Key_Tab);
        QTRY_VERIFY(bold->hasFocus());
        QTest::keyClick(bold, Qt::Key_Escape);
        QTRY_VERIFY(f.editor->visualEdit()->hasFocus());
        f.toolbar->focusFirstControl();
        QTRY_VERIFY(bold->hasFocus());
        QTest::keyClick(bold, Qt::Key_Escape);
        QVERIFY(f.editor->setMode(Mode::Source));
        QTest::keyClick(f.editor->activeEdit(), Qt::Key_F10, Qt::AltModifier);
        QTRY_VERIFY(f.button("more")->hasFocus());
        QTest::keyClick(f.button("more"), Qt::Key_Escape);
        QTRY_VERIFY(f.editor->sourceEdit()->hasFocus());
        f.editor->sourceEdit()->moveCursor(QTextCursor::End);
        const auto before = f.editor->toMarkdownBytes();
        QTest::keyClick(f.editor->sourceEdit(), Qt::Key_Tab);
        QCOMPARE(f.editor->toMarkdownBytes(), before + '\t');
    }

    void compactToolbarKeepsActionsAndKeyboardMenuFocus() {
        ToolbarFixture f(260);
        QTRY_COMPARE(f.host.width(), 260);
        QCOMPARE(f.toolbar->height(), hn::editor::FormattingToolbar::kHeight);
        QVERIFY(f.button("bold")->isVisible());
        QVERIFY(f.button("italic")->isVisible());
        QVERIFY(f.button("more")->isVisible());
        bool overflowed = false;
        for (const char *name : {"style", "bullet", "numbered", "checklist", "link"}) {
            auto *button = f.button(name);
            if (button->isVisible()) continue;
            overflowed = true;
            auto *a = actionNamed(f.toolbar->moreMenu(), button->accessibleName());
            QVERIFY2(actionNamed(f.toolbar->moreMenu(), button->accessibleName()), name);
            QVERIFY(a->isVisible());
            QCOMPARE(a->isEnabled(), button->isEnabled());
            QCOMPARE(a->isCheckable(), button->isCheckable());
            QCOMPARE(a->isChecked(), button->isChecked());
        }
        QVERIFY(overflowed);
        for (auto *b : f.toolbar->findChildren<QToolButton *>())
            if (b->isVisible()) QVERIFY(f.toolbar->rect().contains(inWidget(b, f.toolbar)));
        f.toolbar->setPluginButtons({{"plugin.action", "Plugin action", "bold"}});
        QSignalSpy plugin(f.toolbar, &hn::editor::FormattingToolbar::pluginButtonTriggered);
        auto *more = f.button("more");
        QTRY_VERIFY(f.editor->visualEdit()->hasFocus());
        QTest::keyClick(f.editor->activeEdit(), Qt::Key_F10, Qt::AltModifier);
        QTRY_VERIFY(f.button("bold")->hasFocus());
        QTest::keyClick(f.button("bold"), Qt::Key_Backtab);
        QTRY_VERIFY(more->hasFocus());
        auto *menu = f.toolbar->moreMenu();
        auto *checklist = actionNamed(menu, "Checklist");
        QVERIFY(checklist);
        QVERIFY(!f.button("checklist")->isVisible());
        bool opened = false;
        QString selected;
        QTimer::singleShot(0, menu, [&] {
            // Let the offscreen platform deliver popup activation before synthetic input closes it.
            QCoreApplication::processEvents();
            opened = menu->isVisible();
            selected = f.editor->visualEdit()->textCursor().selectedText();
            menu->setActiveAction(checklist);
            QTest::keyClick(menu, Qt::Key_Return);
        });
        QTest::keyClick(more, Qt::Key_Space);
        QVERIFY(opened);
        QCOMPARE(selected, QString("hello"));
        QTRY_VERIFY(more->hasFocus());
        QVERIFY(f.editor->toMarkdownBytes().contains("- [ ] hello"));
        QVERIFY(checklist->isChecked());
        QVERIFY(f.editor->undo());
        QCOMPARE(f.editor->toMarkdownBytes(), QByteArray("hello world\n"));
        QTimer::singleShot(0, menu, [&] { QCoreApplication::processEvents(); QTest::keyClick(menu, Qt::Key_Escape); });
        QTest::keyClick(more, Qt::Key_Space);
        QTRY_VERIFY(more->hasFocus());
        QTimer::singleShot(0, menu, [&] {
            QCoreApplication::processEvents();
            menu->setActiveAction(actionNamed(menu, "Plugin action"));
            QTest::keyClick(menu, Qt::Key_Return);
        });
        QTest::keyClick(more, Qt::Key_Space);
        QCOMPARE(plugin.count(), 1);
        QTRY_VERIFY(more->hasFocus());
        f.host.resize(640, 400);
        QTRY_VERIFY(f.button("checklist")->isVisible());
        QVERIFY(!checklist->isVisible());
        QVERIFY(actionNamed(menu, "Plugin action"));
    }

    void toolbarLocalThemePaintsFocusAndPressedIcons() {
        ToolbarFixture f;
        auto t = hn::theme::loadTheme("nord", true);
        f.toolbar->setTheme(t); // standalone, without an app session stylesheet
        auto *bold = f.button("bold");
        bold->setFocus(Qt::TabFocusReason);
        QTRY_VERIFY(bold->hasFocus());
        auto image = bold->grab().toImage();
        QCOMPARE(image.pixelColor(image.width() / 2, 0), t.accent);
        QTest::mousePress(bold, Qt::LeftButton);
        QVERIFY(bold->isDown());
        image = bold->grab().toImage();
        QCOMPARE(image.pixelColor(3, image.height() / 2), t.accent);
        const QImage icon = bold->icon().pixmap(16, 16).toImage();
        bool foreground = false;
        for (int y = 0; y < icon.height(); ++y) for (int x = 0; x < icon.width(); ++x)
            if (icon.pixelColor(x, y).alpha() == 255) {
                QCOMPARE(icon.pixelColor(x, y).rgb(), t.accentText.rgb());
                foreground = true;
            }
        QVERIFY(foreground);
        QTest::mouseRelease(bold, Qt::LeftButton);
    }

    void toolbarOpenMenusUsePressedIconTint_data() {
        QTest::addColumn<QString>("name");
        QTest::addColumn<bool>("keyboard");
        QTest::addColumn<bool>("accept");
        for (const QString &name : {QString("style"), QString("more")})
            for (bool keyboard : {false, true}) for (bool accept : {false, true})
                QTest::newRow(qPrintable(QString("%1-%2-%3").arg(name, keyboard ? "keyboard" : "mouse", accept ? "accept" : "cancel")))
                    << name << keyboard << accept;
    }

    void toolbarOpenMenusUsePressedIconTint() {
        QFETCH(QString, name);
        QFETCH(bool, keyboard);
        QFETCH(bool, accept);
        ToolbarFixture f;
        const auto t = hn::theme::loadTheme("nord", true);
        f.toolbar->setTheme(t);
        f.toolbar->setPluginButtons({{"plugin.action", "Plugin action", "bold"}});
        auto *button = f.button(qPrintable(name));
        QVERIFY(button);
        auto *menu = button->menu();
        QVERIFY(menu);
        auto *action = actionNamed(menu, name == "style" ? "Heading 2" : "Plugin action");
        QVERIFY(action);
        QSignalSpy triggered(menu, &QMenu::triggered);
        QSignalSpy plugin(f.toolbar, &hn::editor::FormattingToolbar::pluginButtonTriggered);
        QTRY_VERIFY(f.editor->activeEdit()->hasFocus());
        if (keyboard) {
            button->setFocus(Qt::TabFocusReason);
            QTRY_VERIFY(button->hasFocus());
        }
        const QByteArray bytes = f.editor->toMarkdownBytes();
        bool opened = false, down = false;
        QString selected;
        QImage iconWhileOpen, buttonWhileOpen;
        QTimer::singleShot(0, menu, [&] {
            QCoreApplication::processEvents();
            opened = menu->isVisible();
            down = button->isDown();
            selected = f.editor->visualEdit()->textCursor().selectedText();
            iconWhileOpen = button->icon().pixmap(16, 16).toImage();
            buttonWhileOpen = button->grab().toImage();
            // Always close the real modal popup before assertions, even when tint is wrong.
            if (accept) menu->setActiveAction(action);
            QTest::keyClick(menu, accept ? Qt::Key_Return : Qt::Key_Escape);
        });
        if (keyboard) QTest::keyClick(button, Qt::Key_Space);
        else QTest::mouseClick(button, Qt::LeftButton);
        QVERIFY(opened);
        QVERIFY(down);
        QCOMPARE(selected, QString("hello"));
        QCOMPARE(buttonWhileOpen.pixelColor(3, buttonWhileOpen.height() / 2), t.accent);
        bool foreground = false;
        QPoint sample;
        int sampleAlpha = 0;
        for (int y = 0; y < iconWhileOpen.height(); ++y) for (int x = 0; x < iconWhileOpen.width(); ++x)
            if (const QColor pixel = iconWhileOpen.pixelColor(x, y); pixel.alpha() >= 128) {
                // More's thin strokes have no fully opaque pixels; allow unpremultiplication rounding.
                QVERIFY2(qAbs(pixel.red() - t.accentText.red()) <= 1
                         && qAbs(pixel.green() - t.accentText.green()) <= 1
                         && qAbs(pixel.blue() - t.accentText.blue()) <= 1,
                         qPrintable(QString("open icon %1; expected %2").arg(pixel.name(), t.accentText.name())));
                if (pixel.alpha() > sampleAlpha) { sample = QPoint(x, y); sampleAlpha = pixel.alpha(); }
                foreground = true;
            }
        QVERIFY(foreground);
        // Inspect the painted glyph too, not just the QIcon assigned to the button.
        const QRect glyph((buttonWhileOpen.width() - 16) / 2, (buttonWhileOpen.height() - 16) / 2, 16, 16);
        const auto blend = [sampleAlpha](int fg, int bg) { return qRound((fg * sampleAlpha + bg * (255 - sampleAlpha)) / 255.0); };
        const QColor painted = buttonWhileOpen.pixelColor(glyph.topLeft() + sample);
        QVERIFY(qAbs(painted.red() - blend(t.accentText.red(), t.accent.red())) <= 2);
        QVERIFY(qAbs(painted.green() - blend(t.accentText.green(), t.accent.green())) <= 2);
        QVERIFY(qAbs(painted.blue() - blend(t.accentText.blue(), t.accent.blue())) <= 2);
        QVERIFY(!menu->isVisible());
        QVERIFY(!button->isDown());
        const QImage closedIcon = button->icon().pixmap(16, 16).toImage();
        foreground = false;
        for (int y = 0; y < closedIcon.height(); ++y) for (int x = 0; x < closedIcon.width(); ++x)
            if (const QColor pixel = closedIcon.pixelColor(x, y); pixel.alpha() >= 128) {
                QVERIFY2(qAbs(pixel.red() - t.text.red()) <= 1
                         && qAbs(pixel.green() - t.text.green()) <= 1
                         && qAbs(pixel.blue() - t.text.blue()) <= 1,
                         qPrintable(QString("closed icon %1; expected %2").arg(pixel.name(), t.text.name())));
                foreground = true;
            }
        QVERIFY(foreground);
        QCOMPARE(triggered.count(), accept ? 1 : 0);
        QCOMPARE(plugin.count(), accept && name == "more" ? 1 : 0);
        QTRY_VERIFY(keyboard ? button->hasFocus() : f.editor->activeEdit()->hasFocus());
        QCOMPARE(f.editor->visualEdit()->textCursor().selectedText(), QString("hello"));
        if (accept && name == "style") {
            QVERIFY(f.editor->toMarkdownBytes().startsWith("## hello world"));
            QVERIFY(f.editor->undo());
        }
        QCOMPARE(f.editor->toMarkdownBytes(), bytes);
    }

    void styleMenuKeepsSelectionAndOrigin_data() {
        QTest::addColumn<bool>("keyboard");
        QTest::newRow("mouse") << false;
        QTest::newRow("keyboard") << true;
    }

    void styleMenuKeepsSelectionAndOrigin() {
        QFETCH(bool, keyboard);
        ToolbarFixture f;
        QTRY_VERIFY(f.editor->activeEdit()->hasFocus());
        auto *style = f.button("style");
        if (keyboard) {
            f.toolbar->focusFirstControl();
            QTest::keyClick(f.button("bold"), Qt::Key_Tab);
            QTest::keyClick(f.button("italic"), Qt::Key_Tab);
            QTRY_VERIFY(style->hasFocus());
        }
        auto *menu = style->menu();
        QString selected;
        QTimer::singleShot(0, menu, [&] {
            QCoreApplication::processEvents();
            selected = f.editor->visualEdit()->textCursor().selectedText();
            menu->setActiveAction(actionNamed(menu, "Heading 2"));
            QTest::keyClick(menu, Qt::Key_Return);
        });
        if (keyboard) QTest::keyClick(style, Qt::Key_Space);
        else QTest::mouseClick(style, Qt::LeftButton);
        QCOMPARE(selected, QString("hello"));
        QCOMPARE(f.editor->visualEdit()->textCursor().selectedText(), QString("hello"));
        QVERIFY(f.editor->toMarkdownBytes().startsWith("## hello world"));
        QTRY_VERIFY(keyboard ? style->hasFocus() : f.editor->activeEdit()->hasFocus());
        QVERIFY(f.editor->undo());
        QCOMPARE(f.editor->toMarkdownBytes(), QByteArray("hello world\n"));
        QTimer::singleShot(0, menu, [&] { QCoreApplication::processEvents(); QTest::keyClick(menu, Qt::Key_Escape); });
        if (keyboard) QTest::keyClick(style, Qt::Key_Space);
        else QTest::mouseClick(style, Qt::LeftButton);
        QTRY_VERIFY(keyboard ? style->hasFocus() : f.editor->activeEdit()->hasFocus());
        QCOMPARE(f.editor->visualEdit()->textCursor().selectedText(), QString("hello"));
        if (keyboard) {
            auto *link = f.button("link");
            link->setFocus(Qt::TabFocusReason);
            QTimer::singleShot(0, f.editor, [&] {
                QCoreApplication::processEvents();
                auto *dialog = f.editor->findChild<QInputDialog *>();
                QVERIFY(dialog);
                dialog->setTextValue("https://example.com");
                dialog->accept();
            });
            QTest::keyClick(link, Qt::Key_Space);
            // Offscreen has no compositor to reactivate the owner after a native modal closes.
            f.host.activateWindow();
            QTRY_VERIFY(link->hasFocus());
            QVERIFY(f.editor->toMarkdownBytes().contains("[hello](https://example.com)"));
            QCOMPARE(f.editor->visualEdit()->textCursor().selectedText(), QString("hello"));
        }
    }

    void compactToolbarSurvivesLivePolish() {
        ToolbarFixture f;
        for (const QString &id : {QString("modernist"), QString("nord"), QString("dracula")}) {
            auto t = hn::theme::loadTheme(id, true);
            t.baseSize = 32;
            hn::theme::applyTheme(t);
            f.editor->setTheme(t);
            f.toolbar->setTheme(t);
            QTRY_COMPARE(f.host.styleSheet(), hn::theme::styleSheetFor(t));
            for (int width : {260, 116, 640}) {
                f.host.resize(width, 400);
                QTRY_COMPARE(f.toolbar->width(), width);
                QCOMPARE(f.toolbar->height(), hn::editor::FormattingToolbar::kHeight);
                QList<QRect> buttons;
                for (auto *b : f.toolbar->findChildren<QToolButton *>()) {
                    if (!b->isVisible()) continue;
                    const QRect rect = inWidget(b, f.toolbar);
                    QVERIFY(f.toolbar->rect().contains(rect));
                    QVERIFY(rect.width() >= 32);
                    for (const auto &other : buttons) QVERIFY(!rect.intersects(other));
                    buttons << rect;
                }
                for (const char *name : {"bold", "italic", "more"}) QVERIFY(f.button(name)->isVisible());
            }
            QCOMPARE(f.editor->toMarkdownBytes(), QByteArray("hello world\n"));
            QCOMPARE(f.editor->visualEdit()->textCursor().selectedText(), QString("hello"));
        }
    }

    void linkDialogKeepsNativeCancelAndRemoval() {
        ToolbarFixture f;
        f.editor->setLink("https://example.com");
        const auto linked = f.editor->toMarkdownBytes();
        bool seen = false;
        QTimer::singleShot(0, f.editor, [&] {
            auto *dialog = f.editor->findChild<QInputDialog *>();
            QVERIFY(dialog);
            seen = true;
            dialog->reject();
            QVERIFY(dialog->labelText().startsWith("Link address"));
            QVERIFY(dialog->labelText().contains("empty removes"));
        });
        f.editor->editLink();
        QVERIFY(seen);
        QCOMPARE(f.editor->toMarkdownBytes(), linked);
        QTimer::singleShot(0, f.editor, [&] {
            auto *dialog = f.editor->findChild<QInputDialog *>();
            QVERIFY(dialog);
            dialog->setTextValue("");
            dialog->accept();
        });
        f.editor->editLink();
        QCOMPARE(f.editor->toMarkdownBytes(), QByteArray("hello world\n"));
        QVERIFY(f.editor->undo());
        QCOMPARE(f.editor->toMarkdownBytes(), linked);
    }

    void completionLongLabelsHaveMeasuredEllipsis() {
        QWidget owner;
        hn::editor::CompletionPopup popup(&owner);
        auto theme = hn::theme::loadTheme("tokyo-night", true);
        QFont font(theme.fontFamily);
        font.setPixelSize(18);
        popup.setTheme(theme, font);
        popup.setItems({hn::editor::CompletionItem{
            "A very long note title that cannot fit in a narrow popup",
            "Work/Deep/Folder", "[[target]]", -1}}, "note");
        popup.resize(260, 64);
        const auto row = popup.rowLayout(0);
        QVERIFY(row.labelText.endsWith(QChar(0x2026)));
        QVERIFY(!row.labelRect.intersects(row.detailRect));
        QVERIFY(popup.rect().contains(row.labelRect));
        QVERIFY(popup.rect().contains(row.detailRect));
        QFont bold = font;
        bold.setWeight(QFont::DemiBold);
        QVERIFY(QFontMetrics(bold).horizontalAdvance(row.labelText) <= row.labelRect.width());
        QCOMPARE(popup.itemAt(0).insert, QString("[[target]]"));
    }

    void completionElisionUsesHighlightedMetricsAtAllSizes() {
        QWidget owner;
        hn::editor::CompletionPopup popup(&owner);
        auto t = hn::theme::loadTheme("nord", true);
        const QString label = QString("AVAV wide note title e\u0301 \U0001F600 ").repeated(10);
        for (int size : {8, 14, 18, 32}) for (int width : {80, 260, 440}) {
            QFont font(t.fontFamily);
            font.setPixelSize(size);
            popup.setTheme(t, font);
            QList<hn::editor::CompletionItem> items;
            for (int i = 0; i < 12; ++i) items << hn::editor::CompletionItem{label, "Work/Folder/Deep", "[[target]]", -1};
            popup.setItems(items, "AVAV");
            popup.resize(width, 8 * popup.rowHeight() + 2);
            const auto row = popup.rowLayout(0);
            QVERIFY(row.labelText.endsWith(QChar(0x2026)) || row.labelText.isEmpty());
            QVERIFY(popup.rect().contains(row.labelRect));
            QVERIFY(popup.rect().contains(row.detailRect));
            QVERIFY(!row.labelRect.intersects(row.detailRect));
            QFont bold = font;
            bold.setWeight(QFont::DemiBold);
            QVERIFY(QFontMetrics(bold).horizontalAdvance(row.labelText) <= row.labelRect.width());
            QVERIFY(QFontMetrics(font).horizontalAdvance(row.labelText) <= row.labelRect.width());
            QVERIFY(QFontMetrics(font).horizontalAdvance(row.detailText) <= row.detailRect.width());
            const auto image = popup.grab().toImage();
            QVERIFY(!image.isNull());
            popup.page(1);
            QCOMPARE(popup.current(), 8);
            const auto scrolled = popup.rowLayout(8);
            QVERIFY(popup.rect().contains(scrolled.labelRect));
            QCOMPARE(popup.itemAt(8).insert, QString("[[target]]"));
        }
    }

    void openCompletionRestylesWithoutLosingSession_data() {
        QTest::addColumn<bool>("source");
        QTest::newRow("visual") << false;
        QTest::newRow("source") << true;
    }

    void openCompletionRestylesWithoutLosingSession() {
        QFETCH(bool, source);
        apptest::Lib lib;
        lib.write("completion.md", "");
        hn::app::AppController controller(lib.opts());
        auto *session = controller.openSticky("completion.md");
        QVERIFY(session);
        session->setTiming({60000, 60000});
        auto *editor = session->editor();
        if (source) QVERIFY(editor->setMode(Mode::Source));
        editor->setCompletionTriggers({"[["});
        QSignalSpy requested(editor, &hn::editor::NoteEditor::completionRequested);
        QSignalSpy dismissed(editor, &hn::editor::NoteEditor::completionDismissed);
        editor->window()->activateWindow();
        editor->focusEditor();
        QTRY_VERIFY(editor->activeEdit()->hasFocus());
        QTest::keyClicks(editor->activeEdit(), "[[Al");
        QVERIFY(!requested.isEmpty());
        const int generation = qvariant_cast<hn::editor::CompletionRequest>(requested.last()[0]).generation;
        const int requestCount = requested.count();
        const QList<hn::editor::CompletionItem> items{{"Alpha", "Work", "[[Alpha]]", -1},
                                                    {"Alpha long title", "Work", "[[Alpha|alias]]", 4}};
        editor->showCompletions(items, generation);
        auto *popup = static_cast<hn::editor::CompletionPopup *>(editor->completionPopup());
        QVERIFY(popup);
        QVERIFY(popup->isVisible());
        QTest::keyClick(editor->activeEdit(), Qt::Key_Down);
        QCOMPARE(popup->current(), 1);
        const QByteArray literal = editor->toMarkdownBytes();
        const int revision = editor->revision();
        const int oldHeight = popup->rowHeight();
        for (const QString &id : {QString("nord"), QString("dracula")}) {
            auto settings = controller.settings();
            settings.theme = id;
            settings.colorScheme = "dark";
            settings.fontSize = 32;
            controller.applySettings(settings, controller.prefs());
            QTRY_COMPARE(editor->theme().bg, controller.theme().bg);
            QTRY_COMPARE(editor->window()->styleSheet(), hn::theme::styleSheetFor(controller.theme()));
            QTRY_COMPARE(popup->font().pixelSize(), 32);
            QCOMPARE(popup->font(), editor->activeEdit()->font());
            QVERIFY(popup->rowHeight() > oldHeight);
            QCOMPARE(popup->palette().color(QPalette::Text), controller.theme().text);
            const QImage image = popup->grab().toImage();
            QCOMPARE(image.pixelColor(popup->width() - 6, popup->rowHeight() / 2), controller.theme().surface);
            QVERIFY(editor->completionActive());
            QVERIFY(popup->isVisible());
            QCOMPARE(popup->current(), 1);
            QCOMPARE(requested.count(), requestCount);
            QCOMPARE(dismissed.count(), 0);
            QCOMPARE(editor->toMarkdownBytes(), literal);
            QCOMPARE(editor->revision(), revision);
            QVERIFY(editor->activeEdit()->hasFocus());
            QVERIFY(popup->screen()->availableGeometry().contains(popup->geometry()));
            editor->showCompletions({{"Stale", "", "bad", -1}}, generation - 1);
            QCOMPARE(popup->count(), 2);
            QCOMPARE(popup->current(), 1);
        }
        // The original request generation is still accepted after restyling.
        editor->showCompletions(items, generation);
        QCOMPARE(popup->current(), 0);
        QTest::keyClick(editor->activeEdit(), Qt::Key_Down);
        QTest::keyClick(editor->activeEdit(), Qt::Key_Return);
        QCOMPARE(editor->toMarkdownBytes(), source ? QByteArray("[[Alpha|alias]]") : QByteArray("[[Alpha|alias]]\n"));
        QCOMPARE(source ? editor->sourceEdit()->textCursor().position() : editor->visualEdit()->textCursor().position(), 4);
        QVERIFY(editor->undo());
        QCOMPARE(editor->toMarkdownBytes(), literal);
        QVERIFY(editor->redo());
        QVERIFY(editor->toMarkdownBytes().contains("[[Alpha|alias]]"));
    }

    void completionCornersFollowExactPopupRadius_data() {
        QTest::addColumn<int>("radius");
        QTest::addColumn<int>("expected");
        QTest::newRow("square") << 0 << 0;
        QTest::newRow("built-in") << 4 << 6;
        QTest::newRow("custom") << 12 << 14;
        QTest::newRow("clamped") << 32 << 32;
    }

    void completionCornersFollowExactPopupRadius() {
        QFETCH(int, radius);
        QFETCH(int, expected);
        QWidget owner;
        hn::editor::CompletionPopup popup(&owner);
        auto t = hn::theme::loadTheme("modernist", true);
        t.radius = radius;
        popup.setTheme(t, QFont(t.fontFamily));
        popup.setItems({{"", "", "", -1}, {"", "", "", -1}, {"", "", "", -1}}, "");
        popup.resize(260, 120);
        QImage image(popup.size(), QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        popup.render(&painter, QPoint(), QRegion(), QWidget::DrawChildren);
        painter.end();
        // Circular corner geometry, sampled away from the antialiasing band.
        for (int y = 0; y < qMax(1, expected); ++y) for (int x = 0; x < qMax(1, expected); ++x) {
            const qreal distance = std::hypot(expected - x - 0.5, expected - y - 0.5);
            if (expected && distance > expected + 1.0) QCOMPARE(image.pixelColor(x, y).alpha(), 0);
            if (!expected || distance < expected - 1.5) QCOMPARE(image.pixelColor(x, y).alpha(), 255);
        }
    }

    void sourceSurfaceStaysSquareUnderRoundedHost() {
        QWidget host;
        auto *layout = new QVBoxLayout(&host);
        hn::editor::NoteEditor editor;
        layout->addWidget(&editor);
        host.setStyleSheet("QWidget { background: #ff0000; } QPlainTextEdit { border-radius: 20px; }");
        auto t = hn::theme::loadTheme("modernist", true);
        editor.setTheme(t);
        editor.load("| one | two |\n| --- | --- |\n");
        QCOMPARE(editor.mode(), Mode::Source);
        host.resize(400, 300);
        host.show();
        auto *source = editor.sourceEdit();
        QImage image(source->size(), QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        source->render(&painter, QPoint(), QRegion(), QWidget::DrawWindowBackground);
        painter.end();
        QCOMPARE(image.pixelColor(0, 0), t.bg);
        QCOMPARE(image.pixelColor(image.width() - 1, 0), t.bg);
    }
};

int main(int argc, char **argv) {
    PolishProfile profile;
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    AppEditorPolishTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "app_editor_polish_test.moc"
