#include "plugins_page.h"
#include "consent_dialog.h"
#include "ui_common.h"
#include <QCheckBox>
#include <QDesktopServices>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QShowEvent>
#include <QStyledItemDelegate>
#include <QUrl>
#include <QVBoxLayout>

using namespace hn::plugins;

namespace hn::app {

namespace {
enum Role { IdRole = Qt::UserRole, StatusRole, MetaRole, NativeRole };

QColor statusColor(const QString &chip) {
    const auto &t = ui::theme();
    if (chip == "ENABLED") return t.success;
    if (chip == "NEEDS CONSENT") return t.accent;
    if (chip == "FAILED") return t.danger;
    return t.muted;
}

// Hairline chip painted at (x, y) with the given colour; returns its width.
int paintChip(QPainter *p, int x, int y, const QString &text, const QColor &c, bool filled = false) {
    const QFont f = ui::labelFont(9);
    p->setFont(f);
    const int w = QFontMetrics(f).horizontalAdvance(text) + 14;
    const QRect r(x, y, w, 18);
    if (filled) p->fillRect(r, c);
    else { p->setPen(QPen(c, 1)); p->drawRect(r.adjusted(0, 0, -1, -1)); }
    p->setPen(filled ? ui::theme().bg : c);
    p->drawText(r, Qt::AlignCenter, text);
    return w;
}

class PluginDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override { return {200, 72}; }
    void paint(QPainter *p, const QStyleOptionViewItem &o, const QModelIndex &i) const override {
        const auto &t = ui::theme();
        p->save();
        const bool sel = o.state & QStyle::State_Selected;
        p->fillRect(o.rect, sel ? t.selection : t.bg);
        p->fillRect(QRect(o.rect.left(), o.rect.bottom(), o.rect.width(), 1), t.border);
        if (sel) p->fillRect(QRect(o.rect.left(), o.rect.top(), 3, o.rect.height()), t.accent);
        const QRect in = o.rect.adjusted(14, 8, -10, -8);
        p->setFont(ui::uiFont(13, QFont::Bold));
        p->setPen(t.text);
        p->drawText(QRect(in.left(), in.top(), in.width(), 18), Qt::AlignVCenter, QFontMetrics(p->font()).elidedText(i.data(Qt::DisplayRole).toString(), Qt::ElideRight, in.width()));
        p->setFont(ui::uiFont(11));
        p->setPen(t.muted);
        p->drawText(QRect(in.left(), in.top() + 19, in.width(), 14), Qt::AlignVCenter, QFontMetrics(p->font()).elidedText(i.data(MetaRole).toString(), Qt::ElideRight, in.width()));
        const QString chip = i.data(StatusRole).toString();
        int x = in.left();
        x += paintChip(p, x, in.bottom() - 17, chip, statusColor(chip), chip == "ENABLED" || chip == "FAILED") + 6;
        if (i.data(NativeRole).toBool()) paintChip(p, x, in.bottom() - 17, QObject::tr("NATIVE"), t.danger);
        p->restore();
    }
};

QLabel *sectionLabel(const QString &t, QWidget *p) {
    auto *l = new QLabel(t, p);
    l->setFont(ui::labelFont(10));
    l->setStyleSheet(QString("color: %1;").arg(ui::theme().muted.name()));
    return l;
}
}  // namespace

QString PluginsPage::chipFor(Status s) {
    switch (s) {
    case Status::Enabled: return QObject::tr("ENABLED");
    case Status::Disabled: return QObject::tr("DISABLED");
    case Status::NeedsConsent: return QObject::tr("NEEDS CONSENT");
    case Status::Failed: return QObject::tr("FAILED");
    }
    return {};
}

