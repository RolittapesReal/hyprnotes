#include "market_page.h"
#include "action_row.h"
#include "ui_common.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QPointer>
#include <QPushButton>
#include <QScopeGuard>
#include <QStackedWidget>
#include <QUrl>
#include <QVBoxLayout>

using namespace hn::plugins;

namespace hn::app {

MarketPage::MarketPage(AppController *c, QWidget *parent) : QWidget(parent), m_c(c) {
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 8, 0, 0);
    m_stack = new QStackedWidget(this);
    root->addWidget(m_stack);

    // first-use notice
    m_notice = new QWidget(m_stack);
    m_notice->setObjectName("hnMarketNotice");
    auto *nl = new QVBoxLayout(m_notice);
    nl->setContentsMargins(0, 0, 0, 0);
    nl->setSpacing(8);
    const QString host = QUrl(c->marketIndexUrl()).host();
    auto *text = new QLabel(tr("Browse contacts %1 to list plugins. Nothing about you is sent. Nothing is fetched until you continue.").arg(host), m_notice);
    text->setObjectName("hnMarketNoticeText");
    text->setWordWrap(true);
    text->setTextFormat(Qt::PlainText);
    nl->addWidget(text);
    auto *noticeStatus = new QLabel(m_notice);
    noticeStatus->setObjectName("hnMarketNoticeStatus");
    noticeStatus->setWordWrap(true);
    noticeStatus->setTextFormat(Qt::PlainText);
    noticeStatus->setProperty("hnRole", "danger");
    noticeStatus->setAccessibleName(tr("Registry notice status"));
    noticeStatus->hide();
    nl->addWidget(noticeStatus);
    auto *row = new ui::ActionRow(m_notice);
    for (const auto &[name, label, primary] : {std::tuple{"continue", tr("Continue"), true}, std::tuple{"back", tr("Back"), false}}) {
        auto *b = new ui::WrappingButton(label, m_notice);
        b->setObjectName(QStringLiteral("hnMarket_") + QLatin1String(name));
        b->setMinimumHeight(32);
        if (primary) b->setProperty("hnRole", "primary");
        m_buttons.insert(name, b);
        row->addButton(b);
    }
    nl->addWidget(row);
    nl->addStretch(1);
    m_stack->addWidget(m_notice);

    // list
    auto *listPane = new QWidget(m_stack);
    auto *ll = new QVBoxLayout(listPane);
    ll->setContentsMargins(0, 0, 0, 0);
    ll->setSpacing(8);
    m_search = new QLineEdit(listPane);
    m_search->setObjectName("hnMarketSearch");
    m_search->setPlaceholderText(tr("Search plugins (press / to jump here)"));
    m_search->setClearButtonEnabled(true);
    m_search->setAccessibleName(tr("Search plugins"));
    m_tag = new QComboBox(listPane);
    m_tag->setObjectName("hnMarketTag");
    m_tag->setAccessibleName(tr("Filter by tag"));
    m_tag->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_tag->setMinimumContentsLength(8);
    m_hide = new QCheckBox(tr("Hide dangerous"), listPane);
    m_hide->setObjectName("hnMarketHideDangerous");
    m_hide->setAccessibleName(tr("Hide plugins with dangerous permissions"));
    auto *actions = new ui::ActionRow(listPane);
    auto *refresh = new ui::WrappingButton(tr("Refresh"), listPane);
    refresh->setObjectName("hnMarket_refresh");
    refresh->setMinimumHeight(32);
    m_buttons.insert("refresh", refresh);
    actions->addButton(refresh);
    ll->addWidget(m_search);
    ll->addWidget(m_tag);
    ll->addWidget(m_hide);
    ll->addWidget(actions);

    m_status = new QLabel(listPane);
    m_status->setObjectName("hnMarketStatus");
    m_status->setAccessibleName(tr("Registry status"));
    m_status->setWordWrap(true);
    m_status->setTextFormat(Qt::PlainText);
    ll->addWidget(m_status);

