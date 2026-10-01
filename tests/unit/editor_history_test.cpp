#include <QTemporaryDir>

#include "editor_test_util.h"

using namespace hntest;

static Region textRegion(int chars, int start = 0)
{
    Region r;
    r.start = start;
    BlockSnap b;
    b.frags.append({QString(chars, u'x'), QTextCharFormat()});
    r.blocks.append(b);
    return r;
}
static Tx tx(TxKind k, int charsBefore, int charsAfter, int pos = 0)
{
    Tx t;
    t.kind = k;
    t.before = textRegion(charsBefore);
    t.after = textRegion(charsAfter);
    t.curBeforePos = t.curBeforeAnchor = pos;
    t.curAfterPos = t.curAfterAnchor = pos + 1;
    return t;
}

class HistoryTest : public QObject {
    Q_OBJECT
private slots:
    void capsTransactionsAt500()
    {
        TxHistory h;
        for (int i = 0; i < 600; ++i) h.push(tx(TxKind::Structure, 4, 5));
        QCOMPARE(h.count(), 500);
        QCOMPARE(h.evicted(), 100);
        QCOMPARE(h.position(), 500);
        QVERIFY(h.bytes() < TxHistory::kMaxBytes);
        int undone = 0;
        while (h.canUndo()) { h.takeForUndo(); ++undone; }
        QCOMPARE(undone, 500);
    }
    void capsPayloadAt2MiB()
    {
        TxHistory h;
        for (int i = 0; i < 100; ++i) h.push(tx(TxKind::Structure, 10, 50000));   // ~100 KiB each
        QVERIFY2(h.bytes() <= TxHistory::kMaxBytes, qPrintable(QString::number(h.bytes())));
        QVERIFY(h.count() < 25 && h.count() >= 15);
        QCOMPARE(h.evicted(), 100 - h.count());
    }
    void oversizeTransactionSpillsToDiskAndLoadsBack()
    {
        QTemporaryDir dir;
        TxHistory h;
        h.setSpillDir(dir.path());
        h.push(tx(TxKind::Structure, 3, 4));
        Tx big = tx(TxKind::Replace, 10, 1500000);   // ~3 MB of UTF-16
        big.textBefore = QStringLiteral("before");
        QCOMPARE(int(h.push(big)), int(TxHistory::PushResult::Stored));
        QCOMPARE(h.spilledCount(), 1);
        QVERIFY(h.bytes() < 4096);
        QCOMPARE(h.count(), 2);   // not evicted
        Tx back = h.takeForUndo();
        QCOMPARE(back.after.blocks.first().frags.first().text.size(), 1500000);
        QCOMPARE(back.textBefore, QStringLiteral("before"));
        // spill failure is reported, never silently dropped
        TxHistory bad;
        bad.setSpillDir(QStringLiteral("/nonexistent/definitely/not/here"));
        QVERIFY(!bad.canSpill());
        QCOMPARE(int(bad.push(tx(TxKind::Replace, 10, 1500000))), int(TxHistory::PushResult::SpillFailed));
        QCOMPARE(bad.count(), 0);
    }
    void typingMergesUntilPauseAndCap()
    {
        qint64 clock = 1000;
        TxHistory h;
        h.setClock([&] { return clock; });
        auto typed = [&](int pos, int len) {
            Tx t = tx(TxKind::Typing, len, len + 1, pos);
            t.curBeforePos = t.curBeforeAnchor = pos;
            t.curAfterPos = t.curAfterAnchor = pos + 1;
            return t;
        };
        QCOMPARE(int(h.push(typed(0, 0))), int(TxHistory::PushResult::Stored));
        clock += 300;
        QCOMPARE(int(h.push(typed(1, 1))), int(TxHistory::PushResult::Merged));
        clock += 300;
        QCOMPARE(int(h.push(typed(5, 2))), int(TxHistory::PushResult::Stored));   // cursor moved
        clock += 1500;                                                              // > 1 s pause
        QCOMPARE(int(h.push(typed(6, 3))), int(TxHistory::PushResult::Stored));
        clock += 10;
        h.breakMerge();                                                             // formatting change
        QCOMPARE(int(h.push(typed(7, 4))), int(TxHistory::PushResult::Stored));
        QCOMPARE(h.count(), 4);
        // merged payload is capped at 64 KiB
        TxHistory g;
        g.setClock([&] { return clock; });
        int pos = 0, len = 0, stored = 0;
        for (int i = 0; i < 400; ++i) {
            Tx t = tx(TxKind::Typing, len, len + 200, pos);
            t.curAfterPos = t.curAfterAnchor = pos + 200;
            stored += g.push(t) == TxHistory::PushResult::Stored;
            pos += 200;
            len += 200;
        }
        QVERIFY2(stored >= 2, "merging must stop at the 64 KiB cap");
        while (g.canUndo()) g.takeForUndo();
        TxHistory one;   // the first merged transaction never exceeds the cap
        one.setClock([&] { return clock; });
        int p2 = 0;
        for (int i = 0; i < 400 && one.count() < 2; ++i) {
            Tx t = tx(TxKind::Typing, 0, 200, p2);
            t.before = textRegion(0);
            t.after = textRegion(200);
            t.curAfterPos = t.curAfterAnchor = p2 + 200;
            one.push(t);
            p2 += 200;
        }
        QVERIFY(one.count() >= 1);
    }
    void newEditAfterUndoDiscardsRedo()
    {
        TxHistory h;
        for (int i = 0; i < 5; ++i) h.push(tx(TxKind::Structure, 1, 2));
        h.takeForUndo();
        h.takeForUndo();
        QVERIFY(h.canRedo());
        h.push(tx(TxKind::Structure, 1, 2));
        QVERIFY(!h.canRedo());
        QCOMPARE(h.count(), 4);
    }

