#pragma once
#include <QWidget>
#include <functional>

#include "hn/editor/wiki_links.h"
#include "hn/theme/theme.h"

namespace hn::editor {

// Flat modernist list popup (square, hairline border, accent bar on the selected row, max 8 rows, scroll). It never takes
// focus: NoteEditor keeps it and forwards navigation keys. Private to the editor module.
class CompletionPopup : public QWidget {
public:
    static constexpr int kMaxRows = 8;
    explicit CompletionPopup(QWidget *owner);
    void setTheme(const hn::theme::Theme &t, const QFont &f);
    void setItems(const QList<CompletionItem> &items, const QString &query);
    int count() const { return int(m_items.size()); }
    int current() const { return m_cur; }
    CompletionItem itemAt(int i) const { return m_items.value(i); }
    const CompletionItem *currentItem() const { return m_cur >= 0 && m_cur < count() ? &m_items[m_cur] : nullptr; }
    void step(int delta);                  // Up/Down = +-1, PageUp/PageDown = +-visible rows (clamped, no wrap at ends for pages)
    void page(int dir);
    int rowHeight() const { return m_rowH; }
    int visibleRows(const QRect &avail) const;
    QSize wantedSize(const QRect &avail) const;
    // Pure placement used by show(): below the caret, flipped above when it does not fit, always inside `avail`.
    static QRect place(const QRect &caretGlobal, const QSize &size, const QRect &avail);
    void showAt(const QRect &caretGlobal);
    std::function<void(int)> picked;       // row index clicked

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;

private:
    int rowAt(const QPoint &p) const;
    void ensureVisible();
    QList<CompletionItem> m_items;
    QString m_query;
    hn::theme::Theme m_t;
    int m_cur = 0, m_top = 0, m_rowH = 28, m_rows = kMaxRows;
};

} // namespace hn::editor
