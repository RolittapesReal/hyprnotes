// Plugin API v2 (editor half): wiki-link overlay, activation, completion popup.
#include <QDir>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QRandomGenerator>
#include <QScrollBar>
#include <QScreen>
#include <algorithm>

#include "editor_test_util.h"
#include "hn/core/links.h"
#include "hn/theme/theme.h"

using namespace hntest;

Q_DECLARE_METATYPE(hn::editor::LinkRefInfo)
Q_DECLARE_METATYPE(hn::editor::CompletionRequest)

static LinkState resolve(const QString &t)
{
    if (t.startsWith(QLatin1String("Missing")) || t.startsWith(QLatin1String("Roadmap"))) return LinkState::Unresolved;
    if (t.startsWith(QLatin1String("Dup"))) return LinkState::Ambiguous;
    return LinkState::Resolved;
}

struct LFx : Fx {
    int base = 0;   // NoteEditor::setTheme() re-lays out the document outside a recorder scope (pre-existing): count from here
    explicit LFx(const QByteArray &md = {}, bool dark = false, bool links = true) : Fx()
    {
        ed.setTheme(hn::theme::loadTheme(QStringLiteral("modernist"), dark));
        ed.load(md);
        base = ed.recorder().unrecorded();
        if (links) {
            ed.setLinkResolver(resolve);
            ed.setWikiLinksEnabled(true);
        }
        QCoreApplication::processEvents();
    }
    QPoint at(int block, int offset)
    {
        QTextEdit *ve = ed.visualEdit();
        QPlainTextEdit *se = ed.sourceEdit();
        if (ed.mode() == Mode::Visual) {
            QTextCursor c(ve->document()->findBlockByNumber(block));
            c.setPosition(c.block().position() + offset);
            return ve->cursorRect(c).center() + QPoint(2, 0);
        }
        QTextCursor c(se->document()->findBlockByNumber(block));
        c.setPosition(c.block().position() + offset);
        return se->cursorRect(c).center() + QPoint(2, 0);
    }
    QWidget *vp() { return ed.mode() == Mode::Visual ? static_cast<QWidget *>(ed.visualEdit()->viewport()) : ed.sourceEdit()->viewport(); }
    void moveCaret(int block, int offset)
    {
        QTextCursor c(ed.mode() == Mode::Visual ? ed.visualEdit()->document() : ed.sourceEdit()->document());
        c.setPosition(c.document()->findBlockByNumber(block).position() + offset);
        if (ed.mode() == Mode::Visual) ed.visualEdit()->setTextCursor(c); else ed.sourceEdit()->setTextCursor(c);
    }
    QString plainNow() { return ed.mode() == Mode::Visual ? ed.visualEdit()->toPlainText() : ed.sourceEdit()->toPlainText(); }
};

static QList<CompletionItem> items(int n, const QString &prefix = QStringLiteral("Item"))
{
    QList<CompletionItem> l;
    for (int i = 0; i < n; ++i) l.append({prefix + QString::number(i), QStringLiteral("detail %1").arg(i), QStringLiteral("[[%1%2]]").arg(prefix).arg(i), -1});
    return l;
}

static QByteArray fixture20k(bool links)
{
    QByteArray md;
    int i = 0;
    while (md.size() < 20 * 1024) {
        md += "## Section " + QByteArray::number(i) + "\n\n";
        md += "Paragraph " + QByteArray::number(i) + " with some **bold** text, a bit of `code`, and enough plain words to wrap "
              "across a couple of lines in a narrow window so layout has real work to do.";
        if (links) md += " See [[Alpha " + QByteArray::number(i) + "]] and [[Missing|x]].";
        md += "\n\n- first item " + QByteArray::number(i) + "\n- second item\n\n";
        ++i;
    }
    return md;
}

class LinksTest : public QObject {
    Q_OBJECT
private slots:
    // ---- scanner ----
    void scannerGrammar()
    {
        const QString t = QStringLiteral("a [[Ünï/日本語|al ias]] ![[pic.png]] [[Note#Head|x]] \\[[esc]] `[[code]]` [[#h]] [[a[[b]] [[ ]] [[x]");
        const auto r = scanWikiLinks(t, {}, true);
        QCOMPARE(r.size(), 4);
        QCOMPARE(r[0].ref.target, QStringLiteral("Ünï/日本語"));
        QCOMPARE(r[0].ref.alias, QStringLiteral("al ias"));
        QCOMPARE(t.mid(r[0].start, r[0].end - r[0].start), QStringLiteral("[[Ünï/日本語|al ias]]"));
        QCOMPARE(r[1].ref.kind, LinkRefInfo::Kind::Embed);
        QCOMPARE(t.mid(r[1].start, 3), QStringLiteral("![["));
        QCOMPARE(r[2].ref.target, QStringLiteral("Note"));
        QCOMPARE(r[2].ref.anchor, QStringLiteral("Head"));
        QCOMPARE(r[3].ref.target, QStringLiteral("b"));   // "[[a[[b]]": the inner pair is the link (core rule)
        QCOMPARE(t.mid(r[2].targetStart, r[2].targetEnd - r[2].targetStart), QStringLiteral("Note"));
    }
    void scannerAgreesWithCore()
    {
        const QString md = QStringLiteral("x [[A]] y [[B|b]] ![[C#d]] [[ E ]] z [[Ü]]\n");
        const auto core = hn::core::extractLinks(md);
        const auto mine = scanWikiLinks(md.left(md.size() - 1));
        QCOMPARE(mine.size(), core.size());
        for (int i = 0; i < mine.size(); ++i) {
            QCOMPARE(mine[i].ref.target, core[i].target);
            QCOMPARE(mine[i].ref.alias, core[i].alias);
            QCOMPARE(mine[i].ref.anchor, core[i].anchor);
            QCOMPARE(int(mine[i].ref.kind), int(core[i].kind));
        }
    }

