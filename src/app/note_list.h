#pragma once
// Model + delegate based note list and left rail: no per-row widgets (spec 4.3, 8).
#include "hn/core/library_index.h"
#include <QAbstractListModel>
#include <QStyledItemDelegate>
#include <functional>

namespace hn::app {

class NoteListModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum Role { RelRole = Qt::UserRole + 1, TitleRole, SnippetRole, FolderRole, TagsRole, MtimeRole, DirtyRole, ColorRole, OpenRole };
    static constexpr int kPage = 100;       // rows requested per page
    static constexpr int kMaxRows = 500;    // rows ever held (LibraryIndex caches the same bound)

    explicit NoteListModel(QObject *parent = nullptr) : QAbstractListModel(parent) {}
    int rowCount(const QModelIndex &p = {}) const override { return p.isValid() ? 0 : int(m_rows.size()); }
    QVariant data(const QModelIndex &i, int role) const override;
    bool canFetchMore(const QModelIndex &p) const override { return !p.isValid() && m_hasMore && m_rows.size() < kMaxRows && !m_fetching; }
    void fetchMore(const QModelIndex &p) override;

    void setRows(QList<hn::core::IndexRow> rows, bool hasMore);
    void appendRows(QList<hn::core::IndexRow> rows, bool hasMore);
    void fetchFinished() { m_fetching = false; }
    int rowOf(const QString &rel) const;
    bool truncated() const { return m_hasMore && m_rows.size() >= kMaxRows; }
    bool hasMore() const { return m_hasMore; }

    std::function<int(const QString &)> colorOf;   // note colour index
    std::function<bool(const QString &)> isOpen;   // open in some window

signals:
    void fetchRequested(int offset);

private:
    QList<hn::core::IndexRow> m_rows;
    bool m_hasMore = false, m_fetching = false;
};

class NoteDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    static constexpr int kRowHeight = 80;
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter *p, const QStyleOptionViewItem &o, const QModelIndex &i) const override;
    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override { return {0, kRowHeight}; }
    bool editorEvent(QEvent *e, QAbstractItemModel *m, const QStyleOptionViewItem &o, const QModelIndex &i) override;
    static QRect actionRect(const QRect &row);
signals:
    void openInWindow(const QString &rel);
};

struct RailItem {
    enum Kind { Header, All, Folder, Tag } kind = Header;
    QString label, key;
    int count = -1;
    int depth = 0;
};

class RailModel : public QAbstractListModel {
    Q_OBJECT
public:
    explicit RailModel(QObject *parent = nullptr) : QAbstractListModel(parent) {}
    int rowCount(const QModelIndex &p = {}) const override { return p.isValid() ? 0 : int(m_items.size()); }
    QVariant data(const QModelIndex &i, int role) const override;
    Qt::ItemFlags flags(const QModelIndex &i) const override;
    void setItems(QList<RailItem> items) { beginResetModel(); m_items = std::move(items); endResetModel(); }
    const RailItem &item(int r) const { return m_items[r]; }
    int rowOf(RailItem::Kind k, const QString &key) const;
private:
    QList<RailItem> m_items;
};

class RailDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter *p, const QStyleOptionViewItem &o, const QModelIndex &i) const override;
    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &i) const override;
};

} // namespace hn::app
