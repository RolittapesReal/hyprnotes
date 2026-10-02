#pragma once
// Plugin API v2 panels. PanelHub holds the state the plugin runtime pushes (PanelBridge); PanelView paints one panel's declarative
// blocks (no per-item widgets: rows are laid out into a flat vector and painted in one pass); PanelDock is the tabbed container
// shown as the organizer's right-hand dock and, as a popup, from the sticky header. Nothing here exists until a plugin registers a
// panel: the organizer/sticky create the dock lazily.
#include "hn/plugins/types.h"
#include <QAbstractButton>
#include <QAbstractScrollArea>
#include <QEnterEvent>
#include <QPainter>
#include "ui_common.h"
#include <QPointer>
#include <QTextDocument>
#include <memory>
#include <vector>

namespace hn::app {

class AppController;
class NoteSession;
namespace ui { class IconButton; }

// "Panels" toggle: square outline with a right-hand pane, filled while the dock is open. Painted, no icon asset.
class DockToggle : public QAbstractButton {
public:
    explicit DockToggle(QWidget *p) : QAbstractButton(p) {
        setCheckable(true);
        setFixedSize(32, 32);
        setCursor(Qt::PointingHandCursor);
        setToolTip(QObject::tr("Panels (Ctrl+Shift+L)"));
        setAccessibleName(QObject::tr("Panels"));
    }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        const auto &t = ui::theme();
        if (underMouse()) p.fillRect(rect(), t.selection);
        const QRect r(8, 9, 16, 14);
        p.setPen(QPen(t.text, 1.5));
        p.drawRect(r);
        const QRect pane(r.right() - 5, r.top(), 5, r.height());
        if (isChecked()) p.fillRect(pane, t.accent); else p.drawLine(pane.left(), r.top(), pane.left(), r.bottom());
        if (hasFocus()) ui::paintFocusRing(&p, rect());
    }
    void enterEvent(QEnterEvent *) override { update(); }
    void leaveEvent(QEvent *) override { update(); }
};


class PanelHub : public QObject, public hn::plugins::PanelBridge {
    Q_OBJECT
public:
    struct Panel {
        QString qid, pluginId, id, title, icon, error;
        QList<hn::plugins::PanelBlock> blocks;
        bool rendered = false;
    };
    using QObject::QObject;
    const QList<Panel> &panels() const { return m_panels; }
    const Panel *find(const QString &qid) const;
    void showPanel(const QString &qid, const QString &title, const QString &icon) override;
    void updatePanel(const QString &qid, const QList<hn::plugins::PanelBlock> &blocks, const QString &error) override;
    void removePanel(const QString &qid) override;
signals:
    void panelsChanged();                    // a panel appeared / disappeared / was renamed
    void panelUpdated(const QString &qid);   // new blocks or error
private:
    QList<Panel> m_panels;
};

class PanelView : public QAbstractScrollArea {
    Q_OBJECT
public:
    explicit PanelView(QWidget *parent = nullptr);
    ~PanelView() override;
    void setContent(const QList<hn::plugins::PanelBlock> &blocks, const QString &error);
    int actionableCount() const;
    int currentIndex() const { return m_cur; }            // among actionable rows, -1 = none
    QString currentTitle() const;
    int rowCount() const { return int(m_rows.size()); }
    QString rowText(int row) const;                        // heading/text/item title... for tests
    QRect rowRect(int row) const;                          // viewport coordinates
    void setCurrentIndex(int i);
    void activateCurrent();
    QSize sizeHint() const override { return {320, 400}; }
signals:
    void activated(int click, const QString &path, int line);
    void switchPanel(int delta);
protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void leaveEvent(QEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    void focusInEvent(QFocusEvent *e) override { QAbstractScrollArea::focusInEvent(e); viewport()->update(); }
    void focusOutEvent(QFocusEvent *e) override { QAbstractScrollArea::focusOutEvent(e); viewport()->update(); }
private:
    struct Row {
        enum Kind { Heading, Text, Item, Button, Empty, Markdown, Error } kind = Text;
        int y = 0, h = 0, level = 2, click = 0, line = 0;
        QString text, sub, path;
        int doc = -1;   // Markdown: index into m_docs
    };
    void relayout();
    bool actionable(const Row &r) const;
    int rowAt(int contentY) const;
    void ensureVisible(int row);
    QList<Row> m_rows;
    std::vector<std::unique_ptr<QTextDocument>> m_docs;
    QList<hn::plugins::PanelBlock> m_blocks;
    QString m_error;
    QList<int> m_act;   // row indices of the actionable rows, in order
    int m_cur = -1, m_hover = -1, m_contentH = 0, m_laidOutW = -1;
};

class PanelDock : public QWidget {
    Q_OBJECT
public:
    static constexpr int kMinW = 240, kMaxW = 640, kHeader = 40, kGrip = 4;
    PanelDock(AppController *c, QWidget *parent = nullptr, bool popup = false);
    ~PanelDock() override;
    void setSession(NoteSession *s);
    NoteSession *session() const { return m_session.data(); }
    QString currentPanel() const { return m_cur; }
    void setCurrentPanel(const QString &qid);
    QStringList panelIds() const;
    PanelView *view() const { return m_view; }
    void refresh();                                      // re-render the current panel now
    int tabAt(const QPoint &p) const;
    QRect tabRect(int i) const;
    ui::IconButton *closeButton() const { return m_close; }
signals:
    void closeRequested();
    void widthChosen(int width);                         // the user finished dragging the edge
protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void showEvent(QShowEvent *) override;
    void hideEvent(QHideEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
private:
    void rebuild();
    void markShown();
    void showCurrent();
    void activated(int click, const QString &path, int line);
    QList<QPair<QString, QString>> tabs() const;         // (qid, title)
    AppController *m_c;
    QPointer<NoteSession> m_session;
    PanelView *m_view;
    ui::IconButton *m_close;
    QString m_cur;
    bool m_popup;
    int m_dragX = -1, m_dragW = 0, m_hoverTab = -1;
};

}  // namespace hn::app
