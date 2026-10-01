#include "sticky_window.h"
#include "controller.h"
#include "ui_common.h"
#include <QCloseEvent>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QShortcut>
#include <QTimer>
#include <QVBoxLayout>
#include <QWindow>

using namespace hn::platform;

namespace hn::app {

// 28 px header. Everything but the buttons and chip is a drag handle (native compositor move).
class StickyHeader : public QWidget {
public:
    std::function<void(const QPoint &)> onContext;
    ui::ElidedLabel *title;
    explicit StickyHeader(QWidget *parent) : QWidget(parent) {
        setFixedHeight(StickyWindow::kHeader);
        setCursor(Qt::SizeAllCursor);
        title = new ui::ElidedLabel(this);
        title->setAttribute(Qt::WA_TransparentForMouseEvents);
    }
protected:
    void mousePressEvent(QMouseEvent *e) override {
        if (e->button() == Qt::LeftButton) { if (auto *wh = window()->windowHandle()) wh->startSystemMove(); }
        e->accept();
    }
    void contextMenuEvent(QContextMenuEvent *e) override { if (onContext) onContext(e->globalPos()); }
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.fillRect(rect(), ui::theme().surface);
        p.fillRect(QRect(0, height() - 1, width(), 1), ui::theme().border);
    }
};

StickyWindow::StickyWindow(AppController *c, NoteSession *s)
    : QWidget(nullptr, Qt::Window | Qt::FramelessWindowHint), m_c(c), m_session(s), m_id(WindowIdentity::create(Role::Sticky)) {
    // Identity first: the initial title "hyprnotes-sticky:<token>" is what compositor rules match (before show()).
    applyIdentity(this, m_id);
    setWindowIcon(ui::appIcon());
    setMouseTracking(true);
    setAttribute(Qt::WA_OpaquePaintEvent);
    const auto st = c->settings();
    setMinimumSize(st.stickyMin);
    resize(st.stickySize);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(kEdge, kEdge, kEdge, kEdge);
    root->setSpacing(0);

    m_header = new StickyHeader(this);
    auto *h = new QHBoxLayout(m_header);
    h->setContentsMargins(8, 0, 0, 0);
    h->setSpacing(0);
    m_chip = new ui::ChipButton(m_header);
    m_format = new ui::IconButton("h1", tr("Formatting toolbar"), m_header, kHeader);
    m_format->setCheckable(true);
    m_popIn = new ui::IconButton("popin", tr("Pop in to organizer"), m_header, kHeader);
    m_close = new ui::IconButton("close", tr("Close note"), m_header, kHeader);
    h->addWidget(m_header->title, 1);
    h->addWidget(m_chip);
    h->addSpacing(4);
    h->addWidget(m_format);
    h->addWidget(m_popIn);
    h->addWidget(m_close);
    root->addWidget(m_header);

    m_slot = new QWidget(this);
    m_slotLay = new QVBoxLayout(m_slot);
    m_slotLay->setContentsMargins(0, 0, 0, 0);
    root->addWidget(m_slot, 1);

    m_toolbarHost = new QWidget(this);   // below the editor: toggling never moves or reflows the text above it
    m_toolbarLay = new QVBoxLayout(m_toolbarHost);
    m_toolbarLay->setContentsMargins(0, 0, 0, 0);
    m_toolbarHost->hide();
    root->addWidget(m_toolbarHost);
    m_fade = new ui::FadeOverlay(m_toolbarHost);

    m_status = new StatusStrip(c, this);
    root->addWidget(m_status);

    connect(m_close, &QAbstractButton::clicked, this, [this] { close(); });
    connect(m_popIn, &QAbstractButton::clicked, this, [this] { m_c->popIn(m_session->rel()); });
    connect(m_format, &QAbstractButton::toggled, this, [this](bool on) { setToolbarShown(on); });
    connect(m_chip, &QAbstractButton::clicked, this, [this] {
        m_c->setWorkspaceMode(m_session->rel(), m_mode == WorkspaceMode::ThisWorkspace ? WorkspaceMode::AllWorkspaces : WorkspaceMode::ThisWorkspace);
    });
    m_header->onContext = [this](const QPoint &g) { m_c->showNoteContextMenu(m_session->rel(), g, this, true); };
    connect(s, &NoteSession::titleChanged, this, &StickyWindow::refreshTitle);
    connect(c, &AppController::noteColorChanged, this, [this](const QString &r) { if (r == m_session->rel()) update(); });
    connect(c, &AppController::themeChanged, this, [this] { for (auto *w : findChildren<QWidget *>()) w->update(); update(); });
    setWorkspaceMode(WorkspaceMode::ThisWorkspace);
    refreshTitle();
    m_status->setSession(s);
    attach();
    c->bindShortcuts(this, [this] { return m_session; });
    auto *help = new QShortcut(QKeySequence(Qt::Key_F1), this);
    connect(help, &QShortcut::activated, this, [this] { m_c->showShortcuts(this); });
}

StickyWindow::~StickyWindow() { detach(); }

QWidget *StickyWindow::header() const { return m_header; }

void StickyWindow::attach() {
    if (!m_session || m_session->editor()->parentWidget() == m_slot) return;
    m_slotLay->addWidget(m_session->editor());
    m_toolbarLay->addWidget(m_session->toolbar());
    m_session->editor()->show();
    m_session->toolbar()->show();
}

