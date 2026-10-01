#include "ui_common.h"
#include <QEvent>
#include <QFocusEvent>
#include <QPainter>
#include <QPainterPath>
#include <QVBoxLayout>
#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>

namespace hn::app::ui {

namespace {
hn::theme::Theme g_theme;
hn::theme::AnimationPolicy g_anim;
}

const hn::theme::Theme &theme() { return g_theme; }
void setTheme(const hn::theme::Theme &t) { g_theme = t; }
hn::theme::AnimationPolicy &animation() { return g_anim; }

QIcon appIcon() {
    static QIcon ic;
    if (!ic.isNull()) return ic;
    // Same geometry as packaging/icons/hyprnotes.svg, snapped to whole pixels so it stays crisp at tray sizes.
    for (int px : {16, 20, 22, 24, 32, 48, 64, 128, 256}) {
        QPixmap pm(px, px);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing, false);
        const auto snap = [px](qreal units) { return qRound(units * px / 16.0); };
        const int bar = qMax(2, snap(2));
        p.fillRect(QRect(0, 0, px, px), QColor("#D92E18"));
        p.fillRect(QRect(snap(4), snap(4), snap(8), bar), Qt::white);
        p.fillRect(QRect(snap(4), snap(7), snap(8), bar), Qt::white);
        p.fillRect(QRect(snap(4), snap(10), snap(5), bar), Qt::white);
        ic.addPixmap(pm);
    }
    return ic;
}

QIcon icon(const QString &name, const QColor &tint, int px) {
    return hn::theme::icon(name, tint.isValid() ? tint : g_theme.text, px);
}

QFont uiFont(int px, int weight) {
    QFont f(g_theme.fontFamily.isEmpty() ? QStringLiteral("sans-serif") : g_theme.fontFamily);
    f.setPixelSize(px);
    f.setWeight(QFont::Weight(weight));
    return f;
}

QFont labelFont(int px) {
    QFont f = hn::theme::labelFont(g_theme);
    if (px > 0) f.setPixelSize(px);
    return f;
}

QColor noteColor(int index) { return g_theme.noteAccent[qBound(0, index, 5)]; }

QString relativeTime(const QDateTime &t) {
    const qint64 s = t.secsTo(QDateTime::currentDateTime());
    if (s < 60) return QObject::tr("now");
    if (s < 3600) return QObject::tr("%1m").arg(s / 60);
    if (s < 86400) return QObject::tr("%1h").arg(s / 3600);
    if (s < 7 * 86400) return t.toString("ddd");
    if (t.date().year() == QDate::currentDate().year()) return t.toString("d MMM");
    return t.toString("d MMM yy");
}

const QList<QPair<QString, QString>> &actionNames() {
    static const QList<QPair<QString, QString>> n{
        {"new-note", "New note"}, {"toggle-organizer", "Show or hide organizer"}, {"search", "Search"}, {"toggle-source", "Toggle source mode"},
        {"command-palette", "Command palette"},
        {"bold", "Bold"}, {"italic", "Italic"}, {"strike", "Strikethrough"}, {"code", "Inline code"}, {"link", "Link"}, {"h1", "Heading"},
        {"list-ul", "Bulleted list"}, {"list-ol", "Numbered list"}, {"check", "Checklist"}, {"quote", "Quote"}};
    return n;
}

QKeySequence defaultKey(const QString &id) { return id == "command-palette" ? QKeySequence("Ctrl+Shift+P") : QKeySequence(); }

