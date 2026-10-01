#pragma once
// Shared presentation helpers: theme access, icons, flat buttons, fades, empty states.
#include "hn/theme/animation.h"
#include "hn/theme/theme.h"
#include <QAbstractButton>
#include <QDateTime>
#include <QFocusEvent>
#include <QIcon>
#include <QLabel>
#include <QPushButton>
#include <QVariantAnimation>
#include <QPointer>
#include <QWidget>
#include <QKeySequence>
#include <QMap>

namespace hn::app::ui {

constexpr int kGrid = 8;

const hn::theme::Theme &theme();
void setTheme(const hn::theme::Theme &t);
hn::theme::AnimationPolicy &animation();

QIcon appIcon();
QIcon icon(const QString &name, const QColor &tint = {}, int px = 16);
QFont uiFont(int px, int weight = QFont::Normal);
QFont labelFont(int px = 0);
QColor noteColor(int index);
QString relativeTime(const QDateTime &t);
QString accentButtonStyle();   // flat vermilion primary button (QPushButton)
const QList<QPair<QString, QString>> &actionNames();   // action id -> English title, in display order
QKeySequence defaultKey(const QString &actionId);       // built-in default for actions the config defaults do not know (command-palette)

// Hairline-bordered "#tag" chips painted left to right inside `r` (height = r.height()); returns one hit rect per drawn
// chip, in order. When the row is too narrow the rest collapse into a "+N" chip (no hit rect). `hover` = chip index or -1.
QVector<QRect> paintTagChips(QPainter *p, const QRect &r, const QStringList &tags, int hover = -1, bool onSelection = false);
// 2px accent keyboard focus ring drawn inside `r`.
void paintFocusRing(QPainter *p, const QRect &r);

// Flat icon button: no per-state widgets, painted directly; hover/press/checked/focus states.
class IconButton : public QAbstractButton {
    Q_OBJECT
public:
    explicit IconButton(const QString &iconName, const QString &tip, QWidget *parent = nullptr, int size = 28);
    void setIconName(const QString &n) { m_icon = n; update(); }
    QSize sizeHint() const override { return m_size; }
protected:
    void paintEvent(QPaintEvent *) override;
    void enterEvent(QEnterEvent *) override { update(); }
    void leaveEvent(QEvent *) override { update(); }
    void focusInEvent(QFocusEvent *e) override;
    void focusOutEvent(QFocusEvent *e) override { m_kbd = false; QAbstractButton::focusOutEvent(e); }
private:
    QString m_icon;
    QSize m_size;
    bool m_kbd = false;
};

// Text on a hairline-bordered flat chip (workspace state etc.).
class ChipButton : public QAbstractButton {
    Q_OBJECT
public:
    explicit ChipButton(QWidget *parent = nullptr) : QAbstractButton(parent) { setCursor(Qt::PointingHandCursor); setFocusPolicy(Qt::TabFocus); }
    void setFilled(bool f) { m_filled = f; update(); }
    void setIconName(const QString &n) { m_icon = n; update(); }
    QSize sizeHint() const override;
protected:
    void paintEvent(QPaintEvent *) override;
    void focusInEvent(QFocusEvent *e) override { m_kbd = e->reason() == Qt::TabFocusReason || e->reason() == Qt::BacktabFocusReason; QAbstractButton::focusInEvent(e); }
    void focusOutEvent(QFocusEvent *e) override { m_kbd = false; QAbstractButton::focusOutEvent(e); }
private:
    QString m_icon;
    bool m_filled = false, m_kbd = false;
};

class ElidedLabel : public QWidget {
    Q_OBJECT
public:
    explicit ElidedLabel(QWidget *parent = nullptr) : QWidget(parent) { setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); }
    void setText(const QString &t) { m_text = t; setToolTip(t); update(); }
    QString text() const { return m_text; }
    void setColor(const QColor &c) { m_color = c; update(); }
    void setTextFont(const QFont &f) { setFont(f); updateGeometry(); update(); }
    QSize sizeHint() const override { return {fontMetrics().horizontalAdvance(m_text), fontMetrics().height() + 4}; }
    QSize minimumSizeHint() const override { return {0, fontMetrics().height() + 4}; }
protected:
    void paintEvent(QPaintEvent *) override;
private:
    QString m_text;
    QColor m_color;
};

// Interaction-only crossfade: an opaque-colour overlay whose alpha falls to 0, then hides itself.
// No-op under reduced motion. Never animates document content.
class FadeOverlay : public QWidget {
    Q_OBJECT
public:
    explicit FadeOverlay(QWidget *target);
    void play();
    bool running() const { return m_anim.state() == QAbstractAnimation::Running; }
protected:
    void paintEvent(QPaintEvent *) override;
    bool eventFilter(QObject *o, QEvent *e) override;
private:
    QVariantAnimation m_anim;
    QWidget *m_target;
    int m_alpha = 0;
};

// Flat note-colour picker (six square swatches). Painted, keyboard operable (arrows, Enter, 1-6), closes on pick/Esc/click-away.
class SwatchPopover : public QWidget {
    Q_OBJECT
public:
    explicit SwatchPopover(int current, QWidget *parent = nullptr);
    static QString colorName(int i);
    void popup(const QPoint &globalTopLeft);
signals:
    void picked(int index);
protected:
    void paintEvent(QPaintEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    void leaveEvent(QEvent *) override { m_hover = -1; update(); }
private:
    QRect swatch(int i) const;
    int at(const QPoint &p) const;
    int m_current, m_focus, m_hover = -1;
};

// Full-window flat keyboard cheat sheet (F1 or ? outside text fields). Painted, scrollable, Esc / click / F1 closes it.
class ShortcutSheet : public QWidget {
    Q_OBJECT
public:
    // `extra`: (label, key) rows shown under PLUGINS (plugin-defined default keys).
    static void toggle(QWidget *window, const QMap<QString, QKeySequence> &bindings, const QList<QPair<QString, QKeySequence>> &extra = {});
    static bool isOpen(QWidget *window);
    struct Row { QString group, label; QStringList keys; };
    const QList<Row> &rows() const { return m_rows; }
    static ShortcutSheet *openOn(QWidget *window) { return window ? window->findChild<ShortcutSheet *>("hnSheet", Qt::FindDirectChildrenOnly) : nullptr; }
protected:
    void paintEvent(QPaintEvent *) override;
    bool event(QEvent *e) override;
    void keyPressEvent(QKeyEvent *) override;
    void mousePressEvent(QMouseEvent *) override { close(); }
    void wheelEvent(QWheelEvent *) override;
    bool eventFilter(QObject *o, QEvent *e) override;
private:
    explicit ShortcutSheet(QWidget *window, const QList<Row> &rows);
    void close();
    QList<Row> m_rows;
    QPointer<QWidget> m_prevFocus;
    int m_scroll = 0, m_contentH = 0;
};

enum class Art { Stack, Search, Note, Stickies, Shapes };

// Designed empty state: geometric art painted from simple shapes, title, body, optional buttons.
class EmptyState : public QWidget {
    Q_OBJECT
public:
    explicit EmptyState(QWidget *parent = nullptr);
    void setContent(Art art, const QString &title, const QString &body, const QString &primary = {}, const QString &secondary = {});
protected:
    void resizeEvent(QResizeEvent *) override;
signals:
    void primaryClicked();
    void secondaryClicked();
private:
    class ArtWidget;
    ArtWidget *m_art;
    QLabel *m_title, *m_body;
    QPushButton *m_primary, *m_secondary;
};

} // namespace hn::app::ui