    m_model = new MarketModel(this);
    m_list = new QListView(listPane);
    m_list->setObjectName("hnMarketList");
    m_list->setModel(m_model);
    m_list->setItemDelegate(new MarketDelegate(m_list));
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setResizeMode(QListView::Adjust);
    m_list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setMinimumHeight(220);
    m_list->setAccessibleName(tr("Plugin registry"));
    m_list->installEventFilter(this);
    ll->addWidget(m_list, 1);

    m_detail = new QLabel(listPane);
    m_detail->setObjectName("hnMarketDetail");
    m_detail->setAccessibleName(tr("Plugin details"));
    m_detail->setWordWrap(true);
    m_detail->setTextFormat(Qt::PlainText);
    m_detail->setTextInteractionFlags(Qt::TextSelectableByMouse);
    ll->addWidget(m_detail);
    auto *bar = new ui::ActionRow(listPane);
    auto *install = new ui::WrappingButton(tr("Install"), listPane);
    install->setObjectName("hnMarket_install");
    install->setProperty("hnRole", "primary");
    install->setMinimumHeight(32);
    m_buttons.insert("install", install);
    bar->addButton(install);
    ll->addWidget(bar);
    m_hint = new QLabel(listPane);
    m_hint->setObjectName("hnMarketHint");
    m_hint->setProperty("hnRole", "hint");
    m_hint->setWordWrap(true);
    m_hint->setTextFormat(Qt::PlainText);
    ll->addWidget(m_hint);
    m_stack->addWidget(listPane);

    connect(m_buttons["continue"], &QPushButton::clicked, this, [this] {
        if (!m_c->acknowledgeMarketNotice()) {
            // Without the marker the notice would come back every time; say so and stay on the notice, fetching nothing.
            m_notice->findChild<QLabel *>("hnMarketNoticeStatus")->setText(tr("Could not save your choice (the state folder is not writable). Nothing was fetched."));
            m_notice->findChild<QLabel *>("hnMarketNoticeStatus")->show();
            return;
        }
        activate();
    });
    connect(m_buttons["back"], &QPushButton::clicked, this, [this] {
        // Back leaves Browse; the Plugins page owns the tabs and listens for this.
        emit backRequested();
    });
    connect(refresh, &QPushButton::clicked, this, [this] { this->refresh(); });
    connect(install, &QPushButton::clicked, this, [this] { startInstall(); });
    connect(m_search, &QLineEdit::textChanged, this, [this] { applyFilter(); });
    connect(m_tag, &QComboBox::currentIndexChanged, this, [this] { applyFilter(); });
    connect(m_hide, &QCheckBox::toggled, this, [this] { applyFilter(); });
    connect(m_list->selectionModel(), &QItemSelectionModel::currentChanged, this, [this] { updateDetail(); updateButtons(); });
    connect(m_list, &QListView::activated, this, [this] { if (m_buttons["install"]->isEnabled()) m_buttons["install"]->setFocus(); });
    connect(&c->plugins(), &PluginService::changed, this, [this] { if (m_fetched) reloadModel(); });
    connect(ui::themeNotifier(), &ui::ThemeNotifier::changed, this, &MarketPage::restyle);
    m_stack->setCurrentWidget(m_notice);
    restyle();
    updateDetail();
    updateButtons();
}

void MarketPage::activate() {
    if (!m_c->marketNoticeAcknowledged()) { m_stack->setCurrentWidget(m_notice); return; }
    m_stack->setCurrentIndex(1);
    if (!m_swept) {   // leftovers of a crashed download
        m_swept = true;
        QDir d(m_c->cacheDir() + QStringLiteral("/market"));
        for (const QString &f : d.entryList({QStringLiteral("dl-*.hnplugin")}, QDir::Files)) d.remove(f);
    }
    if (m_fetched || m_fetching) return;
    const IndexResult cached = m_c->market().cached();   // creates the client object, not the network
    if (cached.ok) { showIndex(cached); say(tr("Showing the cached list; checking the registry for updates...")); }
    else say(tr("Loading the registry..."));
    refresh();
}

