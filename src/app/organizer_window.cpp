#include <QDragEnterEvent>
#include <QDropEvent>
#include "organizer_window.h"
#include "controller.h"
#include "settings_dialog.h"
#include "hn/platform/window_identity.h"
#include "ui_common.h"
#include <QAction>
#include <QFile>
#include <QRegularExpression>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QShortcut>
#include <QToolTip>
#include <QHelpEvent>
#include <QCloseEvent>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QLineEdit>
#include <QMenu>
#include <QPainter>
#include <QPushButton>
#include <QStackedLayout>
#include <QVBoxLayout>
#include "tag_edit.h"

using namespace hn::core;

namespace hn::app {

// 72 px pane header: title, folder + hairline tag chips (click a chip to filter), colour swatch, pop-out and more.
class PaneHeader : public QWidget {
public:
    static constexpr int kRight = 136;
    ui::ElidedLabel *title;
    QWidget *pop = nullptr, *more = nullptr;
    int color = 0;
    QString folder, fallback;
    QStringList tags;
    std::function<void(const QString &)> onTag;
    std::function<void(const QPoint &)> onColor;
    explicit PaneHeader(QWidget *p) : QWidget(p) {
        setFixedHeight(72);
        setMouseTracking(true);
        title = new ui::ElidedLabel(this);
    }
    QRect swatchRect() const { return QRect(width() - 116, 26, 20, 20); }
protected:
    bool event(QEvent *e) override {
        if (e->type() == QEvent::ToolTip && swatchRect().contains(static_cast<QHelpEvent *>(e)->pos())) {
            QToolTip::showText(static_cast<QHelpEvent *>(e)->globalPos(), QObject::tr("Note colour"), this);
            return true;
        }
        return QWidget::event(e);
    }
    void resizeEvent(QResizeEvent *) override {
        title->setGeometry(16, 12, width() - 16 - kRight, 28);
        if (pop) pop->move(width() - 80, 20);
        if (more) more->move(width() - 44, 20);
    }
    void mouseMoveEvent(QMouseEvent *e) override {
        int h = -1;
        for (int i = 0; i < m_hits.size(); ++i) if (m_hits[i].contains(e->position().toPoint())) h = i;
        const bool onSwatch = swatchRect().contains(e->position().toPoint());
        setCursor(h >= 0 || onSwatch ? Qt::PointingHandCursor : Qt::ArrowCursor);
        if (h != m_hover) { m_hover = h; update(); }
    }
    void leaveEvent(QEvent *) override { if (m_hover >= 0) { m_hover = -1; update(); } }
    void mousePressEvent(QMouseEvent *e) override {
        if (e->button() != Qt::LeftButton) return;
        const QPoint pt = e->position().toPoint();
        for (int i = 0; i < m_hits.size(); ++i) if (m_hits[i].contains(pt) && onTag) { onTag(tags[i]); return; }
        if (swatchRect().contains(pt) && onColor) onColor(mapToGlobal(swatchRect().bottomLeft() + QPoint(-8, 8)));
    }
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        const auto &t = ui::theme();
        p.fillRect(rect(), t.surface);
        p.fillRect(QRect(0, 0, 4, height()), ui::noteColor(color));
        p.fillRect(QRect(0, height() - 1, width(), 1), t.border);
        const QRect sw = swatchRect();
        p.fillRect(sw, ui::noteColor(color));
        p.setPen(t.border);
        p.drawRect(sw.adjusted(0, 0, -1, -1));
        const bool lightSwatch = ui::noteColor(color).lightness() > 150;
        p.setPen(QPen(lightSwatch ? QColor(Qt::black) : QColor(Qt::white), 1.5, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
        p.drawPolyline(QPolygon{QPoint(sw.center().x() - 3, sw.center().y() - 1), QPoint(sw.center().x(), sw.center().y() + 2), QPoint(sw.center().x() + 4, sw.center().y() - 1)});
        int x = 16;
        const int right = width() - kRight;
        if (!folder.isEmpty()) {
            p.setFont(ui::labelFont(10));
            p.setPen(t.muted);
            const QString f = p.fontMetrics().elidedText(folder, Qt::ElideRight, qMax(0, right - x));
            p.drawText(QRect(x, 44, right - x, 18), Qt::AlignVCenter | Qt::AlignLeft, f);
            x += p.fontMetrics().horizontalAdvance(f) + 12;
        }
        m_hits.clear();
        if (!tags.isEmpty()) m_hits = ui::paintTagChips(&p, QRect(x, 44, qMax(0, right - x), 18), tags, m_hover);
        else if (folder.isEmpty()) {
            p.setFont(ui::uiFont(12));
            p.setPen(t.muted);
            p.drawText(QRect(x, 44, right - x, 18), Qt::AlignVCenter | Qt::AlignLeft, p.fontMetrics().elidedText(fallback, Qt::ElideRight, qMax(0, right - x)));
        }
    }
private:
    QVector<QRect> m_hits;
    int m_hover = -1;
};

OrganizerWindow::OrganizerWindow(AppController *c) : QWidget(nullptr, Qt::Window), m_c(c) {
    const auto id = hn::platform::WindowIdentity::create(hn::platform::Role::Organizer);
    hn::platform::applyIdentity(this, id);   // before show()
    m_token = id.token;
    setWindowIcon(ui::appIcon());
    setMinimumSize(720, 480);
    resize(c->settings().organizerSize);

    m_model = new NoteListModel(this);
    m_model->colorOf = [c](const QString &r) { return c->noteColor(r); };
    m_model->isOpen = [c](const QString &r) { return c->stickyOf(r) != nullptr; };
    m_railModel = new RailModel(this);

    auto *root = new QHBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    auto vline = [this] { auto *l = new QWidget(this); l->setFixedWidth(1); l->setObjectName("hnVLine"); return l; };

    // ---- rail
    auto *railCol = new QWidget(this);
    railCol->setFixedWidth(184);
    auto *rl = new QVBoxLayout(railCol);
    rl->setContentsMargins(0, 0, 0, 0);
    rl->setSpacing(0);
    auto *brand = new QWidget(railCol);
    brand->setFixedHeight(72);
    brand->setObjectName("hnBrand");
    auto *bl = new QHBoxLayout(brand);
    bl->setContentsMargins(16, 0, 8, 0);
    auto *mark = new QLabel(brand);
    mark->setPixmap(ui::appIcon().pixmap(QSize(20, 20), devicePixelRatioF()));
    auto *name = new QLabel(tr("HYPRNOTES"), brand);
    name->setObjectName("hnBrandName");
    m_settingsBtn = new ui::IconButton("more", tr("Settings and quit"), brand, 28);
    bl->addWidget(mark);
    bl->addSpacing(8);
    bl->addWidget(name, 1);
    bl->addWidget(m_settingsBtn);
    rl->addWidget(brand);
    m_rail = new QListView(railCol);
    m_rail->setModel(m_railModel);
    m_rail->setItemDelegate(new RailDelegate(m_rail));
    m_rail->setFrameShape(QFrame::NoFrame);
    m_rail->setStyleSheet("QListView { border: none; } QListView::item { border: none; padding: 0; }");
    m_rail->setMouseTracking(true);
    m_rail->setUniformItemSizes(false);
    m_rail->setAccessibleName(tr("Folders and tags"));
    rl->addWidget(m_rail, 1);
    m_newBtn = new QPushButton(tr("New note"), railCol);
    m_newBtn->setCursor(Qt::PointingHandCursor);
    m_newBtn->setToolTip(tr("New note (Ctrl+N)"));
    auto *nb = new QWidget(railCol);
    auto *nbl = new QVBoxLayout(nb);
    nbl->setContentsMargins(16, 8, 16, 16);
    nbl->addWidget(m_newBtn);
    rl->addWidget(nb);
    root->addWidget(railCol);
    root->addWidget(vline());

    // ---- list
    auto *listCol = new QWidget(this);
    listCol->setFixedWidth(304);
    auto *ll = new QVBoxLayout(listCol);
    ll->setContentsMargins(0, 0, 0, 0);
    ll->setSpacing(0);
    auto *searchRow = new QWidget(listCol);
    auto *sl = new QVBoxLayout(searchRow);
    sl->setContentsMargins(16, 16, 16, 0);
    m_search = new QLineEdit(searchRow);
    m_search->setPlaceholderText(tr("Search notes"));
    m_search->setTextMargins(24, 0, 24, 0);   // room for the painted search icon and the clear button
    m_searchIcon = new QLabel(m_search);
    m_searchIcon->setGeometry(8, 8, 16, 16);
    m_searchIcon->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_clearBtn = new ui::IconButton("close", tr("Clear search"), m_search, 24);
    m_clearBtn->setGeometry(272 - 28, 4, 24, 24);   // listCol 304 - 2 x 16 margin = 272
    m_clearBtn->hide();
    connect(m_clearBtn, &QAbstractButton::clicked, m_search, &QLineEdit::clear);
    connect(m_search, &QLineEdit::textChanged, this, [this](const QString &t) { m_clearBtn->setVisible(!t.isEmpty()); });
    m_search->setFixedHeight(32);
    m_search->setAccessibleName(tr("Search notes"));
    sl->addWidget(m_search);
    ll->addWidget(searchRow);
    m_count = new ui::ElidedLabel(listCol);
    m_count->setFixedHeight(24);
    auto *countRow = new QWidget(listCol);
    countRow->setObjectName("hnHeadLine");   // closes the 72 px column header with a hairline (see restyle)
    auto *cl = new QHBoxLayout(countRow);
    cl->setContentsMargins(16, 0, 16, 1);
    cl->addWidget(m_count);
    countRow->setFixedHeight(24);
    ll->addWidget(countRow);
    auto *stackHost = new QWidget(listCol);
    m_listStack = new QStackedLayout(stackHost);
    m_list = new QListView(stackHost);
    m_list->setModel(m_model);
    auto *delegate = new NoteDelegate(m_list);
    m_list->setItemDelegate(delegate);
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setStyleSheet("QListView { border: none; } QListView::item { border: none; padding: 0; }");
    m_list->setMouseTracking(true);
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_list, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        const QModelIndex i = m_list->indexAt(pos);
        if (!i.isValid()) return;
        m_list->setCurrentIndex(i);
        m_c->showNoteContextMenu(i.data(NoteListModel::RelRole).toString(), m_list->viewport()->mapToGlobal(pos), this);
    });
    m_list->setUniformItemSizes(true);
    m_list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_list->setAccessibleName(tr("Notes"));
    m_listEmpty = new ui::EmptyState(stackHost);
    m_listStack->addWidget(m_list);
    m_listStack->addWidget(m_listEmpty);
    ll->addWidget(stackHost, 1);
    root->addWidget(listCol);
    root->addWidget(vline());

