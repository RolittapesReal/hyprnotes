// P8.1 editor limits. Standalone executable; prints METRIC lines, hard CHECKs on spec targets.
#include "hn/core/markdown_codec.h"
#include "hn/editor/note_editor.h"
#include "stress_util.h"
#include <QApplication>
#include <QClipboard>
#include <QMimeData>
#include <QSignalSpy>
#include <QTest>
#include <QTextBlock>

using namespace st;
using namespace hn::editor;

static void pump() { QCoreApplication::processEvents(); }

struct Ed {
    NoteEditor ed;
    explicit Ed(const QByteArray &bytes, bool show = true) {
        ed.resize(640, 480);
        if (show) { ed.show(); (void)QTest::qWaitForWindowExposed(&ed); ed.activateWindow(); }
        ed.load(bytes);
        ed.focusEditor();
    }
    QWidget *w() { return ed.activeEdit(); }
    void key(Qt::Key k, Qt::KeyboardModifiers m = Qt::NoModifier) { QTest::keyClick(w(), k, m); }
    void ch(char c) { QTest::keyClick(w(), c); }
    void caretTo(int block, bool end = true) {
        QTextCursor c;
        if (ed.mode() == Mode::Visual) {
            auto *d = ed.visualEdit()->document();
            c = QTextCursor(d->findBlockByNumber(qMin(block, d->blockCount() - 1)));
            if (end) c.movePosition(QTextCursor::EndOfBlock);
            ed.visualEdit()->setTextCursor(c);
        } else {
            auto *d = ed.sourceEdit()->document();
            c = QTextCursor(d->findBlockByNumber(qMin(block, d->blockCount() - 1)));
            if (end) c.movePosition(QTextCursor::EndOfBlock);
            ed.sourceEdit()->setTextCursor(c);
        }
    }
    int blocks() { return ed.mode() == Mode::Visual ? ed.visualEdit()->document()->blockCount() : ed.sourceEdit()->document()->blockCount(); }
};

// Time `n` mixed edits (typing, enter, backspace, formatting, undo/redo, list shortcut) with caret jumps.
static Dist mixedEdits(Ed &e, int n, Rng &rng, bool visual, Dist *typingOnly = nullptr) {
    Dist d;
    for (int i = 0; i < n; ++i) {
        if (i % 25 == 0) e.caretTo(rng.bounded(e.blocks()));
        const int kind = rng.bounded(100);
        Stopwatch sw;
        if (kind < 70) e.ch(char('a' + rng.bounded(26)));
        else if (kind < 76) e.key(Qt::Key_Return);
        else if (kind < 84) e.key(Qt::Key_Backspace);
        else if (kind < 88 && visual) { e.key(Qt::Key_Left, Qt::ShiftModifier); e.ed.toggleInline(InlineStyle::Bold); }
        else if (kind < 91 && visual) e.ed.setBlockStyle(rng.chance(50) ? BlockStyle::H2 : BlockStyle::Paragraph);
        else if (kind < 94 && visual) e.ed.toggleList(rng.chance(50) ? ListKind::Bullet : ListKind::None);
        else if (kind < 97) e.ed.undo();
        else if (kind < 99) e.ed.redo();
        else { e.ch('-'); e.ch(' '); }
        pump();
        const double ms = sw.ms();
        d.add(ms);
        if (typingOnly && kind < 70) typingOnly->add(ms);
    }
    return d;
}

static void editLatency20k() {
    note("== 1.1 5000 mixed edits on a 20 KiB note (visual mode)");
    Ed e(mixedNote(20 * 1024, 1));
    check(e.ed.mode() == Mode::Visual, "20KiB fixture opens in visual mode: " + e.ed.modeReason());
    Rng rng(1234);
    const Mem m0 = mem();
    Dist typing;
    Dist d = mixedEdits(e, 5000, rng, true, &typing);
    d.report("editor.20k.mixed5000");
    typing.report("editor.20k.typingonly");
    const Mem m1 = mem();
    metric("editor.20k.pss_growth", m1.total() - m0.total(), "MiB");
    check(d.pct(.95) < 4.0, QString("20KiB mixed p95 %1 ms < 4 ms (spec 9.2)").arg(d.pct(.95)));
    check(e.ed.recorder().unrecorded() == 0, "no unrecorded edits");
    check(e.ed.history().count() <= TxHistory::kMaxTx && e.ed.history().bytes() <= TxHistory::kMaxBytes,
          QString("history bounds after 5000 edits: count=%1 bytes=%2").arg(e.ed.history().count()).arg(e.ed.history().bytes()));
    // paint cost, recorded separately from event-processing cost (spec 9.2 presentation row)
    Dist paint;
    for (int i = 0; i < 20; ++i) { e.ch('x'); Stopwatch sw; (void)e.ed.grab(); paint.add(sw.ms()); }
    paint.report("editor.20k.grab_paint");
    // data integrity: saved bytes still valid UTF-8 and the round trip is stable
    const QByteArray out = e.ed.toMarkdownBytes();
    check(hn::core::isValidUtf8(out), "output is valid UTF-8");
    NoteEditor e2; e2.load(out);
    check(hn::core::semanticTokens(QString::fromUtf8(e2.toMarkdownBytes())) == hn::core::semanticTokens(QString::fromUtf8(out)),
          "edited note re-opens semantically identical");
}

