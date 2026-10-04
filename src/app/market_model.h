#pragma once
// Browse tab data: the registry entries (filtered) as a list model, and the painted row delegate.
// Rows are painted, never widgets, so a 500-entry index costs one model and nothing per row.
#include "hn/plugins/market.h"
#include <QAbstractListModel>
#include <QHash>
#include <QStyledItemDelegate>

namespace hn::app {

class MarketModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum Role { EntryIdRole = Qt::UserRole + 1, StateRole };   // StateRole: hn::plugins::MarketState as int
    using QAbstractListModel::QAbstractListModel;
    int rowCount(const QModelIndex &parent = {}) const override { return parent.isValid() ? 0 : m_rows.size(); }
    QVariant data(const QModelIndex &i, int role) const override;
    // installedVersions: plugin id -> version of what is installed now.
    void setEntries(QList<hn::plugins::MarketEntry> e, QHash<QString, QString> installedVersions);
    // text matches name, description, tags and author (case-insensitive); tag is exact (empty = any);
    // hideDangerous drops native entries and entries asking for a dangerous permission.
    void setFilter(const QString &text, const QString &tag, bool hideDangerous);
    const hn::plugins::MarketEntry *entryAt(int row) const;
    hn::plugins::MarketState stateAt(int row) const;
    int totalCount() const { return m_all.size(); }
    QStringList allTags() const;
private:
    void refilter();
    QList<hn::plugins::MarketEntry> m_all;
    QHash<QString, QString> m_installed;
    QList<int> m_rows;   // indexes into m_all
    QString m_text, m_tag;
    bool m_hideDangerous = false;
};

class MarketDelegate : public QStyledItemDelegate {
public:
    struct Chip { QString text; bool dangerous = false; };
    using QStyledItemDelegate::QStyledItemDelegate;
    // Permission chips of an entry (a native tier adds a NATIVE chip); dangerous ones are painted in the danger colour with the warning icon.
    static QList<Chip> chipsFor(const hn::plugins::MarketEntry &e);
    static QString stateText(hn::plugins::MarketState s);
    // Where everything of one row goes, relative to the row rect. sizeHint and paint both use this, so they cannot disagree.
    struct Geometry { QRect head, state; QList<QRect> chips; QList<Chip> chipData; int height = 0; };
    static Geometry geometry(const hn::plugins::MarketEntry &e, hn::plugins::MarketState s, int rowWidth);
    static constexpr int kMargin = 10;
    static int contentWidth(int rowWidth) { return rowWidth - 2 * kMargin - 4; }
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
};

} // namespace hn::app