    // ---- editor region
    auto *edHost = new QWidget(this);
    m_editorStack = new QStackedLayout(edHost);
    m_editorEmpty = new ui::EmptyState(edHost);
    m_elsewherePanel = new ui::EmptyState(edHost);
    m_pane = new QWidget(edHost);
    auto *pl = new QVBoxLayout(m_pane);
    pl->setContentsMargins(0, 0, 0, 0);
    pl->setSpacing(0);
    m_header = new PaneHeader(m_pane);
    m_popBtn = new ui::IconButton("popout", tr("Open in sticky window"), m_header, 32);
    m_moreBtn = new ui::IconButton("more", tr("Note actions"), m_header, 32);
    m_header->pop = m_popBtn;
    m_header->more = m_moreBtn;
    m_header->onTag = [this](const QString &tag) { setFilter({}, tag); };
    m_header->onColor = [this](const QPoint &g) { pickColor(g); };
    pl->addWidget(m_header);
    m_toolbarSlot = new QWidget(m_pane);
    m_toolbarLay = new QVBoxLayout(m_toolbarSlot);
    m_toolbarLay->setContentsMargins(0, 0, 0, 0);
    pl->addWidget(m_toolbarSlot);
    m_slot = new QWidget(m_pane);
    m_slotLay = new QVBoxLayout(m_slot);
    m_slotLay->setContentsMargins(0, 0, 0, 0);
    pl->addWidget(m_slot, 1);
    m_status = new StatusStrip(c, m_pane);
    pl->addWidget(m_status);
    m_fade = new ui::FadeOverlay(m_slot);
    m_editorStack->addWidget(m_editorEmpty);
    m_editorStack->addWidget(m_pane);
    m_editorStack->addWidget(m_elsewherePanel);
    root->addWidget(edHost, 1);