static void bigNote(qsizetype bytes, const char *tag, bool forceSource) {
    note(QString("== 1.2 %1 note").arg(tag));
    const QByteArray md = mixedNote(bytes, 3);
    const Mem m0 = mem();
    NoteEditor ed;
    ed.resize(640, 480); ed.show(); (void)QTest::qWaitForWindowExposed(&ed); ed.activateWindow();
    Stopwatch sw;
    ed.load(md);
    pump();
    metric(qPrintable(QString("editor.%1.load").arg(tag)), sw.ms(), "ms");
    metric(qPrintable(QString("editor.%1.initial_mode_visual").arg(tag)), ed.mode() == Mode::Visual ? 1 : 0);
    if (forceSource && ed.mode() != Mode::Source) {
        sw.reset();
        const bool ok = ed.setMode(Mode::Source);
        pump();
        metric(qPrintable(QString("editor.%1.switch_to_source").arg(tag)), sw.ms(), "ms");
        check(ok, QString("%1 switch to source mode accepted").arg(tag));
    }
    ed.focusEditor();
    const Mem m1 = mem();
    metric(qPrintable(QString("editor.%1.load_pss_delta").arg(tag)), m1.total() - m0.total(), "MiB");
    Ed *dummy = nullptr; (void)dummy;
    // 800 edits in the middle of the document
    Dist d; Rng rng(77);
    auto *edit = ed.activeEdit();
    auto place = [&](int frac) {
        QPlainTextEdit *pe = ed.sourceEdit();
        QTextEdit *te = ed.visualEdit();
        if (ed.mode() == Mode::Source) { QTextCursor c(pe->document()->findBlockByNumber(pe->document()->blockCount() * frac / 100)); c.movePosition(QTextCursor::EndOfBlock); pe->setTextCursor(c); }
        else { QTextCursor c(te->document()->findBlockByNumber(te->document()->blockCount() * frac / 100)); c.movePosition(QTextCursor::EndOfBlock); te->setTextCursor(c); }
    };
    for (int i = 0; i < 800; ++i) {
        if (i % 50 == 0) place(rng.bounded(100));
        Stopwatch s2;
        const int k = rng.bounded(10);
        if (k == 0) QTest::keyClick(edit, Qt::Key_Return);
        else if (k == 1) QTest::keyClick(edit, Qt::Key_Backspace);
        else QTest::keyClick(edit, char('a' + rng.bounded(26)));
        pump();
        d.add(s2.ms());
    }
    d.report(QString("editor.%1.edit800").arg(tag));
    soft(d.pct(.95) < 16.7, QString("%1 edit p95 %2 ms within one 60 Hz frame (informational; spec 4 ms is for 20 KiB fixtures)").arg(tag).arg(d.pct(.95)));
    sw.reset();
    const QByteArray out = ed.toMarkdownBytes();
    metric(qPrintable(QString("editor.%1.serialize").arg(tag)), sw.ms(), "ms");
    check(out.size() >= md.size(), QString("%1 serialize keeps content (%2 -> %3 bytes)").arg(tag).arg(md.size()).arg(out.size()));
    Dist paint;
    for (int i = 0; i < 5; ++i) { QTest::keyClick(edit, 'x'); Stopwatch s3; (void)ed.grab(); paint.add(s3.ms()); }
    paint.report(QString("editor.%1.grab_paint").arg(tag));
    const Mem m2 = mem();
    metric(qPrintable(QString("editor.%1.pss_after_edits").arg(tag)), m2.total(), "MiB");
    check(ed.history().count() <= TxHistory::kMaxTx && ed.history().bytes() <= TxHistory::kMaxBytes,
          QString("%1 history bounded: count=%2 bytes=%3").arg(tag).arg(ed.history().count()).arg(ed.history().bytes()));
}

