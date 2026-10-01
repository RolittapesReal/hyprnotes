#include "editor_bridge.h"
#include <QTextCursor>

using namespace hn::editor;

namespace hn::app {

QTextCursor EditorBridge::cursor() const {
    return m_ed->mode() == Mode::Visual ? m_ed->visualEdit()->textCursor() : m_ed->sourceEdit()->textCursor();
}
void EditorBridge::setCursor(const QTextCursor &c) {
    if (m_ed->mode() == Mode::Visual) m_ed->visualEdit()->setTextCursor(c); else m_ed->sourceEdit()->setTextCursor(c);
}

QString EditorBridge::selectionText() {
    QString t = cursor().selectedText();
    t.replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
    return t;
}

void EditorBridge::beginTransaction(const QString &) {
    endTransaction();
    if (m_ed->isReadOnly()) return;
    m_scope = m_ed->recorder().begin(TxKind::Format, WindowMode::Selection);
}

void EditorBridge::replaceSelection(const QString &utf8) {
    if (m_ed->isReadOnly()) return;
    QTextCursor c = cursor();
    c.insertText(utf8);
    setCursor(c);
}

void EditorBridge::insertText(const QString &utf8) {
    if (m_ed->isReadOnly()) return;
    QTextCursor c = cursor();
    c.clearSelection();
    c.insertText(utf8);
    setCursor(c);
}

void EditorBridge::endTransaction() { m_scope = EditRecorder::Scope(); }

} // namespace hn::app