    m_debounce.setSingleShot(true);
    m_debounce.setInterval(kDebounceMs);
    m_refreshTimer.setSingleShot(true);
    m_refreshTimer.setInterval(kDebounceMs);
    connect(&m_debounce, &QTimer::timeout, this, [this] { runSearch(0); });
    connect(&m_refreshTimer, &QTimer::timeout, this, [this] { rebuildRail(); runSearch(0, true); });   // keeps the pages already loaded
    connect(m_search, &QLineEdit::textChanged, this, [this] { m_debounce.start(); });   // coalesces typing; superseded queries never run
    connect(m_model, &NoteListModel::fetchRequested, this, [this](int off) { runSearch(off); });
    connect(delegate, &NoteDelegate::openInWindow, this, [this](const QString &r) { m_c->openSticky(r, true); m_list->viewport()->update(); });
    connect(m_list->selectionModel(), &QItemSelectionModel::currentChanged, this, [this](const QModelIndex &cur) {
        if (m_syncing || !cur.isValid()) return;
        m_c->openInOrganizer(cur.data(NoteListModel::RelRole).toString());
    });
    connect(m_list, &QListView::doubleClicked, this, [this](const QModelIndex &i) { m_c->openSticky(i.data(NoteListModel::RelRole).toString(), true); });
    connect(m_rail->selectionModel(), &QItemSelectionModel::currentChanged, this, [this](const QModelIndex &cur) {
        if (m_syncing || !cur.isValid()) return;
        const RailItem &it = m_railModel->item(cur.row());
        m_folder = it.kind == RailItem::Folder ? it.key : QString();
        m_tag = it.kind == RailItem::Tag ? it.key : QString();
        runSearch(0);
    });
    connect(m_newBtn, &QPushButton::clicked, this, [this] { m_c->newNote(m_folder, true); });
    connect(m_listEmpty, &ui::EmptyState::primaryClicked, this, [this] { m_c->newNote(m_folder, true); });
    connect(m_editorEmpty, &ui::EmptyState::primaryClicked, this, [this] { m_c->newNote(m_folder, true); });
    connect(m_elsewherePanel, &ui::EmptyState::primaryClicked, this, [this] { m_c->openSticky(m_elsewhere, true); });
    connect(m_elsewherePanel, &ui::EmptyState::secondaryClicked, this, [this] { m_c->popIn(m_elsewhere); });
    connect(m_popBtn, &QAbstractButton::clicked, this, [this] { if (m_session) m_c->popOut(m_session->rel()); });
    connect(m_moreBtn, &QAbstractButton::clicked, this, &OrganizerWindow::showMoreMenu);
    connect(m_settingsBtn, &QAbstractButton::clicked, this, [this] {
        QMenu m(this);
        connect(m.addAction(tr("Settings…")), &QAction::triggered, this, [this] { m_c->showSettings(this); });
        connect(m.addAction(tr("Plugins…")), &QAction::triggered, this, [this] { m_c->showSettings(this, SettingsDialog::kPluginsTab); });
        m_c->populateToolsMenu(&m, m_session.data());
        m.addSeparator();
        connect(m.addAction(tr("Quit Hyprnotes")), &QAction::triggered, this, [this] { m_c->requestQuit(); });
        m.exec(m_settingsBtn->mapToGlobal(QPoint(0, m_settingsBtn->height())));
    });
    connect(c, &AppController::notesChanged, this, [this] { refresh(); });
    connect(c, &AppController::noteOpened, this, [this] { m_list->viewport()->update(); });
    connect(c, &AppController::noteClosed, this, [this] { m_list->viewport()->update(); });
    connect(c, &AppController::noteColorChanged, this, [this] { m_list->viewport()->update(); m_header->color = m_session ? m_c->noteColor(m_session->rel()) : 0; m_header->update(); });
    connect(c, &AppController::noteRenamed, this, [this] { refresh(); updateHeader(); });
    connect(c, &AppController::themeChanged, this, [this] {
        restyle();
        for (auto *w : findChildren<QWidget *>()) w->update();
        updateStacks();
        updateHeader();
    });
    if (auto *idx = c->index()) {
        connect(idx, &LibraryIndex::searchFinished, this, [this](const SearchPage &p) { onPage(p); });
        connect(idx, &LibraryIndex::synced, this, [this] { refresh(); });
        connect(idx, &LibraryIndex::pathUpdated, this, [this] { refresh(); });
    }
    restyle();
    m_newBtn->setFixedHeight(40);
    m_newBtn->setIconSize(QSize(16, 16));
    auto *help = new QShortcut(QKeySequence(Qt::Key_F1), this);
    connect(help, &QShortcut::activated, this, [this] { m_c->showShortcuts(this); });
    setAcceptDrops(true);
    m_list->installEventFilter(this);   // "?" opens the cheat sheet wherever it is not a typed character
    m_rail->installEventFilter(this);
    m_c->bindShortcuts(this, [this] { return m_session.data(); });
    auto *rn = new QAction(this);
    rn->setShortcut(QKeySequence(Qt::Key_F2));
    addAction(rn);
    connect(rn, &QAction::triggered, this, &OrganizerWindow::rename);
    rebuildRail();
    updateStacks();
    updateHeader();
    runSearch(0);
}

