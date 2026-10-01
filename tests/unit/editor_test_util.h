#pragma once
#include <QApplication>
#include <QClipboard>
#include <QKeyEvent>
#include <QMimeData>
#include <QSignalSpy>
#include <QTest>
#include <QTextBlock>
#include <QTextList>

#include "hn/core/markdown_codec.h"
#include "hn/editor/note_editor.h"

namespace hntest {
using namespace hn::editor;

struct Fx {
    NoteEditor ed;
    QTextEdit *v = nullptr;
    explicit Fx(const QByteArray &initial = {})
    {
        ed.resize(640, 480);
        ed.show();
        (void)QTest::qWaitForWindowExposed(&ed);
        ed.activateWindow();
        ed.load(initial);
        v = ed.visualEdit();
        ed.focusEditor();
    }
    QWidget *w() { return ed.activeEdit(); }
    QString md() { return QString::fromUtf8(ed.toMarkdownBytes()); }
    QString plain() { return v->toPlainText(); }
    void key(Qt::Key k, Qt::KeyboardModifiers m = Qt::NoModifier) { QTest::keyClick(w(), k, m); }
    void text(const QString &s)
    {
        for (qsizetype i = 0; i < s.size(); ++i) {
            const QChar c = s.at(i);
            if (c == u'\n') key(Qt::Key_Return);
            else if (c == u'\t') key(Qt::Key_Tab);
            else if (c.unicode() < 0x80) QTest::keyClick(w(), c.toLatin1());
            else {
                qsizetype n = c.isHighSurrogate() && i + 1 < s.size() ? 2 : 1;
                QKeyEvent ev(QEvent::KeyPress, 0, Qt::NoModifier, s.mid(i, n));
                QApplication::sendEvent(w(), &ev);
                i += n - 1;
            }
        }
    }
    void commit(const QString &s)
    {
        QInputMethodEvent ev;
        ev.setCommitString(s);
        QApplication::sendEvent(w(), &ev);
    }
    void preedit(const QString &s)
    {
        QInputMethodEvent ev(s, {});
        QApplication::sendEvent(w(), &ev);
    }
    void moveEnd() { QTextCursor c = v->textCursor(); c.movePosition(QTextCursor::End); v->setTextCursor(c); }
    void selectAllText() { QTextCursor c(v->document()); c.select(QTextCursor::Document); v->setTextCursor(c); }
    void select(int a, int b) { QTextCursor c(v->document()); c.setPosition(a); c.setPosition(b, QTextCursor::KeepAnchor); v->setTextCursor(c); }
};
inline bool sameSemantics(const QString &a, const QString &b) { return hn::core::semanticTokens(a) == hn::core::semanticTokens(b); }
} // namespace hntest
