#include "ui_common.h"
#include <QApplication>
#include <QKeyEvent>
#include <QPainter>
#include <QTextLayout>
#include <QWheelEvent>

namespace hn::app::ui {
namespace {
QStringList keyParts(const QKeySequence &ks) {
    QStringList out;
    const QString s = ks.toString(QKeySequence::NativeText);
    for (const QString &part : s.split('+')) if (!part.isEmpty()) out << part;
    if (s.endsWith("++")) out << "+";
    return out;
}
QFont titleFont() { return labelFont(qMax(12, theme().baseSize - 8)); }
QFont keyFont() { return uiFont(qMax(11, theme().baseSize - 8), QFont::DemiBold); }
QFont groupFont() { return labelFont(qMax(10, theme().baseSize - 8)); }
int textWidth(const QFont &font, const QString &text) {
    // QTextLine includes fractional advances and glyph overhangs that the integer
    // font metrics used by the old sheet dropped (notably Shift and Esc keycaps).
    QTextLayout layout(text, font);
    layout.beginLayout();
    auto line = layout.createLine();
    if (line.isValid()) line.setLineWidth(100000);
    const int width = line.isValid() ? qCeil(line.naturalTextWidth()) : 0;
    layout.endLayout();
    return width;
}
// One wrapping algorithm for measuring and drawing, including unbroken plugin labels.
int wrapped(const QFont &font, const QString &text, const QRect &rect, QPainter *painter = nullptr, bool center = false) {
    QTextLayout layout(text, font);
    QTextOption option; option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    option.setAlignment(center ? Qt::AlignHCenter : Qt::AlignLeft); layout.setTextOption(option);
    qreal height = 0;
    layout.beginLayout();
    while (true) {
        auto line = layout.createLine(); if (!line.isValid()) break;
        line.setLineWidth(qMax(1, rect.width())); line.setPosition({0, height}); height += line.height();
    }
    layout.endLayout();
    if (painter) layout.draw(painter, QPointF(rect.left(), rect.top() + (center ? (rect.height() - height) / 2 : 0)));
    return qCeil(height);
}
}

ShortcutSheet::ShortcutSheet(QWidget *window, const QList<Row> &rows) : QWidget(window), m_rows(rows) {
    setObjectName("hnSheet"); setAccessibleName(tr("Keyboard shortcuts. Escape or click to close."));
    QStringList description;
    for (const auto &row : rows) description << row.label + ": " + row.keys.join(" + ");
    setAccessibleDescription(description.join('\n'));
    setFocusPolicy(Qt::StrongFocus); setAttribute(Qt::WA_OpaquePaintEvent);
    setGeometry(window->rect()); window->installEventFilter(this);
    m_prevFocus = QApplication::focusWidget();
    connect(themeNotifier(), &ThemeNotifier::changed, this, [this] {
        setFont(uiFont(qMax(13, theme().baseSize - 1))); relayout(); update();
    });
    setFont(uiFont(qMax(13, theme().baseSize - 1))); relayout();
    raise(); show(); setFocus();
}
bool ShortcutSheet::isOpen(QWidget *window) { return openOn(window); }
void ShortcutSheet::toggle(QWidget *window, const QMap<QString, QKeySequence> &kb, const QList<QPair<QString, QKeySequence>> &extra) {
    if (!window) return;
    if (auto *open = openOn(window)) { open->close(); return; }
    QList<Row> rows;
    const QString notes = tr("NOTES"), fmt = tr("FORMAT"), gen = tr("GENERAL"), md = tr("MARKDOWN TYPING");
    static const QStringList noteIds{"new-note", "toggle-organizer", "search", "toggle-source", "command-palette"};
    for (const auto &a : actionNames()) {
        const QKeySequence ks = kb.value(a.first);
        if (!ks.isEmpty()) rows.append({noteIds.contains(a.first) ? notes : fmt, tr(a.second.toUtf8().constData()), keyParts(ks)});
    }
    for (const auto &e : extra) rows.append({tr("PLUGINS"), e.first, keyParts(e.second)});
    rows.append({gen, tr("Show this sheet"), {"F1"}});
    rows.append({gen, tr("Rename note (organizer)"), {"F2"}});
    rows.append({gen, tr("Close sheet, hide toolbar"), {"Esc"}});
    rows.append({md, tr("Heading"), {"#", "Space"}});
    rows.append({md, tr("Bulleted list"), {"-", "Space"}});
    rows.append({md, tr("Task"), {"[ ]", "Space"}});
    rows.append({md, tr("Quote"), {">", "Space"}});
    new ShortcutSheet(window, rows);
}
void ShortcutSheet::close() {
    if (parentWidget()) parentWidget()->removeEventFilter(this);
    hide();
    if (m_prevFocus) m_prevFocus->setFocus();
    deleteLater();
}
bool ShortcutSheet::event(QEvent *e) {
    if (e->type() == QEvent::ShortcutOverride) { e->accept(); return true; }
    if (e->type() == QEvent::KeyPress) {
        const int key = static_cast<QKeyEvent *>(e)->key();
        if (key == Qt::Key_Tab || key == Qt::Key_Backtab) { e->accept(); return true; }
    }
    const bool result = QWidget::event(e);
    if (e->type() == QEvent::FontChange || e->type() == QEvent::StyleChange) { relayout(); update(); }
    return result;
}
bool ShortcutSheet::eventFilter(QObject *o, QEvent *e) {
    if (o == parentWidget() && e->type() == QEvent::Resize) setGeometry(parentWidget()->rect());
    return false;
}
void ShortcutSheet::resizeEvent(QResizeEvent *) { relayout(); }
void ShortcutSheet::scrollTo(int offset) { m_scroll = qBound(0, offset, maximumScroll()); update(); }
void ShortcutSheet::keyPressEvent(QKeyEvent *e) {
    switch (e->key()) {
    case Qt::Key_Down: scrollTo(m_scroll + fontMetrics().height() + 8); break;
    case Qt::Key_Up: scrollTo(m_scroll - fontMetrics().height() - 8); break;
    case Qt::Key_PageDown: scrollTo(m_scroll + qMax(1, height() - m_contentTop)); break;
    case Qt::Key_PageUp: scrollTo(m_scroll - qMax(1, height() - m_contentTop)); break;
    case Qt::Key_End: scrollTo(maximumScroll()); break;
    case Qt::Key_Home: scrollTo(0); break;
    case Qt::Key_Escape: case Qt::Key_F1: close(); break;
    default: e->accept(); break;
    }
}
void ShortcutSheet::wheelEvent(QWheelEvent *e) { scrollTo(m_scroll - e->angleDelta().y() / 2); e->accept(); }

void ShortcutSheet::relayout() {
    const int margin = width() < 360 ? 16 : 24, gap = 8;
    const int available = qMax(1, width() - margin * 2);
    const QString title = tr("KEYBOARD SHORTCUTS"), hint = tr("Esc to close");
    const int titleW = textWidth(titleFont(), title), hintW = textWidth(keyFont(), hint);
    const bool stacked = titleW + hintW + 16 > available;
    const int titleH = wrapped(titleFont(), title, QRect(0, 0, stacked ? available : titleW, 0));
    const int hintH = wrapped(keyFont(), hint, QRect(0, 0, available, 0));
    m_titleRect = QRect(margin, 8, stacked ? available : titleW, titleH);
    m_closeRect = QRect(stacked ? margin : width() - margin - hintW, stacked ? titleH + 16 : 8,
                        stacked ? available : hintW, hintH);
    m_contentTop = qMax(m_titleRect.bottom(), m_closeRect.bottom()) + 9;
    const bool two = width() >= 640;
    const int colW = two ? (available - 32) / 2 : available;
    QList<RowGeometry> local;
    QList<int> heights;
    int total = 0; QString group;
    const int groupH = QFontMetrics(groupFont()).height() + 16;
    for (const auto &row : m_rows) {
        int keysW = -gap;
        QList<QSize> keys;
        for (const auto &key : row.keys) {
            const int w = qMin(colW, qMax(20, textWidth(keyFont(), key) + 12));
            keys << QSize(w, wrapped(keyFont(), key, QRect(0, 0, w - 12, 0)) + 8); keysW += w + gap;
        }
        const int labelNatural = textWidth(font(), row.label);
        const bool beside = keys.isEmpty() || labelNatural + keysW + gap <= colW;
        const int labelW = beside && !keys.isEmpty() ? colW - keysW - gap : colW;
        const int labelH = wrapped(font(), row.label, QRect(0, 0, labelW, 0));
        RowGeometry g{QRect(0, 0, labelW, labelH), {}};
        int x = beside ? colW - qMax(0, keysW) : 0, y = beside ? 0 : labelH + gap, rowH = 0;
        for (const auto &size : keys) {
            if (x && x + size.width() > colW) { x = 0; y += rowH + gap; rowH = 0; }
            g.keys << QRect(QPoint(x, y), size); x += size.width() + gap; rowH = qMax(rowH, size.height());
        }
        const int h = qMax(labelH, y + rowH) + gap;
        local << g; heights << h;
        if (row.group != group) { group = row.group; total += groupH; }
        total += h;
    }
    m_geometry.clear(); m_groups.clear(); m_contentH = 0;
    m_columnHeight[0] = m_columnHeight[1] = 0;
    int col = 0, y = 0; group.clear();
    for (int i = 0; i < m_rows.size(); ++i) {
        if (m_rows[i].group != group) {
            if (two && col == 0 && y >= total / 2) { col = 1; y = 0; }
            group = m_rows[i].group;
            m_groups << GroupGeometry{group, QRect(margin + col * (colW + 32), m_contentTop + y, colW, groupH)};
            y += groupH;
        }
        const QPoint offset(margin + col * (colW + 32), m_contentTop + y);
        auto geometry = local[i]; geometry.label.translate(offset);
        for (auto &key : geometry.keys) key.translate(offset);
        m_geometry << geometry; y += heights[i]; m_contentH = qMax(m_contentH, y); m_columnHeight[col] = y;
    }
    m_scroll = qBound(0, m_scroll, maximumScroll());
}
int ShortcutSheet::columnScroll(int x) const {
    const int col = width() >= 640 && x >= width() / 2 ? 1 : 0;
    return qMin(m_scroll, qMax(0, m_columnHeight[col] - qMax(0, height() - m_contentTop - 8)));
}
QList<ShortcutSheet::RowGeometry> ShortcutSheet::rowGeometries() const {
    auto result = m_geometry;
    for (auto &row : result) {
        const int offset = columnScroll(row.label.x());
        row.label.translate(0, -offset); for (auto &key : row.keys) key.translate(0, -offset);
    }
    return result;
}
void ShortcutSheet::paintEvent(QPaintEvent *) {
    QPainter p(this); const auto &t = theme(); p.fillRect(rect(), t.bg);
    p.save(); p.setClipRect(QRect(0, m_contentTop, width(), qMax(0, height() - m_contentTop)));
    for (const auto &group : m_groups) {
        const QRect r = group.rect.translated(0, -columnScroll(group.rect.x()));
        p.setFont(groupFont()); p.setPen(t.muted); p.drawText(r.adjusted(0, 0, 0, -8), Qt::AlignVCenter, group.text);
        p.fillRect(QRect(r.left(), r.bottom() - 4, r.width(), 1), t.border);
    }
    const auto geometry = rowGeometries();
    for (int i = 0; i < geometry.size(); ++i) {
        const auto &g = geometry[i]; p.setPen(t.text); wrapped(font(), m_rows[i].label, g.label, &p);
        for (int k = 0; k < g.keys.size(); ++k) {
            p.setPen(t.border); p.setBrush(t.surface);
            p.drawRoundedRect(g.keys[k].adjusted(0, 0, -1, -1), t.radius, t.radius);
            p.setPen(t.text); wrapped(keyFont(), m_rows[i].keys[k], g.keys[k].adjusted(6, 4, -6, -4), &p, true);
        }
    }
    p.restore();
    p.fillRect(QRect(0, 0, width(), m_contentTop), t.bg);
    p.fillRect(QRect(0, m_contentTop - 1, width(), 1), t.border);
    p.fillRect(QRect(0, 0, 4, m_contentTop), t.accent);
    p.setPen(t.text); wrapped(titleFont(), tr("KEYBOARD SHORTCUTS"), titleRect(), &p);
    p.setPen(t.muted); wrapped(keyFont(), tr("Esc to close"), closeHintRect(), &p);
}
} // namespace hn::app::ui