    // ---- through the real editor ----
    void editorEvictsOldestButNeverTouchesContent()
    {
        Fx f;
        qint64 clock = 0;
        f.ed.history().setClock([&] { clock += 5000; return clock; });   // no typing merges
        for (int i = 0; i < 400; ++i) f.text(QStringLiteral("x\n"));   // small transactions: 2 per iteration
        const QString expect = f.plain();
        QCOMPARE(f.ed.history().count(), 500);
        QCOMPARE(f.ed.history().evicted(), 300);
        QVERIFY(f.ed.history().bytes() < TxHistory::kMaxBytes);
        while (f.ed.canUndo()) f.ed.undo();
        QVERIFY(f.plain().size() < expect.size());
        QCOMPARE(f.plain().size(), 300);   // the 300 evicted transactions' worth of content is still there (150 x + 150 newlines)
        // eviction only forgets history, never content
        while (f.ed.canRedo()) f.ed.redo();
        QCOMPARE(f.plain(), expect);
        QCOMPARE(f.ed.recorder().unrecorded(), 0);
        QVERIFY(f.ed.revision() > 800);           // revision advances independently of undo position
    }
    void editorPayloadBudgetAndOversizeSpill()
    {
        QTemporaryDir dir;
        Fx f;
        f.ed.history().setSpillDir(dir.path());
        auto lines = [](int chars) {
            QString s;
            while (s.size() < chars) s += QString(99, u'q') + u'\n';
            return s;
        };
        for (int i = 0; i < 8; ++i) {
            QApplication::clipboard()->setText(lines(120000));   // ~ 240 KiB of UTF-16 + block overhead
            f.key(Qt::Key_V, Qt::ControlModifier);
        }
        QVERIFY2(f.ed.history().bytes() <= TxHistory::kMaxBytes, qPrintable(QString::number(f.ed.history().bytes())));
        QVERIFY(f.ed.history().evicted() > 0);
        const QString before = f.plain();
        // a single transaction bigger than the whole budget is kept on disk and still undoable
        QApplication::clipboard()->setText(lines(1200000));
        f.key(Qt::Key_V, Qt::ControlModifier);
        QCOMPARE(f.ed.history().spilledCount(), 1);
        QVERIFY(f.ed.history().bytes() <= TxHistory::kMaxBytes);
        const QString after = f.plain();
        QVERIFY(after.size() > before.size() + 1000000);
        QVERIFY(f.ed.undo());
        QCOMPARE(f.plain(), before);
        QVERIFY(f.ed.redo());
        QCOMPARE(f.plain(), after);
        QCOMPARE(f.ed.recorder().unrecorded(), 0);
    }
    void recoveryWriteFailureIsReportedBeforePaste()
    {
        Fx f;
        f.ed.history().setSpillDir(QStringLiteral("/nonexistent/definitely/not/here"));
        QSignalSpy spy(&f.ed.recorder(), &EditRecorder::recoveryWriteFailed);
        QString s;
        while (s.size() < 700000) s += QString(99, u'q') + u'\n';
        QApplication::clipboard()->setText(s);
        f.key(Qt::Key_V, Qt::ControlModifier);
        QVERIFY(spy.count() >= 1);
        QCOMPARE(f.plain(), QString());   // edit refused rather than becoming unrecoverable
    }
    void saveStateIndependentOfUndo()
    {
        Fx f;
        qint64 clock = 0;
        f.ed.history().setClock([&] { clock += 5000; return clock; });
        f.text("abc");
        QVERIFY(f.ed.isModified());
        f.ed.markSaved();
        QVERIFY(!f.ed.isModified());
        const int rev = f.ed.revision();
        f.text("d");
        QVERIFY(f.ed.revision() > rev && f.ed.isModified());
        f.ed.undo();
        QVERIFY(f.ed.revision() > rev);   // undo is a new revision, not a return to the saved one
        QCOMPARE(QString::fromUtf8(f.ed.toMarkdownBytes()), QStringLiteral("abc\n"));
    }
};

QTEST_MAIN(HistoryTest)
#include "editor_history_test.moc"
