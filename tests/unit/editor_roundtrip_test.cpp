// Import (Qt/MD4C) -> our exporter -> semantic equality against the codec, for constructs the editor supports.
#include <QTextDocument>

#include "editor_test_util.h"
#include "md_io.h"
#include "hn/core/markdown_codec.h"
#include <QFile>
#include <QDir>

using namespace hntest;

class RoundTripTest : public QObject {
    Q_OBJECT
private slots:
    void supportedConstructsRoundTrip_data()
    {
        QTest::addColumn<QString>("md");
        auto row = [](const char *n, const char *md) { QTest::newRow(n) << QString::fromUtf8(md); };
        row("paragraphs", "one\n\ntwo\n\nthree\n");
        row("headings", "# H1\n\n## H2\n\n### H3\n\n#### H4\n\n##### H5\n\n###### H6\n");
        row("inline", "a **b** *c* ~~d~~ `e` f\n");
        row("nested-inline", "**a *b* c** and ~~x **y**~~ and ***z***\n");
        row("underscores", "snake_case_word _em_ __strong__ and \\_lit\\_\n");
        row("escapes", "a \\* b \\[c\\] \\<d\\> \\\\ \\# \\~\\~ &amp; 1\\. x\n");
        row("link", "see [site](https://example.org/a_b) and [p](<https://x.y/a(b)>)\n");
        row("bold-link", "**[bold link](https://a.b)** and *[em link](https://a.b)* and [a **b**](https://a.b)\n");
        row("lists", "- a\n- b\n- c\n");
        row("ordered", "1. a\n2. b\n3. c\n");
        row("ordered-start", "3. a\n4. b\n");
        row("nested", "- a\n  - b\n    - c\n  - d\n- e\n");
        row("nested-ordered", "1. a\n   1. b\n   2. c\n2. d\n");
        row("tasks", "- [ ] a\n- [x] b\n  - [ ] c\n");
        row("quote", "> quote\n");
        row("quote2", "> a\n>\n> b\n\nafter\n");
        row("code", "```py\nx = 1\n\ny = 2\n```\n");
        row("code-nolang", "```\nraw *text* <b>\n```\n");
        row("code-fence-in-code", "````\n```\ninner\n```\n````\n");
        row("codespan-backtick", "use `` a`b `` here\n");
        row("mixed", "# T\n\nintro\n\n- a\n- b\n\n> q\n\n```\ncode\n```\n\nend\n");
        row("list-then-para", "- a\n- b\n\npara\n");
        row("unicode", "caf\xC3\xA9 \xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x92\xBB \xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D\n");
        row("line-start-specials", "\\# not heading\n\n\\- not list\n\n1\\. not ordered\n\n\\> not quote\n");
    }
    void supportedConstructsRoundTrip()
    {
        QFETCH(QString, md);
        Fx f(md.toUtf8());
        QVERIFY2(f.ed.mode() == Mode::Visual, qPrintable(f.ed.modeReason()));
        f.moveEnd();
        f.text("z");
        f.key(Qt::Key_Backspace);   // edit + revert: force the serializer
        QVERIFY(f.ed.isModified());
        const QString out = f.md();
        QVERIFY2(sameSemantics(md, out), qPrintable(md + "\n--- exported ---\n" + out));
        // idempotent: a second generation is semantically identical and stays visual
        Fx g(out.toUtf8());
        QVERIFY2(g.ed.mode() == Mode::Visual, qPrintable(g.ed.modeReason()));
    }
    void visualEditPreservesTrailingAsciiSpacesAndHardBreak()
    {
        auto fragments = [](const QTextDocument &doc) {
            QString text;
            for (auto block = doc.begin(); block.isValid(); block = block.next())
                for (auto it = block.begin(); !it.atEnd(); ++it)
                    text += it.fragment().text();
            return text;
        };
        for (const QString &spaces : {QStringLiteral(" "), QStringLiteral("  ")}) {
            NoteEditor editor;
            const QByteArray original("alpha");
            editor.load(original);
            QCOMPARE(editor.mode(), Mode::Visual);
            const int revision = editor.revision();
            QTextCursor cursor(editor.visualEdit()->document());
            cursor.movePosition(QTextCursor::End);
            cursor.insertText(spaces);
            QVERIFY(editor.revision() != revision);
            const QByteArray saved = editor.toMarkdownBytes();
            QVERIFY(saved != original);
            QTextDocument reimported;
            hn::editor::importMarkdown(&reimported, QString::fromUtf8(saved));
            QCOMPARE(fragments(reimported), QStringLiteral("alpha") + spaces);
        }

        NoteEditor editor;
        editor.load(QByteArray("alpha"));
        QCOMPARE(editor.mode(), Mode::Visual);
        QTextCursor cursor(editor.visualEdit()->document());
        cursor.movePosition(QTextCursor::End);
        cursor.insertText(QString(QChar(QChar::LineSeparator)) + QStringLiteral("beta"));
        const QByteArray saved = editor.toMarkdownBytes();
        QTextDocument reimported;
        hn::editor::importMarkdown(&reimported, QString::fromUtf8(saved));
        QCOMPARE(fragments(reimported), QStringLiteral("alpha") + QChar(QChar::LineSeparator) + QStringLiteral("beta"));
    }
    void visualEditPreservesLeadingAsciiSpacesWithHardBreakAndTrailingText()
    {
        auto fragments = [](const QTextDocument &doc) {
            QString text;
            for (auto block = doc.begin(); block.isValid(); block = block.next())
                for (auto it = block.begin(); !it.atEnd(); ++it)
                    text += it.fragment().text();
            return text;
        };
        NoteEditor editor;
        const QByteArray original("alpha  \nbeta");
        editor.load(original);
        QCOMPARE(editor.mode(), Mode::Visual);
        const int revision = editor.revision();
        QTextCursor cursor(editor.visualEdit()->document());
        cursor.movePosition(QTextCursor::Start);
        cursor.insertText(QStringLiteral("  "));
        cursor.movePosition(QTextCursor::End);
        cursor.insertText(QStringLiteral(" tail "));
        QVERIFY(editor.revision() != revision);
        const QByteArray saved = editor.toMarkdownBytes();
        QVERIFY(saved != original);
        QTextDocument reimported;
        hn::editor::importMarkdown(&reimported, QString::fromUtf8(saved));
        QCOMPARE(fragments(reimported), QStringLiteral("  alpha") + QChar(QChar::LineSeparator) + QStringLiteral("beta tail "));
    }
    void lowLevelLeadingSpacesSurviveInlineMarks()
    {
        for (const auto marks : {QPair{false, false}, QPair{true, false}, QPair{false, true}, QPair{true, true}}) {
            QTextDocument source, imported;
            QTextCursor cursor(&source);
            cursor.insertText(QStringLiteral("  "));
            QTextCharFormat format;
            format.setFontWeight(marks.first ? QFont::Bold : QFont::Normal);
            format.setFontItalic(marks.second);
            cursor.insertText(QStringLiteral("word"), format);
            const QString markdown = hn::editor::exportMarkdown(&source);
            hn::editor::importMarkdown(&imported, markdown);
            QString text;
            for (auto block = imported.begin(); block.isValid(); block = block.next())
                for (auto it = block.begin(); !it.atEnd(); ++it) text += it.fragment().text();
            QCOMPARE(text, QStringLiteral("  word"));
        }
    }
    void lowLevelTrailingSpacesSurviveMarkBoundaries()
    {
        struct MarkCase { bool bold; bool italic; bool code; bool link; };
        const QList<MarkCase> cases = {
            {false, false, false, false}, {false, true, false, false},
            {true, false, false, false}, {true, true, false, false},
            {false, false, true, false}, {false, false, false, true}
        };
        for (const MarkCase marks : cases) {
            QTextDocument source, imported, importedAgain;
            QTextCursor cursor(&source);
            cursor.insertText(QStringLiteral("word"));
            QTextCharFormat format;
            format.setFontWeight(marks.bold ? QFont::Bold : QFont::Normal);
            format.setFontItalic(marks.italic);
            if (marks.code) format.setFontFixedPitch(true);
            if (marks.link) { format.setAnchor(true); format.setAnchorHref(QStringLiteral("https://example.test")); }
            cursor.insertText(QStringLiteral("  "), format);
            const QString once = hn::editor::exportMarkdown(&source);
            hn::editor::importMarkdown(&imported, once);
            QString actual;
            for (auto block = imported.begin(); block.isValid(); block = block.next())
                for (auto it = block.begin(); !it.atEnd(); ++it) actual += it.fragment().text();
            QCOMPARE(actual, QStringLiteral("word  "));
            hn::editor::importMarkdown(&importedAgain, once);
            QCOMPARE(hn::editor::exportMarkdown(&importedAgain), once);
        }
    }
    void whitespaceFormatFixtureAdmissionDiagnostic()
    {
        const QString path = QStringLiteral(HN_FUZZ_MARKDOWN_REGRESSION_DIR)
            + QStringLiteral("/crash-5d45ef1d5753124450f2ffd37f93be370e076dbf.base64");
        QFile fixture(path);
        QVERIFY(fixture.open(QIODevice::ReadOnly));
        const QByteArray original = QByteArray::fromBase64(fixture.readAll());
        NoteEditor editor;
        editor.load(original);
        qInfo() << "whitespace fixture mode=" << (editor.mode() == Mode::Visual ? "Visual" : "Source")
                << "reason=" << editor.modeReason();
        const QByteArray untouched = editor.toMarkdownBytes();
        QCOMPARE(untouched, original);
        if (editor.mode() == Mode::Visual) {
            auto describe = [](const QTextDocument &doc) {
                QStringList state;
                for (auto block = doc.begin(); block.isValid(); block = block.next())
                    for (auto it = block.begin(); !it.atEnd(); ++it) {
                        const QTextFragment fragment = it.fragment();
                        state.append(QStringLiteral("%1[b%2i%3]")
                            .arg(QString::fromLatin1(fragment.text().toUtf8().toHex()))
                            .arg(fragment.charFormat().fontWeight() >= QFont::DemiBold)
                            .arg(fragment.charFormat().fontItalic()));
                    }
                return state.join(QStringLiteral(" "));
            };
            qInfo() << "whitespace fixture before=" << describe(*editor.visualEdit()->document());
            QTextCursor cursor(editor.visualEdit()->document());
            cursor.movePosition(QTextCursor::End);
            cursor.insertText(QStringLiteral("x"));
            const QByteArray saved = editor.toMarkdownBytes();
            QVERIFY(saved != original);
            QTextDocument reimported;
            hn::editor::importMarkdown(&reimported, QString::fromUtf8(saved));
            qInfo() << "whitespace fixture edited-save=" << describe(reimported);
        }
    }
    void exportedDelimitersStayFlankingAcrossPunctuationAndSymbols_data()
    {
        QTest::addColumn<QString>("md");
        QTest::addColumn<QString>("mustContain");   // part of the first export (empty: only the fixed point is checked)
        auto row = [](const char *n, const QString &md, const QString &must = QString()) { QTest::newRow(n) << md << must; };
        // italic closes behind a symbol (U+FFFD is a symbol, so '*' next to it and a letter is not right-flanking)
        row("closer-behind-symbol", QString::fromUtf8("x ***\xE2\x82\xAC\xE2\x82\xAC*bold**\n"));
        row("bold-around-italic-symbols", QString::fromUtf8("***\xEF\xBF\xBD\xEF\xBF\xBD*bold**"));
        row("opener-before-symbol", QString::fromUtf8("a**\xC2\xA9" "b**\n"));
        row("opener-after-letter-before-code", QString::fromUtf8("a**`c`**\n"));
        row("link-then-bold", QString::fromUtf8("**[a](u)**b\n"));
        row("adjacent-bold-italic", QString::fromUtf8("**a**_\xE2\x82\xAC_\n"));
        row("strike-intraword", QString::fromUtf8("a~~\xE2\x82\xAC~~b\n"));
        row("literal-bang-before-link", QStringLiteral("\\![l](u)\n"), QStringLiteral("\\!["));
        row("heading-trailing-hash", QStringLiteral("# a \\#\n"), QStringLiteral("\\#"));
        row("fence-info-backslash", QStringLiteral("```a\\\\b\nx\n```\n"), QStringLiteral("a\\\\b"));
        row("list-item-html", QStringLiteral("- <!a\n"));
    }
    void exportedDelimitersStayFlankingAcrossPunctuationAndSymbols()
    {
        QFETCH(QString, md);
        QFETCH(QString, mustContain);
        QTextDocument first, second;
        hn::editor::importMarkdown(&first, md);
        const QString once = hn::editor::exportMarkdown(&first);
        hn::editor::importMarkdown(&second, once);
        const QString twice = hn::editor::exportMarkdown(&second);
        QCOMPARE(twice, once);
        if (!mustContain.isEmpty()) QVERIFY2(once.contains(mustContain), qPrintable(once));
    }
    void overlappingEmphasisLowLevelPreservesSemanticsAndFixedPoint()
    {
        auto formats = [](const QTextDocument &doc) {
            QString result;
            for (auto block = doc.begin(); block.isValid(); block = block.next())
                for (auto it = block.begin(); !it.atEnd(); ++it) {
                    const auto fragment = it.fragment();
                    const auto format = fragment.charFormat();
                    result += QStringLiteral("[%1 bold=%2 italic=%3]")
                        .arg(fragment.text().toHtmlEscaped())
                        .arg(format.fontWeight() >= QFont::DemiBold).arg(format.fontItalic());
                }
            return result;
        };
        const QString input = QString::fromUtf8(QByteArray::fromHex("5fefbfbd5f5f5f5f5f2a5f5f5f3d5f5f"));
        QTextDocument first, second;
        hn::editor::importMarkdown(&first, input);
        const QString once = hn::editor::exportMarkdown(&first);
        hn::editor::importMarkdown(&second, once);
        const QString twice = hn::editor::exportMarkdown(&second);
        QCOMPARE(first.toPlainText(), QString::fromUtf8("_\xEF\xBF\xBD" "*="));
        QCOMPARE(formats(first), QStringLiteral("[_� bold=0 italic=0][* bold=1 italic=1][= bold=1 italic=0]"));
        QCOMPARE(formats(second), formats(first));
        QCOMPARE(second.toPlainText(), first.toPlainText());
        QCOMPARE(twice, once);

    }
    void punctuationAndWhitespaceCorpusWitnessIsFixedPoint()
    {
        // Reduced from a 162-byte historical corpus witness; the six bytes still fail under active sanitizers.
        const QString input = QString::fromUtf8(QByteArray::fromHex("5c0a7e0a5f7e"));
        auto describe = [](const QTextDocument &doc) {
            QString result;
            for (auto block = doc.begin(); block.isValid(); block = block.next())
                for (auto it = block.begin(); !it.atEnd(); ++it) {
                    const auto fragment = it.fragment();
                    const auto format = fragment.charFormat();
                    result += QStringLiteral("[%1 strike=%2 bold=%3 italic=%4]")
                        .arg(fragment.text().toHtmlEscaped())
                        .arg(format.fontStrikeOut())
                        .arg(format.fontWeight() >= QFont::DemiBold)
                        .arg(format.fontItalic());
                }
            return result;
        };
        QTextDocument first, second;
        hn::editor::importMarkdown(&first, input);
        const QString once = hn::editor::exportMarkdown(&first);
        hn::editor::importMarkdown(&second, once);
        const QString twice = hn::editor::exportMarkdown(&second);
        QVERIFY2(twice == once, qPrintable(QStringLiteral("once=%1\ntwice=%2\nfirst=%3\nsecond=%4")
            .arg(once, twice, describe(first), describe(second))));
    }
    void escapedNewlineTildeSequenceIsFixedPoint()
    {
        const QString input = QString::fromUtf8(QByteArray::fromHex("205c0a7e5c0a5c0a7e"));
        auto describe = [](const QTextDocument &doc) {
            QString result;
            for (auto block = doc.begin(); block.isValid(); block = block.next()) {
                const QTextBlockFormat bf = block.blockFormat();
                result += QStringLiteral("block text=%1 heading=%2 list=%3 indent=%4\n")
                    .arg(block.text().toHtmlEscaped()).arg(bf.headingLevel())
                    .arg(block.textList() ? int(block.textList()->format().style()) : -1)
                    .arg(bf.indent());
                for (auto it = block.begin(); !it.atEnd(); ++it)
                    result += QStringLiteral("  fragment=%1 italic=%2 strike=%3 bold=%4\n")
                        .arg(it.fragment().text().toHtmlEscaped())
                        .arg(it.fragment().charFormat().fontItalic())
                        .arg(it.fragment().charFormat().fontStrikeOut())
                        .arg(it.fragment().charFormat().fontWeight() >= QFont::DemiBold);
            }
            return result;
        };
        QTextDocument first, second;
        hn::editor::importMarkdown(&first, input);
        const QString once = hn::editor::exportMarkdown(&first);
        hn::editor::importMarkdown(&second, once);
        const QString twice = hn::editor::exportMarkdown(&second);
        QCOMPARE(once, QString());
        QVERIFY2(twice == once, qPrintable(QStringLiteral("once=%1\ntwice=%2\nfirst:\n%3second:\n%4")
            .arg(once.toUtf8().toHex().constData(), twice.toUtf8().toHex().constData(), describe(first), describe(second))));
    }
    void longEscapedDelimiterRunAcrossHardBreaksIsFixedPoint()
    {
        const QString input = QString::fromUtf8(QByteArray::fromHex(
            "3d605b73606020203a20206060736060202c606060642d606060606060606060606060606060606060606060606060606060606060606026606060603e7e5c0a7e207e5c0a7e602665606073"));
        QTextDocument first, second;
        hn::editor::importMarkdown(&first, input);
        const QString once = hn::editor::exportMarkdown(&first);
        hn::editor::importMarkdown(&second, once);
        const QString twice = hn::editor::exportMarkdown(&second);
        QVERIFY2(twice == once, qPrintable(QStringLiteral("input=%1\nonce=%2\ntwice=%3")
            .arg(input.toUtf8().toHex().constData(), once.toUtf8().toHex().constData(), twice.toUtf8().toHex().constData())));
    }
    void emptyLinkLabelWithLongBacktickRunIsFixedPoint()
    {
        const QString input = QString::fromUtf8(QByteArray::fromHex(
            "5b605d2860606060606060606060606060606060606060606060606060606060606060606460"));
        QTextDocument first, second;
        hn::editor::importMarkdown(&first, input);
        const QString once = hn::editor::exportMarkdown(&first);
        hn::editor::importMarkdown(&second, once);
        const QString twice = hn::editor::exportMarkdown(&second);
        auto fragments = [](const QTextDocument &doc) {
            QString result;
            for (auto block = doc.begin(); block.isValid(); block = block.next())
                for (auto it = block.begin(); !it.atEnd(); ++it) {
                    const auto fragment = it.fragment();
                    const auto format = fragment.charFormat();
                    result += QStringLiteral("[text=%1 anchor=%2 href=%3 fixed=%4]")
                        .arg(fragment.text().toUtf8().toHex().constData())
                        .arg(format.isAnchor())
                        .arg(format.anchorHref().toHtmlEscaped())
                        .arg(format.fontFixedPitch());
                }
            return result;
        };
        QVERIFY2(twice == once, qPrintable(QStringLiteral("input=%1\nonce=%2\ntwice=%3")
            .arg(input.toUtf8().toHex().constData(), once.toUtf8().toHex().constData(), twice.toUtf8().toHex().constData())
            + QStringLiteral("\nfirst text=%1 fragments=%2\nsecond text=%3 fragments=%4")
                .arg(first.toPlainText().toUtf8().toHex().constData(), fragments(first),
                     second.toPlainText().toUtf8().toHex().constData(), fragments(second))));
    }
    void nestedListsWithTabsAndInlineCodeAreFixedPoint()
    {
        const QString input = QString::fromUtf8(QByteArray::fromHex(
            "096120486060630a3e2d090909096060630a3e2d090909092a09"));
        QTextDocument first, second;
        hn::editor::importMarkdown(&first, input);
        const QString once = hn::editor::exportMarkdown(&first);
        hn::editor::importMarkdown(&second, once);
        const QString twice = hn::editor::exportMarkdown(&second);
        auto describe = [](const QTextDocument &doc) {
            QString result;
            for (QTextBlock block = doc.begin(); block.isValid(); block = block.next()) {
                QTextList *list = block.textList();
                result += QStringLiteral("[text=%1 quote=%2")
                    .arg(block.text().toUtf8().toHex().constData())
                    .arg(block.blockFormat().intProperty(QTextFormat::BlockQuoteLevel));
                if (list)
                    result += QStringLiteral(" indent=%1 style=%2 item=%3 count=%4")
                        .arg(list->format().indent()).arg(int(list->format().style()))
                        .arg(list->itemNumber(block)).arg(list->count());
                result += QLatin1Char(']');
            }
            return result;
        };
        QVERIFY2(twice == once, qPrintable(QStringLiteral("input=%1\\nonce=%2\\ntwice=%3")
            .arg(input.toUtf8().toHex().constData(), once.toUtf8().toHex().constData(), twice.toUtf8().toHex().constData())
            + QStringLiteral("\\nfirst=%1\\nsecond=%2").arg(describe(first), describe(second))));
    }
    void minimizedHistoricalPunctuationWhitespaceWitnessIsFixedPoint()
    {
        const QString input = QString::fromUtf8(QByteArray::fromHex("7e5c0a7e0a2d"));
        auto describe = [](const QTextDocument &doc) {
            QString result;
            for (auto block = doc.begin(); block.isValid(); block = block.next()) {
                const QTextBlockFormat format = block.blockFormat();
                result += QStringLiteral("block heading=%1 list=%2 indent=%3 text=%4")
                    .arg(format.headingLevel())
                    .arg(block.textList() ? int(block.textList()->format().style()) : -1)
                    .arg(format.indent())
                    .arg(block.text().toHtmlEscaped());
                for (auto it = block.begin(); !it.atEnd(); ++it) {
                    const QTextFragment fragment = it.fragment();
                    const QTextCharFormat chars = fragment.charFormat();
                    result += QStringLiteral(" [%1 strike=%2 bold=%3 italic=%4]")
                        .arg(fragment.text().toHtmlEscaped())
                        .arg(chars.fontStrikeOut())
                        .arg(chars.fontWeight() >= QFont::DemiBold)
                        .arg(chars.fontItalic());
                }
                result += QLatin1Char('\n');
            }
            return result;
        };
        QTextDocument first, second;
        hn::editor::importMarkdown(&first, input);
        const QString once = hn::editor::exportMarkdown(&first);
        hn::editor::importMarkdown(&second, once);
        const QString twice = hn::editor::exportMarkdown(&second);
        QVERIFY2(twice == once, qPrintable(QStringLiteral("once=%1\ntwice=%2\nfirst=%3\nsecond=%4")
            .arg(once, twice, describe(first), describe(second))));
    }
    void historicalPunctuationAndWhitespaceCorpusWitnessIsFixedPoint()
    {
        const QString input = QString::fromUtf8(QByteArray::fromHex(
            "5c0a7e5f2a5f0a5c097e5f2a5f0a5c094d4d4d4d4d7e5f2a206c690a5c0a7e0a5c0a7e5f2a5f0a5c090a7e0a5c0a7e5f2a5c0a7e54693f690a5c0a7e0a5f2a5c0a7e5f2a5c0a7e5f2a5f0a5c090a7e0a5c0a5f2a5f0a5c094d4d4d4d4d7e5f2a206c690a5c0a7e0a5c0a5f2a5c0a7e54693f690a5c0a7e0a5f2a5c0a7e5f2a5c0a7e5f2a5f0a5c090a7e0a5c0a7e5f2a206c0a2d2d2d7e5f2a206c0a2d2d2d0d690a"));
        auto describe = [](const QTextDocument &doc) {
            QString result;
            for (auto block = doc.begin(); block.isValid(); block = block.next())
                for (auto it = block.begin(); !it.atEnd(); ++it) {
                    const auto fragment = it.fragment();
                    const auto format = fragment.charFormat();
                    result += QStringLiteral("[%1 strike=%2 bold=%3 italic=%4]")
                        .arg(fragment.text().toHtmlEscaped())
                        .arg(format.fontStrikeOut())
                        .arg(format.fontWeight() >= QFont::DemiBold)
                        .arg(format.fontItalic());
                }
            return result;
        };
        QTextDocument first, second;
        hn::editor::importMarkdown(&first, input);
        const QString once = hn::editor::exportMarkdown(&first);
        hn::editor::importMarkdown(&second, once);
        const QString twice = hn::editor::exportMarkdown(&second);
        QVERIFY2(twice == once, qPrintable(QStringLiteral("once=%1\ntwice=%2\nfirst=%3\nsecond=%4")
            .arg(once, twice, describe(first), describe(second))));
    }
    void historicalListBlockShapeWitnessIsFixedPoint()
    {
        const QString input = QString::fromUtf8(QByteArray::fromHex(
            "2361624b6169536f6c0a2d202a2d20640a20202a202a0a6f6c6f6c6e67545b"));
        auto describe = [](const QTextDocument &doc) {
            QString result;
            for (auto block = doc.begin(); block.isValid(); block = block.next()) {
                const QTextBlockFormat format = block.blockFormat();
                QTextList *list = block.textList();
                result += QStringLiteral("heading=%1 listIndent=%2 item=%3 text=%4")
                    .arg(format.headingLevel())
                    .arg(list ? list->format().indent() : 0)
                    .arg(list ? list->itemNumber(block) : -1)
                    .arg(block.text().toHtmlEscaped());
                for (auto it = block.begin(); !it.atEnd(); ++it) {
                    const QTextFragment fragment = it.fragment();
                    const QTextCharFormat chars = fragment.charFormat();
                    result += QStringLiteral(" [%1 bold=%2 italic=%3 strike=%4]")
                        .arg(fragment.text().toHtmlEscaped())
                        .arg(chars.fontWeight() >= QFont::DemiBold)
                        .arg(chars.fontItalic())
                        .arg(chars.fontStrikeOut());
                }
                result += QLatin1Char('\n');
            }
            return result;
        };
        QTextDocument first, second;
        hn::editor::importMarkdown(&first, input);
        const QString once = hn::editor::exportMarkdown(&first);
        hn::editor::importMarkdown(&second, once);
        const QString twice = hn::editor::exportMarkdown(&second);
        QVERIFY2(twice == once, qPrintable(QStringLiteral("once=%1\ntwice=%2\nfirst=%3\nsecond=%4")
            .arg(once, twice, describe(first), describe(second))));
        QCOMPARE(describe(second), describe(first));
        QCOMPARE(second.toPlainText(), first.toPlainText());
    }
    void historicalWhitespaceEmphasisBackslashWitnessIsFixedPoint()
    {
        const QString input = QString::fromUtf8(QByteArray::fromHex(
            "5c0a5c0a7e5f2a5f5c090909652d0a7e5f2a206c690a5c0a7e090a7e5f5c0909205d6c69"));
        auto describe = [](const QTextDocument &doc) {
            QString result;
            for (auto block = doc.begin(); block.isValid(); block = block.next()) {
                const QTextBlockFormat blockFormat = block.blockFormat();
                result += QStringLiteral("heading=%1 listIndent=%2 text=%3")
                    .arg(blockFormat.headingLevel())
                    .arg(block.textList() ? block.textList()->format().indent() : 0)
                    .arg(block.text().toHtmlEscaped());
                for (auto it = block.begin(); !it.atEnd(); ++it) {
                    const QTextFragment fragment = it.fragment();
                    const QTextCharFormat format = fragment.charFormat();
                    result += QStringLiteral(" [%1 bold=%2 italic=%3 strike=%4 code=%5]")
                        .arg(fragment.text().toHtmlEscaped())
                        .arg(format.fontWeight() >= QFont::DemiBold)
                        .arg(format.fontItalic())
                        .arg(format.fontStrikeOut())
                        .arg(format.fontFixedPitch());
                }
                result += QLatin1Char('\n');
            }
            return result;
        };
        QTextDocument first, second;
        hn::editor::importMarkdown(&first, input);
        const QString once = hn::editor::exportMarkdown(&first);
        hn::editor::importMarkdown(&second, once);
        const QString twice = hn::editor::exportMarkdown(&second);
        QVERIFY2(twice == once, qPrintable(QStringLiteral("once=%1\ntwice=%2\nfirst=%3\nsecond=%4")
            .arg(once, twice, describe(first), describe(second))));
    }
    void minimizedWhitespaceEmphasisBackslashWitnessIsFixedPoint()
    {
        const QString input = QString::fromUtf8(QByteArray::fromHex("5f5c0a7e097e5f"));
        auto describe = [](const QTextDocument &doc) {
            QString result;
            for (auto block = doc.begin(); block.isValid(); block = block.next())
                for (auto it = block.begin(); !it.atEnd(); ++it) {
                    const QTextFragment fragment = it.fragment();
                    const QTextCharFormat format = fragment.charFormat();
                    result += QStringLiteral("[%1 bold=%2 italic=%3 strike=%4]")
                        .arg(fragment.text().toHtmlEscaped())
                        .arg(format.fontWeight() >= QFont::DemiBold)
                        .arg(format.fontItalic())
                        .arg(format.fontStrikeOut());
                }
            return result;
        };
        QTextDocument first, second;
        hn::editor::importMarkdown(&first, input);
        const QString once = hn::editor::exportMarkdown(&first);
        hn::editor::importMarkdown(&second, once);
        const QString twice = hn::editor::exportMarkdown(&second);
        QVERIFY2(twice == once, qPrintable(QStringLiteral("once=%1\ntwice=%2\nfirst=%3\nsecond=%4")
            .arg(once, twice, describe(first), describe(second))));
    }
    void minimizedHardBreakStrikeWitnessIsFixedPoint()
    {
        const QString input = QString::fromUtf8(QByteArray::fromHex("7e5c0a7e"));
        QTextDocument first, second;
        hn::editor::importMarkdown(&first, input);
        const QString once = hn::editor::exportMarkdown(&first);
        hn::editor::importMarkdown(&second, once);
        const QString twice = hn::editor::exportMarkdown(&second);
        QVERIFY2(twice == once, qPrintable(QStringLiteral("input=%1 firstText=%2 once=%3 secondText=%4 twice=%5")
            .arg(input.toUtf8().toHex().constData(), first.toPlainText().toUtf8().toHex().constData(),
                once.toUtf8().toHex().constData(), second.toPlainText().toUtf8().toHex().constData(),
                twice.toUtf8().toHex().constData())));
    }
    void historicalBacktickSeedIsFixedPoint()
    {
        const QString input = QString::fromUtf8(QByteArray::fromHex(
            "606f73606020203a20206060736060202c606060602d606060606060606060606060606060606060606060606060606060606060606026606060606060606060203a202060602665732665"));
        auto describe = [](const QTextDocument &doc) {
            QString result;
            for (auto block = doc.begin(); block.isValid(); block = block.next())
                for (auto it = block.begin(); !it.atEnd(); ++it) {
                    const auto fragment = it.fragment();
                    result += QStringLiteral("[%1 code=%2]")
                        .arg(fragment.text().toUtf8().toHex().constData())
                        .arg(fragment.charFormat().fontFixedPitch());
                }
            return result;
        };
        QTextDocument first, second;
        hn::editor::importMarkdown(&first, input);
        const QString once = hn::editor::exportMarkdown(&first);
        hn::editor::importMarkdown(&second, once);
        const QString twice = hn::editor::exportMarkdown(&second);
        QVERIFY2(twice == once, qPrintable(QStringLiteral("input=%1\nonce=%2\ntwice=%3\nfirst=%4\nsecond=%5")
            .arg(input.toUtf8().toHex().constData(), once.toUtf8().toHex().constData(),
                twice.toUtf8().toHex().constData(), describe(first), describe(second))));
    }
    void historicalBomHeadingSeedIsFixedPoint()
    {
        const QString input = QString::fromUtf8(QByteArray::fromHex(
            "2360693261efbbbfefbbbf2a234e486577696b692d6c69613261efbbbf696e670aefbbbf2a6c6961efbbbf6414696e670aefbbbfefbbbf6414696e670aefbbbf2a2364"));
        auto describe = [](const QTextDocument &doc) {
            QString result;
            for (auto block = doc.begin(); block.isValid(); block = block.next())
                for (auto it = block.begin(); !it.atEnd(); ++it) {
                    const auto fragment = it.fragment();
                    result += QStringLiteral("[%1 code=%2 bold=%3 italic=%4 strike=%5]")
                        .arg(fragment.text().toUtf8().toHex().constData())
                        .arg(fragment.charFormat().fontFixedPitch())
                        .arg(fragment.charFormat().fontWeight() >= QFont::DemiBold)
                        .arg(fragment.charFormat().fontItalic())
                        .arg(fragment.charFormat().fontStrikeOut());
                }
            return result;
        };
        QTextDocument first, second;
        hn::editor::importMarkdown(&first, input);
        const QString once = hn::editor::exportMarkdown(&first);
        hn::editor::importMarkdown(&second, once);
        const QString twice = hn::editor::exportMarkdown(&second);
        QVERIFY2(twice == once, qPrintable(QStringLiteral("input=%1\nonce=%2\ntwice=%3\nfirst=%4\nsecond=%5")
            .arg(input.toUtf8().toHex().constData(), once.toUtf8().toHex().constData(),
                twice.toUtf8().toHex().constData(), describe(first), describe(second))));
    }
    void numericEntityBomIsPreserved()
    {
        QTextDocument doc;
        hn::editor::importMarkdown(&doc, QStringLiteral("a&#65279;b"));
        QVERIFY2(doc.toPlainText().contains(QChar(0xFEFF)), qPrintable(doc.toPlainText().toUtf8().toHex()));
    }
    void historicalMixedHardBreakSeedIsFixedPoint()
    {
        const QString input = QString::fromUtf8(QByteArray::fromHex("20560a5c0a7e5c0a7e"));
        auto describe = [](const QTextDocument &doc) {
            QString result;
            for (auto block = doc.begin(); block.isValid(); block = block.next()) {
                result += QStringLiteral("{block=%1}").arg(block.text().toUtf8().toHex().constData());
                for (auto it = block.begin(); !it.atEnd(); ++it) {
                    const auto fragment = it.fragment();
                    result += QStringLiteral("[%1 strike=%2]")
                        .arg(fragment.text().toUtf8().toHex().constData())
                        .arg(fragment.charFormat().fontStrikeOut());
                }
            }
            return result;
        };
        QTextDocument first, second;
        hn::editor::importMarkdown(&first, input);
        const QString once = hn::editor::exportMarkdown(&first);
        hn::editor::importMarkdown(&second, once);
        const QString twice = hn::editor::exportMarkdown(&second);
        QVERIFY2(twice == once, qPrintable(QStringLiteral("input=%1\nonce=%2\ntwice=%3\nfirst=%4\nsecond=%5")
            .arg(input.toUtf8().toHex().constData(), once.toUtf8().toHex().constData(),
                twice.toUtf8().toHex().constData(), describe(first), describe(second))));
    }
    void overlappingEmphasisEditBuiltDocumentPreservesSemanticsAndFixedPoint()
    {
        auto formats = [](const QTextDocument &doc) {
            QString result;
            for (auto block = doc.begin(); block.isValid(); block = block.next())
                for (auto it = block.begin(); !it.atEnd(); ++it) {
                    const auto fragment = it.fragment();
                    const auto format = fragment.charFormat();
                    result += QStringLiteral("[%1 bold=%2 italic=%3]")
                        .arg(fragment.text().toHtmlEscaped())
                        .arg(format.fontWeight() >= QFont::DemiBold).arg(format.fontItalic());
                }
            return result;
        };
        QTextDocument edited;
        QTextCursor cursor(&edited);
        QTextCharFormat italic; italic.setFontItalic(true);
        QTextCharFormat both = italic; both.setFontWeight(QFont::Bold);
        QTextCharFormat bold; bold.setFontWeight(QFont::Bold);
        cursor.insertText(QString::fromUtf8("\xEF\xBF\xBD"), italic);
        cursor.insertText(QStringLiteral("*"), both);
        cursor.insertText(QStringLiteral("="), bold);
        const QString editedOnce = hn::editor::exportMarkdown(&edited);
        QTextDocument editedAgain, editedThird;
        hn::editor::importMarkdown(&editedAgain, editedOnce);
        const QString editedTwice = hn::editor::exportMarkdown(&editedAgain);
        hn::editor::importMarkdown(&editedThird, editedTwice);
        QCOMPARE(editedAgain.toPlainText(), edited.toPlainText());
        QCOMPARE(formats(editedAgain), formats(edited));
        QCOMPARE(formats(editedThird), formats(edited));
        QCOMPARE(editedTwice, editedOnce);
    }
    void editBuiltNbspEmphasisPreservesTextFormatsAndFixedPoint()
    {
        struct Run { QString text; bool bold; bool italic; };
        const QString nbsp(QChar(0x00a0));
        const QList<QList<Run>> cases = {
            {{QStringLiteral("a"), false, false}, {nbsp, false, true}, {QStringLiteral("b"), false, false}},
            {{nbsp, false, true}, {QStringLiteral("b"), false, false}},
            {{QStringLiteral("a"), false, false}, {nbsp, false, true}},
            {{QStringLiteral("a"), false, false}, {nbsp, true, false}, {QStringLiteral("b"), false, false}},
            {{nbsp, true, false}, {QStringLiteral("b"), false, false}},
            {{QStringLiteral("a"), false, false}, {nbsp, true, false}},
            {{QStringLiteral("a"), false, false}, {nbsp, true, true}, {QStringLiteral("b"), false, false}},
            {{nbsp, true, true}, {QStringLiteral("b"), false, false}},
            {{QStringLiteral("a"), false, false}, {nbsp, true, true}},
            {{QStringLiteral("_"), false, false}, {nbsp, false, true}, {QStringLiteral("*"), false, false}},
            {{nbsp, false, false}},
        };
        QStringList failures;
        for (qsizetype caseIndex = 0; caseIndex < cases.size(); ++caseIndex) {
            QTextDocument source, first, second;
            QTextCursor cursor(&source);
            QString expected;
            QString expectedFormats;
            for (const Run &run : cases[caseIndex]) {
                QTextCharFormat format;
                format.setFontWeight(run.bold ? QFont::Bold : QFont::Normal);
                format.setFontItalic(run.italic);
                cursor.insertText(run.text, format);
                expected += run.text;
                for (qsizetype i = 0; i < run.text.size(); ++i)
                    expectedFormats += QStringLiteral("%1%2").arg(run.bold ? QLatin1Char('B') : QLatin1Char('-')).arg(run.italic ? QLatin1Char('I') : QLatin1Char('-'));
            }
            const QString once = hn::editor::exportMarkdown(&source);
            hn::editor::importMarkdown(&first, once);
            hn::editor::importMarkdown(&second, hn::editor::exportMarkdown(&first));
            auto fragmentText = [](const QTextDocument &doc) {
                QString text;
                for (auto block = doc.begin(); block.isValid(); block = block.next())
                    for (auto it = block.begin(); !it.atEnd(); ++it) text += it.fragment().text();
                return text;
            };
            QString actualFormats;
            for (auto block = first.begin(); block.isValid(); block = block.next())
                for (auto it = block.begin(); !it.atEnd(); ++it) {
                    const auto format = it.fragment().charFormat();
                    for (qsizetype i = 0; i < it.fragment().text().size(); ++i)
                        actualFormats += QStringLiteral("%1%2").arg(format.fontWeight() >= QFont::DemiBold ? QLatin1Char('B') : QLatin1Char('-')).arg(format.fontItalic() ? QLatin1Char('I') : QLatin1Char('-'));
                }
            const QString context = QStringLiteral("case %1 input=%2 once=%3 first=%4 formats=%5 expectedFormats=%6 twice=%7")
                .arg(caseIndex).arg(expected.toUtf8().toHex().constData(), once.toUtf8().toHex().constData(),
                    first.toPlainText().toUtf8().toHex().constData(), actualFormats, expectedFormats,
                    hn::editor::exportMarkdown(&second).toUtf8().toHex().constData());
            if (fragmentText(first) != expected || actualFormats != expectedFormats
                || fragmentText(second) != expected || hn::editor::exportMarkdown(&second) != once)
                failures.append(context);
        }
        QVERIFY2(failures.isEmpty(), qPrintable(failures.join(QLatin1Char('\n'))));
    }
    void bracketColonParagraphDoesNotBecomeReferenceDefinition()
    {
        // "[foo]: url" typed as text must come back escaped, not as an invisible link reference definition.
        Fx f(QByteArray("\\[foo]: http://x.example\n\nb\n"));
        QVERIFY2(f.ed.mode() == Mode::Visual, qPrintable(f.ed.modeReason()));
        f.moveEnd();
        f.text("z");
        f.key(Qt::Key_Backspace);
        const QString out = f.md();
        QVERIFY2(out.contains(QStringLiteral("\\[foo]")), qPrintable(out));
        Fx g(out.toUtf8());
        QVERIFY2(g.ed.mode() == Mode::Visual, qPrintable(g.ed.modeReason()));
        QCOMPARE(g.v->document()->blockCount(), f.v->document()->blockCount());
    }
    void literalBracketsSplitAcrossFormattingRunsStayLiteral()
    {
        // "[" + bold + "](u)": the link syntax is only complete across runs, so the "[" must stay escaped.
        for (const char *md : {"\\[**b**](u)\n", "\\[a *b* c](u)\n", "\\[**b**]\\[ref]\n", "\\[x `c` y](u)\n", "\\[**b**]: http://x.example\n",
                                  "\\\\\\[x](y)\n", "\\[a\\\\\\](u)\n", "\\[\\[a](b)\n"}) {
            Fx f{QByteArray(md)};
            QVERIFY2(f.ed.mode() == Mode::Visual, md);
            f.moveEnd();
            f.text("z");
            f.key(Qt::Key_Backspace);
            const QString out = f.md();
            QVERIFY2(sameSemantics(QString::fromUtf8(md), out), qPrintable(QString::fromUtf8(md) + "\n--- exported ---\n" + out));
            QVERIFY2(!out.startsWith(QLatin1Char('[')), qPrintable(out));
            Fx g(out.toUtf8());
            QVERIFY2(g.ed.mode() == Mode::Visual, qPrintable(g.ed.modeReason()));
            QCOMPARE(g.md(), f.md());
        }
    }
    void shortcutReferenceWithDefinitionNeverLosesBytes()
    {
        // "[a]" with a definition elsewhere is a link in CommonMark. The editor must either keep the exact bytes (source mode)
        // or round-trip with the same semantics; it must never silently turn the reference into plain text or drop the definition.
        const QByteArray md("see [a] here\n\n[a]: http://x.example\n");
        Fx f(md);
        if (f.ed.mode() == Mode::Visual) {
            f.moveEnd();
            f.text("z");
            f.key(Qt::Key_Backspace);
            QVERIFY2(sameSemantics(QString::fromUtf8(md), f.md()), qPrintable(f.md()));
        } else {
            QCOMPARE(f.md().toUtf8(), md);
        }
    }
    void literalByteOrderMarkAfterEscapeSurvivesImport()
    {
        // md4c splits text at backslash escapes; a piece starting with U+FEFF must keep it (QString::fromUtf8 would drop it).
        const QString bom(QChar(0xFEFF));
        for (const QString &md : {QStringLiteral("\\<\\<") + bom + QStringLiteral("\\<\\<\n"),
                                  QStringLiteral("a\\*") + bom + bom + QStringLiteral("b\n"),
                                  QStringLiteral("<<<") + bom + QStringLiteral("<<<\n")}) {
            QTextDocument first, second;
            hn::editor::importMarkdown(&first, md);
            const QString once = hn::editor::exportMarkdown(&first);
            hn::editor::importMarkdown(&second, once);
            QCOMPARE(hn::editor::exportMarkdown(&second), once);
            QVERIFY2(second.toPlainText().count(QChar(0xFEFF)) == first.toPlainText().count(QChar(0xFEFF)), qPrintable(once));
        }
    }
    void nbspOnlyParagraphIsKept()
    {
        // &nbsp; spacer lines are content, not empty paragraphs.
        Fx f(QByteArray("a\n\n&nbsp;\n\nb\n"));
        QVERIFY2(f.ed.mode() == Mode::Visual, qPrintable(f.ed.modeReason()));
        const int blocks = f.v->document()->blockCount();
        f.moveEnd();
        f.text("z");
        f.key(Qt::Key_Backspace);
        const QString out = f.md();
        Fx g(out.toUtf8());
        QVERIFY2(g.ed.mode() == Mode::Visual, qPrintable(g.ed.modeReason()));
        QCOMPARE(g.v->document()->blockCount(), blocks);
    }
    void hardBreakExport()
    {
        Fx f;
        f.text("a");
        f.key(Qt::Key_Return, Qt::ShiftModifier);
        f.text("b");
        f.key(Qt::Key_Return);
        f.text("- x");
        f.key(Qt::Key_Return, Qt::ShiftModifier);
        f.text("y");
        const QString out = f.md();
        QCOMPARE(out, QStringLiteral("a\\\nb\n\n- x\\\n  y\n"));
        QVERIFY(sameSemantics(out, QStringLiteral("a  \nb\n\n- x  \n  y\n")));
    }
    void hardBreakBeforeEscapedThematicTextIsIdempotent()
    {
        const QString source = QString::fromLatin1("\\\n~\n~-");
        QTextDocument first, second;
        hn::editor::importMarkdown(&first, source);
        const QString once = hn::editor::exportMarkdown(&first);
        hn::editor::importMarkdown(&second, once);
        const QString twice = hn::editor::exportMarkdown(&second);
        QCOMPARE(twice, once);
    }
    void adjacentItalicAndStrikeDelimitersAreIdempotent()
    {
        const QString source = QStringLiteral("_d_4_~_l_~");
        auto formats = [](const QTextDocument &doc) {
            QString result;
            for (auto block = doc.begin(); block.isValid(); block = block.next())
                for (auto it = block.begin(); !it.atEnd(); ++it) {
                    const QTextFragment fragment = it.fragment();
                    const QTextCharFormat format = fragment.charFormat();
                    result += QStringLiteral("[%1 bold=%2 italic=%3 strike=%4]")
                        .arg(fragment.text().toHtmlEscaped())
                        .arg(format.fontWeight() >= QFont::DemiBold)
                        .arg(format.fontItalic())
                        .arg(format.fontStrikeOut());
                }
            return result;
        };
        QTextDocument first, second;
        hn::editor::importMarkdown(&first, source);
        const QString once = hn::editor::exportMarkdown(&first);
        hn::editor::importMarkdown(&second, once);
        const QString twice = hn::editor::exportMarkdown(&second);
        QVERIFY2(twice == once,
                 qPrintable(QStringLiteral("once=%1\ntwice=%2\nfirst formats=%3\nsecond formats=%4")
                     .arg(once.toHtmlEscaped(), twice.toHtmlEscaped(), formats(first), formats(second))));
    }
    void nestedItalicStrongDelimitersAreIdempotent()
    {
        const QString source = QStringLiteral("*i*__l*i*__");
        auto formats = [](const QTextDocument &doc) {
            QString result;
            for (auto block = doc.begin(); block.isValid(); block = block.next())
                for (auto it = block.begin(); !it.atEnd(); ++it) {
                    const QTextFragment fragment = it.fragment();
                    const QTextCharFormat format = fragment.charFormat();
                    result += QStringLiteral("[%1 bold=%2 italic=%3]")
                        .arg(fragment.text().toHtmlEscaped())
                        .arg(format.fontWeight() >= QFont::DemiBold)
                        .arg(format.fontItalic());
                }
            return result;
        };
        QTextDocument first, second;
        hn::editor::importMarkdown(&first, source);
        const QString once = hn::editor::exportMarkdown(&first);
        hn::editor::importMarkdown(&second, once);
        const QString twice = hn::editor::exportMarkdown(&second);
        QVERIFY2(twice == once,
                 qPrintable(QStringLiteral("once=%1\ntwice=%2\nfirst=%3\nsecond=%4")
                     .arg(once.toHtmlEscaped(), twice.toHtmlEscaped(), formats(first), formats(second))));
    }
    void escapedHardBreakAcrossBlockquoteBoundaryIsIdempotent()
    {
        const QString source = QStringLiteral("> ~\\\n~");
        QTextDocument first, second;
        hn::editor::importMarkdown(&first, source);
        const QString once = hn::editor::exportMarkdown(&first);
        QCOMPARE(once, QStringLiteral("> &#32;\n"));
        hn::editor::importMarkdown(&second, once);
        const QString twice = hn::editor::exportMarkdown(&second);
        QCOMPARE(second.begin().blockFormat().intProperty(QTextFormat::BlockQuoteLevel), 1);
        QCOMPARE(second.begin().text(), QStringLiteral(" "));
        QCOMPARE(twice, once);
    }
    void invalidLeadingUtf8BacktickRunIsIdempotent()
    {
        const QByteArray raw = QByteArray::fromHex(
            "f06060206060606060606060606060606060606060606060606060606060606060606060266060");
        const QString source = QString::fromUtf8(raw);
        QTextDocument first, second;
        hn::editor::importMarkdown(&first, source);
        const QString once = hn::editor::exportMarkdown(&first);
        hn::editor::importMarkdown(&second, once);
        const QString twice = hn::editor::exportMarkdown(&second);
        QCOMPARE(twice, once);
    }
    void fuzzRegressionMarkdownRoundtripIsIdempotent()
    {
        const QDir dir(QStringLiteral(HN_FUZZ_MARKDOWN_REGRESSION_DIR));
        auto fixtures = dir.entryList({QStringLiteral("*.base64")}, QDir::Files, QDir::Name);
        QVERIFY(!fixtures.isEmpty());
        const QString knownMismatch = QStringLiteral("crash-fdcb7155250c7c86d78eee024362d97ca0c9c755.base64");
        QVERIFY(fixtures.removeOne(knownMismatch));
        fixtures.prepend(knownMismatch);
        auto describeFormats = [](const QTextDocument &doc) {
            QString result;
            for (auto block = doc.begin(); block.isValid(); block = block.next()) {
                for (auto it = block.begin(); !it.atEnd(); ++it) {
                    const QTextFragment fragment = it.fragment();
                    const QTextCharFormat format = fragment.charFormat();
                    result += QStringLiteral("[%1 strike=%2 code=%3 bold=%4 italic=%5]")
                        .arg(fragment.text().toHtmlEscaped())
                        .arg(format.fontStrikeOut())
                        .arg(format.fontFixedPitch())
                        .arg(format.fontWeight() >= QFont::DemiBold)
                        .arg(format.fontItalic());
                }
                result += QLatin1Char('\n');
            }
            return result;
        };
        QStringList failures;
        for (const auto &name : fixtures) {
            QFile fixture(dir.filePath(name));
            QVERIFY(fixture.open(QIODevice::ReadOnly));
            const QByteArray input = QByteArray::fromBase64(fixture.readAll());
            const QString source = QString::fromUtf8(input);
            QTextDocument first, second;
            hn::editor::importMarkdown(&first, source);
            const QString once = hn::editor::exportMarkdown(&first);
            hn::editor::importMarkdown(&second, once);
            const QString twice = hn::editor::exportMarkdown(&second);
            if (twice != once) failures.append(QStringLiteral(
                "low-level fixture: %1\nonce:\n%2\ntwice:\n%3\nfirst formats:\n%4\nsecond formats:\n%5")
                .arg(name, once, twice, describeFormats(first), describeFormats(second)));

            NoteEditor admission;
            admission.load(input);
            if (admission.mode() != Mode::Visual) {   // never exported by the product: bytes are preserved instead
                QCOMPARE(admission.toMarkdownBytes(), input);
                continue;
            }
        }
        QVERIFY2(failures.isEmpty(), qPrintable(failures.join(QStringLiteral("\n--- next fixture ---\n"))));
    }
    void unstableFuzzMarkdownFallsBackAndPreservesOriginalBytes()
    {
        const QString fixturePath = QStringLiteral(HN_FUZZ_MARKDOWN_REGRESSION_DIR)
            + QStringLiteral("/crash-fdcb7155250c7c86d78eee024362d97ca0c9c755.base64");
        QFile fixture(fixturePath);
        QVERIFY(fixture.open(QIODevice::ReadOnly));
        const QByteArray original = QByteArray::fromBase64(fixture.readAll());
        NoteEditor editor;
        editor.load(original);
        QCOMPARE(editor.mode(), Mode::Source);
        QCOMPARE(editor.toMarkdownBytes(), original);
    }
    void literalNumericEntitySurvivesMarkdownRoundtrip()
    {
        auto fixedPoint = [](const QString &markdown) {
            QTextDocument first, second;
            hn::editor::importMarkdown(&first, markdown);
            const QString once = hn::editor::exportMarkdown(&first);
            hn::editor::importMarkdown(&second, once);
            const QString twice = hn::editor::exportMarkdown(&second);
            return qMakePair(once, twice);
        };

        const auto literalEntity = fixedPoint(QStringLiteral("\\&#105; literal\n"));
        QVERIFY2(literalEntity.first.contains(QStringLiteral("\\&#105;")), qPrintable(literalEntity.first));
        QCOMPARE(literalEntity.second, literalEntity.first);
    }