PluginsPage::PluginsPage(AppController *c, QWidget *parent) : QWidget(parent), m_c(c) {
    const auto &t = ui::theme();
    setAcceptDrops(false);   // the settings dialog owns drag and drop
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(24, 16, 24, 16);
    root->setSpacing(8);

    auto *warn = new QFrame(this);
    warn->setObjectName("hnPluginsWarning");
    warn->setStyleSheet(QString("QFrame#hnPluginsWarning { background: %1; border: 1px solid %2; border-left: 6px solid %2; }").arg(t.surface.name(), t.danger.name()));
    auto *wl = new QHBoxLayout(warn);
    wl->setContentsMargins(10, 8, 12, 8);
    wl->setSpacing(10);
    auto *wi = new QLabel(warn);
    wi->setPixmap(warningIcon(t.danger, 18).pixmap(18, 18));
    wi->setStyleSheet("background: transparent; border: none;");
    m_warning = new QLabel(ConsentDialog::warningText(), warn);
    m_warning->setWordWrap(true);
    m_warning->setStyleSheet("background: transparent; border: none;");
    m_warning->setObjectName("hnPluginsWarningText");
    wl->addWidget(wi, 0, Qt::AlignTop);
    wl->addWidget(m_warning, 1);
    root->addWidget(warn);

    auto *bar = new QHBoxLayout;
    auto *install = new QPushButton(tr("Install plugin…"), this);
    install->setObjectName("hnPluginInstall");
    install->setStyleSheet(ui::accentButtonStyle() + "QPushButton { min-height: 32px; text-align: center; }");
    auto *menu = new QMenu(install);
    connect(menu->addAction(tr("Plugin file (.hnplugin)…")), &QAction::triggered, this, [this] {
        const QString p = QFileDialog::getOpenFileName(this, tr("Install plugin"), QString(), tr("Hyprnotes plugin (*.hnplugin *.zip *.tar *.tar.gz)"));
        if (!p.isEmpty()) installPath(p);
    });
    connect(menu->addAction(tr("Plugin folder…")), &QAction::triggered, this, [this] {
        const QString p = QFileDialog::getExistingDirectory(this, tr("Install plugin from folder"));
        if (!p.isEmpty()) installPath(p);
    });
    install->setMenu(menu);
    m_buttons.insert("install", install);
    m_note = new QLabel(this);
    m_note->setWordWrap(true);
    m_note->setObjectName("hnPluginsNote");
    bar->addWidget(install);
    bar->addSpacing(8);
    bar->addWidget(m_note, 1);
    root->addLayout(bar);
    auto *dropHint = new QLabel(tr("Or drop a .hnplugin file or a plugin folder on this window."), this);
    dropHint->setStyleSheet(QString("color: %1;").arg(t.muted.name()));
    root->addWidget(dropHint);

    auto *row = new QHBoxLayout;
    row->setSpacing(0);
    m_list = new QListWidget(this);
    m_list->setObjectName("hnPluginList");
    m_list->setItemDelegate(new PluginDelegate(m_list));
    m_list->setFixedWidth(248);
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setStyleSheet(QString("QListWidget { border: 1px solid %1; }").arg(t.border.name()));
    m_list->setAccessibleName(tr("Installed plugins"));
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);   // labels wrap instead
    m_detail = new QWidget;
    m_detailLay = new QVBoxLayout(m_detail);
    m_detailLay->setContentsMargins(20, 0, 4, 0);
    m_detailLay->setSpacing(8);
    scroll->setWidget(m_detail);
    row->addWidget(m_list);
    row->addWidget(scroll, 1);
    root->addLayout(row, 1);

    connect(m_list, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *cur) {
        m_sel = cur ? cur->data(IdRole).toString() : QString();
        rebuildDetail();
    });
    connect(&c->plugins(), &PluginService::changed, this, [this] { if (m_loaded) refresh(); });
}

void PluginsPage::showEvent(QShowEvent *e) {
    QWidget::showEvent(e);
    ensureLoaded();
}

void PluginsPage::ensureLoaded() {
    if (m_loaded) return;
    m_loaded = true;
    m_c->plugins().manager();   // first use: scan manifests + migrate legacy mods
    refresh();
    if (const int n = m_c->plugins().legacyModsAwaitingApproval(); n > 0)
        say(tr("%n mod(s) from your old mods list are listed below as native plugins and need approval before they run.", "", n));
}

void PluginsPage::say(const QString &m) { m_note->setText(m); }

QString PluginsPage::statusText(const QString &id) const {
    auto *mgr = m_c->plugins().managerIfActive();
    return mgr ? chipFor(mgr->info(id).status) : QString();
}

void PluginsPage::select(const QString &id) {
    for (int r = 0; r < m_list->count(); ++r)
        if (m_list->item(r)->data(IdRole).toString() == id) { m_list->setCurrentRow(r); return; }
}

