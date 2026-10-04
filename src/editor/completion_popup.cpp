#include "completion_popup.h"

#include <QGuiApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QTextLayout>
#include <QWheelEvent>
#include <QtMath>

namespace hn::editor {

CompletionPopup::CompletionPopup(QWidget *owner) : QWidget(owner, Qt::ToolTip | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus)
{
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_TranslucentBackground);
    setFocusPolicy(Qt::NoFocus);
    setMouseTracking(true);
}

void CompletionPopup::setTheme(const hn::theme::Theme &t, const QFont &f)
{
    m_t = t;
    setFont(f);
    QPalette pal = palette();
    pal.setColor(QPalette::Window, t.surface);
    pal.setColor(QPalette::Text, t.text);
    pal.setColor(QPalette::Highlight, t.accent);
    setPalette(pal);
    m_rowH = qMax(28, ((QFontMetrics(f).height() + 10) + 3) / 4 * 4);   // 4 px grid
    update();
}

void CompletionPopup::setItems(const QList<CompletionItem> &items, const QString &query)
{
    m_items = items;
    m_query = query;
    m_cur = 0;
    m_top = 0;
    update();
}

int CompletionPopup::visibleRows(const QRect &avail) const
{
    const int fit = qMax(1, (avail.height() - 8 - 2) / m_rowH);
    return qMax(1, qMin(qMin(count(), kMaxRows), fit));
}

QSize CompletionPopup::wantedSize(const QRect &avail) const
{
    QFont bold = font();
    bold.setWeight(QFont::DemiBold);
    const QFontMetrics fm(bold);
    int w = 0;
    for (const auto &it : m_items) w = qMax(w, fm.horizontalAdvance(it.label) + (it.detail.isEmpty() ? 0 : 24 + fm.horizontalAdvance(it.detail)));
    w = qBound(240, w + 12 + 12 + 6, 440);
    w = qMin(w, qMax(80, avail.width() - 8));
    return QSize(w, visibleRows(avail) * m_rowH + 2);
}

QRect CompletionPopup::place(const QRect &caret, const QSize &size, const QRect &avail)
{
    int x = qMin(caret.left(), avail.right() + 1 - size.width());
    x = qMax(x, avail.left());
    const int below = caret.bottom() + 1 + 2, above = caret.top() - 2 - size.height();
    int y;
    if (below + size.height() <= avail.bottom() + 1) y = below;
    else if (above >= avail.top()) y = above;
    else y = qMax(avail.top(), qMin(below, avail.bottom() + 1 - size.height()));
    return QRect(QPoint(x, y), size);
}

void CompletionPopup::showAt(const QRect &caretGlobal)
{
    const QScreen *s = QGuiApplication::screenAt(caretGlobal.center());
    if (!s) s = QGuiApplication::primaryScreen();
    const QRect avail = s ? s->availableGeometry() : QRect(0, 0, 1024, 768);
    m_rows = visibleRows(avail);
    setGeometry(place(caretGlobal, wantedSize(avail), avail));
    ensureVisible();
    if (!isVisible()) show();
    update();
}

void CompletionPopup::ensureVisible()
{
    const int rows = qMax(1, (height() - 2) / m_rowH);
    if (m_cur < m_top) m_top = m_cur;
    if (m_cur >= m_top + rows) m_top = m_cur - rows + 1;
    m_top = qBound(0, m_top, qMax(0, count() - rows));
}

void CompletionPopup::step(int d)
{
    if (!count()) return;
    m_cur = ((m_cur + d) % count() + count()) % count();   // wraps like a menu
    ensureVisible();
    update();
}

void CompletionPopup::page(int dir)
{
    if (!count()) return;
    const int rows = qMax(1, (height() - 2) / m_rowH);
    m_cur = qBound(0, m_cur + dir * rows, count() - 1);
    ensureVisible();
    update();
}

int CompletionPopup::rowAt(const QPoint &p) const
{
    const int r = m_top + (p.y() - 1) / m_rowH;
    return p.y() >= 1 && r >= 0 && r < count() ? r : -1;
}

void CompletionPopup::mousePressEvent(QMouseEvent *e)
{
    const int r = rowAt(e->position().toPoint());
    if (r >= 0 && picked) picked(r);
    e->accept();
}

void CompletionPopup::mouseMoveEvent(QMouseEvent *e)
{
    const int r = rowAt(e->position().toPoint());
    if (r >= 0 && r != m_cur) { m_cur = r; update(); }
}

void CompletionPopup::wheelEvent(QWheelEvent *e)
{
    const int rows = qMax(1, (height() - 2) / m_rowH);
    const int d = e->angleDelta().y() > 0 ? -1 : (e->angleDelta().y() < 0 ? 1 : 0);
    m_top = qBound(0, m_top + d, qMax(0, count() - rows));
    update();
    e->accept();
}

// Characters of `label` to emphasise: first case-insensitive substring hit of the query, else an in-order subsequence.
static QList<bool> matchMask(const QString &label, const QString &q)
{
    QList<bool> m(label.size(), false);
    if (q.isEmpty()) return m;
    const int at = label.indexOf(q, 0, Qt::CaseInsensitive);
    if (at >= 0) { for (int i = 0; i < q.size(); ++i) m[at + i] = true; return m; }
    int qi = 0;
    for (int i = 0; i < label.size() && qi < q.size(); ++i)
        if (label[i].toLower() == q[qi].toLower()) { m[i] = true; ++qi; }
    if (qi < q.size()) m.fill(false);
    return m;
}