QVector<QRect> paintTagChips(QPainter *p, const QRect &r, const QStringList &tags, int hover, bool onSelection) {
    QVector<QRect> hits;
    const auto &t = theme();
    const QFont f = uiFont(11, QFont::DemiBold);
    const QFontMetrics fm(f);
    p->save();
    p->setFont(f);
    int x = r.left();
    for (int i = 0; i < tags.size(); ++i) {
        const QString txt = "#" + tags[i];
        const int w = fm.horizontalAdvance(txt) + 12;
        const QString more = QObject::tr("+%1").arg(tags.size() - i);
        const int moreW = fm.horizontalAdvance(more) + 12;
        if (x + w > r.right() + 1) {
            if (x + moreW <= r.right() + 1) {
                const QRect c(x, r.top(), moreW, r.height());
                p->setPen(QPen(t.border, 1)); p->drawRect(c.adjusted(0, 0, -1, -1));
                p->setPen(t.muted); p->drawText(c, Qt::AlignCenter, more);
            }
            break;
        }
        const QRect c(x, r.top(), w, r.height());
        if (i == hover) p->fillRect(c, t.text);
        p->setPen(QPen(i == hover ? t.text : (onSelection ? t.muted : t.border), 1));
        p->drawRect(c.adjusted(0, 0, -1, -1));
        p->setPen(i == hover ? t.bg : t.text);
        p->drawText(c, Qt::AlignCenter, txt);
        hits << c;
        x += w + 4;
    }
    p->restore();
    return hits;
}

void paintFocusRing(QPainter *p, const QRect &r) {
    p->save();
    p->setPen(QPen(theme().accent, 2));
    p->setBrush(Qt::NoBrush);
    p->drawRect(r.adjusted(1, 1, -1, -1));
    p->restore();
}

QString accentButtonStyle() {
    const auto &t = g_theme;
    return QString("QPushButton { background: %1; color: %2; border: none; border-radius: 0; padding: 0 16px; min-height: 40px; font-weight: 700; text-align: left; }"
                   "QPushButton:hover { background: %3; color: %4; }"
                   "QPushButton:pressed { background: %3; color: %4; }"
                   "QPushButton:focus { border: 2px solid %3; padding: 0 14px; }")
        .arg(t.accent.name(), t.accentText.name(), t.text.name(), t.bg.name());
}

// ---------------------------------------------------------------- IconButton
IconButton::IconButton(const QString &iconName, const QString &tip, QWidget *parent, int size)
    : QAbstractButton(parent), m_icon(iconName), m_size(size, size) {
    setToolTip(tip);
    setAccessibleName(tip);
    setFixedSize(m_size);
    setFocusPolicy(Qt::TabFocus);
    setCursor(Qt::PointingHandCursor);
    setAttribute(Qt::WA_Hover);
}

void IconButton::focusInEvent(QFocusEvent *e) {
    m_kbd = e->reason() == Qt::TabFocusReason || e->reason() == Qt::BacktabFocusReason;   // ring for keyboard users only
    QAbstractButton::focusInEvent(e);
}

void IconButton::paintEvent(QPaintEvent *) {
    QPainter p(this);
    const auto &t = theme();
    QColor fg = t.text;
    if (!isEnabled()) fg = t.muted;
    if (isDown()) { p.fillRect(rect(), t.accent); fg = t.accentText; }
    else if (underMouse() && isEnabled()) p.fillRect(rect(), t.selection);
    else if (isChecked()) { p.fillRect(rect(), t.selection); p.fillRect(QRect(0, height() - 2, width(), 2), t.accent); }   // toggled on
    const QIcon ic = ui::icon(m_icon, fg, 16);
    const QPixmap pm = ic.pixmap(QSize(16, 16), devicePixelRatioF());
    p.drawPixmap((width() - 16) / 2, (height() - 16) / 2, pm);
    if (hasFocus() && m_kbd) {
        p.setPen(QPen(t.accent, 2));
        p.drawRect(rect().adjusted(1, 1, -1, -1));
    }
}

// ---------------------------------------------------------------- ChipButton
QSize ChipButton::sizeHint() const {
    QFontMetrics fm(labelFont(10));
    return {fm.horizontalAdvance(text()) + (m_icon.isEmpty() ? 16 : 34), 20};
}