void StickyWindow::detach() {
    if (!m_session) return;
    for (QWidget *w : {static_cast<QWidget *>(m_session->editor()), static_cast<QWidget *>(m_session->toolbar())}) {
        if (w->parentWidget() == m_slot || w->parentWidget() == m_toolbarHost) {
            w->hide();
            w->setParent(nullptr);
        }
    }
}

void StickyWindow::present(bool focus) {
    if (!isVisible()) {
        setAttribute(Qt::WA_ShowWithoutActivating, !focus);   // passive session restore must not steal focus
        show();
    }
    if (focus) {
        raise();
        activateWindow();
        m_session->editor()->focusEditor();
    }
}

bool StickyWindow::toolbarShown() const { return m_toolbarHost->isVisibleTo(const_cast<StickyWindow *>(this)); }

void StickyWindow::setToolbarShown(bool on) {
    if (on == toolbarShown()) return;
    m_format->setChecked(on);
    m_toolbarHost->setVisible(on);
    if (on) m_fade->play();
    if (auto *ed = m_session->editor(); ed) ed->focusEditor();
}

void StickyWindow::setWorkspaceMode(WorkspaceMode m) {
    m_mode = m;
    m_chip->setFilled(m == WorkspaceMode::AllWorkspaces);
    m_chip->setIconName(m == WorkspaceMode::AllWorkspaces ? "pin" : "");
    updateChip();
}

void StickyWindow::updateChip() {
    const bool all = m_mode == WorkspaceMode::AllWorkspaces;
    const bool wide = width() >= 330;
    m_chip->setText(all ? (wide ? tr("ALL WORKSPACES") : tr("ALL")) : (wide ? tr("THIS WORKSPACE") : tr("HERE")));
    m_chip->setToolTip(all ? tr("Shown on all workspaces (pinned). Click for this workspace only.") : tr("Shown on this workspace only. Click to show on all workspaces."));
    m_chip->setAccessibleName(m_chip->toolTip());
    m_chip->setFixedSize(m_chip->sizeHint());
    m_chip->setFont(ui::labelFont(10));
}

void StickyWindow::refreshTitle() {
    const QString t = m_session->title();
    m_header->title->setText(t.isEmpty() ? tr("Untitled") : t);
    m_header->title->setTextFont(ui::uiFont(13, QFont::Bold));
    if (m_titled) setWindowTitle(m_header->title->text());   // only after the initial identity title was mapped
}

// ---------------------------------------------------------------- painting / chrome
void StickyWindow::paintEvent(QPaintEvent *) {
    QPainter p(this);
    const auto &t = ui::theme();
    p.fillRect(rect(), t.bg);
    p.setPen(t.border);
    p.drawRect(rect().adjusted(0, 0, -1, -1));
    p.fillRect(QRect(0, 0, 4, height()), ui::noteColor(m_c->noteColor(m_session->rel())));
    if (!m_titled) {
        m_titled = true;   // first frame committed: from now on the window title may follow the note
        QTimer::singleShot(0, this, [this] { refreshTitle(); setWindowTitle(m_header->title->text()); });
    }
}

Qt::Edges StickyWindow::edgesAt(const QPoint &p) const {
    Qt::Edges e;
    if (p.x() < kEdge) e |= Qt::LeftEdge;
    if (p.x() >= width() - kEdge) e |= Qt::RightEdge;
    if (p.y() < kEdge) e |= Qt::TopEdge;
    if (p.y() >= height() - kEdge) e |= Qt::BottomEdge;
    return e;
}

void StickyWindow::mouseMoveEvent(QMouseEvent *e) {
    const Qt::Edges ed = edgesAt(e->position().toPoint());
    if (ed == (Qt::LeftEdge | Qt::TopEdge) || ed == (Qt::RightEdge | Qt::BottomEdge)) setCursor(Qt::SizeFDiagCursor);
    else if (ed == (Qt::RightEdge | Qt::TopEdge) || ed == (Qt::LeftEdge | Qt::BottomEdge)) setCursor(Qt::SizeBDiagCursor);
    else if (ed & (Qt::LeftEdge | Qt::RightEdge)) setCursor(Qt::SizeHorCursor);
    else if (ed & (Qt::TopEdge | Qt::BottomEdge)) setCursor(Qt::SizeVerCursor);
    else unsetCursor();
}

void StickyWindow::mousePressEvent(QMouseEvent *e) {
    const Qt::Edges ed = edgesAt(e->position().toPoint());
    if (e->button() == Qt::LeftButton && ed && windowHandle()) windowHandle()->startSystemResize(ed);   // native compositor resize
}

void StickyWindow::resizeEvent(QResizeEvent *e) {
    QWidget::resizeEvent(e);
    updateChip();
    if (isVisible()) m_c->stickyGeometryChanged(m_session->rel());
}
void StickyWindow::moveEvent(QMoveEvent *e) {
    QWidget::moveEvent(e);
    if (isVisible()) m_c->stickyGeometryChanged(m_session->rel());
}

void StickyWindow::keyPressEvent(QKeyEvent *e) {
    if (e->key() == Qt::Key_Escape) { if (toolbarShown()) setToolbarShown(false); return; }
    QWidget::keyPressEvent(e);
}

void StickyWindow::closeEvent(QCloseEvent *e) {
    if (m_transferred || m_c->exiting()) { e->accept(); return; }
    e->ignore();                          // the controller closes it once the save path allows
    m_c->closeNote(m_session->rel());
}

} // namespace hn::app