    void generatedEntityBeforeStrikeCodeBoundaryPreservesFragments()
    {
        struct FragmentState { QString text; bool strike; bool code; };
        auto fragments = [](const QTextDocument &doc) {
            QList<FragmentState> result;
            for (auto block = doc.begin(); block.isValid(); block = block.next())
                for (auto it = block.begin(); !it.atEnd(); ++it) {
                    const auto fragment = it.fragment();
                    const auto format = fragment.charFormat();
                    result.append({fragment.text(), format.fontStrikeOut(), format.fontFixedPitch()});
                }
            return result;
        };
        auto describe = [](const QList<FragmentState> &states) {
            QStringList result;
            for (const auto &state : states)
                result.append(QStringLiteral("[%1 strike=%2 code=%3]")
                    .arg(state.text.toHtmlEscaped()).arg(state.strike).arg(state.code));
            return result.join(QString());
        };
        QTextDocument original;
        QTextCursor cursor(&original);
        cursor.insertText(QStringLiteral("li"));
        QTextCharFormat strike;
        strike.setFontStrikeOut(true);
        cursor.insertText(QStringLiteral("8"), strike);
        QTextCharFormat code;
        code.setFontFixedPitch(true);
        code.setFontStrikeOut(true);
        cursor.insertText(QStringLiteral("x"), code);

        const QString once = hn::editor::exportMarkdown(&original);
        QTextDocument first, second;
        hn::editor::importMarkdown(&first, once);
        const QString twice = hn::editor::exportMarkdown(&first);
        hn::editor::importMarkdown(&second, twice);
        qInfo().noquote() << "once:" << once << "twice:" << twice
                          << "original:" << describe(fragments(original))
                          << "first:" << describe(fragments(first))
                          << "second:" << describe(fragments(second));

        QCOMPARE(fragments(first).size(), fragments(original).size());
        QCOMPARE(describe(fragments(first)), describe(fragments(original)));
        QCOMPARE(describe(fragments(second)), describe(fragments(original)));
    }