void ChipButton::paintEvent(QPaintEvent *) {
    QPainter p(this);
    const auto &t = theme();
    const QColor fg = m_filled ? t.accentText : t.text;
    if (m_filled) p.fillRect(rect(), t.accent);
    else {
        if (underMouse()) p.fillRect(rect(), t.selection);
        p.setPen(QPen(t.border, 1));
        p.drawRect(rect().adjusted(0, 0, -1, -1));
    }
    int x = 8;
    if (!m_icon.isEmpty()) {
        p.drawPixmap(x - 2, (height() - 12) / 2, ui::icon(m_icon, fg, 12).pixmap(QSize(12, 12), devicePixelRatioF()));
        x += 16;
    }
    p.setPen(fg);
    p.setFont(labelFont(10));
    p.drawText(QRect(x, 0, width() - x - 6, height()), Qt::AlignVCenter | Qt::AlignLeft, text());
    if (hasFocus() && m_kbd) { p.setPen(QPen(t.accent, 2)); p.drawRect(rect().adjusted(1, 1, -1, -1)); }
}

// ---------------------------------------------------------------- ElidedLabel
void ElidedLabel::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setFont(font());
    p.setPen(m_color.isValid() ? m_color : theme().text);
    p.drawText(rect(), Qt::AlignVCenter | Qt::AlignLeft, fontMetrics().elidedText(m_text, Qt::ElideRight, width()));
}

// ---------------------------------------------------------------- FadeOverlay
FadeOverlay::FadeOverlay(QWidget *target) : QWidget(target), m_target(target) {
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);
    target->installEventFilter(this);
    hide();
    m_anim.setStartValue(255);
    m_anim.setEndValue(0);
    connect(&m_anim, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) { m_alpha = v.toInt(); update(); });
    connect(&m_anim, &QVariantAnimation::finished, this, [this] { hide(); });
}

void FadeOverlay::play() {
    const int d = animation().duration(120);
    if (d == 0 || !m_target->isVisible()) return;
    setGeometry(m_target->rect());
    m_alpha = 255;
    m_anim.stop();
    m_anim.setDuration(d);
    show();
    raise();
    m_anim.start();
}

bool FadeOverlay::eventFilter(QObject *o, QEvent *e) {
    if (o == m_target && e->type() == QEvent::Resize) setGeometry(m_target->rect());
    return false;
}

void FadeOverlay::paintEvent(QPaintEvent *) {
    QPainter p(this);
    QColor c = theme().bg;
    c.setAlpha(m_alpha);
    p.fillRect(rect(), c);
}

// ---------------------------------------------------------------- EmptyState
class EmptyState::ArtWidget : public QWidget {
public:
    explicit ArtWidget(QWidget *p) : QWidget(p) { setFixedSize(176, 112); setAttribute(Qt::WA_TransparentForMouseEvents); }
    Art art = Art::Stack;
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        const auto &t = theme();
        p.setRenderHint(QPainter::Antialiasing);
        const QPen line(t.text, 2, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin);
        const QPen soft(t.border, 2, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin);
        auto sq = [&](QRectF r, const QColor &fill, const QPen &pen) { p.setPen(pen); p.setBrush(fill); p.drawRect(r); };
        switch (art) {
        case Art::Stack:
            p.translate(8, 0);
            sq({40, 28, 96, 72}, t.bg, soft);
            sq({32, 20, 96, 72}, t.bg, soft);
            sq({24, 12, 96, 72}, t.surface, line);
            p.fillRect(QRectF(36, 28, 40, 6), t.text);
            p.fillRect(QRectF(36, 42, 64, 3), t.border);
            p.fillRect(QRectF(36, 52, 52, 3), t.border);
            p.fillRect(QRectF(36, 62, 28, 3), t.border);
            p.fillRect(QRectF(112, 72, 24, 24), t.accent);
            break;
        case Art::Search:
            p.setPen(line); p.setBrush(t.surface);
            p.drawEllipse(QRectF(40, 16, 64, 64));
            p.setPen(QPen(t.text, 6, Qt::SolidLine, Qt::SquareCap));
            p.drawLine(QPointF(96, 68), QPointF(132, 100));
            p.fillRect(QRectF(62, 38, 20, 20), t.accent);
            break;
        case Art::Note:
            sq({48, 8, 80, 96}, t.surface, line);
            p.fillRect(QRectF(48, 8, 10, 96), t.accent);
            p.fillRect(QRectF(68, 28, 44, 6), t.text);
            p.fillRect(QRectF(68, 44, 48, 3), t.border);
            p.fillRect(QRectF(68, 54, 40, 3), t.border);
            p.fillRect(QRectF(68, 64, 44, 3), t.border);
            p.fillRect(QRectF(68, 74, 20, 3), t.border);
            break;
        case Art::Stickies:
            sq({20, 24, 72, 72}, t.noteAccent[2], line);
            sq({84, 12, 72, 72}, t.surface, line);
            p.fillRect(QRectF(84, 12, 8, 72), t.noteAccent[1]);
            p.fillRect(QRectF(102, 30, 40, 5), t.text);
            p.fillRect(QRectF(102, 44, 40, 3), t.border);
            p.fillRect(QRectF(102, 54, 28, 3), t.border);
            p.setPen(Qt::NoPen); p.setBrush(t.accent);
            p.drawEllipse(QRectF(36, 40, 16, 16));
            break;
        case Art::Shapes:
            sq({24, 40, 56, 56}, t.accent, Qt::NoPen);
            p.setPen(line); p.setBrush(t.surface);
            p.drawEllipse(QRectF(72, 16, 64, 64));
            sq({104, 64, 40, 32}, t.bg, line);
            break;
        }
    }
};