void MarketPage::refresh() {
    if (m_fetching) return;
    m_fetching = true;
    updateButtons();
    QPointer<MarketPage> self(this);
    m_c->market().fetchIndex([self](IndexResult r) {
        if (!self) return;
        self->m_fetching = false;
        self->m_fetched = true;
        self->showIndex(r);
        self->updateButtons();
    });
}

void MarketPage::showIndex(const IndexResult &r) {
    if (!r.ok) {
        m_entries.clear();
        reloadModel();
        say(tr("The plugin registry cannot be reached and nothing is cached yet. Check your connection and press Refresh. (%1)").arg(r.error));
        return;
    }
    m_entries = r.index.entries;
    reloadModel();
    const QString skipped = r.index.skipped ? tr(" %n entry(ies) in the registry were ignored as invalid.", "", r.index.skipped) : QString();
    if (r.fromCache && !r.revalidated)
        say(tr("Showing the cached list from %1; the registry could not be reached. (%2)%3").arg(QLocale().toString(r.cachedAt, QLocale::ShortFormat), r.error, skipped));
    else
        say(tr("Registry ready: %n plugin(s), updated %1.", "", m_entries.size()).arg(r.index.updated) + skipped);
}

void MarketPage::reloadModel() {
    QHash<QString, QString> installed;
    if (auto *mgr = m_c->plugins().managerIfActive())
        for (const auto &p : mgr->plugins()) installed.insert(p.manifest.id, p.manifest.version);
    const QString keep = m_model->entryAt(currentRow()) ? m_model->entryAt(currentRow())->id : QString();
    m_model->setEntries(m_entries, installed);
    const QString tag = m_tag->currentText();
    {
        QSignalBlocker b(m_tag);
        m_tag->clear();
        m_tag->addItem(tr("All tags"), QString());
        for (const QString &t : m_model->allTags()) m_tag->addItem(t, t);
        const int at = m_tag->findText(tag);
        m_tag->setCurrentIndex(at < 0 ? 0 : at);
    }
    applyFilter();
    if (!keep.isEmpty()) select(keep);
    updateDetail();
    updateButtons();
}

void MarketPage::applyFilter() {
    m_model->setFilter(m_search->text(), m_tag->currentData().toString(), m_hide->isChecked());
    if (!m_list->currentIndex().isValid() && m_model->rowCount()) m_list->setCurrentIndex(m_model->index(0));
    updateDetail();
    updateButtons();
}

int MarketPage::currentRow() const { return m_list->currentIndex().isValid() ? m_list->currentIndex().row() : -1; }

void MarketPage::select(const QString &id) {
    for (int r = 0; r < m_model->rowCount(); ++r)
        if (m_model->index(r).data(MarketModel::EntryIdRole).toString() == id) { m_list->setCurrentIndex(m_model->index(r)); return; }
}

void MarketPage::updateDetail() {
    const MarketEntry *e = m_model->entryAt(currentRow());
    if (!e) {
        m_detail->setText(m_model->totalCount() ? tr("No plugin matches the filters.") : QString());
        return;
    }
    QStringList l;
    l << tr("%1 version %2 by %3 (id %4)").arg(e->name, e->version, e->author, e->id);
    l << e->description;
    if (e->permissions.isEmpty()) l << tr("Permissions: none");
    else {
        l << tr("Permissions:");
        for (const QString &p : e->permissions)
            l << QStringLiteral("  %1%2: %3").arg(p, isDangerous(p) ? tr(" (dangerous)") : QString(), describePermission(p));
    }
    if (!e->netHosts.isEmpty()) l << tr("Allowed network hosts: %1").arg(e->netHosts.join(QStringLiteral(", ")));
    l << tr("Size: %1 KiB. Needs Hyprnotes %2 or newer.").arg(qMax<qint64>(1, e->size / 1024)).arg(e->minApp);
    if (e->native()) l << tr("Native plugins need signed releases before they can be installed from the registry. Install is disabled.");
    m_detail->setText(l.join('\n'));
}

