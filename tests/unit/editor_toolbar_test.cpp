#include <QLineEdit>
#include <QMenu>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include "editor_test_util.h"
#include "hn/editor/formatting_toolbar.h"
#include "hn/theme/theme.h"

using namespace hntest;

struct TbFx {
    QWidget host;
    QVBoxLayout *lay;
    FormattingToolbar *tb;
    NoteEditor *ed;
    QTextEdit *v;
    TbFx()
    {
        lay = new QVBoxLayout(&host);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(0);
        ed = new NoteEditor;
        tb = new FormattingToolbar(ed);
        lay->addWidget(tb);
        lay->addWidget(ed, 1);
        host.resize(640, 400);
        host.show();
        (void)QTest::qWaitForWindowExposed(&host);
        host.activateWindow();
        ed->load({});
        v = ed->visualEdit();
        ed->focusEditor();
        QTest::qWait(20);
    }
    QToolButton *btn(const char *name) { return tb->findChild<QToolButton *>(QString::fromLatin1(name)); }
    void click(const char *name) { QTest::mouseClick(btn(name), Qt::LeftButton); }
    QAction *menuAction(const char *menu, const QString &text)
    {
        for (QAction *a : tb->findChild<QMenu *>(QString::fromLatin1(menu))->actions()) if (a->text() == text) return a;
        return nullptr;
    }
    void typeText(const QString &s) { QTest::keyClicks(v, s); }
};