EmptyState::EmptyState(QWidget *parent) : QWidget(parent) {
    setAutoFillBackground(false);
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(32, 32, 32, 32);
    lay->setSpacing(0);
    m_art = new ArtWidget(this);
    m_title = new QLabel(this);
    m_body = new QLabel(this);
    m_body->setWordWrap(true);
    m_body->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    m_body->setMaximumWidth(280);
    m_title->setAlignment(Qt::AlignHCenter);
    m_primary = new QPushButton(this);
    m_secondary = new QPushButton(this);
    m_secondary->setFlat(true);
    lay->addStretch(3);
    lay->addWidget(m_art, 0, Qt::AlignHCenter);
    lay->addSpacing(24);
    lay->addWidget(m_title, 0, Qt::AlignHCenter);
    lay->addSpacing(8);
    lay->addWidget(m_body, 0, Qt::AlignHCenter);
    lay->addSpacing(24);
    lay->addWidget(m_primary, 0, Qt::AlignHCenter);
    lay->addSpacing(8);
    lay->addWidget(m_secondary, 0, Qt::AlignHCenter);
    lay->addStretch(4);
    connect(m_primary, &QPushButton::clicked, this, &EmptyState::primaryClicked);
    connect(m_secondary, &QPushButton::clicked, this, &EmptyState::secondaryClicked);
}

void EmptyState::resizeEvent(QResizeEvent *e) {
    QWidget::resizeEvent(e);
    const int w = qBound(120, width() - 48, 280);
    m_body->setFixedWidth(w);
    m_body->setFixedHeight(qMax(0, m_body->heightForWidth(w)));
}

void EmptyState::setContent(Art art, const QString &title, const QString &body, const QString &primary, const QString &secondary) {
    m_art->art = art;
    m_art->update();
    const auto &t = theme();
    m_title->setFont(uiFont(18, QFont::Bold));
    m_title->setText(title);
    m_title->setStyleSheet(QString("color: %1; background: transparent;").arg(t.text.name()));
    m_body->setFont(uiFont(13));
    m_body->setText(body);
    QResizeEvent re(size(), size());
    resizeEvent(&re);
    m_body->setStyleSheet(QString("color: %1; background: transparent;").arg(t.muted.name()));
    m_primary->setText(primary);
    m_primary->setVisible(!primary.isEmpty());
    m_primary->setStyleSheet(accentButtonStyle() + "QPushButton { text-align: center; }");
    m_primary->setIcon(primary == QObject::tr("New note") ? icon("plus", t.accentText, 16) : QIcon());
    m_primary->setFont(uiFont(13, QFont::Bold));
    m_primary->setMinimumWidth(176);
    m_primary->setCursor(Qt::PointingHandCursor);
    m_secondary->setText(secondary);
    m_secondary->setVisible(!secondary.isEmpty());
    m_secondary->setCursor(Qt::PointingHandCursor);
}

} // namespace hn::app::ui