// The single gate for Install / Update: the button state and startInstall() both use it.
bool MarketPage::canInstall() const {
    const MarketEntry *e = m_model->entryAt(currentRow());
    const MarketState s = m_model->stateAt(currentRow());
    return e && !e->native() && (s == MarketState::NotInstalled || s == MarketState::UpdateAvailable) && !m_busy;
}

void MarketPage::updateButtons() {
    auto *install = m_buttons["install"];
    const MarketEntry *e = m_model->entryAt(currentRow());
    const MarketState s = m_model->stateAt(currentRow());
    const bool can = canInstall();
    install->setText(s == MarketState::UpdateAvailable ? tr("Update") : tr("Install"));
    install->setEnabled(can);
    install->setAccessibleName(e ? tr("%1 %2").arg(install->text(), e->name) : install->text());
    m_buttons["refresh"]->setEnabled(!m_fetching && !m_busy);
    QString hint;
    if (e && !can && !m_busy) {
        if (e->native()) hint = tr("Native plugins cannot be installed from the registry.");
        else if (s == MarketState::Installed) hint = tr("This version is already installed.");
        else if (s == MarketState::InstalledNewer) hint = tr("A newer version than the registry offers is installed.");
    }
    m_hint->setText(hint);
    m_hint->setVisible(!hint.isEmpty());
}

void MarketPage::say(const QString &m) { m_status->setText(m); }

void MarketPage::startInstall() {
    if (!canInstall()) return;
    const MarketEntry *e = m_model->entryAt(currentRow());
    m_busy = true;
    updateButtons();
    const MarketEntry entry = *e;
    const bool upgrade = m_model->stateAt(currentRow()) == MarketState::UpdateAvailable;
    const QString dest = m_c->cacheDir() + QStringLiteral("/market/dl-") + entry.id + QStringLiteral(".hnplugin");
    say(tr("Downloading %1...").arg(entry.name));
    QPointer<MarketPage> self(this);
    m_c->market().download(entry, dest, [self, entry, dest, upgrade](DownloadResult r) {
        auto cleanup = qScopeGuard([&] {
            QFile::remove(dest);
            if (self) { self->m_busy = false; self->updateButtons(); }
        });
        if (!self) return;
        if (!r.ok) { self->say(r.error); return; }
        const Preflight pf = preflightPackage(dest, entry);
        if (!pf.ok) { self->say(pf.error); return; }
        QString msg;
        const InstallResult res = self->m_c->plugins().install(dest, self->window(), &msg, {upgrade, QStringLiteral("marketplace: ") + entry.url});
        if (!self) return;
        self->say(msg);
        if (res.ok) { self->reloadModel(); emit self->installed(res.id); }
    });
}

bool MarketPage::eventFilter(QObject *w, QEvent *ev) {
    if (w == m_list && ev->type() == QEvent::KeyPress && static_cast<QKeyEvent *>(ev)->key() == Qt::Key_Slash) {
        m_search->setFocus();
        return true;
    }
    return QWidget::eventFilter(w, ev);
}

void MarketPage::resizeEvent(QResizeEvent *e) {
    QWidget::resizeEvent(e);
    m_list->doItemsLayout();   // row heights depend on the width (chips wrap)
}

void MarketPage::restyle() {
    m_list->setStyleSheet(QString("QListView { border: 1px solid %1; }").arg(ui::theme().border.name()));
    m_list->doItemsLayout();
    m_list->viewport()->update();
}

} // namespace hn::app