void OrganizerWindow::dragEnterEvent(QDragEnterEvent *e) {
    if (!AppController::themeFileFromMime(e->mimeData()).isEmpty() || !AppController::pluginPathFromMime(e->mimeData()).isEmpty()) e->acceptProposedAction();
}

// A dropped theme file imports and applies; shown in the status strip. (The text editor itself keeps its own drop handling.)
void OrganizerWindow::dropEvent(QDropEvent *e) {
    if (const QString pp = AppController::pluginPathFromMime(e->mimeData()); !pp.isEmpty()) {
        e->acceptProposedAction();
        QTimer::singleShot(0, this, [this, pp] {   // install, then the consent dialog (modal) - never inside the drop handler
            QString msg;
            m_c->plugins().install(pp, this, &msg);
            m_status->flash(msg);
        });
        return;
    }
    const QString p = AppController::themeFileFromMime(e->mimeData());
    if (p.isEmpty()) return;
    e->acceptProposedAction();
    QString msg;
    m_c->importThemeFile(p, &msg);
    m_status->flash(msg.section('\n', 0, 0) + (msg.contains('\n') ? " (+warnings)" : QString()));
}

OrganizerWindow::~OrganizerWindow() { detachSession(); }

bool OrganizerWindow::eventFilter(QObject *o, QEvent *e) {
    if ((o == m_list || o == m_rail) && e->type() == QEvent::KeyPress && static_cast<QKeyEvent *>(e)->key() == Qt::Key_Question) {
        m_c->showShortcuts(this);
        return true;
    }
    return QWidget::eventFilter(o, e);
}