static QList<QTextLayout::FormatRange> highlights(const QString &original, const QString &label,
                                                 const QString &query, const QColor &accent)
{
    const auto mask = matchMask(original, query);
    const int retained = label.size() - (label != original && label.endsWith(QChar(0x2026)) ? 1 : 0);
    QList<QTextLayout::FormatRange> formats;
    for (int i = 0; i < retained;) {
        if (!mask.value(i)) { ++i; continue; }
        const int start = i++;
        while (i < retained && mask.value(i)) ++i;
        QTextCharFormat format;
        format.setFontWeight(QFont::DemiBold);
        format.setForeground(accent);
        formats.append({start, i - start, format});
    }
    return formats;
}

CompletionPopup::RowLayout CompletionPopup::rowLayout(int index) const
{
    RowLayout result;
    const int rows = qMax(1, (height() - 2) / m_rowH);
    if (index < m_top || index >= qMin(count(), m_top + rows)) return result;
    const QRect row(1, 1 + (index - m_top) * m_rowH, width() - 2, m_rowH);
    const auto &item = m_items[index];
    const QFontMetrics fm(font());
    QFont bold = font();
    bold.setWeight(QFont::DemiBold);
    const QFontMetrics boldMetrics(bold);
    const int right = row.right() + 1 - (count() > rows ? 8 : 4);
    const int detailWidth = item.detail.isEmpty() ? 0 : qMin(fm.horizontalAdvance(item.detail), row.width() * 9 / 20);
    result.detailRect = QRect(right - detailWidth, row.top(), detailWidth, row.height());
    result.detailText = fm.elidedText(item.detail, Qt::ElideRight, detailWidth);
    result.labelRect = QRect(row.left() + 12, row.top(), qMax(0, right - detailWidth - (detailWidth ? 12 : 0) - row.left() - 12), row.height());
    int budget = result.labelRect.width();
    while (budget > 0) {
        result.labelText = boldMetrics.elidedText(item.label, Qt::ElideRight, budget);
        // Demi-bold is the conservative default; also account for unusual fonts and mixed-run shaping.
        QTextLayout layout(result.labelText, font());
        QTextOption option;
        option.setWrapMode(QTextOption::NoWrap);
        layout.setTextOption(option);
        layout.setFormats(highlights(item.label, result.labelText, m_query, m_t.accent));
        layout.beginLayout();
        QTextLine line = layout.createLine();
        if (line.isValid()) line.setLineWidth(result.labelRect.width());
        layout.endLayout();
        const qreal measured = qMax(qreal(fm.horizontalAdvance(result.labelText)), line.isValid() ? line.naturalTextWidth() : 0);
        if (measured <= result.labelRect.width()) break;
        budget -= qMax(1, qCeil(measured - result.labelRect.width()));
    }
    if (budget <= 0) result.labelText.clear();
    return result;
}

void CompletionPopup::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);
    p.setCompositionMode(QPainter::CompositionMode_Source);
    p.fillRect(rect(), Qt::transparent);
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    const int radius = hn::theme::popupRadius(m_t);
    QPainterPath outline;
    outline.addRoundedRect(QRectF(rect()), radius, radius);
    p.fillPath(outline, m_t.surface);
    p.setPen(m_t.border);
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), qMax(0.0, radius - 0.5), qMax(0.0, radius - 0.5));
    p.save();
    p.setRenderHint(QPainter::Antialiasing, false); // solid row fills and a crisp two-pixel accent bar
    QPainterPath interior;
    interior.addRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), qMax(0, radius - 1), qMax(0, radius - 1));
    p.setClipPath(interior);
    const int rows = qMax(1, (height() - 2) / m_rowH);
    for (int r = 0; r < rows && m_top + r < count(); ++r) {
        const int i = m_top + r;
        const QRect row(1, 1 + r * m_rowH, width() - 2, m_rowH);
        const bool sel = i == m_cur;
        if (sel) {
            p.fillRect(row, m_t.dark ? m_t.bg.lighter(125) : m_t.bg.darker(104));
            p.fillRect(QRect(row.left(), row.top(), 2, row.height()), m_t.accent);
        }
        const auto text = rowLayout(i);
        p.setPen(m_t.muted);
        p.setFont(font());
        p.drawText(text.detailRect, Qt::AlignVCenter | Qt::AlignRight, text.detailText);
        p.save();
        p.setClipRect(text.labelRect, Qt::IntersectClip);
        p.setPen(m_t.text);
        QTextLayout layout(text.labelText, font());
        QTextOption option;
        option.setWrapMode(QTextOption::NoWrap);
        layout.setTextOption(option);
        layout.setFormats(highlights(m_items[i].label, text.labelText, m_query, m_t.accent));
        layout.beginLayout();
        QTextLine line = layout.createLine();
        if (line.isValid()) line.setLineWidth(text.labelRect.width());
        layout.endLayout();
        layout.draw(&p, QPointF(text.labelRect.left(), text.labelRect.top() + (text.labelRect.height() - layout.boundingRect().height()) / 2));
        p.restore();
    }
    if (count() > rows) {   // thin scroll indicator
        const int track = height() - 2, th = qMax(12, track * rows / count()), ty = 1 + (track - th) * m_top / qMax(1, count() - rows);
        p.fillRect(QRect(width() - 3, ty, 2, th), m_t.border);
    }
    p.restore();
}

} // namespace hn::editor