void PluginsPage::refresh() {
    auto *mgr = m_c->plugins().managerIfActive();
    if (!mgr) return;
    const QString keep = m_sel;
    QSignalBlocker b(m_list);
    m_list->clear();
    QList<PluginInfo> infos = mgr->plugins();
    for (const auto &i : std::as_const(infos)) {
        auto *it = new QListWidgetItem(i.manifest.name.isEmpty() ? i.manifest.id : i.manifest.name, m_list);
        it->setData(IdRole, i.manifest.id);
        it->setData(StatusRole, chipFor(i.status));
        it->setData(MetaRole, tr("v%1 by %2").arg(i.manifest.version.isEmpty() ? QStringLiteral("?") : i.manifest.version, i.manifest.author.isEmpty() ? tr("unknown") : i.manifest.author));
        it->setData(NativeRole, i.manifest.tier == Tier::Native);
        it->setSizeHint(QSize(200, 72));
        it->setToolTip(i.detail);
    }
    QString show = keep;
    bool found = false;
    for (int r = 0; r < m_list->count(); ++r) if (m_list->item(r)->data(IdRole).toString() == show) { m_list->setCurrentRow(r); found = true; }
    if (!found && m_list->count()) { m_list->setCurrentRow(0); show = m_list->item(0)->data(IdRole).toString(); }
    m_sel = m_list->count() ? show : QString();
    rebuildDetail();
}