class ToolbarTest : public QObject {
    Q_OBJECT
private slots:
    void buttonsPreserveSelectionAndFocus()
    {
        TbFx f;
        f.typeText(QStringLiteral("hello world"));
        QTextCursor c(f.v->document());
        c.setPosition(0);
        c.setPosition(5, QTextCursor::KeepAnchor);
        f.v->setTextCursor(c);
        QVERIFY(f.v->hasFocus());
        const QRect geomBefore = f.ed->geometry();
        const QSize viewBefore = f.v->viewport()->size();
        const int tbH = f.tb->height();

        for (const char *name : {"bold", "italic", "bold", "italic", "bullet", "bullet", "numbered", "numbered", "checklist", "checklist"}) {
            f.click(name);
            QCOMPARE(f.v->textCursor().selectionStart(), 0);
            QCOMPARE(f.v->textCursor().selectionEnd(), 5);
            QVERIFY2(f.v->hasFocus(), name);
            QVERIFY2(QApplication::focusWidget() == f.v, name);
        }
        QCOMPARE(f.ed->geometry(), geomBefore);
        QCOMPARE(f.v->viewport()->size(), viewBefore);
        QCOMPARE(f.tb->height(), tbH);
        QCOMPARE(f.tb->height(), FormattingToolbar::kHeight);
        QCOMPARE(QString::fromUtf8(f.ed->toMarkdownBytes()), QStringLiteral("hello world\n") /* all toggled back off */);
    }
    void commandsApplyAndUndo()
    {
        TbFx f;
        f.typeText(QStringLiteral("hello world"));
        f.v->selectAll();
        f.click("bold");
        QVERIFY(f.btn("bold")->isChecked());
        QCOMPARE(QString::fromUtf8(f.ed->toMarkdownBytes()), QStringLiteral("**hello world**\n"));
        f.click("italic");
        QVERIFY(QString::fromUtf8(f.ed->toMarkdownBytes()).contains(QStringLiteral("***hello world***")));
        f.ed->undo();
        QCOMPARE(QString::fromUtf8(f.ed->toMarkdownBytes()), QStringLiteral("**hello world**\n"));
        f.click("checklist");
        QVERIFY(f.btn("checklist")->isChecked());
        QVERIFY2(QString::fromUtf8(f.ed->toMarkdownBytes()).contains(QStringLiteral("- [ ] ")), "checklist");
        f.menuAction("styleMenu", QStringLiteral("Heading 2"))->trigger();   // a heading is not a list item
        QVERIFY(QString::fromUtf8(f.ed->toMarkdownBytes()).contains(QStringLiteral("## ")));
        QVERIFY(f.v->hasFocus());
        f.menuAction("moreMenu", QStringLiteral("Strikethrough"))->trigger();
        QVERIFY(QString::fromUtf8(f.ed->toMarkdownBytes()).contains(QStringLiteral("~~")));
        f.menuAction("moreMenu", QStringLiteral("Heading 5"))->trigger();
        QVERIFY(QString::fromUtf8(f.ed->toMarkdownBytes()).contains(QStringLiteral("##### ")));
        f.menuAction("moreMenu", QStringLiteral("Code block"))->trigger();
        QVERIFY(QString::fromUtf8(f.ed->toMarkdownBytes()).contains(QStringLiteral("```")));
        QCOMPARE(f.ed->recorder().unrecorded(), 0);
    }
    void noSelectionSetsTypingFormat()
    {
        TbFx f;
        f.click("bold");
        f.typeText(QStringLiteral("x"));
        f.click("bold");
        f.typeText(QStringLiteral("y"));
        QCOMPARE(QString::fromUtf8(f.ed->toMarkdownBytes()), QStringLiteral("**x**y\n"));
        QVERIFY(f.v->hasFocus());
    }
    void overflowSourceToggleKeepsLayout()
    {
        TbFx f;
        f.typeText(QStringLiteral("abc"));
        const int h = f.tb->height();
        f.menuAction("moreMenu", QStringLiteral("Switch to source mode"))->trigger();
        QCOMPARE(f.ed->mode(), Mode::Source);
        QCOMPARE(f.tb->height(), h);
        QVERIFY(!f.btn("bold")->isEnabled());
        QVERIFY(f.btn("more")->isEnabled());
        QVERIFY(f.ed->sourceEdit()->hasFocus());
        QVERIFY(f.menuAction("moreMenu", QStringLiteral("Switch to visual mode")));
        f.menuAction("moreMenu", QStringLiteral("Switch to visual mode"))->trigger();
        QCOMPARE(f.ed->mode(), Mode::Visual);
        QVERIFY(f.btn("bold")->isEnabled());
    }
    void accessibilityAndIcons()
    {
        TbFx f;
        const auto buttons = f.tb->findChildren<QToolButton *>();
        QCOMPARE(buttons.size(), 8);
        for (auto *b : buttons) {
            QVERIFY2(!b->toolTip().isEmpty(), qPrintable(b->objectName()));
            QVERIFY2(!b->accessibleName().isEmpty(), qPrintable(b->objectName()));
            QVERIFY2(!b->icon().isNull(), qPrintable(b->objectName()));
            QCOMPARE(b->focusPolicy(), Qt::TabFocus);
        }
        QVERIFY(f.btn("bold")->toolTip().contains(QStringLiteral("Ctrl+B")));
        QCOMPARE(f.btn("bold")->accessibleName(), QStringLiteral("Bold"));
    }
    void themeAppliesWithoutMarkupInMarkdown()
    {
        TbFx f;
        f.typeText(QStringLiteral("themed text"));
        const QString before = QString::fromUtf8(f.ed->toMarkdownBytes());
        for (bool dark : {false, true}) {
            const auto t = hn::theme::loadTheme(QStringLiteral("modernist"), dark);
            f.ed->setTheme(t);
            f.tb->setTheme(t);
            QCOMPARE(f.v->document()->defaultFont().pixelSize(), t.baseSize);
            QCOMPARE(f.v->document()->defaultFont().weight(), QFont::Normal);
            QCOMPARE(int(f.v->document()->documentMargin()), t.padding);
            QVERIFY(f.v->styleSheet().contains(t.bg.name()));
            QVERIFY(qAbs(f.v->document()->begin().blockFormat().lineHeight() - t.lineHeight * 100) < 0.5);
            QCOMPARE(QString::fromUtf8(f.ed->toMarkdownBytes()), before);   // styling never reaches the file
            f.typeText(QStringLiteral("!"));
            QVERIFY2(!QString::fromUtf8(f.ed->toMarkdownBytes()).contains(QLatin1Char('<')), "no html/markup");
            f.ed->undo();
        }
        QCOMPARE(f.tb->height(), FormattingToolbar::kHeight);
    }
    void unfocusedEditorHasNoCaretTimer()
    {
        TbFx f;
        f.typeText(QStringLiteral("x"));
        QLineEdit other;
        other.show();
        (void)QTest::qWaitForWindowExposed(&other);
        other.activateWindow();
        other.setFocus();
        QTest::qWait(30);
        QVERIFY(!f.v->hasFocus());
        // QWidgetTextControl only runs its blink timer while it has focus; the editor adds no timers of its own.
        QCOMPARE(f.ed->findChildren<QTimer *>().size(), 0);
    }
};

QTEST_MAIN(ToolbarTest)
#include "editor_toolbar_test.moc"
