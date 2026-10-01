#include "tag_edit.h"
#include <QRegularExpression>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>

using namespace hn::editor;

namespace hn::app {

QStringList parseTagInput(const QString &text) {
    static const QRegularExpression sep(QStringLiteral("[\\s,]+")), bad(QStringLiteral("[^\\p{L}\\p{N}_-]"));
    QStringList out;
    for (QString t : text.split(sep, Qt::SkipEmptyParts)) {
        if (t.startsWith('#')) t.remove(0, 1);
        t.remove(bad);
        t = t.toLower();
        if (!t.isEmpty() && !out.contains(t)) out << t;
    }
    return out;
}

int applyTagEdit(NoteEditor *ed, const QStringList &add, const QStringList &remove) {
    if (ed->isReadOnly() || (add.isEmpty() && remove.isEmpty())) return 0;
    const bool visual = ed->mode() == Mode::Visual;
    QTextDocument *doc = visual ? ed->visualEdit()->document() : ed->sourceEdit()->document();
    auto scope = ed->recorder().begin(TxKind::Format, WindowMode::WholeDoc);
    int removed = 0;
    for (const QString &tag : remove) {
        const QRegularExpression re(QStringLiteral("(?<![\\p{L}\\p{N}_#-])#%1(?![\\p{L}\\p{N}_-])").arg(QRegularExpression::escape(tag)),
                                    QRegularExpression::CaseInsensitiveOption | QRegularExpression::UseUnicodePropertiesOption);
        int pos = 0;
        for (;;) {
            QTextCursor from(doc);
            from.setPosition(pos);
            QTextCursor hit = doc->find(re, from);
            if (hit.isNull()) break;
            const int start = hit.selectionStart();
            if (hit.charFormat().fontFixedPitch() || !hit.charFormat().anchorHref().isEmpty()) { pos = hit.selectionEnd(); continue; }
            hit.removeSelectedText();
            // drop one separating space too so "a #x b" does not leave a double space
            const QChar before = start > 0 ? doc->characterAt(start - 1) : QChar(), after = doc->characterAt(start);
            if (before == QLatin1Char(' ') && (after == QLatin1Char(' ') || after == QChar::ParagraphSeparator || after.isNull())) {
                QTextCursor sp(doc);
                sp.setPosition(start - 1);
                sp.setPosition(start, QTextCursor::KeepAnchor);
                sp.removeSelectedText();
                pos = start - 1;
            } else pos = start;
            ++removed;
        }
    }
    if (!add.isEmpty()) {
        QTextCursor c(doc);
        c.movePosition(QTextCursor::End);
        QStringList tokens;
        for (const QString &t : add) tokens << QLatin1Char('#') + t;
        const QString text = tokens.join(QLatin1Char(' '));
        if (!c.block().text().trimmed().isEmpty()) c.insertBlock(QTextBlockFormat(), QTextCharFormat());
        c.insertText(text);
    }
    return removed;   // scope destructor commits the single transaction
}

} // namespace hn::app
