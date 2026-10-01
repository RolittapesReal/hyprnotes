#include "editor_test_util.h"

using namespace hntest;

static const QByteArray kBom("\xEF\xBB\xBF");

class IoTest : public QObject {
    Q_OBJECT
private slots:
    void supportedNoteOpensVisualAndIsNotRewritten()
    {
        const QByteArray src = "# Title\n\nhello **w** and _e_\n\n- a\n- b\n";
        Fx f(src);
        QCOMPARE(f.ed.mode(), Mode::Visual);
        QVERIFY(f.ed.modeReason().isEmpty());
        QCOMPARE(f.ed.toMarkdownBytes(), src);   // byte-identical: opening never canonicalizes
        QVERIFY(!f.ed.isModified());
    }
    void underscoresKeepEmphasisSemantics()
    {
        Fx f("snake_case_word _em_ __strong__ a\\*b\n");
        QCOMPARE(f.ed.mode(), Mode::Visual);
        f.moveEnd();
        f.text("!");   // forces a re-export
        const QString m = f.md();
        QVERIFY2(f.ed.mode() == Mode::Visual, "still visual");
        QVERIFY2(m.contains(QStringLiteral("snake_case_word")), qPrintable(m));
        QVERIFY2(m.contains(QStringLiteral("**strong**")), qPrintable(m));
        QVERIFY2(m.contains(QStringLiteral("*em*")) || m.contains(QStringLiteral("_em_")), qPrintable(m));
        QVERIFY(!m.contains(QStringLiteral("<u>")));
        QVERIFY(sameSemantics(m, QStringLiteral("snake_case_word _em_ __strong__ a\\*b!\n")));
    }
    void saveReopenSemanticEquality()
    {
        Fx f;
        f.text("# Plan\nintro **bold** *it* ~~old~~ `code`\n- one\ntwo\n\t");
        f.text("nested\n\n\n> quote\n\n1. first\nsecond\n\n- [ ] todo\n");
        f.key(Qt::Key_Return);
        f.text("```py\nprint(1)\n\nend");
        const QByteArray a = f.ed.toMarkdownBytes();
        QVERIFY(!a.isEmpty());
        Fx g(a);
        QVERIFY2(g.ed.mode() == Mode::Visual, qPrintable(g.ed.modeReason()));
        QCOMPARE(g.plain(), f.plain());
        QCOMPARE(g.v->document()->blockCount(), f.v->document()->blockCount());
        g.moveEnd();
        g.text("x");
        g.key(Qt::Key_Backspace);   // edit + revert forces a fresh export
        QVERIFY2(sameSemantics(QString::fromUtf8(a), g.md()), qPrintable(QString::fromUtf8(a) + "\n---\n" + g.md()));
        QVERIFY(sameSemantics(QString::fromUtf8(a), f.md()));
    }
    void unsupportedContentOpensInSourceWithBytesPreserved()
    {
        const QList<QPair<QByteArray, QString>> cases = {
            {"| a | b |\n|---|---|\n| 1 | 2 |\n", "table"},
            {"text <div>html</div>\n", "html"},
            {"---\ntitle: x\n---\n\nbody\n", "front matter"},
            {"![img](a.png)\n", "image"},
            {"note[^1]\n\n[^1]: fn\n", "footnote"},
            {"$$x^2$$\n", "math"},
        };
        for (const auto &c : cases) {
            Fx f(c.first);
            QVERIFY2(f.ed.mode() == Mode::Source, c.second.toLatin1().constData());
            QVERIFY2(!f.ed.modeReason().isEmpty(), c.second.toLatin1().constData());
            QCOMPARE(f.ed.toMarkdownBytes(), c.first);
            QCOMPARE(f.ed.sourceEdit()->toPlainText().toUtf8(), c.first);
        }
    }
    void largeNoteOpensInSource()
    {
        QByteArray big;
        while (big.size() <= 256 * 1024) big += "line of text for the large note\n\n";
        Fx f(big);
        QCOMPARE(f.ed.mode(), Mode::Source);
        QVERIFY(!f.ed.modeReason().isEmpty());
        QCOMPARE(f.ed.toMarkdownBytes(), big);
    }
    void sourceSavePreservesBomAndNewlines()
    {
        const QByteArray src = kBom + "| a |\r\n|---|\r\n| 1 |\r\n\r\ntail\r\n";
        Fx f(src);
        QCOMPARE(f.ed.mode(), Mode::Source);
        QCOMPARE(f.ed.toMarkdownBytes(), src);
        QTextCursor c = f.ed.sourceEdit()->textCursor();
        c.movePosition(QTextCursor::End);
        f.ed.sourceEdit()->setTextCursor(c);
        f.text("X");
        QCOMPARE(f.ed.toMarkdownBytes(), kBom + "| a |\r\n|---|\r\n| 1 |\r\n\r\ntail\r\nX");
        // visual notes keep their convention as well
        Fx g(kBom + "hello\r\n\r\nworld\r\n");
        QCOMPARE(g.ed.mode(), Mode::Visual);
        g.moveEnd();
        g.text("!");
        const QByteArray out = g.ed.toMarkdownBytes();
        QVERIFY(out.startsWith(kBom));
        QVERIFY2(out.contains("\r\n") && !out.contains("\r\r") && !QByteArray(out).replace("\r\n", "").contains('\n'), out.toHex().constData());
    }
    void invalidUtf8IsReadOnlyAndPreserved()
    {
        const QByteArray src = QByteArray("ab\xff\xfe cd\n");
        Fx f(src);
        QCOMPARE(f.ed.mode(), Mode::Source);
        QVERIFY(f.ed.isReadOnly() && f.ed.hasInvalidUtf8());
        QVERIFY(f.ed.sourceEdit()->isReadOnly());
        f.text("zzz");
        f.key(Qt::Key_Backspace);
        QCOMPARE(f.ed.toMarkdownBytes(), src);
        QSignalSpy refused(&f.ed, &NoteEditor::switchRefused);
        QVERIFY(!f.ed.setMode(Mode::Visual));
        QCOMPARE(refused.count(), 1);
        f.ed.convertInvalidUtf8();   // explicit action
        QVERIFY(!f.ed.isReadOnly());
        QVERIFY(f.ed.isModified());
        QVERIFY(f.ed.toMarkdownBytes().contains("\xEF\xBF\xBD"));
        f.text("ok");
        QVERIFY(f.ed.sourceEdit()->toPlainText().contains(QStringLiteral("ok")));
    }
    void softAndHardBreaksStayVisual()
    {
        const QByteArray src = "line one  \nline two\n\nsoft\nwrapped\n";
        Fx f(src);
        QCOMPARE(f.ed.mode(), Mode::Visual);
        QCOMPARE(f.ed.toMarkdownBytes(), src);   // opening never rewrites
        QVERIFY(f.v->document()->blockCount() == 2);   // hard break stays inside one block
    }
    void roundTripFailureFallsBackToSource()
    {
        // A list inside a block quote cannot be represented: the check must refuse visual mode and keep the bytes.
        const QByteArray src = "> - item one\n> - two\n";
        Fx f(src);
        QCOMPARE(f.ed.toMarkdownBytes(), src);
        QCOMPARE(f.ed.mode(), Mode::Source);
        QVERIFY(f.ed.modeReason().contains(QStringLiteral("check")));
    }
    void modeSwitchIsUndoable()
    {
        Fx f;
        QSignalSpy modes(&f.ed, &NoteEditor::modeChanged);
        f.text("hello **w**");
        QVERIFY(f.ed.setMode(Mode::Source));
        QCOMPARE(f.ed.mode(), Mode::Source);
        QCOMPARE(f.ed.sourceEdit()->toPlainText(), QStringLiteral("hello **w**\n"));
        f.moveEnd();
        QTextCursor c = f.ed.sourceEdit()->textCursor();
        c.movePosition(QTextCursor::End);
        f.ed.sourceEdit()->setTextCursor(c);
        f.text("\ntail");
        QCOMPARE(f.ed.sourceEdit()->toPlainText(), QStringLiteral("hello **w**\n\ntail"));
        QVERIFY(f.ed.setMode(Mode::Visual));
        QCOMPARE(f.ed.mode(), Mode::Visual);
        QVERIFY(f.plain().contains(QStringLiteral("tail")));
        // undo: visual->source, source typing, source->visual, then the visual typing itself
        QVERIFY(f.ed.undo());
        QCOMPARE(f.ed.mode(), Mode::Source);
        QCOMPARE(f.ed.sourceEdit()->toPlainText(), QStringLiteral("hello **w**\n\ntail"));
        int srcUndos = 0;
        while (f.ed.mode() == Mode::Source && f.ed.sourceEdit()->toPlainText() != QStringLiteral("hello **w**\n")) { QVERIFY(f.ed.undo()); ++srcUndos; }
        QVERIFY(srcUndos >= 1);   // Enter and the typed word are separate steps
        QVERIFY(f.ed.undo());     // source -> visual switch
        QCOMPARE(f.ed.mode(), Mode::Visual);
        QCOMPARE(f.plain(), QStringLiteral("hello w"));
        QVERIFY(f.ed.undo());   // the auto-format
        QCOMPARE(f.plain(), QStringLiteral("hello **w**"));
        while (f.ed.canRedo()) QVERIFY(f.ed.redo());
        QCOMPARE(f.ed.mode(), Mode::Visual);
        QVERIFY(f.plain().contains(QStringLiteral("tail")));
        QCOMPARE(f.ed.recorder().unrecorded(), 0);
        QVERIFY(modes.count() >= 6);
    }
    void failedSwitchToVisualKeepsSource()
    {
        Fx f;
        f.ed.setMode(Mode::Source);
        f.text("| a |\n|---|\n| 1 |\n");
        QSignalSpy refused(&f.ed, &NoteEditor::switchRefused);
        QVERIFY(!f.ed.setMode(Mode::Visual));
        QCOMPARE(refused.count(), 1);
        QCOMPARE(f.ed.mode(), Mode::Source);
        QCOMPARE(f.ed.sourceEdit()->toPlainText(), QStringLiteral("| a |\n|---|\n| 1 |\n"));
    }
    void untouchedSourceSwitchKeepsExactText()
    {
        Fx g("a\n\n*b*\n");
        QCOMPARE(g.ed.mode(), Mode::Visual);
        g.ed.setMode(Mode::Source);
        QCOMPARE(g.ed.sourceEdit()->toPlainText(), QStringLiteral("a\n\n*b*\n"));   // unedited note shows its exact bytes
    }
};

QTEST_MAIN(IoTest)
#include "editor_io_test.moc"