void OrganizerWindow::pickColor(const QPoint &global) {
    if (!m_session) return;
    const QString rel = m_session->rel();
    auto *pop = new ui::SwatchPopover(m_c->noteColor(rel), this);
    connect(pop, &ui::SwatchPopover::picked, this, [this, rel](int i) { m_c->setNoteColor(rel, i); });
    pop->popup(global);
}

void OrganizerWindow::restyle() {
    const auto &t = ui::theme();
    m_newBtn->setStyleSheet(ui::accentButtonStyle());
    m_newBtn->setIcon(ui::icon("plus", t.accentText, 16));
    m_searchIcon->setPixmap(ui::icon("search", t.muted).pixmap(QSize(16, 16), devicePixelRatioF()));
    for (auto *w : findChildren<QWidget *>("hnBrand")) w->setStyleSheet(QString("QWidget#hnBrand { border-bottom: 1px solid %1; }").arg(t.border.name()));
    for (auto *w : findChildren<QWidget *>("hnHeadLine")) w->setStyleSheet(QString("QWidget#hnHeadLine { border-bottom: 1px solid %1; }").arg(t.border.name()));
    for (auto *l : findChildren<QWidget *>("hnVLine")) l->setStyleSheet(QString("background: %1;").arg(t.border.name()));
    if (auto *n = findChild<QLabel *>("hnBrandName")) { n->setFont(ui::labelFont(11)); n->setStyleSheet("background: transparent;"); }
    m_search->setFont(ui::uiFont(13));
}

