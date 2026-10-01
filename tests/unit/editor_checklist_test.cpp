#include <QDir>
#include <QMouseEvent>
#include <QPainter>

#include "../../src/editor/visual_edit.h"   // private: checkRect()
#include "editor_test_util.h"
#include "hn/theme/theme.h"

using namespace hntest;
using MT = QTextBlockFormat::MarkerType;

class ChecklistTest : public QObject {
    Q_OBJECT
    static constexpr const char *kMd = "- [ ] open task\n- [x] done task\n  - [ ] nested open\n  - [x] nested done\n- plain bullet\n\n1. one\n2. two\n";

    static QImage render(Fx &f, qreal dpr)
    {
        QWidget *vp = f.v->viewport();
        QImage img((vp->size() * dpr).expandedTo(QSize(1, 1)), QImage::Format_ARGB32_Premultiplied);
        img.setDevicePixelRatio(dpr);
        img.fill(Qt::transparent);
        vp->render(&img);
        return img;
    }
    static Fx *themed(bool dark)
    {
        auto *f = new Fx(kMd);
        f->ed.setTheme(hn::theme::loadTheme(QStringLiteral("modernist"), dark));
        QCoreApplication::processEvents();
        return f;
    }
    static VisualEdit *ve(Fx &f) { return static_cast<VisualEdit *>(f.v); }

private slots:
    void indentIsOnGrid()
    {
        Fx f(kMd);
        QCOMPARE(f.v->document()->indentWidth(), 24.0);
    }
    void clickTogglesAtBoxOnlyUndoableCaretKept()
    {
        std::unique_ptr<Fx> f(themed(false));
        QTextBlock b0 = f->v->document()->findBlockByNumber(0);
        QTextCursor c = f->v->textCursor();
        c.setPosition(b0.position() + 3);
        f->v->setTextCursor(c);
        const int caret = f->v->textCursor().position();
        const QRect r = ve(*f)->checkRect(b0);
        QTest::mouseClick(f->v->viewport(), Qt::LeftButton, Qt::NoModifier, r.center());
        QCOMPARE(b0.blockFormat().marker(), MT::Checked);
        QCOMPARE(f->v->textCursor().position(), caret);
        QCOMPARE(f->md().left(15), QStringLiteral("- [x] open task"));
        QVERIFY(f->ed.undo());
        QCOMPARE(f->v->document()->findBlockByNumber(0).blockFormat().marker(), MT::Unchecked);
        // click on the text, or far left of the box, does not toggle
        QTest::mouseClick(f->v->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(r.left() - 12, r.center().y()));
        QTest::mouseClick(f->v->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(r.right() + 30, r.center().y()));
        QCOMPARE(f->v->document()->findBlockByNumber(0).blockFormat().marker(), MT::Unchecked);
        // keyboard toggle unchanged
        QVERIFY(ve(*f)->toggleCheckAtCursor());
        QCOMPARE(f->v->document()->findBlockByNumber(0).blockFormat().marker(), MT::Checked);
    }
    void paintsOwnMarker_data()
    {
        QTest::addColumn<bool>("dark");
        QTest::addColumn<int>("dpr");
        QTest::newRow("light1") << false << 1;
        QTest::newRow("light2") << false << 2;
        QTest::newRow("dark1") << true << 1;
        QTest::newRow("dark2") << true << 2;
    }
    void paintsOwnMarker()
    {
        QFETCH(bool, dark);
        QFETCH(int, dpr);
        std::unique_ptr<Fx> f(themed(dark));
        const auto t = hn::theme::loadTheme(QStringLiteral("modernist"), dark);
        const QImage img = render(*f, dpr);
        // sample images
        const QString dir = qEnvironmentVariable("HN_SAMPLE_DIR");
        if (!dir.isEmpty() && dpr == 2) {
            QImage out = render(*f, 2);
            QVERIFY(out.save(QDir(dir).filePath(QStringLiteral("editor-checklist-%1.png").arg(dark ? "dark" : "light"))));
        }
        auto px = [&](QPoint p) { return img.pixelColor(p * dpr); };
        const QRect open = ve(*f)->checkRect(f->v->document()->findBlockByNumber(0));
        const QRect done = ve(*f)->checkRect(f->v->document()->findBlockByNumber(1));
        // unchecked: hairline border colour on the edge, background inside (no native "X")
        QCOMPARE(px(open.topLeft()), t.border);
        QCOMPARE(px(open.center()), t.bg);
        for (int d = 3; d < 13; ++d) QCOMPARE(px(open.topLeft() + QPoint(d, d)), t.bg);   // diagonal an "X" would cross
        // checked: accent fill with a light tick
        QCOMPARE(px(done.topLeft() + QPoint(1, 1)), t.accent);
        QCOMPARE(px(done.topLeft()), t.accent);
    }
};

QTEST_MAIN(ChecklistTest)
#include "editor_checklist_test.moc"
