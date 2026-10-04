#include "market_model.h"
#include "consent_dialog.h"
#include "ui_common.h"
#include <QAbstractItemView>
#include <QPainter>
#include <QSet>
#include <algorithm>

using namespace hn::plugins;

namespace hn::app {

namespace {
bool dangerousEntry(const MarketEntry &e) {
    if (e.native()) return true;
    return std::any_of(e.permissions.cbegin(), e.permissions.cend(), [](const QString &p) { return isDangerous(p); });
}
}  // namespace

QVariant MarketModel::data(const QModelIndex &i, int role) const {
    if (!i.isValid() || i.row() < 0 || i.row() >= m_rows.size()) return {};
    const MarketEntry &e = m_all.at(m_rows.at(i.row()));
    switch (role) {
    case Qt::DisplayRole: return e.name;
    case EntryIdRole: return e.id;
    case StateRole: return int(marketState(e, m_installed.value(e.id)));
    }
    return {};
}

void MarketModel::setEntries(QList<MarketEntry> e, QHash<QString, QString> installedVersions) {
    beginResetModel();
    m_all = std::move(e);
    m_installed = std::move(installedVersions);
    m_rows.clear();   // refilter() below fills it; no second reset signal
    endResetModel();
    refilter();
}

void MarketModel::setFilter(const QString &text, const QString &tag, bool hideDangerous) {
    m_text = text.trimmed();
    m_tag = tag;
    m_hideDangerous = hideDangerous;
    refilter();
}

void MarketModel::refilter() {
    beginResetModel();
    m_rows.clear();
    for (int i = 0; i < m_all.size(); ++i) {
        const MarketEntry &e = m_all.at(i);
        if (!m_tag.isEmpty() && !e.tags.contains(m_tag)) continue;
        if (m_hideDangerous && dangerousEntry(e)) continue;
        if (!m_text.isEmpty()) {
            const bool hit = e.name.contains(m_text, Qt::CaseInsensitive) || e.description.contains(m_text, Qt::CaseInsensitive)
                             || e.author.contains(m_text, Qt::CaseInsensitive)
                             || std::any_of(e.tags.cbegin(), e.tags.cend(), [&](const QString &t) { return t.contains(m_text, Qt::CaseInsensitive); });
            if (!hit) continue;
        }
        m_rows << i;
    }
    endResetModel();
}

const MarketEntry *MarketModel::entryAt(int row) const {
    return row >= 0 && row < m_rows.size() ? &m_all.at(m_rows.at(row)) : nullptr;
}

MarketState MarketModel::stateAt(int row) const {
    const MarketEntry *e = entryAt(row);
    return e ? marketState(*e, m_installed.value(e->id)) : MarketState::NotInstalled;
}

QStringList MarketModel::allTags() const {
    QSet<QString> tags;
    for (const auto &e : m_all) for (const QString &t : e.tags) tags.insert(t);
    QStringList out(tags.cbegin(), tags.cend());
    out.sort(Qt::CaseInsensitive);
    return out;
}

// ---- delegate ----

namespace {
constexpr int kGap = 6;
constexpr int kMargin = MarketDelegate::kMargin;

QFont nameFont() { return ui::uiFont(ui::theme().baseSize, QFont::Bold); }
QFont bodyFont() { return ui::uiFont(qMax(11, ui::theme().baseSize - 2)); }
QFont chipFont() { return ui::labelFont(qMax(10, ui::theme().baseSize - 3)); }

int chipWidth(const MarketDelegate::Chip &c) {
    const QFontMetrics fm(chipFont());
    return fm.horizontalAdvance(c.text) + 16 + (c.dangerous ? fm.height() + 4 : 0);
}
int chipHeight() { return QFontMetrics(chipFont()).height() + 8; }

// Tags then permissions, wrapped to the width; returns the line count (at most kMaxChipLines, the rest is in the detail strip).
constexpr int kMaxChipLines = 3;
struct Placed { QRect r; MarketDelegate::Chip chip; bool tag; };
QList<Placed> layoutChips(const MarketEntry &e, int width, int *lines) {
    QList<Placed> out;
    QList<QPair<MarketDelegate::Chip, bool>> all;
    for (const QString &t : e.tags) all.append({{t.toUpper(), false}, true});
    for (const auto &c : MarketDelegate::chipsFor(e)) all.append({c, false});
    int x = 0, line = 0;
    const int h = chipHeight();
    for (const auto &[chip, isTag] : std::as_const(all)) {
        const int w = qMin(chipWidth(chip), qMax(width, 1));
        if (x > 0 && x + w > width) { x = 0; ++line; }
        if (line >= kMaxChipLines) break;
        out.append({QRect(x, line * (h + 4), w, h), chip, isTag});
        x += w + 6;
    }
    if (lines) *lines = all.isEmpty() ? 0 : out.isEmpty() ? 0 : out.last().r.y() / (h + 4) + 1;
    return out;
}
}  // namespace

QList<MarketDelegate::Chip> MarketDelegate::chipsFor(const MarketEntry &e) {
    QList<Chip> out;
    if (e.native()) out.append({QObject::tr("NATIVE"), true});
    for (const QString &p : e.permissions) out.append({p, isDangerous(p)});
    return out;
}

QString MarketDelegate::stateText(MarketState s) {
    switch (s) {
    case MarketState::Installed: return QObject::tr("INSTALLED");
    case MarketState::UpdateAvailable: return QObject::tr("UPDATE AVAILABLE");
    case MarketState::InstalledNewer: return QObject::tr("NEWER INSTALLED");
    case MarketState::NotInstalled: break;
    }
    return {};
}

MarketDelegate::Geometry MarketDelegate::geometry(const MarketEntry &e, MarketState st, int rowWidth) {
    Geometry g;
    const int w = qMax(1, contentWidth(rowWidth));
    const int x = kMargin + 4;
    const int nameH = QFontMetrics(nameFont()).height(), lineH = QFontMetrics(bodyFont()).height();
    const QString state = stateText(st);
    const int stateW = state.isEmpty() ? 0 : QFontMetrics(bodyFont()).horizontalAdvance(state) + kGap;
    g.head = QRect(x, kMargin, qMax(1, w - stateW), nameH);
    if (stateW) g.state = QRect(x + w - stateW, kMargin, stateW, nameH);
    const int chipsTop = kMargin + nameH + kGap + 2 * lineH + kGap;
    int lines = 0;
    for (const Placed &c : layoutChips(e, w, &lines)) {
        g.chips << c.r.translated(x, chipsTop);
        g.chipData << c.chip;
    }
    g.height = chipsTop + (lines ? lines * (chipHeight() + 4) : 0) + kMargin;
    return g;
}

QSize MarketDelegate::sizeHint(const QStyleOptionViewItem &o, const QModelIndex &i) const {
    // Name line + two description lines + the wrapped chips, all from font metrics so the row grows with the text size.
    // QListView hands over an empty rect when it lays rows out, so take the width from the view's viewport.
    const auto *view = qobject_cast<const QAbstractItemView *>(o.widget);
    const int width = o.rect.width() > 0 ? o.rect.width() : view ? view->viewport()->width() : 320;
    const auto *model = qobject_cast<const MarketModel *>(i.model());
    const MarketEntry *e = model ? model->entryAt(i.row()) : nullptr;
    return {width, e ? geometry(*e, model->stateAt(i.row()), width).height : 0};
}

void MarketDelegate::paint(QPainter *p, const QStyleOptionViewItem &o, const QModelIndex &i) const {
    const auto *model = qobject_cast<const MarketModel *>(i.model());
    const MarketEntry *e = model ? model->entryAt(i.row()) : nullptr;
    if (!e) return;
    const auto &t = ui::theme();
    p->save();
    const bool sel = o.state & QStyle::State_Selected;
    p->fillRect(o.rect, sel ? t.selection : t.bg);
    p->fillRect(QRect(o.rect.left(), o.rect.bottom(), o.rect.width(), 1), t.border);
    if (sel) p->fillRect(QRect(o.rect.left(), o.rect.top(), 3, o.rect.height()), t.accent);
    const MarketState st = model->stateAt(i.row());
    const Geometry g = geometry(*e, st, o.rect.width());
    const QRect in = o.rect.adjusted(kMargin + 4, kMargin, -kMargin, -kMargin);
    int y = in.top();

    // line 1: name, version and author on the left, state on the right
    p->setFont(nameFont());
    const int nameH = p->fontMetrics().height();
    const QString head = QObject::tr("%1  v%2 by %3").arg(e->name, e->version, e->author);
    p->setPen(t.text);
    const QRect headR = g.head.translated(o.rect.topLeft());
    p->drawText(headR, Qt::AlignVCenter, p->fontMetrics().elidedText(head, Qt::ElideRight, headR.width()));
    if (!g.state.isNull()) {
        p->setFont(bodyFont());
        p->setPen(st == MarketState::UpdateAvailable ? t.accent : st == MarketState::Installed ? t.success : t.muted);
        p->drawText(g.state.translated(o.rect.topLeft()), Qt::AlignVCenter | Qt::AlignRight, stateText(st));
    }
    y += nameH + kGap;

    // lines 2-3: description, at most two lines
    p->setFont(bodyFont());
    p->setPen(t.muted);
    const int lineH = p->fontMetrics().height();
    // first line: as many whole words as fit; second line: the rest, elided
    const QString text = e->description.simplified();
    const QFontMetrics fm = p->fontMetrics();
    int cut = text.size();
    if (fm.horizontalAdvance(text) > in.width()) {
        cut = text.lastIndexOf(' ', text.size());
        while (cut > 0 && fm.horizontalAdvance(text.left(cut)) > in.width()) cut = text.lastIndexOf(' ', cut - 1);
        if (cut <= 0) cut = qMax(1, fm.elidedText(text, Qt::ElideRight, in.width()).size() - 1);   // one very long word
    }
    QStringList lines{text.left(cut)};
    if (cut < text.size()) lines << fm.elidedText(text.mid(cut).trimmed(), Qt::ElideRight, in.width());
    for (int l = 0; l < lines.size(); ++l)
        p->drawText(QRect(in.left(), y + l * lineH, in.width(), lineH), Qt::AlignVCenter, p->fontMetrics().elidedText(lines[l], Qt::ElideRight, in.width()));
    y += 2 * lineH + kGap;

    // chips
    p->setFont(chipFont());
    const int ih = QFontMetrics(chipFont()).height();
    for (int c = 0; c < g.chips.size(); ++c) {
        const QRect r = g.chips[c].translated(o.rect.topLeft());
        const Chip &chip = g.chipData[c];
        const QColor col = chip.dangerous ? t.danger : t.muted;
        p->setPen(QPen(col, 1));
        p->drawRect(r.adjusted(0, 0, -1, -1));
        QRect text = r;
        if (chip.dangerous) {
            p->drawPixmap(r.left() + 6, r.top() + (r.height() - ih) / 2, warningIcon(col, ih).pixmap(ih, ih));
            text.setLeft(r.left() + 6 + ih);
        }
        p->setPen(col);
        p->drawText(text, Qt::AlignCenter, chip.text);
    }
    p->restore();
}

} // namespace hn::app