void OrganizerWindow::present(bool focus) {
    if (!isVisible()) { setAttribute(Qt::WA_ShowWithoutActivating, !focus); show(); }
    if (focus) { raise(); activateWindow(); }
}

void OrganizerWindow::focusSearch() { m_search->setFocus(); m_search->selectAll(); }

void OrganizerWindow::refresh() { m_refreshTimer.start(); }

void OrganizerWindow::setFilter(const QString &folder, const QString &tag) {
    m_folder = folder;
    m_tag = tag;
    syncSelection();
    runSearch(0);
}

void OrganizerWindow::closeEvent(QCloseEvent *e) {
    if (m_c->organizerCloseRequested()) e->accept(); else e->ignore();
}

void OrganizerWindow::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.fillRect(rect(), ui::theme().bg);
    if (!m_titled) { m_titled = true; QTimer::singleShot(0, this, [this] { setWindowTitle(tr("Hyprnotes")); }); }
}

// ---------------------------------------------------------------- search / list
void OrganizerWindow::runSearch(int offset, bool keepExtent) {
    auto *idx = m_c->index();
    IndexQuery q;
    q.text = m_search->text().trimmed();
    q.folder = m_folder;
    q.tag = m_tag;
    q.limit = keepExtent && offset == 0 ? qBound(int(NoteListModel::kPage), m_model->rowCount(), int(NoteListModel::kMaxRows)) : int(NoteListModel::kPage);
    q.offset = offset;
    ++m_searches;
    m_lastOffset = offset;
    m_lastId = idx->search(q);
}

// The index only stores snippets for text queries; list rows get a first-lines preview read lazily
// (first 700 bytes, at most 100 files per page, cached by path+mtime, never more than 500 entries).
static QString previewFor(const QString &abs) {
    QFile f(abs);
    if (!f.open(QIODevice::ReadOnly)) return {};
    const QString text = QString::fromUtf8(f.read(700));
    static const QRegularExpression lead(QStringLiteral("^(\\s*(#+\\s|>|[-*+]\\s(\\[[ xX]\\]\\s)?|\\d+\\.\\s))+"));
    static const QRegularExpression marks(QStringLiteral("[*_`]+"));
    QStringList parts;
    bool first = true;
    for (QString line : text.split('\n')) {
        line = line.trimmed();
        if (line.isEmpty()) continue;
        const bool heading = line.startsWith('#') && line.size() > 1 && (line[1] == ' ' || line[1] == '#');
        if (first && heading) { first = false; continue; }
        first = false;
        const QStringList tok = line.split(' ', Qt::SkipEmptyParts);
        bool tagsOnly = true;
        for (const auto &t : tok) tagsOnly = tagsOnly && t.startsWith('#') && t.size() > 1;
        if (tagsOnly) continue;
        line.remove(lead);
        line.remove(marks);
        parts << line;
        if (parts.join(' ').size() > 140) break;
    }
    return parts.join(' ').simplified().left(140);
}