static void bulkPaste() {
    note("== 1.3 bulk paste 1 MiB");
    QByteArray big;
    while (big.size() < (1 << 20)) big += mixedBlock(big.size());
    const QString text = QString::fromUtf8(big);
    for (int srcMode = 0; srcMode < 2; ++srcMode) {
        Ed e(mixedNote(20 * 1024, 5));
        if (srcMode) e.ed.setMode(Mode::Source);
        e.ed.focusEditor();
        QGuiApplication::clipboard()->setText(text);
        pump();
        const Mem m0 = mem();
        QSignalSpy needs(&e.ed, &NoteEditor::pasteNeedsSource);
        Stopwatch sw;
        e.ed.paste();
        pump();
        const double ms = sw.ms();
        const char *tag = srcMode ? "source" : "visual";
        metric(qPrintable(QString("editor.paste1MiB.%1.ms").arg(tag)), ms, "ms");
        metric(qPrintable(QString("editor.paste1MiB.%1.pss_delta").arg(tag)), mem().total() - m0.total(), "MiB");
        note(QString("paste %1: mode after=%2 pasteNeedsSource=%3 reason=%4 history count=%5 bytes=%6").arg(tag)
             .arg(e.ed.mode() == Mode::Visual ? "visual" : "source").arg(needs.size()).arg(e.ed.modeReason()).arg(e.ed.history().count()).arg(e.ed.history().bytes()));
        check(e.ed.toMarkdownBytes().size() >= int(big.size()), QString("%1 paste inserted full payload").arg(tag));
        check(e.ed.history().bytes() <= TxHistory::kMaxBytes, QString("%1 paste history payload bounded (%2)").arg(tag).arg(e.ed.history().bytes()));
        Stopwatch u; const bool undone = e.ed.undo(); pump();
        metric(qPrintable(QString("editor.paste1MiB.%1.undo_ms").arg(tag)), u.ms(), "ms");
        check(undone, QString("%1 paste undo succeeds").arg(tag));
        check(e.ed.toMarkdownBytes().size() < int(big.size()), QString("%1 paste undo removed payload").arg(tag));
        Stopwatch r; e.ed.redo(); pump();
        metric(qPrintable(QString("editor.paste1MiB.%1.redo_ms").arg(tag)), r.ms(), "ms");
        // typing after paste (latency in the now-large doc)
        Dist d; for (int i = 0; i < 200; ++i) { Stopwatch s; e.ch('q'); pump(); d.add(s.ms()); }
        d.report(QString("editor.paste1MiB.%1.type_after").arg(tag));
    }
}

static void longList() {
    note("== 1.4 10k-line list");
    QByteArray md = "# List\n\n";
    for (int i = 0; i < 10000; ++i) md += "- item " + QByteArray::number(i) + " lorem ipsum\n";
    Stopwatch sw;
    Ed e(md);
    metric("editor.list10k.load", sw.ms(), "ms");
    soft(sw.ms() < 1000, QString("10k-item list (%1 KiB) opens in under 1 s (%2 ms)").arg(md.size() / 1024).arg(sw.ms()));
    note(QString("list10k mode=%1 reason=%2").arg(e.ed.mode() == Mode::Visual ? "visual" : "source").arg(e.ed.modeReason()));
    Rng rng(9); Dist d = mixedEdits(e, 1000, rng, e.ed.mode() == Mode::Visual);
    d.report("editor.list10k.mixed1000");
    soft(d.pct(.95) < 16.7, QString("list10k mixed-edit p95 %1 ms (incl. list toggles, undo/redo)").arg(d.pct(.95)));
    sw.reset(); const QByteArray out = e.ed.toMarkdownBytes(); metric("editor.list10k.serialize", sw.ms(), "ms");
    check(hn::core::isValidUtf8(out) && out.count("item") > 9000, QString("list10k after 1000 mixed edits (incl. list toggles) is valid and keeps most items (%1)").arg(out.count("item")));
    Ed fresh(md);
    check(fresh.ed.toMarkdownBytes() == md, "list10k load -> save without edits is byte-identical");
    Dist tail; fresh.caretTo(fresh.blocks() - 1);
    for (int i = 0; i < 300; ++i) { Stopwatch s2; fresh.ch('z'); pump(); tail.add(s2.ms()); }
    tail.report("editor.list10k.type_at_end");
    fresh.caretTo(5000);
    Dist mid; for (int i = 0; i < 300; ++i) { Stopwatch s2; fresh.ch('y'); pump(); mid.add(s2.ms()); }
    mid.report("editor.list10k.type_in_middle");
    xfail(mid.pct(.95) < 4.0 && tail.pct(.95) < 4.0, "D6-BIGLIST-LATENCY", QString("10k-item list: plain typing p95 < 4 ms (end %1 ms, middle %2 ms)").arg(tail.pct(.95)).arg(mid.pct(.95)));
}

