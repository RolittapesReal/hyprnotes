// Prints p50/p95/p99/max event processing time for 1000 edits on a ~20 KiB note (target p95 < 4 ms).
#include <QElapsedTimer>
#include <QRandomGenerator>
#include <algorithm>

#include "editor_test_util.h"

using namespace hntest;

static QByteArray fixture20k()
{
    QByteArray md;
    int i = 0;
    while (md.size() < 20 * 1024) {
        md += "## Section " + QByteArray::number(i) + "\n\n";
        md += "Paragraph " + QByteArray::number(i) + " with some **bold** text, a bit of `code`, and enough plain words to wrap "
              "across a couple of lines in a narrow window so layout has real work to do.\n\n";
        md += "- first item " + QByteArray::number(i) + "\n- second item\n\n";
        ++i;
    }
    return md;
}

class PerfTest : public QObject {
    Q_OBJECT
private slots:
    void p95EventTimeFor1000Edits()
    {
        Fx f(fixture20k());
        QVERIFY2(f.ed.mode() == Mode::Visual, qPrintable(f.ed.modeReason()));
        qInfo() << "fixture bytes" << f.ed.toMarkdownBytes().size() << "blocks" << f.v->document()->blockCount();
        QRandomGenerator rng(12345);
        QList<double> ms;
        auto timed = [&](auto &&fn) {
            QElapsedTimer t;
            t.start();
            fn();
            QCoreApplication::processEvents();   // include layout + repaint triggered by the edit
            ms << t.nsecsElapsed() / 1e6;
        };
        for (int i = 0; i < 1000; ++i) {
            if (i % 25 == 0) {   // move the caret somewhere else (not timed)
                const int b = rng.bounded(f.v->document()->blockCount());
                QTextCursor c(f.v->document()->findBlockByNumber(b));
                c.movePosition(QTextCursor::EndOfBlock);
                f.v->setTextCursor(c);
            }
            const int kind = i % 10;
            if (kind == 9) timed([&] { f.key(Qt::Key_Return); });
            else if (kind == 8) timed([&] { f.key(Qt::Key_Backspace); });
            else timed([&] { QTest::keyClick(f.w(), char('a' + rng.bounded(26))); });
        }
        std::sort(ms.begin(), ms.end());
        auto pct = [&](double p) { return ms[qMin(ms.size() - 1, qsizetype(p * ms.size()))]; };
        qInfo().nospace() << "edit timing over " << ms.size() << " edits: p50=" << pct(0.50) << " ms  p95=" << pct(0.95)
                          << " ms  p99=" << pct(0.99) << " ms  max=" << ms.last() << " ms  (target p95 < 4 ms)";
        QCOMPARE(f.ed.recorder().unrecorded(), 0);
        QVERIFY(f.ed.history().count() > 0);
        QVERIFY2(pct(0.95) < 4.0, qPrintable(QStringLiteral("p95 %1 ms exceeds the 4 ms target").arg(pct(0.95))));
    }
};

QTEST_MAIN(PerfTest)
#include "editor_perf_test.moc"