    // ---- overlay ranges ----
    void visualRangesStatesAndCode()
    {
        LFx f("see [[Alpha]] and [[Missing|al]] and [[Dup#h]] ![[pic.png]] and `[[code]]` ok\n\n```\n[[fenced]]\n```\n\n[web](http://x.org) [[Ünï]]\n");
        QCOMPARE(f.ed.mode(), Mode::Visual);
        const auto r = f.ed.linkRangesInBlock(0);
        QCOMPARE(r.size(), 4);
        QCOMPARE(r[0].state, LinkState::Resolved);
        QCOMPARE(r[1].state, LinkState::Unresolved);
        QCOMPARE(r[2].state, LinkState::Ambiguous);
        QCOMPARE(r[3].ref.kind, LinkRefInfo::Kind::Embed);
        QCOMPARE(f.plain().mid(r[0].start, r[0].end - r[0].start), QStringLiteral("[[Alpha]]"));
        QCOMPARE(f.ed.linkRangesInBlock(1).size(), 0);   // fenced code block
        QCOMPARE(f.ed.linkRangesInBlock(f.v->document()->blockCount() - 1).size(), 1);
        const auto hit = f.ed.linkAt(f.at(0, 7));
        QVERIFY(hit.has_value());
        QCOMPARE(hit->target, QStringLiteral("Alpha"));
        QVERIFY(!f.ed.linkAt(f.at(0, 1)).has_value());
        QVERIFY(!f.ed.linkAt(QPoint(f.vp()->width() - 3, f.at(0, 1).y())).has_value());   // empty space right of the text
    }
    void sourceRangesFencesAndSpans()
    {
        LFx f("[[One]] `[[no]]` \\[[no]] [[Two|t]]\n```\n[[fenced]]\n```\n[[Three]]\n");
        QVERIFY(f.ed.setMode(Mode::Source));
        QCOMPARE(f.ed.linkRangesInBlock(0).size(), 2);
        QCOMPARE(f.ed.linkRangesInBlock(2).size(), 0);
        QCOMPARE(f.ed.linkRangesInBlock(4).size(), 1);
        QCOMPARE(f.ed.linkRangesInBlock(4)[0].ref.line, 5);
        // editing a fence line re-evaluates later blocks
        f.moveCaret(1, 0);
        f.key(Qt::Key_Delete);   // "``` " -> "``"
        QCOMPARE(f.ed.linkRangesInBlock(2).size(), 1);
        QCOMPARE(f.ed.linkRangesInBlock(4).size(), 0);   // fence parity flipped: now inside code
        QVERIFY(f.ed.undo());
        QCOMPARE(f.ed.linkRangesInBlock(2).size(), 0);
        QCOMPARE(f.ed.linkRangesInBlock(4).size(), 1);
    }
    void cacheInvalidatedByEditsAndCodeToggle()
    {
        LFx f("alpha [[Alpha]] omega\n");
        QCOMPARE(f.ed.linkRangesInBlock(0).size(), 1);
        f.moveCaret(0, 0);
        f.text("zz");
        QCOMPARE(f.ed.linkRangesInBlock(0)[0].start, 8);
        // make the link text code (format-only change): ranges must vanish
        QTextCursor c(f.v->document());
        c.setPosition(8);
        c.setPosition(17, QTextCursor::KeepAnchor);
        f.v->setTextCursor(c);
        f.ed.toggleInline(InlineStyle::Code);
        QCOMPARE(f.ed.linkRangesInBlock(0).size(), 0);
        f.ed.undo();
        QCOMPARE(f.ed.linkRangesInBlock(0).size(), 1);
    }
    void resolverCachedUntilInvalidated()
    {
        LFx f("[[Alpha]] [[Alpha]] [[Beta]]\n", false, false);
        int calls = 0;
        LinkState answer = LinkState::Resolved;
        f.ed.setLinkResolver([&](const QString &) { ++calls; return answer; });
        f.ed.setWikiLinksEnabled(true);
        QCOMPARE(f.ed.linkRangesInBlock(0)[0].state, LinkState::Resolved);
        f.ed.linkRangesInBlock(0);
        f.v->viewport()->grab();
        QCOMPARE(calls, 2);   // Alpha, Beta, each once
        answer = LinkState::Unresolved;
        QCOMPARE(f.ed.linkRangesInBlock(0)[0].state, LinkState::Resolved);   // still cached
        f.ed.invalidateLinkStates();
        QCOMPARE(f.ed.linkRangesInBlock(0)[0].state, LinkState::Unresolved);
    }
    void roundTripUnchangedWithOverlay()
    {
        const QByteArray md = "# T\n\nsee [[Alpha]] and [[Missing|a]] ![[p.png]] and `[[c]]`\n\n```\n[[fenced]]\n```\n\n- item [[Dup]]\n";
        auto run = [&](bool on, bool paint) {
            LFx f(md, false, on);
            f.moveEnd();
            f.text(" tail [[x]]");
            if (paint) f.v->viewport()->grab();
            const QByteArray visual = f.ed.toMarkdownBytes();
            f.ed.setMode(Mode::Source);
            if (paint) f.ed.sourceEdit()->viewport()->grab();
            return QList<QByteArray>{visual, f.ed.toMarkdownBytes()};
        };
        // painting / hovering never changes what is saved
        QCOMPARE(run(true, true), run(true, false));
        QCOMPARE(run(false, true), run(false, false));
        // links survive a visual-mode save verbatim whether or not the overlay is on
        const auto off = run(false, false), on = run(true, false);
        QCOMPARE(off[0], on[0]);
        QVERIFY(!off[0].contains("\\[\\["));
        QVERIFY(on[0].contains("see [[Alpha]] and [[Missing|a]] ![[p.png]] and `[[c]]`"));
        QVERIFY(on[0].contains("tail [[x]]"));
        QCOMPARE(on[1], on[0]);   // visual -> source switch keeps the same bytes
        const auto links = hn::core::extractLinks(on[0]);
        QStringList targets;
        for (const auto &l : links) targets << l.target;
        QCOMPARE(targets, (QStringList{"Alpha", "Missing", "p.png", "Dup", "x"}));
        QVERIFY(on[0].contains("```\n[[fenced]]\n```"));
        // unedited: bytes identical to the input
        LFx g(md);
        g.v->viewport()->grab();
        QCOMPARE(g.ed.toMarkdownBytes(), md);
        QVERIFY(!g.ed.isModified());
    }
    void paintsOverlayOnlyWhenEnabled()
    {
        LFx f("[[Alpha]] and [[Missing]]\n", false, false);
        const QImage off = f.v->viewport()->grab().toImage();
        f.ed.setLinkResolver(resolve);
        f.ed.setWikiLinksEnabled(true);
        const QImage on = f.v->viewport()->grab().toImage();
        QVERIFY(on != off);
        f.ed.setWikiLinksEnabled(false);
        QCOMPARE(f.v->viewport()->grab().toImage(), off);
        QVERIFY(!f.ed.wikiLinksEnabled());
    }