    void encodedLeftBoundaryBeforeStrikeCodeIsStable()
    {
        const QString candidate = QStringLiteral("l&#105;~~&#56;`x`~~");
        QTextDocument first, second;
        hn::editor::importMarkdown(&first, candidate);
        auto describe = [](const QTextDocument &doc) {
            QString result;
            for (auto block = doc.begin(); block.isValid(); block = block.next())
                for (auto it = block.begin(); !it.atEnd(); ++it) {
                    const auto fragment = it.fragment();
                    const auto format = fragment.charFormat();
                    result += QStringLiteral("[%1 strike=%2 code=%3]")
                        .arg(fragment.text().toHtmlEscaped())
                        .arg(format.fontStrikeOut()).arg(format.fontFixedPitch());
                }
            return result;
        };
        const QString once = hn::editor::exportMarkdown(&first);
        hn::editor::importMarkdown(&second, once);
        const QString twice = hn::editor::exportMarkdown(&second);
        qInfo().noquote() << "candidate:" << candidate << "once:" << once << "twice:" << twice
                          << "formats:" << describe(first) << "second formats:" << describe(second);
        QCOMPARE(first.toPlainText(), QStringLiteral("li8x"));
        QCOMPARE(describe(first), QStringLiteral("[li strike=0 code=0][8 strike=1 code=0][x strike=1 code=1]"));
        QCOMPARE(describe(second), describe(first));
        QCOMPARE(twice, once);
    }