namespace hn::app::ui {

// ---------------------------------------------------------------- SwatchPopover
namespace { constexpr int kSw = 24, kSwGap = 8, kSwPad = 16; }

SwatchPopover::SwatchPopover(int current, QWidget *parent)
    : QWidget(parent, Qt::Popup | Qt::FramelessWindowHint), m_current(qBound(0, current, 5)), m_focus(qBound(0, current, 5)) {
    setAttribute(Qt::WA_DeleteOnClose);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setFixedSize(2 * kSwPad + 6 * kSw + 5 * kSwGap, 80);
    setAccessibleName(tr("Note colour"));
}

QString SwatchPopover::colorName(int i) {
    static const char *n[6] = {QT_TR_NOOP("Red"), QT_TR_NOOP("Blue"), QT_TR_NOOP("Yellow"), QT_TR_NOOP("Green"), QT_TR_NOOP("Black"), QT_TR_NOOP("White")};
    return QObject::tr(n[qBound(0, i, 5)]);
}

QRect SwatchPopover::swatch(int i) const { return QRect(kSwPad + i * (kSw + kSwGap), 40, kSw, kSw); }

int SwatchPopover::at(const QPoint &p) const {
    for (int i = 0; i < 6; ++i) if (swatch(i).adjusted(-4, -4, 4, 4).contains(p)) return i;
    return -1;
}

void SwatchPopover::popup(const QPoint &g) {
    move(g);
    show();
    setFocus();
}

void SwatchPopover::paintEvent(QPaintEvent *) {
    QPainter p(this);
    const auto &t = theme();
    p.fillRect(rect(), t.surface);
    p.setPen(t.border);
    p.drawRect(rect().adjusted(0, 0, -1, -1));
    p.setFont(labelFont(10));
    p.setPen(t.muted);
    p.drawText(QRect(kSwPad, 12, width() - 2 * kSwPad, 16), Qt::AlignVCenter | Qt::AlignLeft, tr("NOTE COLOUR"));
    const int shown = m_hover >= 0 ? m_hover : m_focus;
    p.setPen(t.text);
    p.setFont(uiFont(11, QFont::DemiBold));
    p.drawText(QRect(kSwPad, 12, width() - 2 * kSwPad, 16), Qt::AlignVCenter | Qt::AlignRight, colorName(shown));
    for (int i = 0; i < 6; ++i) {
        const QRect r = swatch(i);
        p.fillRect(r, noteColor(i));
        p.setPen(QPen(t.border, 1));
        p.drawRect(r.adjusted(0, 0, -1, -1));
        if (i == m_current) {   // current colour: solid ring
            p.setPen(QPen(t.text, 2));
            p.drawRect(r.adjusted(-3, -3, 2, 2));
        } else if (i == m_hover) {
            p.setPen(QPen(t.muted, 1));
            p.drawRect(r.adjusted(-3, -3, 2, 2));
        }
        if (i == m_focus && hasFocus()) {   // keyboard cursor: accent underline
            p.fillRect(QRect(r.left(), r.bottom() + 5, r.width(), 2), t.accent);
        }
    }
}

void SwatchPopover::mouseMoveEvent(QMouseEvent *e) {
    const int h = at(e->position().toPoint());
    if (h != m_hover) { m_hover = h; update(); }
}

void SwatchPopover::mousePressEvent(QMouseEvent *e) {
    const int i = at(e->position().toPoint());
    if (i < 0) { QWidget::mousePressEvent(e); return; }
    emit picked(i);
    close();
}

void SwatchPopover::keyPressEvent(QKeyEvent *e) {
    switch (e->key()) {
    case Qt::Key_Left: m_focus = (m_focus + 5) % 6; m_hover = -1; update(); return;
    case Qt::Key_Right: m_focus = (m_focus + 1) % 6; m_hover = -1; update(); return;
    case Qt::Key_Return: case Qt::Key_Enter: case Qt::Key_Space: emit picked(m_focus); close(); return;
    case Qt::Key_Escape: close(); return;
    default:
        if (e->key() >= Qt::Key_1 && e->key() <= Qt::Key_6) { emit picked(e->key() - Qt::Key_1); close(); return; }
    }
    QWidget::keyPressEvent(e);
}

// ---------------------------------------------------------------- ShortcutSheet
namespace {
QStringList keyParts(const QKeySequence &ks) {
    QStringList out;
    const QString s = ks.toString(QKeySequence::NativeText);
    if (s.isEmpty()) return out;
    for (QString part : s.split('+')) if (!part.isEmpty()) out << part;
    if (s.endsWith("++")) out << "+";
    return out;
}
constexpr int kSheetRow = 28, kSheetHead = 40;
}

ShortcutSheet::ShortcutSheet(QWidget *window, const QList<Row> &rows) : QWidget(window), m_rows(rows) {
    setObjectName("hnSheet");
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setGeometry(window->rect());
    window->installEventFilter(this);
    m_prevFocus = QApplication::focusWidget();
    raise();
    show();
    setFocus();
}

bool ShortcutSheet::isOpen(QWidget *window) { return window && window->findChild<ShortcutSheet *>("hnSheet", Qt::FindDirectChildrenOnly); }

void ShortcutSheet::toggle(QWidget *window, const QMap<QString, QKeySequence> &kb, const QList<QPair<QString, QKeySequence>> &extra) {
    if (auto *open = window->findChild<ShortcutSheet *>("hnSheet", Qt::FindDirectChildrenOnly)) { open->close(); return; }
    QList<Row> rows;
    const QString notes = QObject::tr("NOTES"), fmt = QObject::tr("FORMAT"), gen = QObject::tr("GENERAL"), md = QObject::tr("MARKDOWN TYPING");
    static const QStringList noteIds{"new-note", "toggle-organizer", "search", "toggle-source", "command-palette"};
    for (const auto &a : actionNames()) {
        const QKeySequence ks = kb.value(a.first);
        if (ks.isEmpty()) continue;
        rows.append({noteIds.contains(a.first) ? notes : fmt, QObject::tr(a.second.toUtf8().constData()), keyParts(ks)});
    }
    for (const auto &e : extra) rows.append({QObject::tr("PLUGINS"), e.first, keyParts(e.second)});
    rows.append({gen, QObject::tr("Show this sheet"), {"F1"}});
    rows.append({gen, QObject::tr("Rename note (organizer)"), {"F2"}});
    rows.append({gen, QObject::tr("Close sheet, hide toolbar"), {"Esc"}});
    rows.append({md, QObject::tr("Heading"), {"#", "Space"}});
    rows.append({md, QObject::tr("Bulleted list"), {"-", "Space"}});
    rows.append({md, QObject::tr("Task"), {"[ ]", "Space"}});
    rows.append({md, QObject::tr("Quote"), {">", "Space"}});
    new ShortcutSheet(window, rows);
}

void ShortcutSheet::close() {
    if (parentWidget()) parentWidget()->removeEventFilter(this);
    if (m_prevFocus) m_prevFocus->setFocus();
    hide();
    deleteLater();
}

bool ShortcutSheet::event(QEvent *e) {
    if (e->type() == QEvent::ShortcutOverride) { e->accept(); return true; }   // window shortcuts must not act on the note behind
    return QWidget::event(e);
}

bool ShortcutSheet::eventFilter(QObject *o, QEvent *e) {
    if (o == parentWidget() && e->type() == QEvent::Resize) setGeometry(parentWidget()->rect());
    return false;
}

void ShortcutSheet::keyPressEvent(QKeyEvent *e) {
    switch (e->key()) {
    case Qt::Key_Down: m_scroll += kSheetRow; update(); return;
    case Qt::Key_Up: m_scroll -= kSheetRow; update(); return;
    case Qt::Key_PageDown: m_scroll += height() / 2; update(); return;
    case Qt::Key_PageUp: m_scroll -= height() / 2; update(); return;
    case Qt::Key_Shift: case Qt::Key_Control: case Qt::Key_Alt: case Qt::Key_Meta: return;
    default: close();
    }
}

void ShortcutSheet::wheelEvent(QWheelEvent *e) {
    m_scroll -= e->angleDelta().y() / 2;
    update();
}

void ShortcutSheet::paintEvent(QPaintEvent *) {
    QPainter p(this);
    const auto &t = theme();
    p.fillRect(rect(), t.bg);
    const int margin = 24, headH = 56;
    const bool two = width() >= 640;
    const int colW = two ? (width() - 2 * margin - 32) / 2 : width() - 2 * margin;

    // lay out: column-major, split near half height at a group boundary
    struct Item { int col, y; const Row *row; QString head; };
    QList<Item> items;
    QString group;
    int y = 0, col = 0, total = 0;
    for (const auto &r : m_rows) {
        if (r.group != group) { group = r.group; total += kSheetHead; }
        total += kSheetRow;
    }
    group.clear();
    const int half = two ? total / 2 : total + 1;
    int colTop = 0;
    for (const auto &r : m_rows) {
        const bool newGroup = r.group != group;
        if (newGroup && col == 0 && y >= half) { col = 1; y = 0; }
        if (newGroup) { group = r.group; items.append({col, y, nullptr, group}); y += kSheetHead; }
        items.append({col, y, &r, {}});
        y += kSheetRow;
        colTop = qMax(colTop, y);
    }
    m_contentH = colTop;
    const int viewH = height() - headH - margin;
    m_scroll = qBound(0, m_scroll, qMax(0, m_contentH - viewH));

    p.save();
    p.setClipRect(QRect(0, headH, width(), height() - headH));
    for (const auto &it : items) {
        const int x = margin + it.col * (colW + 32);
        const int top = headH + it.y - m_scroll;
        if (top + kSheetHead < headH || top > height()) continue;
        if (!it.row) {
            p.setFont(labelFont(10));
            p.setPen(t.muted);
            p.drawText(QRect(x, top + 8, colW, 24), Qt::AlignVCenter | Qt::AlignLeft, it.head);
            p.fillRect(QRect(x, top + 32, colW, 1), t.border);
            continue;
        }
        p.setFont(uiFont(13));
        p.setPen(t.text);
        const QRect lab(x, top, colW - 8, kSheetRow);
        p.drawText(lab, Qt::AlignVCenter | Qt::AlignLeft, p.fontMetrics().elidedText(it.row->label, Qt::ElideRight, colW / 2 + 40));
        // keycaps, right-aligned
        const QFont kf = uiFont(11, QFont::DemiBold);
        const QFontMetrics km(kf);
        int kx = x + colW;
        for (int i = it.row->keys.size() - 1; i >= 0; --i) {
            const int w = qMax(20, km.horizontalAdvance(it.row->keys[i]) + 12);
            kx -= w;
            const QRect c(kx, top + 4, w, 20);
            p.setPen(QPen(t.border, 1));
            p.setBrush(t.surface);
            p.drawRect(c.adjusted(0, 0, -1, -1));
            p.setFont(kf);
            p.setPen(t.text);
            p.drawText(c, Qt::AlignCenter, it.row->keys[i]);
            kx -= 4;
        }
    }
    p.restore();

    p.fillRect(QRect(0, 0, width(), headH), t.bg);
    p.fillRect(QRect(0, headH - 1, width(), 1), t.border);
    p.fillRect(QRect(0, 0, 4, headH), t.accent);
    p.setFont(labelFont(12));
    p.setPen(t.text);
    p.drawText(QRect(margin, 0, width() - 2 * margin, headH), Qt::AlignVCenter | Qt::AlignLeft, tr("KEYBOARD SHORTCUTS"));
    p.setFont(uiFont(11, QFont::DemiBold));
    p.setPen(t.muted);
    p.drawText(QRect(margin, 0, width() - 2 * margin, headH), Qt::AlignVCenter | Qt::AlignRight, tr("Esc to close"));
}

} // namespace hn::app::ui