    // ---- activation ----
    void ctrlClickActivatesPlainClickPlacesCaret()
    {
        LFx f("go [[Notes/Alpha#Intro|the alpha]] now\n");
        qRegisterMetaType<LinkRefInfo>();
        QSignalSpy spy(&f.ed, &NoteEditor::wikiLinkActivated);
        const QPoint p = f.at(0, 10);
        QTest::mouseClick(f.vp(), Qt::LeftButton, Qt::NoModifier, p);
        QCOMPARE(spy.count(), 0);
        QVERIFY(f.v->textCursor().position() >= 5 && f.v->textCursor().position() <= 14);
        f.moveCaret(0, 0);
        QTest::mouseClick(f.vp(), Qt::LeftButton, Qt::ControlModifier, p);
        QCOMPARE(spy.count(), 1);
        const auto info = spy.first().first().value<LinkRefInfo>();
        QCOMPARE(info.target, QStringLiteral("Notes/Alpha"));
        QCOMPARE(info.anchor, QStringLiteral("Intro"));
        QCOMPARE(info.alias, QStringLiteral("the alpha"));
        QCOMPARE(info.line, 1);
        QVERIFY(f.v->textCursor().position() > 2);   // the ctrl+click placed the caret too
        // click outside any link: nothing
        QTest::mouseClick(f.vp(), Qt::LeftButton, Qt::ControlModifier, f.at(0, 1));
        QCOMPARE(spy.count(), 1);
    }
    void plainClickActivatesOnlyWhenEnabledAndNotDragging()
    {
        LFx f("go [[Alpha]] now [[Beta]]\n");
        QSignalSpy spy(&f.ed, &NoteEditor::wikiLinkActivated);
        f.ed.setLinkClickActivates(true);
        QVERIFY(f.ed.linkClickActivates());
        QTest::mouseClick(f.vp(), Qt::LeftButton, Qt::NoModifier, f.at(0, 6));
        QCOMPARE(spy.count(), 1);
        QCOMPARE(f.v->textCursor().position(), f.v->cursorForPosition(f.at(0, 6)).position());
        // drag-select across the link: no activation
        QTest::mousePress(f.vp(), Qt::LeftButton, Qt::NoModifier, f.at(0, 4));
        QTest::mouseMove(f.vp(), f.at(0, 9));
        QTest::mouseRelease(f.vp(), Qt::LeftButton, Qt::NoModifier, f.at(0, 9));
        QCOMPARE(spy.count(), 1);
        QVERIFY(f.v->textCursor().hasSelection());
        // press on a link, release elsewhere: no activation
        QTest::mousePress(f.vp(), Qt::LeftButton, Qt::NoModifier, f.at(0, 6));
        QTest::mouseRelease(f.vp(), Qt::LeftButton, Qt::NoModifier, f.at(0, 1));
        QCOMPARE(spy.count(), 1);
        f.ed.setLinkClickActivates(false);
        QTest::mouseClick(f.vp(), Qt::LeftButton, Qt::NoModifier, f.at(0, 6));
        QCOMPARE(spy.count(), 1);
    }
    void ctrlEnterInsideLink()
    {
        LFx f("a [[Alpha]] b\n\n- [ ] task [[Beta]]\n");
        QSignalSpy spy(&f.ed, &NoteEditor::wikiLinkActivated);
        f.moveCaret(0, 5);
        f.key(Qt::Key_Return, Qt::ControlModifier);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.first().first().value<LinkRefInfo>().target, QStringLiteral("Alpha"));
        f.moveCaret(0, 1);
        f.key(Qt::Key_Return, Qt::ControlModifier);
        QCOMPARE(spy.count(), 1);   // outside: ordinary Ctrl+Enter (nothing to toggle here)
        const QString before = f.md();
        f.moveCaret(1, 10);   // inside the checklist item's link: link wins
        f.key(Qt::Key_Return, Qt::ControlModifier);
        QCOMPARE(spy.count(), 2);
        QCOMPARE(f.md(), before);
        // source mode too
        QVERIFY(f.ed.setMode(Mode::Source));
        f.moveCaret(0, 5);
        f.key(Qt::Key_Return, Qt::ControlModifier);
        QCOMPARE(spy.count(), 3);
    }
    void hoverCursorNeedsCtrl()
    {
        LFx f("go [[Alpha]] now\n");
        QWidget *vp = f.vp();
        auto move = [&](QPoint p, Qt::KeyboardModifiers m) {
            QMouseEvent e(QEvent::MouseMove, QPointF(p), QPointF(vp->mapToGlobal(p)), Qt::NoButton, Qt::NoButton, m);
            QApplication::sendEvent(vp, &e);
        };
        move(f.at(0, 6), Qt::NoModifier);
        QVERIFY(vp->cursor().shape() != Qt::PointingHandCursor);
        move(f.at(0, 6), Qt::ControlModifier);
        QCOMPARE(vp->cursor().shape(), Qt::PointingHandCursor);
        move(f.at(0, 1), Qt::ControlModifier);
        QVERIFY(vp->cursor().shape() != Qt::PointingHandCursor);
    }

    // ---- completion triggers ----
    void triggerRules()
    {
        LFx f({}, false, false);
        f.ed.setCompletionTriggers({"[[", "/"});
        qRegisterMetaType<CompletionRequest>();
        QSignalSpy req(&f.ed, &NoteEditor::completionRequested);
        f.text("[[");
        QCOMPARE(req.count(), 1);
        auto r = req.last().first().value<CompletionRequest>();
        QCOMPARE(r.triggerId, QStringLiteral("[["));
        QCOMPARE(r.query, QString());
        QVERIFY(r.atLineStart);
        QVERIFY(r.caretRect.isValid());
        QVERIFY(f.ed.completionActive());
        f.text("Al p");   // spaces allowed in a [[ query
        QCOMPARE(req.last().first().value<CompletionRequest>().query, QStringLiteral("Al p"));
        QVERIFY(f.ed.completionActive());
        const int n = req.count();
        f.key(Qt::Key_Backspace);
        QCOMPARE(req.last().first().value<CompletionRequest>().query, QStringLiteral("Al "));
        QCOMPARE(req.count(), n + 1);
        f.text("]]");   // closed: session over
        QVERIFY(!f.ed.completionActive());
        // slash: only at a token start
        req.clear();
        f.text(" a/");
        QCOMPARE(req.count(), 0);
        f.text(" /");
        QCOMPARE(req.count(), 1);
        QVERIFY(!req.last().first().value<CompletionRequest>().atLineStart);
        f.text("he");
        QCOMPARE(req.last().first().value<CompletionRequest>().query, QStringLiteral("he"));
        f.text(" ");   // whitespace ends a "/" query
        QVERIFY(!f.ed.completionActive());
        f.key(Qt::Key_Return);
        req.clear();
        f.text("/");
        QCOMPARE(req.count(), 1);
        QVERIFY(req.last().first().value<CompletionRequest>().atLineStart);
        // caret leaves the range
        QSignalSpy dis(&f.ed, &NoteEditor::completionDismissed);
        f.key(Qt::Key_Left);
        QCOMPARE(dis.count(), 1);
        QVERIFY(!f.ed.completionActive());
        // moving the caret never starts a session
        f.key(Qt::Key_Right);
        QVERIFY(!f.ed.completionActive());
    }
    void noTriggerInCodeOrIme()
    {
        LFx f("`code` and\n\n```\nblock\n```\n", false, false);
        f.ed.setCompletionTriggers({"[[", "/"});
        QSignalSpy req(&f.ed, &NoteEditor::completionRequested);
        // inline code (visual): caret at the end of the code span
        f.moveCaret(0, 4);
        f.text("[[");
        f.text("/");
        QCOMPARE(req.count(), 0);
        // code block
        f.moveCaret(1, 5);
        f.text("[[");
        QCOMPARE(req.count(), 0);
        // IME preedit and commit
        f.moveCaret(0, 10);
        f.preedit("[[");
        f.preedit("/");
        f.commit("[[");
        QCOMPARE(req.count(), 0);
        f.preedit({});
        // selection active: no trigger
        f.ed.selectAll();
        f.text("/");   // replaces the selection; afterTyping does not run with a selection
        // source mode: fenced + inline backticks
        LFx s("x\n```\ncode\n```\nplain `a\n", false, false);
        s.ed.setCompletionTriggers({"[["});
        QVERIFY(s.ed.setMode(Mode::Source));
        QSignalSpy sreq(&s.ed, &NoteEditor::completionRequested);
        s.moveCaret(2, 4);
        s.text("[[");
        QCOMPARE(sreq.count(), 0);
        s.moveCaret(4, 8);   // after "plain `a": inside an open inline code span
        s.text("[[");
        QCOMPARE(sreq.count(), 0);
        s.moveCaret(0, 1);
        s.text(" [[");
        QCOMPARE(sreq.count(), 1);
    }
    void disabledByDefaultAndEscapeEnds()
    {
        LFx f({}, false, false);
        QSignalSpy req(&f.ed, &NoteEditor::completionRequested);
        f.text("[[ /");
        QCOMPARE(req.count(), 0);
        QVERIFY(!f.ed.completionPopup());
        QVERIFY(!f.ed.completionActive());
        f.ed.showCompletions(items(3));   // no session: ignored
        QVERIFY(!f.ed.completionPopup());
        f.ed.setCompletionTriggers({"/"});
        f.key(Qt::Key_Return);
        f.text("/");
        f.ed.showCompletions(items(3));
        QVERIFY(f.ed.completionPopup() && f.ed.completionPopup()->isVisible());
        f.key(Qt::Key_Escape);
        QVERIFY(!f.ed.completionPopup()->isVisible());
        QVERIFY(!f.ed.completionActive());
        QVERIFY(f.plainNow().endsWith(QLatin1Char('/')));   // Esc keeps the typed text
    }

    // ---- popup behaviour ----
    void staleRepliesIgnored()
    {
        LFx f({}, false, false);
        f.ed.setCompletionTriggers({"/"});
        QSignalSpy req(&f.ed, &NoteEditor::completionRequested);
        f.text("/a");
        const int g1 = req.last().first().value<CompletionRequest>().generation;
        f.text("b");
        const int g2 = req.last().first().value<CompletionRequest>().generation;
        QVERIFY(g2 > g1);
        f.ed.showCompletions(items(3, "Old"), g1);
        QVERIFY(!f.ed.completionPopup() || !f.ed.completionPopup()->isVisible());
        f.ed.showCompletions(items(3, "New"), g2);
        QVERIFY(f.ed.completionPopup()->isVisible());
        // a reply for a request that was dismissed
        f.key(Qt::Key_Escape);
        f.ed.showCompletions(items(3, "Late"), g2);
        QVERIFY(!f.ed.completionPopup()->isVisible());
    }
    void acceptIsOneUndoStep()
    {
        LFx f({}, false, false);
        f.ed.setCompletionTriggers({"[["});
        QSignalSpy acc(&f.ed, &NoteEditor::completionAccepted);
        f.text("see [[Al");
        f.ed.showCompletions({{"Alpha", "note", "[[Alpha]]", -1}, {"Alpine", "", "[[Alpine|]]", 9}});
        QVERIFY(f.ed.completionPopup()->isVisible());
        QVERIFY(f.ed.activeEdit()->hasFocus());   // focus never leaves the editor
        f.key(Qt::Key_Down);
        f.key(Qt::Key_Return);
        QCOMPARE(acc.count(), 1);
        QCOMPARE(f.plain(), QStringLiteral("see [[Alpine|]]"));
        QCOMPARE(f.v->textCursor().positionInBlock(), 4 + 9);
        QCOMPARE(f.ed.recorder().unrecorded(), f.base);
        QVERIFY(!f.ed.completionPopup()->isVisible());
        QVERIFY(f.ed.undo());
        QCOMPARE(f.plain(), QStringLiteral("see [[Al"));   // literal typed text restored in ONE undo
        QVERIFY(f.ed.redo());
        QCOMPARE(f.plain(), QStringLiteral("see [[Alpine|]]"));
        f.ed.setWikiLinksEnabled(true);
        QVERIFY(f.md().contains(QStringLiteral("[[Alpine|]]")));
    }
    void acceptByTabAndMouseAndSource()
    {
        LFx f({}, false, false);
        f.ed.setCompletionTriggers({"/"});
        f.text("/he");
        f.ed.showCompletions({{"Heading", "", "## ", -1}});
        f.key(Qt::Key_Tab);
        QCOMPARE(f.plain(), QStringLiteral("## "));
        f.key(Qt::Key_Return);
        f.text("/x");
        f.ed.showCompletions(items(3));
        auto *pop = f.ed.completionPopup();
        const int rh = (pop->height() - 2) / 3;
        QTest::mouseClick(pop, Qt::LeftButton, Qt::NoModifier, QPoint(20, 1 + rh * 2 + 5));
        QVERIFY(f.plain().endsWith(QStringLiteral("[[Item2]]")));
        // source mode
        QVERIFY(f.ed.setMode(Mode::Source));
        f.moveEnd();
        QTextCursor c = f.ed.sourceEdit()->textCursor();
        c.movePosition(QTextCursor::End);
        f.ed.sourceEdit()->setTextCursor(c);
        f.key(Qt::Key_Return);
        f.text("/q");
        f.ed.showCompletions({{"Quote", "", "> ", -1}});
        f.key(Qt::Key_Return);
        QVERIFY(f.ed.sourceEdit()->toPlainText().endsWith(QStringLiteral("\n> ")));
        QVERIFY(f.ed.undo());
        QVERIFY(f.ed.sourceEdit()->toPlainText().endsWith(QStringLiteral("\n/q")));
    }
    void keyboardNavigation()
    {
        LFx f({}, false, false);
        f.ed.setCompletionTriggers({"/"});
        f.text("/");
        f.ed.showCompletions(items(20));
        auto *pop = f.ed.completionPopup();
        QCOMPARE((pop->height() - 2) % 8, 0);   // exactly 8 rows tall (max), the other 12 scroll
        QVERIFY((pop->height() - 2) / 8 >= 28);
        f.key(Qt::Key_Down);
        f.key(Qt::Key_Down);
        f.key(Qt::Key_Up);
        f.key(Qt::Key_PageDown);   // 1 + 8
        QVERIFY(f.ed.completionActive());
        QCOMPARE(f.plain(), QStringLiteral("/"));   // navigation keys never reach the document
        f.key(Qt::Key_Return);
        QCOMPARE(f.plain(), QStringLiteral("[[Item9]]"));
        f.ed.undo();
        // wrap-around and PageUp
        f.ed.setCompletionTriggers({"/"});
        f.key(Qt::Key_Return);
        f.text("/");
        f.ed.showCompletions(items(20));
        f.key(Qt::Key_Up);   // wraps to the last row
        f.key(Qt::Key_PageUp);   // 19 - 8
        f.key(Qt::Key_Tab);
        QVERIFY(f.plain().endsWith(QStringLiteral("[[Item11]]")));
    }
    void highlightedRowIsAccentBar()
    {
        LFx f({}, false, false);
        f.ed.setCompletionTriggers({"/"});
        f.text("/al");
        f.ed.showCompletions(items(3, "Alpha"));
        const QImage img = f.ed.completionPopup()->grab().toImage();
        const auto t = hn::theme::loadTheme(QStringLiteral("modernist"), false);
        const int rh = (img.height() - 2) / 3;
        QCOMPARE(img.pixelColor(1, 5), t.accent);        // selected row: accent bar
        QVERIFY(img.pixelColor(1, rh + 5) != t.accent);   // other rows: none
        QCOMPARE(img.pixelColor(0, 0), t.border);         // hairline border, square corners
        QCOMPARE(img.pixelColor(img.width() - 1, img.height() - 1), t.border);
    }
    void popupGeometryOnSmallScreens()
    {
        const QSize sz(300, 8 * 28 + 2);
        const QRect screen(0, 0, 1280, 720), caret(100, 100, 2, 20);
        QCOMPARE(NoteEditor::placeCompletionPopup(caret, sz, screen), QRect(QPoint(100, 122), sz));
        // near the bottom: flips above
        const QRect low(100, 650, 2, 20);
        QCOMPARE(NoteEditor::placeCompletionPopup(low, sz, screen), QRect(QPoint(100, 650 - 2 - sz.height()), sz));
        // right edge: shifts left
        const QRect right(1270, 100, 2, 20);
        QCOMPARE(NoteEditor::placeCompletionPopup(right, sz, screen).right(), 1279);
        // tiny screen: neither fits, stays inside
        const QRect tiny(0, 0, 320, 200);
        for (const QRect &c : {QRect(10, 10, 2, 20), QRect(300, 180, 2, 20), QRect(150, 100, 2, 20)}) {
            const QRect r = NoteEditor::placeCompletionPopup(c, QSize(300, 120), tiny);
            QVERIFY2(tiny.contains(r), qPrintable(QStringLiteral("%1,%2 %3x%4").arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height())));
        }
        // a real popup with a caret at the bottom-right of the real screen stays inside it
        LFx f({}, false, false);
        f.ed.setCompletionTriggers({"/"});
        const QRect avail = QGuiApplication::primaryScreen()->availableGeometry();
        f.ed.window()->move(avail.right() - 300, avail.bottom() - 120);
        QCoreApplication::processEvents();
        f.text("/");
        f.ed.showCompletions(items(12));
        QVERIFY2(avail.contains(f.ed.completionPopup()->geometry()), qPrintable(QStringLiteral("popup outside screen")));
        QVERIFY(f.ed.completionPopup()->height() <= 8 * 40 + 2);
        QCOMPARE((f.ed.completionPopup()->height() - 2) % 8, 0);
    }
    void clickAwayAndModeSwitchDismiss()
    {
        LFx f({}, false, false);
        f.ed.setCompletionTriggers({"/"});
        f.text("/");
        f.ed.showCompletions(items(3));
        QTest::mouseClick(f.vp(), Qt::LeftButton, Qt::NoModifier, QPoint(5, 5));
        QVERIFY(!f.ed.completionActive());
        QVERIFY(!f.ed.completionPopup()->isVisible());
        f.text("/");
        f.ed.showCompletions(items(3));
        f.ed.setMode(Mode::Source);
        QVERIFY(!f.ed.completionActive());
        QVERIFY(!f.ed.completionPopup()->isVisible());
    }

    // ---- performance ----
    void perfTypingWithOverlayAndCompletion()
    {
        LFx f(fixture20k(true));
        f.ed.setCompletionTriggers({"[[", "/"});
        QVERIFY2(f.ed.mode() == Mode::Visual, qPrintable(f.ed.modeReason()));
        QRandomGenerator rng(12345);
        QList<double> ms;
        auto timed = [&](auto &&fn) {
            QElapsedTimer t;
            t.start();
            fn();
            QCoreApplication::processEvents();
            ms << t.nsecsElapsed() / 1e6;
        };
        for (int i = 0; i < 1000; ++i) {
            if (i % 25 == 0) {
                const int b = rng.bounded(f.v->document()->blockCount());
                QTextCursor c(f.v->document()->findBlockByNumber(b));
                c.movePosition(QTextCursor::EndOfBlock);
                f.v->setTextCursor(c);
            }
            const int kind = i % 10;
            if (kind == 9) timed([&] { f.key(Qt::Key_Return); });
            else if (kind == 8) timed([&] { f.key(Qt::Key_Backspace); });
            else if (kind == 7) timed([&] { f.text(i % 20 == 7 ? "[" : "/"); });
            else timed([&] { QTest::keyClick(f.w(), char('a' + rng.bounded(26))); });
        }
        std::sort(ms.begin(), ms.end());
        auto pct = [&](double p) { return ms[qMin(ms.size() - 1, qsizetype(p * ms.size()))]; };
        qInfo().nospace() << "overlay+completion edit timing over " << ms.size() << " edits: p50=" << pct(0.50) << " ms  p95=" << pct(0.95)
                          << " ms  p99=" << pct(0.99) << " ms  max=" << ms.last() << " ms  (target p95 < 4 ms)";
        QCOMPARE(f.ed.recorder().unrecorded(), f.base);
        QVERIFY2(pct(0.95) < 4.0, qPrintable(QStringLiteral("p95 %1 ms").arg(pct(0.95))));
    }
    void paintCostIndependentOfNoteSize()
    {
        auto note = [](int blocks) {
            QByteArray md;
            for (int i = 0; i < blocks; ++i) md += "Paragraph " + QByteArray::number(i) + " links [[Alpha " + QByteArray::number(i) + "]] and [[Missing|m]] end.\n\n";
            return md;
        };
        auto paintMs = [&](LFx &f, bool overlay) {   // best of 7 batches of 20 grabs (the machine is noisy)
            f.ed.setWikiLinksEnabled(overlay);
            f.v->viewport()->grab();
            double best = 1e9;
            for (int b = 0; b < 7; ++b) {
                QElapsedTimer t;
                t.start();
                for (int i = 0; i < 20; ++i) f.v->viewport()->grab();
                best = qMin(best, t.nsecsElapsed() / 1e6 / 20);
            }
            return best;
        };
        double small = 0, big = 0;
        {
            LFx f(note(100), false, false);
            f.ed.setLinkResolver(resolve);
            const double off = paintMs(f, false), on = paintMs(f, true);
            small = qMax(0.0, on - off);
            qInfo().nospace() << "100 links-paragraphs: paint " << off << " ms off, " << on << " ms on";
        }
        {
            LFx f(note(2500), false, false);   // largest note that still opens in visual mode (limit 256 KiB); the 5001-block case below runs in source mode
            f.ed.setLinkResolver(resolve);
            qInfo() << "blocks" << f.v->document()->blockCount() << "mode" << int(f.ed.mode());
            const double off = paintMs(f, false), on = paintMs(f, true);
            big = qMax(0.0, on - off);
            qInfo().nospace() << f.v->document()->blockCount() << " blocks: paint " << off << " ms off, " << on << " ms on (overlay cost "
                              << big << " ms vs " << small << " ms for 100)";
            f.v->verticalScrollBar()->setValue(f.v->verticalScrollBar()->maximum() / 2);   // mid-document is as cheap
            QCoreApplication::processEvents();
            const double mid = paintMs(f, true);
            qInfo() << "mid-document paint with overlay (ms)" << mid;
            QVERIFY(big < 5.0);
        }
    }
    void fiveThousandBlocksSourceAndVisual()
    {
        QByteArray md;
        for (int i = 0; i < 5000; ++i) md += (i % 50 == 0 ? "[[Alpha " + QByteArray::number(i) + "]] line\n" : "plain line " + QByteArray::number(i) + "\n");
        // source mode (visual limit is 256 KiB; force source via setMode)
        LFx f(md, false, false);
        f.ed.setLinkResolver(resolve);
        if (f.ed.mode() == Mode::Visual) f.ed.setMode(Mode::Source);
        QElapsedTimer t;
        t.start();
        f.ed.setWikiLinksEnabled(true);   // one O(n) fence pass
        const double enable = t.nsecsElapsed() / 1e6;
        t.restart();
        for (int i = 0; i < 30; ++i) f.vp()->grab();
        const double paint = t.nsecsElapsed() / 1e6 / 30;
        QElapsedTimer te;
        QList<double> ms;
        f.moveCaret(2500, 3);
        for (int i = 0; i < 200; ++i) {
            te.restart();
            QTest::keyClick(f.w(), 'x');
            QCoreApplication::processEvents();
            ms << te.nsecsElapsed() / 1e6;
        }
        std::sort(ms.begin(), ms.end());
        qInfo().nospace() << "blocks=" << f.ed.sourceEdit()->document()->blockCount() << " mode=" << int(f.ed.mode()) << " enable=" << enable
                          << " ms, paint=" << paint << " ms, typing p95=" << ms[190] << " ms";
        QVERIFY(ms[190] < 4.0);
    }

    // ---- screenshots ----
    void screenshots_data()
    {
        QTest::addColumn<bool>("dark");
        QTest::newRow("light") << false;
        QTest::newRow("dark") << true;
    }
    void screenshots()
    {
        QFETCH(bool, dark);
        const QString dir = qEnvironmentVariable("HN_SAMPLE_DIR");
        if (dir.isEmpty()) QSKIP("HN_SAMPLE_DIR not set");
        const QByteArray md =
            "# Project notes\n\n"
            "Kick-off with [[Alpha Project]] and the [[Meeting notes#Decisions|decisions]] from last week.\n\n"
            "Still missing: [[Roadmap 2027]] and ![[diagram.png]]. Two notes share a name: [[Dup]].\n\n"
            "- Follow up on [[Alpha Project]]\n- Inline code stays plain: `[[not a link]]`\n\n"
            "Type [[Al";
        auto render = [&](LFx &f, bool withPopup) {
            const qreal dpr = 2;
            QWidget *top = f.ed.window();
            QImage img((top->size() * dpr), QImage::Format_ARGB32_Premultiplied);
            img.setDevicePixelRatio(dpr);
            img.fill(Qt::transparent);
            top->render(&img);
            if (withPopup && f.ed.completionPopup() && f.ed.completionPopup()->isVisible()) {
                QPainter p(&img);
                p.drawPixmap(top->mapFromGlobal(f.ed.completionPopup()->pos()), f.ed.completionPopup()->grab());
            }
            return img;
        };
        const QString name = dark ? "dark" : "light";
        {
            LFx f(md, dark);
            f.ed.resize(560, 360);
            f.ed.window()->resize(560, 360);
            QCoreApplication::processEvents();
            QVERIFY(render(f, false).save(QDir(dir).filePath(QStringLiteral("editor-links-%1.png").arg(name))));
        }
        {
            LFx f(md, dark);
            f.ed.window()->resize(560, 500);
            QCoreApplication::processEvents();
            f.ed.setCompletionTriggers({"[["});
            f.moveEnd();
            f.text("p");
            QCOMPARE(f.ed.completionActive(), true);
            f.ed.showCompletions({{"Alpha Project", "Projects/Alpha Project.md", "[[Alpha Project]]", -1},
                                  {"Alpine notes", "Inbox/Alpine notes.md", "[[Alpine notes]]", -1},
                                  {"Alpha retrospective", "Archive/2025/Alpha retrospective.md", "[[Alpha retrospective]]", -1},
                                  {"Meeting notes", "Work/Meeting notes.md", "[[Meeting notes]]", -1}});
            f.key(Qt::Key_Down);
            QCoreApplication::processEvents();
            QVERIFY(render(f, true).save(QDir(dir).filePath(QStringLiteral("editor-complete-%1.png").arg(name))));
        }
    }
};

QTEST_MAIN(LinksTest)
#include "editor_links_test.moc"