    void adjacentStrikeBoundariesRemainStable()
    {
        struct Case { QString markdown; QString text; QString fragments; };
        const QList<Case> cases = {
            {QStringLiteral("a~~b~~"), QStringLiteral("a~~b~~"), QStringLiteral("[a~~b~~ strike=0 code=0]")},
            {QStringLiteral("!~~b~~"), QStringLiteral("!b"), QStringLiteral("[! strike=0 code=0][b strike=1 code=0]")},
            {QStringLiteral("*a*~~b~~"), QStringLiteral("ab"), QStringLiteral("[a strike=0 code=0][b strike=1 code=0]")},
            {QStringLiteral("~~a**b**~~"), QStringLiteral("ab"), QStringLiteral("[a strike=1 code=0][b strike=1 code=0]")},
            {QStringLiteral("~~a `b`~~"), QStringLiteral("a b"), QStringLiteral("[a  strike=1 code=0][b strike=1 code=1]")},
            {QStringLiteral("é&#49;~~&#50;x~~"), QStringLiteral("é12x"), QStringLiteral("[é1 strike=0 code=0][2x strike=1 code=0]")},
        };
        auto describe = [](const QTextDocument &doc) {
            QString result;
            for (auto block = doc.begin(); block.isValid(); block = block.next())
                for (auto it = block.begin(); !it.atEnd(); ++it) {
                    const auto f = it.fragment();
                    result += QStringLiteral("[%1 strike=%2 code=%3]").arg(f.text().toHtmlEscaped())
                        .arg(f.charFormat().fontStrikeOut()).arg(f.charFormat().fontFixedPitch());
                }
            return result;
        };
        for (const Case &test : cases) {
            QTextDocument first, second;
            hn::editor::importMarkdown(&first, test.markdown);
            const QString once = hn::editor::exportMarkdown(&first);
            hn::editor::importMarkdown(&second, once);
            const QString twice = hn::editor::exportMarkdown(&second);
            QCOMPARE(first.toPlainText(), test.text);
            QCOMPARE(describe(first), test.fragments);
            QCOMPARE(describe(second), test.fragments);
            QCOMPARE(twice, once);
        }
    }