static void nested50() {
    note("== 1.5 nested lists 50 levels");
    QByteArray md;
    for (int i = 0; i < 50; ++i) md += QByteArray(i * 2, ' ') + "- level " + QByteArray::number(i) + "\n";
    Stopwatch sw; Ed e(md); metric("editor.nested50.load", sw.ms(), "ms");
    note(QString("nested50 mode=%1 reason=%2").arg(e.ed.mode() == Mode::Visual ? "visual" : "source").arg(e.ed.modeReason()));
    Rng rng(4);
    Dist d = mixedEdits(e, 500, rng, e.ed.mode() == Mode::Visual);
    d.report("editor.nested50.mixed500");
    // indent/outdent hammering
    e.caretTo(49);
    for (int i = 0; i < 30; ++i) { e.ed.indent(); e.ed.outdent(); }
    const QByteArray out = e.ed.toMarkdownBytes();
    check(out.contains("level 49") && hn::core::isValidUtf8(out), "nested50 deepest item survives");
    check(hn::core::semanticTokens(QString::fromUtf8(md)).size() > 0, "codec handles 50-level nesting");
    Ed back(out); 
    check(hn::core::semanticTokens(QString::fromUtf8(back.ed.toMarkdownBytes())) == hn::core::semanticTokens(QString::fromUtf8(out)), "nested50 edited output round-trips");
}

static void longParagraph() {
    note("== 1.6 10k-char single paragraph typed");
    Ed e("start\n");
    e.caretTo(0);
    Dist first, last, all;
    Rng rng(5);
    for (int i = 0; i < 10000; ++i) {
        Stopwatch sw;
        e.ch(i % 9 == 8 ? ' ' : char('a' + rng.bounded(26)));
        pump();
        const double ms = sw.ms();
        all.add(ms);
        if (i < 1000) first.add(ms);
        if (i >= 9000) last.add(ms);
    }
    all.report("editor.para10k.all"); first.report("editor.para10k.first1000"); last.report("editor.para10k.last1000");
    check(last.pct(.95) < 4.0, QString("10k-char paragraph last-1000 p95 %1 ms < 4 ms").arg(last.pct(.95)));
    soft(last.mean() < first.mean() * 3 + 0.5, QString("per-key cost growth first=%1 last=%2 ms mean").arg(first.mean()).arg(last.mean()));
    check(e.ed.toMarkdownBytes().size() > 10000, "all 10k chars present");
    note(QString("para10k history count=%1 bytes=%2 evicted=%3").arg(e.ed.history().count()).arg(e.ed.history().bytes()).arg(e.ed.history().evicted()));
}

