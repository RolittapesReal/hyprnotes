#include "editor_test_util.h"

#include <QDropEvent>
#include <QMouseEvent>

using namespace hntest;

class EditTest : public QObject {
    Q_OBJECT
private slots:
    void inlineShortcutsAndUndoRestoresLiteral()
    {
        Fx f;
        f.text("**bold**");
        QCOMPARE(f.plain(), QStringLiteral("bold"));
        QVERIFY(f.md().contains(QStringLiteral("**bold**")));
        f.key(Qt::Key_Z, Qt::ControlModifier);   // transformation is its own step: literal comes back
        QCOMPARE(f.plain(), QStringLiteral("**bold**"));
        f.key(Qt::Key_Z, Qt::ControlModifier);   // then the typing itself
        QCOMPARE(f.plain(), QString());
        f.key(Qt::Key_Y, Qt::ControlModifier);
        f.key(Qt::Key_Y, Qt::ControlModifier);
        QCOMPARE(f.plain(), QStringLiteral("bold"));

        Fx g;
        g.text("a *it* ~~s~~ `c` _u_ x");
        QCOMPARE(g.plain(), QStringLiteral("a it s c u x"));
        const QString m = g.md();
        QVERIFY2(m.contains(QStringLiteral("*it*")) || m.contains(QStringLiteral("_it_")), qPrintable(m));
        QVERIFY2(m.contains(QStringLiteral("~~s~~")), qPrintable(m));
        QVERIFY2(m.contains(QStringLiteral("`c`")), qPrintable(m));
        QVERIFY(!m.contains(QStringLiteral("**")));   // nothing became bold
        QCOMPARE(g.ed.recorder().unrecorded(), 0);
    }
    void nestedFormatting()
    {
        Fx f;
        f.text("x");
        f.select(0, 1);
        f.ed.toggleInline(InlineStyle::Bold);
        f.ed.toggleInline(InlineStyle::Italic);
        f.ed.toggleInline(InlineStyle::Strike);
        QVERIFY(f.ed.inlineActive(InlineStyle::Bold) && f.ed.inlineActive(InlineStyle::Italic) && f.ed.inlineActive(InlineStyle::Strike));
        const QString m = f.md();
        QVERIFY2(m.contains(QStringLiteral("~~")) && (m.contains(QStringLiteral("***")) || m.contains(QStringLiteral("**_"))), qPrintable(m));
        QVERIFY(sameSemantics(m, m));
        f.ed.toggleInline(InlineStyle::Bold);
        QVERIFY(!f.ed.inlineActive(InlineStyle::Bold) && f.ed.inlineActive(InlineStyle::Italic));
        f.key(Qt::Key_Z, Qt::ControlModifier);
        QVERIFY(f.ed.inlineActive(InlineStyle::Bold));
    }
    void blockShortcutsAndLists()
    {
        Fx f;
        f.text("# Title\nbody\n- a\nb\n\nafter");
        // Enter on empty item exits the list; heading Enter gives a paragraph.
        QCOMPARE(f.md(), QStringLiteral("# Title\n\nbody\n\n- a\n- b\n\nafter\n"));
        f.key(Qt::Key_Z, Qt::ControlModifier);
        f.key(Qt::Key_Z, Qt::ControlModifier);
        QVERIFY(f.md().endsWith(QStringLiteral("- b\n\n")) || f.md().contains(QStringLiteral("- b")));

        Fx g;
        g.text("1. one\ntwo\n\n> q\n- [ ] t\nu");
        const QString m = g.md();
        QVERIFY2(m.contains(QStringLiteral("1.  one")) || m.contains(QStringLiteral("1. one")), qPrintable(m));
        QVERIFY2(m.contains(QStringLiteral("2.")), qPrintable(m));
        QVERIFY2(m.contains(QStringLiteral("> q")), qPrintable(m));
        QVERIFY2(m.contains(QStringLiteral("- [ ] t")) && m.contains(QStringLiteral("- [ ] u")), qPrintable(m));
    }
    void tabIndentsOnlyListItemsAndEmptyItemExitsLevel()
    {
        Fx f;
        f.text("- a\nb\n");
        f.key(Qt::Key_Tab);   // empty item -> nested? (empty item has a previous sibling)
        f.text("c");
        QVERIFY2(f.md().contains(QStringLiteral("- a\n- b\n  - c")), qPrintable(f.md()));
        f.key(Qt::Key_Return);
        f.key(Qt::Key_Return);   // empty nested item: one level out
        f.text("d");
        QVERIFY2(f.md().contains(QStringLiteral("  - c\n- d")), qPrintable(f.md()));
        f.key(Qt::Key_Tab, Qt::ShiftModifier);   // top level: no-op
        QVERIFY2(f.md().contains(QStringLiteral("- d")), qPrintable(f.md()));
        // Tab in a plain paragraph must not insert a tab character
        Fx g;
        g.text("plain");
        g.key(Qt::Key_Tab);
        QCOMPARE(g.plain(), QStringLiteral("plain"));
        QCOMPARE(f.ed.recorder().unrecorded(), 0);
    }
    void shiftEnterHardBreak()
    {
        Fx f;
        f.text("one");
        f.key(Qt::Key_Return, Qt::ShiftModifier);
        f.text("two");
        QCOMPARE(f.v->document()->blockCount(), 1);
        QVERIFY2(f.md().contains(QStringLiteral("one\\\ntwo")), qPrintable(f.md()));
        f.key(Qt::Key_Return);
        f.text("three");
        QCOMPARE(f.v->document()->blockCount(), 2);
    }
    void codeFences()
    {
        Fx f;
        f.text("```py\nx = **1**\ny\n\n");
        f.text("after");
        const QString m = f.md();
        QVERIFY2(m.contains(QStringLiteral("```py\nx = **1**\ny\n```")), qPrintable(m));
        QVERIFY2(m.contains(QStringLiteral("\nafter")), qPrintable(m));
        QVERIFY(m.indexOf(QStringLiteral("after")) > m.lastIndexOf(QStringLiteral("```")));
        // undo of the fence conversion returns the literal fence text
        Fx g;
        g.text("```\n");
        g.key(Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(g.plain(), QStringLiteral("```"));
    }
    void imeCommitAndPreedit()
    {
        Fx f;
        f.preedit(QStringLiteral("にほ"));
        QVERIFY(qobject_cast<QTextEdit *>(f.w()));
        f.commit(QStringLiteral("日本"));
        f.text("x");
        QCOMPARE(f.plain(), QStringLiteral("日本x"));
        f.commit(QStringLiteral("**y**"));   // IME-committed delimiters never auto-format
        QCOMPARE(f.plain(), QStringLiteral("日本x**y**"));
        f.preedit(QStringLiteral("zz"));
        f.preedit(QString());
        QCOMPARE(f.plain(), QStringLiteral("日本x**y**"));
        f.key(Qt::Key_Z, Qt::ControlModifier);
        QVERIFY(f.plain().startsWith(QStringLiteral("日本")) || f.plain().isEmpty());
        QCOMPARE(f.ed.recorder().unrecorded(), 0);
    }
    void unicodeGraphemes()
    {
        Fx f;
        const QString combining = QStringLiteral("é");                  // e + combining acute
        const QString emoji = QString::fromUtf8("\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x92\xBB");   // woman technologist ZWJ
        const QString rtl = QString::fromUtf8("שלום עולם");
        f.text(combining);
        f.text(QStringLiteral(" "));
        f.commit(emoji);
        f.text(QStringLiteral(" "));
        f.commit(rtl);
        const QString all = combining + u' ' + emoji + u' ' + rtl;
        QCOMPARE(f.plain(), all);
        QCOMPARE(QString::fromUtf8(f.ed.toMarkdownBytes()).trimmed(), all);
        // backspacing never leaves a lone surrogate / replacement character behind
        f.moveEnd();
        for (int i = 0; i < rtl.size() + 1; ++i) f.key(Qt::Key_Backspace);   // the RTL run and the space
        QCOMPARE(f.plain(), combining + u' ' + emoji);
        f.key(Qt::Key_Backspace);
        const QString rest = f.plain();
        QVERIFY(!rest.contains(QChar::ReplacementCharacter));
        QVERIFY2(rest == combining + u' ' || rest == combining + u' ' + emoji.left(emoji.size() - 2), qPrintable(rest));
        for (qsizetype i = 0; i < rest.size(); ++i) QVERIFY(!rest.at(i).isHighSurrogate() || rest.at(i + 1).isLowSurrogate());
        QCOMPARE(f.ed.recorder().unrecorded(), 0);
        // undo everything returns to empty
        while (f.ed.canUndo()) f.ed.undo();
        QCOMPARE(f.plain(), QString());
    }
    void typingOverSelectionUndoRestoresTextAndSelection()
    {
        Fx f;
        f.text("hello world");
        f.ed.history().breakMerge();
        f.select(6, 11);
        f.text("X");
        QCOMPARE(f.plain(), QStringLiteral("hello X"));
        f.ed.undo();
        QCOMPARE(f.plain(), QStringLiteral("hello world"));
        QCOMPARE(f.v->textCursor().selectionStart(), 6);
        QCOMPARE(f.v->textCursor().selectionEnd(), 11);
        f.ed.redo();
        QCOMPARE(f.plain(), QStringLiteral("hello X"));
        // deleting across a paragraph boundary
        Fx g;
        g.text("one\ntwo");
        g.ed.history().breakMerge();
        QTextCursor c(g.v->document());
        c.setPosition(2);
        c.setPosition(5, QTextCursor::KeepAnchor);
        g.v->setTextCursor(c);
        g.key(Qt::Key_Backspace);
        QCOMPARE(g.plain(), QStringLiteral("onwo"));
        QCOMPARE(g.v->document()->blockCount(), 1);
        g.ed.undo();
        QCOMPARE(g.plain(), QStringLiteral("one\ntwo"));
        QCOMPARE(g.v->document()->blockCount(), 2);
    }
    void selectionAcrossListItems()
    {
        Fx f;
        f.text("- a\nb\nc\nd");
        f.selectAllText();
        f.ed.toggleList(ListKind::Bullet);   // all items are bullets: toggles the list off
        QCOMPARE(f.md(), QStringLiteral("a\n\nb\n\nc\n\nd\n"));
        f.key(Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(f.md(), QStringLiteral("- a\n- b\n- c\n- d\n"));
        // indent two middle items together
        QTextBlock b1 = f.v->document()->findBlockByNumber(1), b2 = f.v->document()->findBlockByNumber(2);
        f.select(b1.position() + 1, b2.position() + 1);
        f.key(Qt::Key_Tab);
        QCOMPARE(f.md(), QStringLiteral("- a\n  - b\n  - c\n- d\n"));
        f.key(Qt::Key_Tab, Qt::ShiftModifier);
        QCOMPARE(f.md(), QStringLiteral("- a\n- b\n- c\n- d\n"));
        f.ed.toggleList(ListKind::Ordered);
        QVERIFY2(f.md().contains(QStringLiteral("1.")), qPrintable(f.md()));
        QCOMPARE(f.ed.recorder().unrecorded(), 0);
    }
    void pasteCutDrop()
    {
        Fx f;
        f.text("hello world");
        // plain-text paste by default, even when html is on the clipboard
        auto *m = new QMimeData;
        m->setText(QStringLiteral("A\nB"));
        m->setHtml(QStringLiteral("<b>A</b><p>B</p>"));
        QApplication::clipboard()->setMimeData(m);
        f.moveEnd();
        f.key(Qt::Key_V, Qt::ControlModifier);
        QCOMPARE(f.plain(), QStringLiteral("hello worldA\nB"));
        QVERIFY(!f.md().contains(QStringLiteral("**")));
        f.key(Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(f.plain(), QStringLiteral("hello world"));
        f.key(Qt::Key_Y, Qt::ControlModifier);
        QCOMPARE(f.plain(), QStringLiteral("hello worldA\nB"));
        // cut
        f.select(0, 6);
        f.key(Qt::Key_X, Qt::ControlModifier);
        QCOMPARE(f.plain(), QStringLiteral("worldA\nB"));
        QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("hello "));
        f.key(Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(f.plain(), QStringLiteral("hello worldA\nB"));
        // external drop
        QMimeData dm;
        dm.setText(QStringLiteral("DROP"));
        const QPoint at = f.v->cursorRect(QTextCursor(f.v->document())).center();
        QDragEnterEvent en(at, Qt::CopyAction, &dm, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(f.v->viewport(), &en);
        QDropEvent de(QPointF(at), Qt::CopyAction, &dm, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(f.v->viewport(), &de);
        QVERIFY2(f.plain().contains(QStringLiteral("DROP")), qPrintable(f.plain()));
        f.key(Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(f.plain(), QStringLiteral("hello worldA\nB"));
        QCOMPARE(f.ed.recorder().unrecorded(), 0);
    }
    void pasteAsMarkdownAction()
    {
        Fx f;
        QApplication::clipboard()->setText(QStringLiteral("# H\n\n- x\n- y\n\n**b**"));
        f.ed.pasteAsMarkdown();
        const QString m = f.md();
        QVERIFY2(m.contains(QStringLiteral("# H")) && m.contains(QStringLiteral("- x")) && m.contains(QStringLiteral("**b**")), qPrintable(m));
        f.ed.undo();
        QCOMPARE(f.plain(), QString());
        QSignalSpy spy(&f.ed, &NoteEditor::pasteNeedsSource);
        QApplication::clipboard()->setText(QStringLiteral("| a | b |\n|---|---|\n| 1 | 2 |"));
        f.ed.pasteAsMarkdown();
        QCOMPARE(spy.count(), 1);
        QVERIFY(f.plain().contains(QStringLiteral("| a | b |")));
    }
    void checkboxMouseKeyboardUndo()
    {
        Fx f("- [ ] one\n- [x] two\n");
        QCOMPARE(f.ed.mode(), Mode::Visual);
        QTextBlock b = f.v->document()->findBlockByNumber(0);
        QVERIFY(b.blockFormat().marker() == QTextBlockFormat::MarkerType::Unchecked);
        f.select(2, 2);
        const int caret = f.v->textCursor().position();
        const QRect r = f.v->cursorRect(QTextCursor(b));
        const QPoint p(r.left() - 12, r.center().y());
        QTest::mouseClick(f.v->viewport(), Qt::LeftButton, Qt::NoModifier, p);
        QVERIFY(f.v->document()->findBlockByNumber(0).blockFormat().marker() == QTextBlockFormat::MarkerType::Checked);
        QCOMPARE(f.v->textCursor().position(), caret);   // caret preserved
        QVERIFY2(f.md().contains(QStringLiteral("- [x] one")), qPrintable(f.md()));
        f.key(Qt::Key_Z, Qt::ControlModifier);
        QVERIFY(f.v->document()->findBlockByNumber(0).blockFormat().marker() == QTextBlockFormat::MarkerType::Unchecked);
        QCOMPARE(f.v->textCursor().position(), caret);
        f.key(Qt::Key_Return, Qt::ControlModifier);   // keyboard toggle
        QVERIFY2(f.md().contains(QStringLiteral("- [x] one")), qPrintable(f.md()));
        QCOMPARE(f.v->textCursor().position(), caret);
        f.ed.undo();
        QVERIFY2(f.md().contains(QStringLiteral("- [ ] one")), qPrintable(f.md()));
        QCOMPARE(f.ed.recorder().unrecorded(), 0);
    }
    void linksOnlyOpenWithCtrlClick()
    {
        Fx f("see [site](https://example.org/x) now\n");
        QSignalSpy spy(&f.ed, &NoteEditor::linkActivated);
        const QTextBlock b = f.v->document()->findBlockByNumber(0);
        QTextCursor c(b);
        c.setPosition(b.position() + 6);
        const QPoint p = f.v->cursorRect(c).center() + QPoint(2, 0);
        QTest::mouseClick(f.v->viewport(), Qt::LeftButton, Qt::NoModifier, p);
        QCOMPARE(spy.count(), 0);
        QTest::mouseClick(f.v->viewport(), Qt::LeftButton, Qt::ControlModifier, p);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.first().first().toUrl(), QUrl(QStringLiteral("https://example.org/x")));
        // set/remove link via API
        Fx g;
        g.text("word");
        g.selectAllText();
        g.ed.setLink(QStringLiteral("https://a.b"));
        QVERIFY2(g.md().contains(QStringLiteral("[word](https://a.b)")), qPrintable(g.md()));
        g.ed.setLink(QString());
        QVERIFY2(!g.md().contains(QStringLiteral("](")), qPrintable(g.md()));
    }
    void headingAndQuoteViaApiAndUndo()
    {
        Fx f;
        f.text("title");
        f.ed.setBlockStyle(BlockStyle::H2);
        QCOMPARE(f.md(), QStringLiteral("## title\n"));
        f.ed.setBlockStyle(BlockStyle::Quote);
        QCOMPARE(f.md(), QStringLiteral("> title\n"));
        f.ed.setBlockStyle(BlockStyle::Code);
        QCOMPARE(f.md(), QStringLiteral("```\ntitle\n```\n"));
        f.ed.setBlockStyle(BlockStyle::Paragraph);
        QCOMPARE(f.md(), QStringLiteral("title\n"));
        for (int i = 0; i < 4; ++i) f.ed.undo();
        QCOMPARE(f.md(), QStringLiteral("title\n"));
        QCOMPARE(f.ed.blockStyle(), BlockStyle::Paragraph);
        f.ed.redo();
        QCOMPARE(f.ed.blockStyle(), BlockStyle::H2);
    }
};

QTEST_MAIN(EditTest)
#include "editor_edit_test.moc"