    void strikeBeforeCodeSpanWithMarkdownPunctuationIsStable()
    {
        QTextDocument source;
        QTextCursor cursor(&source);
        cursor.insertText(QStringLiteral("li"));
        QTextCharFormat strike;
        strike.setFontStrikeOut(true);
        cursor.insertText(QStringLiteral("8"), strike);
        QTextCharFormat code;
        code.setFontFixedPitch(true);
        code.setFontFamilies({QStringLiteral("monospace")});
        cursor.insertText(QStringLiteral("~*"), code);

        const QString once = hn::editor::exportMarkdown(&source);
        QTextDocument imported;
        hn::editor::importMarkdown(&imported, once);
        QCOMPARE(imported.toPlainText(), QStringLiteral("li8~*"));
        const QString twice = hn::editor::exportMarkdown(&imported);
        QCOMPARE(twice, once);

        auto flags = [](const QTextDocument &doc) {
            QString result;
            for (auto block = doc.begin(); block.isValid(); block = block.next())
                for (auto it = block.begin(); !it.atEnd(); ++it) {
                    const auto fr = it.fragment();
                    result += QStringLiteral("%1:%2:%3|").arg(fr.text())
                        .arg(fr.charFormat().fontStrikeOut()).arg(fr.charFormat().fontFixedPitch());
                }
            return result;
        };
        QCOMPARE(flags(imported), QStringLiteral("li:0:0|8:1:0|~*:0:1|"));
        QTextDocument second;
        hn::editor::importMarkdown(&second, twice);
        QCOMPARE(second.toPlainText(), QStringLiteral("li8~*"));
        QCOMPARE(flags(second), flags(imported));
        QCOMPARE(hn::editor::exportMarkdown(&second), twice);
    }