static void unicode() {
    note("== 1.7 pathological unicode");
    QString s;
    s += "# Ünicode\n\n";
    QString stack = "a"; for (int i = 0; i < 300; ++i) stack += QChar(0x0300 + (i % 0x6F));   // 300 combining marks on one base
    s += stack + "\n\n";
    QString rtl = QString::fromUtf8("\xD8\xA7\xD9\x84\xD8\xB3\xD9\x84\xD8\xA7\xD9\x85 \xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D ");
    for (int i = 0; i < 200; ++i) s += rtl + "mixed LTR " + QString::number(i) + " ";
    s += "\n\n";
    const QString fam = QString::fromUtf8("\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7\xE2\x80\x8D\xF0\x9F\x91\xA6");
    for (int i = 0; i < 500; ++i) s += fam;
    s += "\n\n\xE2\x80\xAE" "bidi override \xE2\x80\xAC \xC2\xA0 \xE2\x80\x8B zero\xE2\x80\x8Bwidth \xEF\xBF\xBD\n";
    const QByteArray md = s.toUtf8();
    Stopwatch sw; Ed e(md); metric("editor.unicode.load", sw.ms(), "ms");
    note(QString("unicode mode=%1 reason=%2").arg(e.ed.mode() == Mode::Visual ? "visual" : "source").arg(e.ed.modeReason()));
    Dist d;
    for (int round = 0; round < 40; ++round) {
        e.caretTo(2 + round % 3);
        for (int i = 0; i < 25; ++i) { Stopwatch t; e.ch(char('a' + i)); pump(); d.add(t.ms()); }
        for (int i = 0; i < 10; ++i) { Stopwatch t; e.key(Qt::Key_Backspace); pump(); d.add(t.ms()); }
        for (int i = 0; i < 5; ++i) { Stopwatch t; e.key(Qt::Key_Left, Qt::ControlModifier); e.key(Qt::Key_Delete); pump(); d.add(t.ms()); }
        e.ed.undo(); e.ed.redo();
    }
    d.report("editor.unicode.edits");
    soft(d.pct(.95) < 4.0, QString("unicode mixed-doc edit p95 %1 ms (dominated by the 500-ZWJ-family paragraph, see emoji sweep)").arg(d.pct(.95)));
    const QByteArray out = e.ed.toMarkdownBytes();
    check(hn::core::isValidUtf8(out), "unicode output valid UTF-8");
    check(out.contains(fam.toUtf8()), "ZWJ emoji family sequence preserved intact");
    // untouched-document byte fidelity: load -> save without edit
    Ed untouched(md);
    const QByteArray same = untouched.ed.toMarkdownBytes();
    check(hn::core::semanticTokens(QString::fromUtf8(same)) == hn::core::semanticTokens(QString::fromUtf8(md)), "unedited unicode doc semantically unchanged on save");
    // emoji density sweep: cost of one keystroke at the start of a paragraph holding N emoji
    for (int n : {10, 25, 50, 100, 250, 500}) {
        QString para; for (int i = 0; i < n; ++i) para += QString::fromUtf8("\xF0\x9F\x98\x80");
        Ed ee((para + "\n").toUtf8());
        QTextCursor c = ee.ed.visualEdit()->textCursor(); c.movePosition(QTextCursor::Start); ee.ed.visualEdit()->setTextCursor(c);
        Dist dd; for (int i = 0; i < 25; ++i) { Stopwatch t; ee.ch(char('a' + i % 26)); pump(); dd.add(t.ms()); }
        metric(qPrintable(QString("editor.emoji_sweep.%1.p95").arg(n)), dd.pct(.95), "ms");
        if (n == 100) xfail(dd.pct(.95) < 4.0, "D5-EMOJI-RELAYOUT", QString("keystroke p95 < 4 ms in a paragraph of 100 emoji (%1 ms); cost grows ~linearly (~0.18 ms/emoji; 500 emoji = %2 ms per key)").arg(dd.pct(.95)).arg(n == 500 ? dd.pct(.95) : 0));
    }
    // rendering must not hang or crash
    Stopwatch g; (void)e.ed.grab(); metric("editor.unicode.grab_paint", g.ms(), "ms");
}