void PluginsPage::rebuildDetail() {
    while (QLayoutItem *it = m_detailLay->takeAt(0)) { if (auto *w = it->widget()) w->deleteLater(); delete it; }
    m_permLabels.clear();
    m_regLabels.clear();
    m_settingEditors.clear();
    for (const char *n : {"enable", "disable", "reload", "remove", "folder", "audit"}) m_buttons.remove(n);
    auto *mgr = m_c->plugins().managerIfActive();
    const auto &t = ui::theme();
    if (!mgr || m_sel.isEmpty()) {
        auto *l = new QLabel(tr("No plugins installed.\n\nUse Install plugin… or run  hyprnotes --new-plugin NAME  to start your own."), m_detail);
        l->setStyleSheet(QString("color: %1; padding-top: 24px;").arg(t.muted.name()));
        l->setWordWrap(true);
        m_detailLay->addWidget(l);
        m_detailLay->addStretch(1);
        return;
    }
    const PluginInfo info = mgr->info(m_sel);
    const Manifest &m = info.manifest;
    const bool native = m.tier == Tier::Native;
    const QString chip = chipFor(info.status);

    auto *title = new QLabel(m.name.isEmpty() ? m.id : m.name, m_detail);
    title->setFont(ui::uiFont(22, QFont::Bold));
    title->setWordWrap(true);
    title->setObjectName("hnPluginTitle");
    m_detailLay->addWidget(title);
    auto *sub = new QLabel(tr("Version %1 by %2   |   id %3").arg(m.version, m.author, m.id), m_detail);
    sub->setStyleSheet(QString("color: %1;").arg(t.muted.name()));
    sub->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_detailLay->addWidget(sub);

    auto *chips = new QLabel(m_detail);   // chips are painted into a pixmap so they match the list exactly
    QPixmap pm(360, 22);
    pm.fill(Qt::transparent);
    {
        QPainter p(&pm);
        int x = 0;
        x += paintChip(&p, x, 2, native ? tr("NATIVE") : tr("SCRIPT"), native ? t.danger : t.muted, native) + 6;
        paintChip(&p, x, 2, chip, statusColor(chip), chip == "ENABLED" || chip == "FAILED");
    }
    chips->setPixmap(pm);
    chips->setAccessibleName(tr("Tier %1, status %2").arg(native ? tr("native") : tr("script"), chip));
    m_detailLay->addWidget(chips);

    if (!m.description.isEmpty()) { auto *d = new QLabel(m.description, m_detail); d->setWordWrap(true); m_detailLay->addWidget(d); }
    if (!info.detail.isEmpty()) {
        auto *d = new QLabel(info.detail, m_detail);
        d->setWordWrap(true);
        d->setObjectName("hnPluginDetail");
        d->setStyleSheet(QString("color: %1; font-weight: 600;").arg(info.status == Status::Failed ? t.danger.name() : t.accent.name()));
        m_detailLay->addWidget(d);
    }
    if (native) {
        auto *n = new QLabel(ConsentDialog::nativeWarningText(), m_detail);
        n->setWordWrap(true);
        n->setStyleSheet(QString("color: %1; font-weight: 700;").arg(t.danger.name()));
        m_detailLay->addWidget(n);
    }

    m_detailLay->addWidget(sectionLabel(m.permissions.isEmpty() ? tr("PERMISSIONS: NONE") : tr("PERMISSIONS"), m_detail));
    for (const QString &p : m.permissions) {
        const bool danger = isDangerousPermission(p);
        auto *l = new QLabel(QStringLiteral("%1  %2").arg(p, permissionDescription(p)), m_detail);
        l->setWordWrap(true);
        l->setProperty("permission", p);
        l->setProperty("danger", danger);
        l->setAccessibleName(tr("%1 permission%2").arg(p, danger ? tr(" (dangerous)") : QString()));
        l->setStyleSheet(danger ? QString("color: %1; font-weight: 700; padding-left: 4px; border-left: 3px solid %1;").arg(t.danger.name())
                                : QString("padding-left: 4px; border-left: 3px solid %1;").arg(t.border.name()));
        m_permLabels << l;
        m_detailLay->addWidget(l);
    }
    if (const PluginRegs *regs = mgr->registry()->of(m.id); regs && info.status == Status::Enabled) {   // what the plugin added to the app (cached, no Lua needed)
        QStringList rows;
        for (const auto &p : regs->panels) rows << tr("Panel: %1").arg(p.title);
        QStringList trig;
        for (const auto &c : regs->completions) trig << QStringLiteral("\"%1\"").arg(c.trigger);
        if (!trig.isEmpty()) rows << tr("Completion popup after %1").arg(trig.join(QStringLiteral(", ")));
        if (!regs->linkHandlers.isEmpty()) rows << tr("Handles clicks on [[links]]");
        if (!rows.isEmpty()) {
            m_detailLay->addWidget(sectionLabel(tr("ADDS TO THE APP"), m_detail));
            for (const QString &r : rows) {
                auto *l = new QLabel(r, m_detail);
                l->setWordWrap(true);
                l->setStyleSheet(QString("padding-left: 4px; border-left: 3px solid %1;").arg(t.accent.name()));
                m_regLabels << l;
                m_detailLay->addWidget(l);
            }
        }
    }
    if (!m.netHosts.isEmpty()) {
        auto *h = new QLabel(tr("Allowed network hosts: %1").arg(m.netHosts.join(QStringLiteral(", "))), m_detail);
        h->setWordWrap(true);
        h->setTextInteractionFlags(Qt::TextSelectableByMouse);
        h->setStyleSheet(QString("color: %1; font-weight: 600;").arg(t.danger.name()));
        m_detailLay->addWidget(h);
    }

    // settings (hn.setting): editable while the plugin is enabled, because values live in its declared state
    const auto settings = mgr->registry()->settings(m.id);
    if (!settings.isEmpty() && info.status == Status::Enabled) {
        m_detailLay->addWidget(sectionLabel(tr("SETTINGS"), m_detail));
        for (const auto &s : settings) {
            auto *lab = new QLabel(s.title, m_detail);
            m_detailLay->addWidget(lab);
            const QVariant cur = mgr->host()->setting(m.id, s.id);
            const QString pid = m.id, sid = s.id;
            QWidget *ed = nullptr;
            if (s.type == "bool") {
                auto *cb = new QCheckBox(s.title, m_detail);
                lab->hide();
                cb->setChecked(cur.toBool());
                connect(cb, &QCheckBox::toggled, this, [mgr, pid, sid](bool v) { mgr->host()->setSetting(pid, sid, v); });
                ed = cb;
            } else if (s.type == "number") {
                auto *sp = new QDoubleSpinBox(m_detail);
                sp->setRange(-1e9, 1e9);
                sp->setDecimals(4);
                sp->setValue(cur.toDouble());
                connect(sp, &QDoubleSpinBox::editingFinished, this, [mgr, pid, sid, sp] { mgr->host()->setSetting(pid, sid, sp->value()); });
                ed = sp;
            } else {
                auto *le = new QLineEdit(cur.toString(), m_detail);
                connect(le, &QLineEdit::editingFinished, this, [mgr, pid, sid, le] { mgr->host()->setSetting(pid, sid, le->text()); });
                ed = le;
            }
            ed->setProperty("settingId", s.id);
            ed->setAccessibleName(s.title);
            m_settingEditors << ed;
            m_detailLay->addWidget(ed);
        }
    }

    // actions
    auto *acts = new QWidget(m_detail);
    auto *col = new QVBoxLayout(acts);
    col->setContentsMargins(0, 12, 0, 0);
    col->setSpacing(8);
    auto *al = new QHBoxLayout, *al2 = new QHBoxLayout;   // main actions, then housekeeping
    al->setSpacing(8);
    al2->setSpacing(8);
    col->addLayout(al);
    col->addLayout(al2);
    auto add = [&](const char *name, const QString &text, const QString &tip, std::function<void()> fn, bool primary = false, bool danger = false, bool second = false) {
        auto *b = new QPushButton(text, acts);
        b->setObjectName(QStringLiteral("hnPlugin_") + QLatin1String(name));
        b->setToolTip(tip);
        b->setMinimumHeight(32);
        if (primary) b->setStyleSheet(ui::accentButtonStyle() + "QPushButton { min-height: 32px; text-align: center; }");
        else if (danger) b->setStyleSheet(QString("QPushButton { color: %1; border: 1px solid %1; } QPushButton:hover { background: %1; color: %2; border-color: %1; }").arg(t.danger.name(), t.bg.name()));
        connect(b, &QPushButton::clicked, this, [fn] { fn(); });
        m_buttons.insert(name, b);
        (second ? al2 : al)->addWidget(b);
    };
    const QString id = m.id;
    if (info.status == Status::Enabled) add("disable", tr("Disable"), tr("Stop this plugin"), [this, mgr, id] { mgr->disable(id); say(tr("Disabled.")); refresh(); });
    else add("enable", tr("Enable…"), tr("Review the permissions, then enable"), [this, id] { QString msg; m_c->plugins().enable(id, window(), &msg); say(msg); refresh(); }, true);
    add("reload", tr("Reload"), tr("Developer: re-read the plugin files. Edits that add no permissions are accepted; anything else asks again."),
        [this, mgr, id] { QString err; const bool ok = mgr->reload(id, true, &err); say(ok ? tr("Reloaded.") : tr("Reload: %1").arg(err)); refresh(); });
    add("folder", tr("Open folder"), m.dir, [m] { QDesktopServices::openUrl(QUrl::fromLocalFile(m.dir)); }, false, false, true);
    add("audit", tr("View audit log"), tr("Installs, approvals, denials, network calls and failures"), [this, id] { showAudit(id); }, false, false, true);
    add("remove", tr("Remove…"), tr("Delete this plugin and its stored data"), [this, mgr, id, m] {
        const QString q = tr("Remove %1 and delete its stored data?").arg(m.name);
        const bool yes = m_c->plugins().hooks().confirm ? m_c->plugins().hooks().confirm(m.name, q) : QMessageBox::question(this, tr("Remove plugin"), q) == QMessageBox::Yes;
        if (!yes) return;
        QString err;
        say(mgr->remove(id, &err) ? tr("Removed %1.").arg(m.name) : tr("Could not remove: %1").arg(err));
        m_sel.clear();
        refresh();
    }, false, true, true);
    al->addStretch(1);
    al2->addStretch(1);
    m_detailLay->addWidget(acts);
    m_detailLay->addStretch(1);
}