    void listIndentTrimEscapesOnlyContentStart()
    {
        auto fixedPoint = [](const QString &markdown) {
            QTextDocument first, second;
            hn::editor::importMarkdown(&first, markdown);
            const QString once = hn::editor::exportMarkdown(&first);
            hn::editor::importMarkdown(&second, once);
            return qMakePair(once, hn::editor::exportMarkdown(&second));
        };
        QTextDocument listDoc;
        QTextCursor cursor(&listDoc);
        cursor.insertText(QStringLiteral("    # heading\n    ---"));
        cursor.select(QTextCursor::Document);
        cursor.createList(QTextListFormat{});
        const QString listOnce = hn::editor::exportMarkdown(&listDoc);
        QTextDocument listSecond;
        hn::editor::importMarkdown(&listSecond, listOnce);
        const QString listTwice = hn::editor::exportMarkdown(&listSecond);
        QVERIFY2(listOnce.contains(QStringLiteral("- ") + QStringLiteral("\\# heading")), qPrintable(listOnce));
        QVERIFY2(listOnce.contains(QStringLiteral("- ") + QStringLiteral("\\---")), qPrintable(listOnce));
        QCOMPARE(listTwice, listOnce);

        const auto nested = fixedPoint(QStringLiteral("- parent\n  - child\n"));
        QVERIFY2(nested.first.contains(QStringLiteral("  - child")), qPrintable(nested.first));
        QCOMPARE(nested.second, nested.first);
    }
    void repeatedLeadingBomFallsBackAndPreservesOriginalBytes()
    {
        const QByteArray original("\xEF\xBB\xBF\xEF\xBB\xBF" "text\n");
        NoteEditor editor;
        editor.load(original);
        QCOMPARE(editor.mode(), Mode::Source);
        QCOMPARE(editor.toMarkdownBytes(), original);
    }
    void inlineCodeRetainsStrongFormatting()
    {
        QTextDocument doc;
        hn::editor::importMarkdown(&doc, QStringLiteral("**strong `code`**"));
        bool found = false;
        for (auto block = doc.begin(); block.isValid(); block = block.next())
            for (auto it = block.begin(); !it.atEnd(); ++it) {
                const auto fragment = it.fragment();
                if (fragment.text() != QStringLiteral("code") || !fragment.charFormat().fontFixedPitch()) continue;
                found = true;
                QVERIFY(fragment.charFormat().fontWeight() >= QFont::DemiBold);
            }
        QVERIFY(found);
    }
    void inlineCodeWithTabWhitespaceIsFixedPoint()
    {
        const QString input = QString::fromUtf8(QByteArray::fromHex("60202009202060"));
        QTextDocument first, second;
        hn::editor::importMarkdown(&first, input);
        const QString once = hn::editor::exportMarkdown(&first);
        hn::editor::importMarkdown(&second, once);
        const QString twice = hn::editor::exportMarkdown(&second);
        QCOMPARE(first.toPlainText(), QStringLiteral(" \t "));
        QCOMPARE(second.toPlainText(), first.toPlainText());
        QVERIFY2(twice == once, qPrintable(QStringLiteral("once=%1\\ntwice=%2\\nfirst-text=%3\\nsecond-text=%4")
            .arg(once.toUtf8().toHex().constData(), twice.toUtf8().toHex().constData(),
                 first.toPlainText().toUtf8().toHex().constData(), second.toPlainText().toUtf8().toHex().constData())));
    }
    void delimiterFamilyTransitionWithIntrawordUnderscoreIsFixedPoint()
    {
        const QString input = QString::fromUtf8(QByteArray::fromHex("2d7e5f0a7e5c0a7e5f6c7e0a2d"));
        auto describe = [](const QTextDocument &doc) {
            QString result;
            for (auto block = doc.begin(); block.isValid(); block = block.next()) {
                for (auto it = block.begin(); !it.atEnd(); ++it) {
                    const auto fragment = it.fragment();
                    const auto format = fragment.charFormat();
                    result += QStringLiteral("[%1 strike=%2 italic=%3]")
                        .arg(fragment.text().toUtf8().toHex().constData())
                        .arg(format.fontStrikeOut()).arg(format.fontItalic());
                }
                result += QLatin1Char('|');
            }
            return result;
        };
        QTextDocument first, second;
        hn::editor::importMarkdown(&first, input);
        const QString once = hn::editor::exportMarkdown(&first);
        hn::editor::importMarkdown(&second, once);
        const QString twice = hn::editor::exportMarkdown(&second);
        QVERIFY2(twice == once, qPrintable(QStringLiteral("input=%1\\nonce=%2\\ntwice=%3\\nfirst=%4\\nsecond=%5")
            .arg(input.toUtf8().toHex().constData(), once.toUtf8().toHex().constData(),
                 twice.toUtf8().toHex().constData(), describe(first), describe(second))));
    }
    void inlineCodeWithThirtyTwoBackticksIsStable()
    {
        const QString codeText(32, QLatin1Char('`'));
        QTextDocument source, first, second;
        QTextCursor cursor(&source);
        QTextCharFormat code;
        code.setFontFixedPitch(true);
        cursor.insertText(codeText, code);
        const QString once = hn::editor::exportMarkdown(&source);
        hn::editor::importMarkdown(&first, once);
        const QString twice = hn::editor::exportMarkdown(&first);
        QVERIFY2(first.toPlainText() == codeText, qPrintable(QStringLiteral("markdown=%1 text=%2")
            .arg(once.toUtf8().toHex().constData(), first.toPlainText().toUtf8().toHex().constData())));
        hn::editor::importMarkdown(&second, once);
        QCOMPARE(hn::editor::exportMarkdown(&second), once);
        QCOMPARE(twice, once);
    }
    void trailingHeadingSpaceAfterHardBreakIsIdempotent()
    {
        const QString input = QStringLiteral("~l\n\\\n~\n-");
        QTextDocument first, second;
        hn::editor::importMarkdown(&first, input);
        const QString once = hn::editor::exportMarkdown(&first);
        hn::editor::importMarkdown(&second, once);
        QCOMPARE(hn::editor::exportMarkdown(&second), once);
    }
    void inlineCodeWithThirtyOneBackticksUsesAcceptedFence()
    {
        const QString codeText(31, QLatin1Char('`'));
        QTextDocument source, first;
        QTextCursor cursor(&source);
        QTextCharFormat code;
        code.setFontFixedPitch(true);
        cursor.insertText(codeText, code);
        const QString once = hn::editor::exportMarkdown(&source);
        hn::editor::importMarkdown(&first, once);
        QCOMPARE(first.toPlainText(), codeText);
        bool importedAsCode = false;
        for (auto block = first.begin(); block.isValid(); block = block.next())
            for (auto it = block.begin(); !it.atEnd(); ++it)
                if (it.fragment().charFormat().fontFixedPitch()) importedAsCode = true;
        QVERIFY2(importedAsCode, qPrintable(once.toUtf8().toHex()));
    }
    void inlineCodeWithUnsupportedDelimiterFallsBackToSourceMode()
    {
        const QByteArray codeText(32, '`');
        const QByteArray markdown = QByteArray("`` ") + codeText + QByteArray(" ``\n");
        NoteEditor editor;
        editor.load(markdown);
        QCOMPARE(editor.mode(), Mode::Source);
        QCOMPARE(editor.toMarkdownBytes(), markdown);
    }
    void emphasisWhitespaceStaysOutsideDelimiters()
    {
        Fx f;
        f.text("a b c");
        f.select(1, 4);   // " b "
        f.ed.toggleInline(InlineStyle::Bold);
        QCOMPARE(f.md(), QStringLiteral("a **b** c\n"));
    }
    void adjacentListsStayApart()
    {
        Fx f;
        f.text("- a\n\n");   // exit list via Enter on empty item
        f.key(Qt::Key_Return);
        f.text("- b");
        const QString m = f.md();
        QVERIFY2(m.contains(QStringLiteral("- a")) && (m.contains(QStringLiteral("* b")) || m.contains(QStringLiteral("\n\n- b"))), qPrintable(m));
    }
    void orderedListInsideQuoteStaysDistinctFromFollowingList()
    {
        const QStringList inputs = {QStringLiteral(">0)\n0)"), QStringLiteral("> 1. quoted\n1. outside\n"),
                                    QStringLiteral("0)\n>\n0)"), QStringLiteral(">- -")};
        auto describe = [](const QTextDocument &doc) {
            QString result;
            for (QTextBlock block = doc.begin(); block.isValid(); block = block.next()) {
                const auto format = block.blockFormat();
                auto *list = block.textList();
                result += QStringLiteral("[text=%1 quote=%2")
                    .arg(block.text().toUtf8().toHex().constData()).arg(format.intProperty(QTextFormat::BlockQuoteLevel));
                if (list)
                    result += QStringLiteral(" start=%1 item=%2 count=%3 indent=%4")
                        .arg(list->format().start()).arg(list->itemNumber(block)).arg(list->count()).arg(list->format().indent());
                result += QLatin1Char(']');
            }
            return result;
        };
        for (const QString &input : inputs) {
            QTextDocument first, second;
            hn::editor::importMarkdown(&first, input);
            const QString once = hn::editor::exportMarkdown(&first);
            hn::editor::importMarkdown(&second, once);
            const QString twice = hn::editor::exportMarkdown(&second);
            QVERIFY2(twice == once, qPrintable(QStringLiteral("input=%1\\nonce=%2\\ntwice=%3")
                .arg(input.toUtf8().toHex().constData(), once.toUtf8().toHex().constData(),
                     twice.toUtf8().toHex().constData()) + QStringLiteral("\\nfirst=%1\\nsecond=%2")
                .arg(describe(first), describe(second))));
            if (input == QStringLiteral("0)\n>\n0)"))
                QVERIFY2(once.contains(QStringLiteral("\n> &#32;\n")), qPrintable(once.toUtf8().toHex()));
            else
                QVERIFY2(once.startsWith(QStringLiteral("> ")), qPrintable(once.toUtf8().toHex()));
        }
    }
    void chunkedListsRetainNumberingNestingAndExactExport()
    {
        QString md;
        for (int i = 0; i < 520; ++i)
            md += QStringLiteral("%1. item%2\n").arg(i + 7).arg(i);
        md += QStringLiteral("\n- root\n  - child\n    - deep\n\n30. restart\n31. next\n");

        QTextDocument doc;
        hn::editor::importMarkdown(&doc, md);
        const QString out = hn::editor::exportMarkdown(&doc);
        QCOMPARE(out, md);
        QVERIFY(sameSemantics(md, out));

        QTextDocument reimported;
        hn::editor::importMarkdown(&reimported, out);
        QCOMPARE(hn::editor::exportMarkdown(&reimported), out);
    }

