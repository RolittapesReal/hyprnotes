#pragma once
// Frameless modernist sticky note: 28 px header (drag area), 6 px resize band, note-colour stripe,
// workspace chip, toolbar toggle, pop-in and close. Hosts a NoteSession's editor; never owns content.
#include "hn/platform/window_identity.h"
#include "hn/platform/window_placement.h"
#include "note_session.h"
#include "status_strip.h"
#include <QWidget>

class QVBoxLayout;

namespace hn::app {

class AppController;
class StickyHeader;
class PanelDock;
class DockToggle;
namespace ui { class IconButton; class ChipButton; class FadeOverlay; }

class StickyWindow : public QWidget {
    Q_OBJECT
public:
    static constexpr int kHeader = 28;
    static constexpr int kEdge = 6;
    StickyWindow(AppController *c, NoteSession *s);
    ~StickyWindow() override;

    QString token() const { return m_id.token; }
    NoteSession *session() const { return m_session.data(); }
    void attach();                    // place the session's editor/toolbar into this window
    void detach();                    // give them back (reparented to nothing); content stays with the session
    void present(bool focus);         // passive (no focus) for session restore
    void markTransferred() { m_transferred = true; }
    void setWorkspaceMode(hn::platform::WorkspaceMode m);
    hn::platform::WorkspaceMode workspaceMode() const { return m_mode; }
    bool toolbarShown() const;
    void setToolbarShown(bool on);
    void refreshTitle();
    QWidget *header() const;
    ui::IconButton *closeButton() const { return m_close; }
    ui::IconButton *popInButton() const { return m_popIn; }
    ui::ChipButton *chip() const { return m_chip; }
    StatusStrip *status() const { return m_status; }
    DockToggle *panelButton() const { return m_panels; }      // null until a plugin registers a panel
    PanelDock *openPanels();                                  // popup with the same panels as the organizer dock

protected:
    void paintEvent(QPaintEvent *) override;
    void closeEvent(QCloseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void moveEvent(QMoveEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    bool eventFilter(QObject *, QEvent *) override;

private:
    Qt::Edges edgesAt(const QPoint &p) const;
    void updateChip();
    AppController *m_c;
    QPointer<NoteSession> m_session;
    hn::platform::WindowIdentity m_id;
    hn::platform::WorkspaceMode m_mode = hn::platform::WorkspaceMode::ThisWorkspace;
    StickyHeader *m_header;
    QWidget *m_slot, *m_toolbarHost;
    QVBoxLayout *m_slotLay, *m_toolbarLay;
    StatusStrip *m_status;
    ui::IconButton *m_format, *m_popIn, *m_close;
    ui::ChipButton *m_chip;
    ui::FadeOverlay *m_fade;
    void syncPanels();
    DockToggle *m_panels = nullptr;
    QPointer<PanelDock> m_popup;
    bool m_transferred = false, m_titled = false;
};

} // namespace hn::app