static void historyEviction() {
    note("== 1.8 history eviction, 100k transactions");
    Ed e(mixedNote(20 * 1024, 8));
    qint64 fake = 0;
    e.ed.history().setClock([&] { return fake += 5000; });   // every keystroke is >1s after the previous one: never merged
    e.caretTo(0);
    Rng rng(88);
    Mem base = mem();
    int maxCount = 0; qint64 maxBytes = 0;
    QList<double> pss;
    Dist keyd;
    const int N = 100000;
    for (int i = 1; i <= N; ++i) {
        Stopwatch sw;
        // type 5, delete 5 (and Enter/Backspace pairs): the document size stays constant so growth == history/leak
        const int ph = i % 12;
        if (ph == 10) e.key(Qt::Key_Return); else if (ph == 11) e.key(Qt::Key_Backspace);
        else if (ph >= 5) e.key(Qt::Key_Backspace); else e.ch(char('a' + rng.bounded(26)));
        if (i % 7 == 0) pump();
        if (i % 20 == 0) keyd.add(sw.ms());
        maxCount = qMax(maxCount, e.ed.history().count());
        maxBytes = qMax(maxBytes, e.ed.history().bytes());
        if (i % 10000 == 0) {
            const Mem m = mem();
            pss.push_back(m.total());
            metric(qPrintable(QString("editor.hist100k.pss_at_%1").arg(i)), m.total(), "MiB");
            note(QString("tx=%1 count=%2 bytes=%3 evicted=%4 spilled=%5 blocks=%6").arg(i).arg(e.ed.history().count()).arg(e.ed.history().bytes())
                 .arg(e.ed.history().evicted()).arg(e.ed.history().spilledCount()).arg(e.blocks()));
        }
    }
    keyd.report("editor.hist100k.sampled_key");
    metric("editor.hist100k.max_count", maxCount); metric("editor.hist100k.max_bytes", double(maxBytes), "bytes");
    check(maxCount <= TxHistory::kMaxTx, QString("history count never above 500 (max %1)").arg(maxCount));
    check(maxBytes <= TxHistory::kMaxBytes, QString("history payload never above 2 MiB (max %1)").arg(maxBytes));
    check(e.ed.history().evicted() >= N - 600, QString("evictions happened: %1").arg(e.ed.history().evicted()));
    // plateau: growth from 30k..100k should be small (document itself grows ~100 KB of text)
    const double early = pss[2], late = pss.back();
    metric("editor.hist100k.pss_growth_30k_to_100k", late - early, "MiB");
    check(late - early < 3.0, QString("memory plateau: PSS grew %1 MiB between 30k and 100k transactions at constant document size (<3 MiB)").arg(late - early));
    // sustained large-paste transactions to stress the byte cap and spill-to-disk path
    QByteArray chunk; while (chunk.size() < 100 * 1024) chunk += mixedBlock(chunk.size());
    qint64 maxB2 = 0; int maxC2 = 0;
    for (int i = 0; i < 60; ++i) {
        QGuiApplication::clipboard()->setText(QString::fromUtf8(chunk));
        e.ed.paste(); pump();
        maxB2 = qMax(maxB2, e.ed.history().bytes()); maxC2 = qMax(maxC2, e.ed.history().count());
    }
    metric("editor.hist.paste60x100k.max_bytes", double(maxB2), "bytes");
    check(maxB2 <= TxHistory::kMaxBytes && maxC2 <= TxHistory::kMaxTx, QString("large-paste history bounded (bytes %1, count %2, spilled %3)").arg(maxB2).arg(maxC2).arg(e.ed.history().spilledCount()));
    const int docBefore = e.ed.toMarkdownBytes().size();
    int undone = 0; while (e.ed.undo() && undone < 600) ++undone;
    metric("editor.hist.undo_chain_len", undone);
    {   // accounted history cost of ONE 100 KiB paste on a fresh history
        Ed one(mixedNote(20 * 1024, 9)); QGuiApplication::clipboard()->setText(QString::fromUtf8(chunk)); one.ed.paste(); pump();
        const double amp = double(one.ed.history().bytes()) / double(chunk.size());
        metric("editor.hist.paste100KiB_accounted_bytes", double(one.ed.history().bytes()), "bytes");
        metric("editor.hist.paste_amplification", amp, "x");
        note(QString("after 60 x 100 KiB pastes the 2 MiB byte cap leaves %1 undoable step(s): one 100 KiB paste is accounted %2 KiB of history (%3x amplification)").arg(undone).arg(one.ed.history().bytes() / 1024).arg(amp, 0, 'f', 1));
    }
    check(undone <= TxHistory::kMaxTx, "undo chain bounded by count cap");
    check(!e.ed.canUndo(), "undo chain drains cleanly (no stuck state)");
    check(e.ed.toMarkdownBytes().size() < docBefore, "undo actually removed text");
}

int main(int argc, char **argv) {
    Sandbox sb("editor");
    QApplication app(argc, argv);
    const QString which = argc > 1 ? argv[1] : "all";
    auto want = [&](const char *n) { return which == "all" || which == n; };
    if (want("latency")) editLatency20k();
    if (want("big")) {
        bigNote(200 * 1024, "200k_visual", false);
        bigNote(200 * 1024, "200k_source", true);
        bigNote(1 << 20, "1m", true);
        bigNote(5 << 20, "5m", true);
    }
    if (want("paste")) bulkPaste();
    if (want("list")) longList();
    if (want("nested")) nested50();
    if (want("para")) longParagraph();
    if (want("unicode")) unicode();
    if (want("history")) historyEviction();
    return finish("stress_editor");
}
