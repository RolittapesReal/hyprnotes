#include "note_list.h"
#include "ui_common.h"
#include <QAbstractItemView>
#include <QApplication>
#include <QMouseEvent>
#include <QPainter>

using namespace hn::core;

namespace hn::app {

// ---------------------------------------------------------------- NoteListModel
QVariant NoteListModel::data(const QModelIndex &i, int role) const {
    if (!i.isValid() || i.row() >= m_rows.size()) return {};
    const IndexRow &r = m_rows[i.row()];
    switch (role) {
    case Qt::DisplayRole: case TitleRole: return r.title;
    case Qt::AccessibleTextRole: return QStringLiteral("%1. %2").arg(r.title, r.snippet);
    case RelRole: return r.relPath;
    case SnippetRole: return r.snippet;
    case FolderRole: return r.folder;
    case TagsRole: return r.tags;
    case MtimeRole: return QDateTime::fromMSecsSinceEpoch(r.mtimeMs);
    case DirtyRole: return r.dirty;
    case ColorRole: return colorOf ? colorOf(r.relPath) : 0;
    case OpenRole: return isOpen ? isOpen(r.relPath) : false;
    }
    return {};
}

void NoteListModel::fetchMore(const QModelIndex &) {
    m_fetching = true;
    emit fetchRequested(int(m_rows.size()));
}

void NoteListModel::setRows(QList<IndexRow> rows, bool hasMore) {
    beginResetModel();
    m_rows = std::move(rows);
    if (m_rows.size() > kMaxRows) m_rows.resize(kMaxRows);
    m_hasMore = hasMore;
    m_fetching = false;
    endResetModel();
}

void NoteListModel::appendRows(QList<IndexRow> rows, bool hasMore) {
    m_fetching = false;
    m_hasMore = hasMore;
    const int room = kMaxRows - int(m_rows.size());
    if (room <= 0 || rows.isEmpty()) return;
    if (rows.size() > room) rows.resize(room);
    beginInsertRows({}, int(m_rows.size()), int(m_rows.size() + rows.size()) - 1);
    m_rows += rows;
    endInsertRows();
}

int NoteListModel::rowOf(const QString &rel) const {
    for (int i = 0; i < m_rows.size(); ++i) if (m_rows[i].relPath == rel) return i;
    return -1;
}

// ---------------------------------------------------------------- NoteDelegate
QRect NoteDelegate::actionRect(const QRect &row) { return QRect(row.right() - 16 - 24 + 1, row.top() + 52, 24, 24); }

void NoteDelegate::paint(QPainter *p, const QStyleOptionViewItem &o, const QModelIndex &i) const {
    const auto &t = ui::theme();
    const QRect r = o.rect;
    const bool sel = o.state & QStyle::State_Selected, hover = o.state & QStyle::State_MouseOver;
    p->save();
    p->fillRect(r, sel ? t.selection : (hover ? t.surface : t.bg));
    p->fillRect(QRect(r.left(), r.bottom(), r.width(), 1), t.border);
    p->fillRect(QRect(r.left(), r.top(), sel ? 6 : 4, r.height() - 1), ui::noteColor(i.data(NoteListModel::ColorRole).toInt()));

    const int x = r.left() + 20, right = r.right() - 16;
    const QString time = ui::relativeTime(i.data(NoteListModel::MtimeRole).toDateTime());
    p->setFont(ui::uiFont(12));
    const int timeW = p->fontMetrics().horizontalAdvance(time);
    p->setPen(t.muted);
    p->drawText(QRect(right - timeW, r.top() + 12, timeW, 20), Qt::AlignVCenter | Qt::AlignRight, time);
    int titleRight = right - timeW - 12;
    if (i.data(NoteListModel::OpenRole).toBool()) {
        const QFont lf = ui::labelFont(9);
        p->setFont(lf);
        const QString open = QObject::tr("OPEN");
        const int w = QFontMetrics(lf).horizontalAdvance(open) + 12;
        const QRect chip(titleRight - w, r.top() + 14, w, 16);
        p->fillRect(chip, t.accent);
        p->setPen(t.accentText);
        p->drawText(chip, Qt::AlignCenter, open);
        titleRight -= w + 8;
    }
    if (i.data(NoteListModel::DirtyRole).toBool()) { p->fillRect(QRect(titleRight - 8, r.top() + 18, 8, 8), t.accent); titleRight -= 16; }

    QFont tf = ui::uiFont(14, QFont::Bold);
    p->setFont(tf);
    p->setPen(t.text);
    QString title = i.data(NoteListModel::TitleRole).toString();
    if (title.isEmpty()) title = QObject::tr("Untitled");
    p->drawText(QRect(x, r.top() + 12, titleRight - x, 20), Qt::AlignVCenter | Qt::AlignLeft, QFontMetrics(tf).elidedText(title, Qt::ElideRight, titleRight - x));

    p->setFont(ui::uiFont(12));
    p->setPen(t.muted);
    const QString snip = i.data(NoteListModel::SnippetRole).toString().simplified();
    p->drawText(QRect(x, r.top() + 34, right - x, 18), Qt::AlignVCenter | Qt::AlignLeft,
                p->fontMetrics().elidedText(snip.isEmpty() ? QObject::tr("No content yet") : snip, Qt::ElideRight, right - x));

    // meta: FOLDER  #tag #tag
    int mx = x;
    const QString folder = i.data(NoteListModel::FolderRole).toString();
    const QFont lf = ui::labelFont(10);
    p->setFont(lf);
    p->setPen(t.muted);
    if (!folder.isEmpty()) {
        const QString f = QFontMetrics(lf).elidedText(folder.section('/', -1), Qt::ElideRight, 96);
        p->drawText(QRect(mx, r.top() + 56, 96, 16), Qt::AlignVCenter | Qt::AlignLeft, f);
        mx += QFontMetrics(lf).horizontalAdvance(f) + 12;
    }
    const QStringList tags = i.data(NoteListModel::TagsRole).toStringList();
    const int actionW = 36;
    if (!tags.isEmpty() && right - mx - actionW > 24) ui::paintTagChips(p, QRect(mx, r.top() + 55, right - mx - actionW, 18), tags, -1, sel);

    if (hover || sel) {
        const QRect a = actionRect(r);
        const auto *view = qobject_cast<const QAbstractItemView *>(o.widget);
        const QPoint cur = view ? view->viewport()->mapFromGlobal(QCursor::pos()) : QPoint(-1, -1);
        const bool inv = hover && a.contains(cur);
        if (inv) p->fillRect(a, t.text);
        p->drawPixmap(a.left() + 4, a.top() + 4, ui::icon("popout", inv ? t.bg : t.text, 16).pixmap(QSize(16, 16), p->device()->devicePixelRatioF()));
    }
    if ((o.state & QStyle::State_HasFocus) && sel) ui::paintFocusRing(p, QRect(r.left(), r.top(), r.width(), r.height() - 1));
    p->restore();
}

bool NoteDelegate::editorEvent(QEvent *e, QAbstractItemModel *, const QStyleOptionViewItem &o, const QModelIndex &i) {
    if (e->type() == QEvent::MouseButtonRelease) {
        auto *me = static_cast<QMouseEvent *>(e);
        if (me->button() == Qt::LeftButton && actionRect(o.rect).contains(me->position().toPoint())) {
            emit openInWindow(i.data(NoteListModel::RelRole).toString());
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------- RailModel
QVariant RailModel::data(const QModelIndex &i, int role) const {
    if (!i.isValid() || i.row() >= m_items.size()) return {};
    const RailItem &it = m_items[i.row()];
    if (role == Qt::DisplayRole || role == Qt::AccessibleTextRole) return it.label;
    if (role == Qt::UserRole) return it.key;
    return {};
}
Qt::ItemFlags RailModel::flags(const QModelIndex &i) const {
    if (!i.isValid() || m_items[i.row()].kind == RailItem::Header) return Qt::NoItemFlags;
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
}
int RailModel::rowOf(RailItem::Kind k, const QString &key) const {
    for (int i = 0; i < m_items.size(); ++i) if (m_items[i].kind == k && m_items[i].key == key) return i;
    return -1;
}

// ---------------------------------------------------------------- RailDelegate
QSize RailDelegate::sizeHint(const QStyleOptionViewItem &, const QModelIndex &i) const {
    const auto *m = qobject_cast<const RailModel *>(i.model());
    return {0, m && m->item(i.row()).kind == RailItem::Header ? 40 : 32};
}

void RailDelegate::paint(QPainter *p, const QStyleOptionViewItem &o, const QModelIndex &i) const {
    const auto &t = ui::theme();
    const auto *m = qobject_cast<const RailModel *>(i.model());
    const RailItem &it = m->item(i.row());
    const QRect r = o.rect;
    p->save();
    p->fillRect(r, t.bg);
    if (it.kind == RailItem::Header) {
        p->setFont(ui::labelFont(10));
        p->setPen(t.muted);
        p->drawText(QRect(r.left() + 16, r.top() + 8, r.width() - 32, r.height() - 8), Qt::AlignVCenter | Qt::AlignLeft, it.label);
        p->restore();
        return;
    }
    const bool sel = o.state & QStyle::State_Selected, hover = o.state & QStyle::State_MouseOver;
    if (sel) { p->fillRect(r, t.selection); p->fillRect(QRect(r.left(), r.top(), 3, r.height()), t.accent); }
    else if (hover) p->fillRect(r, t.surface);
    int x = r.left() + 16 + it.depth * 12;
    const QString ic = it.kind == RailItem::Folder ? "folder" : it.kind == RailItem::Tag ? "tag" : "search";
    if (it.kind != RailItem::All) p->drawPixmap(x, r.top() + 8, ui::icon(ic, sel ? t.text : t.muted, 16).pixmap(QSize(16, 16), p->device()->devicePixelRatioF()));
    else p->fillRect(QRect(x + 3, r.top() + 11, 10, 10), sel ? t.accent : t.muted);
    x += 26;
    p->setFont(ui::uiFont(13, sel ? QFont::Bold : QFont::DemiBold));
    p->setPen(t.text);
    int right = r.right() - 16;
    if (it.count >= 0) {
        const QString c = QString::number(it.count);
        p->setFont(ui::uiFont(12));
        p->setPen(t.muted);
        const int w = p->fontMetrics().horizontalAdvance(c);
        p->drawText(QRect(right - w, r.top(), w, r.height()), Qt::AlignVCenter | Qt::AlignRight, c);
        right -= w + 8;
        p->setFont(ui::uiFont(13, sel ? QFont::Bold : QFont::DemiBold));
        p->setPen(t.text);
    }
    p->drawText(QRect(x, r.top(), right - x, r.height()), Qt::AlignVCenter | Qt::AlignLeft, p->fontMetrics().elidedText(it.label, Qt::ElideRight, right - x));
    if ((o.state & QStyle::State_HasFocus) && sel) ui::paintFocusRing(p, r);
    p->restore();
}

} // namespace hn::app