    void orderedListChunkContinuesAfterInsertionAtBoundary()
    {
        QString md;
        for (int i = 0; i < 520; ++i)
            md += QStringLiteral("%1. item%2\n").arg(i + 7).arg(i);

        QTextDocument doc;
        hn::editor::importMarkdown(&doc, md);
        QTextBlock boundary;
        QTextBlock nextChunk;
        for (QTextBlock block = doc.begin(); block.isValid(); block = block.next())
            if (block.text() == QStringLiteral("item255")) {
                boundary = block;
                nextChunk = block.next();
            }
        QVERIFY(boundary.isValid());
        QTextList *firstChunk = boundary.textList();
        QVERIFY(firstChunk);
        QVERIFY(nextChunk.isValid());
        QCOMPARE(nextChunk.text(), QStringLiteral("item256"));
        QCOMPARE(nextChunk.textList(), firstChunk);

        QTextCursor cursor(boundary);
        cursor.movePosition(QTextCursor::EndOfBlock);
        cursor.insertBlock();
        cursor.insertText(QStringLiteral("inserted"));

        const QString exported = hn::editor::exportMarkdown(&doc);
        QVERIFY2(exported.contains(QStringLiteral("263. inserted\n264. item256\n")), qPrintable(exported));
        QCOMPARE(firstChunk->format().start() + firstChunk->itemNumber(cursor.block()), 263);
        QCOMPARE(firstChunk->format().start() + firstChunk->itemNumber(nextChunk), 264);
    }
};

QTEST_MAIN(RoundTripTest)
#include "editor_roundtrip_test.moc"