void OrganizerWindow::onPage(const SearchPage &page0) {
    SearchPage page = page0;
    for (auto &r : page.rows) {
        if (!r.snippet.isEmpty()) { r.snippet.remove('[').remove(']'); continue; }
        const QString key = r.relPath;
        if (auto *hit = m_previews.object(key); hit && hit->first == r.mtimeMs) { r.snippet = hit->second; continue; }
        r.snippet = previewFor(m_c->repo().absolutePath(r.relPath));
        m_previews.insert(key, new QPair<qint64, QString>(r.mtimeMs, r.snippet));
    }
    if (page.id != m_lastId) return;   // superseded
    const bool append = m_lastOffset > 0;
    QList<IndexRow> rows = page.rows;
    if (!append) rows = LibraryIndex::mergeDirty(rows, m_c->dirtyDocs(), m_search->text().trimmed());
    if (append) m_model->appendRows(rows, page.hasMore); else m_model->setRows(rows, page.hasMore);
    m_model->fetchFinished();
    const int n = m_model->rowCount();
    m_count->setTextFont(ui::labelFont(10));
    m_count->setColor(ui::theme().muted);
    m_count->setText(m_model->truncated() ? tr("%1+ NOTES").arg(n) : (n == 1 ? tr("1 NOTE") : tr("%1 NOTES").arg(n)));
    m_total = n;
    syncSelection();
    updateStacks();
}

void OrganizerWindow::rebuildRail() {
    auto *idx = m_c->index();
    QList<RailItem> items;
    items.append({RailItem::Header, tr("LIBRARY"), {}, -1, 0});
    items.append({RailItem::All, tr("All notes"), {}, idx->count(), 0});
    const QStringList folders = idx->folders();
    if (!folders.isEmpty()) {
        items.append({RailItem::Header, tr("FOLDERS"), {}, -1, 0});
        for (const auto &f : folders) items.append({RailItem::Folder, f.section('/', -1), f, -1, int(f.count('/'))});
    }
    const auto tags = idx->tags();
    if (!tags.isEmpty()) {
        items.append({RailItem::Header, tr("TAGS"), {}, -1, 0});
        for (const auto &t : tags) items.append({RailItem::Tag, "#" + t.first, t.first, t.second, 0});
    }
    m_syncing = true;
    m_railModel->setItems(items);
    m_syncing = false;
    syncSelection();
}

void OrganizerWindow::syncSelection() {
    m_syncing = true;
    int r = !m_folder.isEmpty() ? m_railModel->rowOf(RailItem::Folder, m_folder) : !m_tag.isEmpty() ? m_railModel->rowOf(RailItem::Tag, m_tag) : m_railModel->rowOf(RailItem::All, {});
    if (r >= 0) m_rail->setCurrentIndex(m_railModel->index(r));
    const int row = m_session ? m_model->rowOf(m_session->rel()) : (m_elsewhere.isEmpty() ? -1 : m_model->rowOf(m_elsewhere));
    if (row >= 0) m_list->setCurrentIndex(m_model->index(row)); else m_list->selectionModel()->clearCurrentIndex();
    m_syncing = false;
}

void OrganizerWindow::updateStacks() {
    const bool searching = !m_search->text().trimmed().isEmpty() || !m_folder.isEmpty() || !m_tag.isEmpty();
    if (m_model->rowCount() == 0 && m_lastId) {
        if (searching) m_listEmpty->setContent(ui::Art::Search, tr("No matches"), tr("Nothing fits that search. Try fewer words, or clear the filter."));
        else m_listEmpty->setContent(ui::Art::Stack, tr("Nothing here yet"), tr("Notes are plain Markdown files in your folder. Start with one."), tr("New note"));
        m_listStack->setCurrentWidget(m_listEmpty);
    } else m_listStack->setCurrentWidget(m_list);

    if (m_session) m_editorStack->setCurrentWidget(m_pane);
    else if (!m_elsewhere.isEmpty() && m_c->stickyOf(m_elsewhere)) {
        m_elsewherePanel->setContent(ui::Art::Stickies, tr("Open in a sticky window"), tr("“%1” is being edited in its own window, so there is only ever one editor for it.")
                                         .arg(QFileInfo(m_elsewhere).completeBaseName()), tr("Show sticky window"), tr("Pop in here"));
        m_editorStack->setCurrentWidget(m_elsewherePanel);
    } else {
        if (m_total == 0 && !searching) m_editorEmpty->setContent(ui::Art::Note, tr("Write something worth keeping"), tr("One note, one Markdown file. Format with the toolbar or type Markdown."));
        else m_editorEmpty->setContent(ui::Art::Shapes, tr("Pick a note"), tr("Choose one from the list, or press Ctrl+N to start a new one."));
        m_editorStack->setCurrentWidget(m_editorEmpty);
    }
}

