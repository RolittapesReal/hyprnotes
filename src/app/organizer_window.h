#pragma once
// Organizer 900x640: rail (folders/tags), debounced search, model/delegate note list, editor region.
#include "hn/core/library_index.h"
#include "note_list.h"
#include "note_session.h"
#include "status_strip.h"
#include <QCache>
#include <QListView>
#include <QPointer>
#include <QTimer>
#include <QWidget>

class QLabel;
class QLineEdit;
class QPushButton;
class QStackedLayout;
class QVBoxLayout;

namespace hn::app {

class AppController;
class PaneHeader;
namespace ui { class EmptyState; class FadeOverlay; class ElidedLabel; class IconButton; }

class OrganizerWindow : public QWidget {
    Q_OBJECT
public:
    static constexpr int kDebounceMs = 120;
    explicit OrganizerWindow(AppController *c);
    ~OrganizerWindow() override;

    QString token() const { return m_token; }
    NoteSession *currentSession() const { return m_session; }
    void attachSession(NoteSession *s);
    void detachSession();                       // editor ownership returns to the controller
    void noteReleased(const QString &rel);      // its sticky closed: drop a stale 'open elsewhere' panel
    void showElsewhere(const QString &rel);     // note is open in a sticky: show entry, offer focus / pop in
    void present(bool focus);
    void refresh();                             // re-run rail + query (debounced)
    void focusSearch();
    int searchCount() const { return m_searches; }
    NoteListModel *model() const { return m_model; }
    QLineEdit *searchEdit() const { return m_search; }
    QListView *list() const { return m_list; }
    QListView *rail() const { return m_rail; }
    StatusStrip *status() const { return m_status; }
    QWidget *editorPane() const { return m_pane; }
    void setFilter(const QString &folder, const QString &tag);
    void runSearchNow() { m_debounce.stop(); runSearch(0); }

protected:
    void closeEvent(QCloseEvent *) override;
    void dragEnterEvent(QDragEnterEvent *) override;
    void dropEvent(QDropEvent *) override;
    void paintEvent(QPaintEvent *) override;
    bool eventFilter(QObject *o, QEvent *e) override;

private:
    void pickColor(const QPoint &global);
    void runSearch(int offset, bool keepExtent = false);
    void onPage(const hn::core::SearchPage &page);
    void rebuildRail();
    void syncSelection();
    void updateHeader();
    void updateStacks();
    void rename();
    void editTags();
    void showMoreMenu();
    void restyle();

    AppController *m_c;
    QString m_token, m_folder, m_tag, m_elsewhere;
    QPointer<NoteSession> m_session;
    QMetaObject::Connection m_titleConn;
    NoteListModel *m_model;
    RailModel *m_railModel;
    QListView *m_list, *m_rail;
    QLineEdit *m_search;
    QLabel *m_searchIcon = nullptr;
    ui::IconButton *m_clearBtn = nullptr;
    ui::ElidedLabel *m_count;
    QStackedLayout *m_listStack, *m_editorStack;
    ui::EmptyState *m_listEmpty, *m_editorEmpty, *m_elsewherePanel;
    QWidget *m_pane, *m_slot, *m_toolbarSlot;
    QVBoxLayout *m_slotLay, *m_toolbarLay;
    PaneHeader *m_header;
    StatusStrip *m_status;
    ui::FadeOverlay *m_fade;
    QPushButton *m_newBtn;
    ui::IconButton *m_settingsBtn, *m_popBtn, *m_moreBtn;
    QCache<QString, QPair<qint64, QString>> m_previews{500};
    QTimer m_debounce, m_refreshTimer;
    quint64 m_lastId = 0;
    int m_searches = 0, m_lastOffset = 0;
    bool m_syncing = false, m_titled = false;
    int m_total = 0;
};

} // namespace hn::app