bool PluginsPage::installPath(const QString &path) {
    QString msg;
    const InstallResult r = m_c->plugins().install(path, window(), &msg);
    say(msg);
    m_loaded = true;
    refresh();
    if (r.ok) select(r.id);
    return r.ok;
}

void PluginsPage::showAudit(const QString &id) {
    auto *d = new QDialog(this);
    d->setObjectName("hnAuditDialog");
    d->setAttribute(Qt::WA_DeleteOnClose);
    d->setWindowTitle(tr("Plugin audit log"));
    d->resize(720, 420);
    auto *l = new QVBoxLayout(d);
    auto *ed = new QPlainTextEdit(d);
    ed->setObjectName("hnAuditText");
    ed->setReadOnly(true);
    ed->setLineWrapMode(QPlainTextEdit::NoWrap);
    QFont mono = ed->font();
    mono.setFamily(ui::theme().monoFamily.isEmpty() ? QStringLiteral("monospace") : ui::theme().monoFamily);
    ed->setFont(mono);
    QStringList lines;
    for (const QString &ln : m_c->plugins().auditText().split('\n')) if (id.isEmpty() || ln.contains(id)) lines << ln;
    ed->setPlainText(lines.isEmpty() ? tr("No audit entries for this plugin.") : lines.join('\n'));
    l->addWidget(ed);
    d->show();
}

} // namespace hn::app