// ---------------------------------------------------------------- editor region
void OrganizerWindow::attachSession(NoteSession *s) {
    if (m_session == s) return;
    detachSession();
    m_session = s;
    m_elsewhere.clear();
    m_slotLay->addWidget(s->editor());
    m_toolbarLay->addWidget(s->toolbar());
    s->editor()->show();
    s->toolbar()->show();
    m_status->setSession(s);
    m_titleConn = connect(s, &NoteSession::titleChanged, this, [this] { updateHeader(); refresh(); });
    updateHeader();
    updateStacks();
    syncSelection();
    m_fade->play();
}

void OrganizerWindow::detachSession() {
    if (!m_session) return;
    for (QWidget *w : {static_cast<QWidget *>(m_session->editor()), static_cast<QWidget *>(m_session->toolbar())}) {
        if (w->parentWidget() == m_slot || w->parentWidget() == m_toolbarSlot) { w->hide(); w->setParent(nullptr); }
    }
    disconnect(m_titleConn);
    m_session = nullptr;
    m_status->setSession(nullptr);
    updateStacks();
}

void OrganizerWindow::noteReleased(const QString &rel) {
    if (m_elsewhere != rel) return;
    m_elsewhere.clear();
    updateStacks();
    syncSelection();
}

void OrganizerWindow::showElsewhere(const QString &rel) {
    detachSession();
    m_elsewhere = rel;
    updateStacks();
    syncSelection();
}

void OrganizerWindow::updateHeader() {
    if (!m_session) return;
    const QString t = m_session->title().isEmpty() ? tr("Untitled") : m_session->title();
    m_header->title->setText(t);
    m_header->title->setTextFont(ui::uiFont(20, QFont::Bold));
    m_header->folder = m_session->folder();
    m_header->tags = m_session->tags();
    m_header->fallback = QFileInfo(m_session->rel()).fileName();
    m_header->color = m_c->noteColor(m_session->rel());
    m_header->update();
}

void OrganizerWindow::rename() {
    if (!m_session) return;
    bool ok = false;
    const QString cur = QFileInfo(m_session->rel()).completeBaseName();
    const QString name = QInputDialog::getText(this, tr("Rename note"), tr("File name"), QLineEdit::Normal, cur, &ok);
    if (!ok) return;
    QString err;
    if (!m_c->renameNote(m_session->rel(), name, &err)) m_status->flash(err);
}

void OrganizerWindow::editTags() {
    if (!m_session) return;
    bool ok = false;
    QStringList cur;
    for (const auto &g : m_session->tags()) cur << "#" + g;
    const QString text = QInputDialog::getText(this, tr("Edit tags"), tr("Tags are #tokens in the note text, e.g. #work #ideas"), QLineEdit::Normal, cur.join(' '), &ok);
    if (ok) m_c->editTags(m_session->rel(), parseTagInput(text));
}

void OrganizerWindow::showMoreMenu() {
    if (!m_session) return;
    QMenu m(this);
    connect(m.addAction(ui::icon("more"), tr("Rename…  F2")), &QAction::triggered, this, &OrganizerWindow::rename);
    connect(m.addAction(ui::icon("tag"), tr("Edit tags…")), &QAction::triggered, this, &OrganizerWindow::editTags);
    m_c->populateNoteMenu(&m, m_session->rel(), true);
    m.exec(m_moreBtn->mapToGlobal(QPoint(0, m_moreBtn->height())));
}

} // namespace hn::app
