#pragma once
#include <QColor>
#include <QPainter>
#include <QTextBlock>
#include <QPlainTextEdit>
#include <QTextEdit>
#include <QUrl>
#include <functional>

#include "hn/editor/history.h"
#include "hn/editor/types.h"

namespace hn::editor {

// Every edit route is wrapped in an EditRecorder scope; QTextDocument native undo is disabled.
class VisualEdit : public QTextEdit {
    Q_OBJECT
public:
    VisualEdit(EditRecorder *rec, QWidget *parent = nullptr);

    void toggleInline(InlineStyle s);
    bool inlineActive(InlineStyle s) const;
    void setBlockStyle(BlockStyle s);
    BlockStyle blockStyle() const;
    void toggleList(ListKind k);
    ListKind listKind() const;
    bool toggleCheckAtCursor();
    bool toggleCheck(const QTextBlock &b);
    bool indentList(int dir);
    void setLink(const QString &url);
    QString linkAtCursor() const;
    void insertMarkdownFragment(const QTextDocument &parsed);   // "Paste as Markdown"
    void cutRecorded();
    void normalizeDocument();
    void setStyleContext(const QString &body, const QString &mono, qreal lineHeight, const QColor &link);
    bool imePreedit() const { return m_preedit; }
    QString monoFamily() const { return m_mono; }
    void setBackgroundColor(const QColor &c) { m_bg = c; viewport()->update(); }   // used to hide Qt's native task box
    QRect checkRect(const QTextBlock &b) const;   // viewport coords of the painted (and clickable) checklist box
    void setRuleColor(const QColor &c) { m_rule = c; viewport()->update(); }   // horizontal rule hairline (theme border)
    std::function<void()> afterTyping;   // additive: called after a plain typed character (not IME, no selection)
    std::function<void(QPainter &)> overlayPaint;   // additive: painted on the viewport after everything else

signals:
    void undoRequested();
    void redoRequested();
    void linkActivated(const QUrl &url);
    void linkRequested();

protected:
    bool event(QEvent *e) override;
    void paintEvent(QPaintEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void inputMethodEvent(QInputMethodEvent *e) override;
    void insertFromMimeData(const QMimeData *src) override;
    void dropEvent(QDropEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;

private:
    void handleEnter(bool shift);
    bool tryInlineShortcut();
    bool tryBlockShortcut();
    bool tryFenceShortcut();
    bool tryRuleShortcut();
    bool guardRule(QKeyEvent *e);
    void healRules();
    void openLineAfterRule();
    void styleBlock(const QTextBlock &b, BlockStyle s);
    void applyList(QList<QTextBlock> blocks, ListKind k, int start = 1);
    bool selectionTouchesList() const;
    QList<QTextBlock> selectedBlocks() const;
    void normalizeRange(int firstBlock, int lastBlock);
    EditRecorder *m_rec;
    QString m_body, m_mono;
    qreal m_lh = 1.45;
    QColor m_link, m_rule, m_bg;
    static constexpr int kCheckBox = 16, kCheckGap = 8;
    void paintChecks(QPainter &p);
    bool m_preedit = false;
};

class SourceEdit : public QPlainTextEdit {
    Q_OBJECT
public:
    SourceEdit(EditRecorder *rec, QWidget *parent = nullptr);
    void cutRecorded();
    std::function<void()> afterTyping;   // additive, see VisualEdit
    std::function<void(QPainter &)> overlayPaint;   // additive, see VisualEdit
    bool imePreedit() const { return m_preedit; }
    QTextBlock firstVisible() const { return firstVisibleBlock(); }
    QPointF blockOrigin(const QTextBlock &b) const { return blockBoundingGeometry(b).translated(contentOffset()).topLeft(); }
signals:
    void undoRequested();
    void redoRequested();

protected:
    void keyPressEvent(QKeyEvent *e) override;
    void paintEvent(QPaintEvent *e) override;
    void inputMethodEvent(QInputMethodEvent *e) override;
    void insertFromMimeData(const QMimeData *src) override;
    void dropEvent(QDropEvent *e) override;

private:
    EditRecorder *m_rec;
    bool m_preedit = false;
};

} // namespace hn::editor
