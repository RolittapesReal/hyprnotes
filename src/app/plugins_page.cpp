#include "plugins_page.h"
#include "action_row.h"
#include "consent_dialog.h"
#include "ui_common.h"
#include <QCheckBox>
#include <QDesktopServices>
#include <QDoubleSpinBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QShowEvent>
#include <QResizeEvent>
#include <QStyledItemDelegate>
#include <QUrl>
#include <QVBoxLayout>

using namespace hn::plugins;

namespace hn::app {

namespace {
enum Role { IdRole = Qt::UserRole, StatusRole, MetaRole, NativeRole };

// Keep QLabel wrapping and mnemonic support, but give the whole caption the
// same press/release activation as its checkbox indicator.
class CheckCaption : public QLabel {
public:
    using QLabel::QLabel;
protected:
    void mousePressEvent(QMouseEvent *event) override {
        auto *check = qobject_cast<QCheckBox *>(buddy());
        if (event->button() == Qt::LeftButton && check && check->isEnabled()) {
            m_pressed = true;
            check->setFocus(Qt::MouseFocusReason);
            check->setDown(true);
            event->accept();
            return;
        }
        QLabel::mousePressEvent(event);
    }
    void mouseMoveEvent(QMouseEvent *event) override {
        if (m_pressed) {
            if (auto *check = qobject_cast<QCheckBox *>(buddy())) check->setDown(rect().contains(event->position().toPoint()));
            event->accept();
            return;
        }
        QLabel::mouseMoveEvent(event);
    }
    void mouseReleaseEvent(QMouseEvent *event) override {
        if (event->button() == Qt::LeftButton && m_pressed) {
            m_pressed = false;
            if (auto *check = qobject_cast<QCheckBox *>(buddy())) {
                check->setDown(false);
                if (check->isEnabled() && rect().contains(event->position().toPoint())) check->click();
            }
            event->accept();
            return;
        }
        QLabel::mouseReleaseEvent(event);
    }
private:
    bool m_pressed = false;
};

QColor statusColor(const QString &chip) {
    const auto &t = ui::theme();
    if (chip == "ENABLED") return t.success;
    if (chip == "NEEDS CONSENT") return t.accent;
    if (chip == "FAILED") return t.danger;
    return t.muted;
}

// Hairline chip painted at (x, y) with the given colour; returns its width.
QFont chipFont() { return ui::labelFont(qMax(10, ui::theme().baseSize - 3)); }
QSize chipSize(const QString &text) {
    const QFontMetrics fm(chipFont());
    return {fm.horizontalAdvance(text) + 16, fm.height() + 8};
}
int paintChip(QPainter *p, int x, int y, const QString &text, const QColor &c, bool filled = false) {
    const QFont f = chipFont();
    p->setFont(f);
    const QSize size = chipSize(text);
    const int w = size.width();
    const QRect r(QPoint(x, y), size);
    if (filled) p->fillRect(r, c);
    else { p->setPen(QPen(c, 1)); p->drawRect(r.adjusted(0, 0, -1, -1)); }
    p->setPen(filled ? ui::theme().bg : c);
    p->drawText(r, Qt::AlignCenter, text);
    return w;
}

class PluginDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &i) const override {
        const int base = ui::theme().baseSize;
        return {200, QFontMetrics(ui::uiFont(base, QFont::Bold)).height()
            + QFontMetrics(ui::uiFont(qMax(11, base - 2))).height()
            + (i.data(NativeRole).toBool() ? 2 : 1) * (chipSize(i.data(StatusRole).toString()).height() + 4) + 24};
    }
    void paint(QPainter *p, const QStyleOptionViewItem &o, const QModelIndex &i) const override {
        const auto &t = ui::theme();
        p->save();
        const bool sel = o.state & QStyle::State_Selected;
        p->fillRect(o.rect, sel ? t.selection : t.bg);
        p->fillRect(QRect(o.rect.left(), o.rect.bottom(), o.rect.width(), 1), t.border);
        if (sel) p->fillRect(QRect(o.rect.left(), o.rect.top(), 3, o.rect.height()), t.accent);
        const QRect in = o.rect.adjusted(14, 8, -10, -8);
        p->setFont(ui::uiFont(t.baseSize, QFont::Bold));
        p->setPen(t.text);
        const int nameH = p->fontMetrics().height();
        p->drawText(QRect(in.left(), in.top(), in.width(), nameH), Qt::AlignVCenter, QFontMetrics(p->font()).elidedText(i.data(Qt::DisplayRole).toString(), Qt::ElideRight, in.width()));
        p->setFont(ui::uiFont(qMax(11, t.baseSize - 2)));
        p->setPen(t.muted);
        const int metaH = p->fontMetrics().height();
        p->drawText(QRect(in.left(), in.top() + nameH + 4, in.width(), metaH), Qt::AlignVCenter, QFontMetrics(p->font()).elidedText(i.data(MetaRole).toString(), Qt::ElideRight, in.width()));
        const QString chip = i.data(StatusRole).toString();
        const int y = in.top() + nameH + metaH + 8;
        paintChip(p, in.left(), y, chip, statusColor(chip), chip == "ENABLED" || chip == "FAILED");
        if (i.data(NativeRole).toBool()) paintChip(p, in.left(), y + chipSize(chip).height() + 4, QObject::tr("NATIVE"), t.danger);
        p->restore();
    }
};

QLabel *sectionLabel(const QString &t, QWidget *p) {
    auto *l = new QLabel(t, p);
    l->setProperty("hnRole", "section");
    l->setWordWrap(true);
    l->setTextFormat(Qt::PlainText);
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
    setAcceptDrops(false);   // the settings dialog owns drag and drop
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    auto *outer = new QScrollArea(this);
    outer->setObjectName("hnPluginsScroll");
    outer->setWidgetResizable(true);
    outer->setFrameShape(QFrame::NoFrame);
    outer->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *content = new QWidget;
    auto *root = new QVBoxLayout(content);
    root->setContentsMargins(24, 16, 24, 16);
    root->setSpacing(8);
    outer->setWidget(content);
    layout->addWidget(outer);

    auto *warn = new QFrame(this);
    m_warningFrame = warn;
    warn->setObjectName("hnPluginsWarning");
    auto *wl = new QHBoxLayout(warn);
    wl->setContentsMargins(10, 8, 12, 8);
    wl->setSpacing(10);
    auto *wi = new QLabel(warn);
    m_warningIcon = wi;
    wi->setStyleSheet("background: transparent; border: none;");
    m_warning = new QLabel(ConsentDialog::warningText(), warn);
    m_warning->setWordWrap(true);
    m_warning->setStyleSheet("background: transparent; border: none;");
    m_warning->setObjectName("hnPluginsWarningText");
    wl->addWidget(wi, 0, Qt::AlignTop);
    wl->addWidget(m_warning, 1);
    root->addWidget(warn);

    auto *bar = new ui::ActionRow(this);
    auto *install = new ui::WrappingButton(tr("Install plugin…"), this);
    install->setObjectName("hnPluginInstall");
    install->setProperty("hnRole", "primary");
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
    m_note->setTextFormat(Qt::PlainText);
    m_note->setObjectName("hnPluginsNote");
    bar->addButton(install);
    root->addWidget(bar);
    root->addWidget(m_note);
    m_note->hide();
    auto *dropHint = new QLabel(tr("Or drop a .hnplugin file or a plugin folder on this window."), this);
    dropHint->setProperty("hnRole", "hint");
    dropHint->setWordWrap(true);
    root->addWidget(dropHint);

    auto *row = new QBoxLayout(QBoxLayout::LeftToRight);
    m_row = row;
    row->setSpacing(0);
    m_list = new QListWidget(this);
    m_list->setObjectName("hnPluginList");
    m_list->setItemDelegate(new PluginDelegate(m_list));
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setAccessibleName(tr("Installed plugins"));
    auto *scroll = new QScrollArea(this);
    scroll->setObjectName("hnPluginDetailScroll");
    scroll->setMinimumHeight(240);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);   // labels wrap instead
    m_detail = new QWidget;
    m_detail->installEventFilter(this);
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
    connect(ui::themeNotifier(), &ui::ThemeNotifier::changed, this, &PluginsPage::restyle);
    restyle();
}

void PluginsPage::arrange() {
    const bool narrow = width() < 800;
    m_row->setDirection(narrow ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
    m_row->setSpacing(narrow ? 8 : 0);
    m_list->setMinimumSize(0, 0);
    m_list->setMaximumSize(narrow ? QWIDGETSIZE_MAX : qBound(180, width() / 3, 360),
                           narrow ? qMin(180, QFontMetrics(ui::uiFont(ui::theme().baseSize)).height() * 3 + 32) : QWIDGETSIZE_MAX);
    m_list->setMinimumHeight(narrow ? m_list->maximumHeight() : 160);
    m_detailLay->setContentsMargins(narrow ? 0 : 20, 0, 4, 0);
}

void PluginsPage::resizeEvent(QResizeEvent *e) {
    QWidget::resizeEvent(e);
    arrange();
}

bool PluginsPage::eventFilter(QObject *watched, QEvent *event) {
    if (watched == m_detail && event->type() == QEvent::Resize) updateChips();
    return QWidget::eventFilter(watched, event);
}

void PluginsPage::updateChips() {
    if (!m_chips) return;
    const auto &t = ui::theme();
    const QString tier = m_native ? tr("NATIVE") : tr("SCRIPT");
    const QSize first = chipSize(tier), second = chipSize(m_chip);
    const int available = qMax(1, m_detail->contentsRect().width() - m_detailLay->contentsMargins().left() - m_detailLay->contentsMargins().right());
    const bool wrap = first.width() + 8 + second.width() > available;
    QPixmap pm(wrap ? qMax(first.width(), second.width()) : first.width() + 8 + second.width(),
               wrap ? first.height() + 8 + second.height() : qMax(first.height(), second.height()));
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    paintChip(&p, 0, 0, tier, m_native ? t.danger : t.muted, m_native);
    paintChip(&p, wrap ? 0 : first.width() + 8, wrap ? first.height() + 8 : 0, m_chip, statusColor(m_chip), m_chip == "ENABLED" || m_chip == "FAILED");
    p.end();
    m_chips->setPixmap(pm);
}

void PluginsPage::restyle() {
    const auto &t = ui::theme();
    m_warningFrame->setStyleSheet(QString("QFrame#hnPluginsWarning { background: %1; border: 1px solid %2; border-left: 6px solid %2; }").arg(t.surface.name(), t.danger.name()));
    const int px = qMax(18, t.baseSize);
    m_warningIcon->setPixmap(warningIcon(t.danger, px).pixmap(px, px));
    m_list->setStyleSheet(QString("QListWidget { border: 1px solid %1; }").arg(t.border.name()));
    for (auto *l : std::as_const(m_permLabels))
        l->setStyleSheet(QString("padding-left: 4px; border-left: 3px solid %1;").arg(l->property("danger").toBool() ? t.danger.name() : t.border.name()));
    for (auto *l : std::as_const(m_regLabels))
        l->setStyleSheet(QString("padding-left: 4px; border-left: 3px solid %1;").arg(t.accent.name()));
    for (auto *button : std::as_const(m_buttons))
        if (button->property("hnDanger").toBool())
            button->setStyleSheet(QString("QPushButton { color: %1; border-color: %1; } QPushButton:hover { background: %1; color: %2; }").arg(t.danger.name(), t.bg.name()));
    arrange();
    updateChips();
    m_list->doItemsLayout();
    m_list->viewport()->update();
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

void PluginsPage::say(const QString &m) { m_note->setText(m); m_note->setVisible(!m.isEmpty()); }

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
    m_chips = nullptr;
    for (const char *n : {"enable", "disable", "reload", "remove", "folder", "audit"}) m_buttons.remove(n);
    auto *mgr = m_c->plugins().managerIfActive();
    if (!mgr || m_sel.isEmpty()) {
        auto *l = new QLabel(tr("No plugins installed.\n\nUse Install plugin… or run  hyprnotes --new-plugin NAME  to start your own."), m_detail);
        l->setProperty("hnRole", "hint");
        l->setWordWrap(true);
        m_detailLay->addWidget(l);
        m_detailLay->addStretch(1);
        return;
    }
    const PluginInfo info = mgr->info(m_sel);
    const Manifest &m = info.manifest;
    const bool native = m.tier == Tier::Native;
    const QString chip = chipFor(info.status);
    m_native = native;
    m_chip = chip;

    auto *title = new QLabel(m.name.isEmpty() ? m.id : m.name, m_detail);
    title->setProperty("hnRole", "heading");
    title->setTextFormat(Qt::PlainText);
    title->setWordWrap(true);
    title->setObjectName("hnPluginTitle");
    m_detailLay->addWidget(title);
    auto *sub = new QLabel(tr("Version %1 by %2   |   id %3").arg(m.version, m.author, m.id), m_detail);
    sub->setProperty("hnRole", "hint");
    sub->setTextFormat(Qt::PlainText);
    sub->setWordWrap(true);
    sub->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_detailLay->addWidget(sub);

    auto *chips = new QLabel(m_detail);   // chips are painted into a pixmap so they match the list exactly
    m_chips = chips;
    chips->setObjectName("hnPluginChips");
    chips->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    chips->setAccessibleName(tr("Tier %1, status %2").arg(native ? tr("native") : tr("script"), chip));
    m_detailLay->addWidget(chips);

    if (!m.description.isEmpty()) { auto *d = new QLabel(m.description, m_detail); d->setTextFormat(Qt::PlainText); d->setWordWrap(true); m_detailLay->addWidget(d); }
    if (!info.detail.isEmpty()) {
        auto *d = new QLabel(info.detail, m_detail);
        d->setWordWrap(true);
        d->setObjectName("hnPluginDetail");
        d->setProperty("hnRole", info.status == Status::Failed ? "danger" : "accent");
        d->setTextFormat(Qt::PlainText);
        m_detailLay->addWidget(d);
    }
    if (native) {
        auto *n = new QLabel(ConsentDialog::nativeWarningText(), m_detail);
        n->setWordWrap(true);
        n->setProperty("hnRole", "danger");
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
        if (danger) l->setProperty("hnRole", "danger");
        l->setTextFormat(Qt::PlainText);
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
                l->setTextFormat(Qt::PlainText);
                m_regLabels << l;
                m_detailLay->addWidget(l);
            }
        }
    }
    if (!m.netHosts.isEmpty()) {
        auto *h = new QLabel(tr("Allowed network hosts: %1").arg(m.netHosts.join(QStringLiteral(", "))), m_detail);
        h->setWordWrap(true);
        h->setTextInteractionFlags(Qt::TextSelectableByMouse);
        h->setProperty("hnRole", "danger");
        h->setTextFormat(Qt::PlainText);
        m_detailLay->addWidget(h);
    }

    // settings (hn.setting): editable while the plugin is enabled, because values live in its declared state
    const auto settings = mgr->registry()->settings(m.id);
    if (!settings.isEmpty() && info.status == Status::Enabled) {
        m_detailLay->addWidget(sectionLabel(tr("SETTINGS"), m_detail));
        for (const auto &s : settings) {
            QLabel *lab = s.type == "bool" ? new CheckCaption(s.title, m_detail) : new QLabel(s.title, m_detail);
            lab->setWordWrap(true);
            lab->setTextFormat(Qt::PlainText);
            m_detailLay->addWidget(lab);
            const QVariant cur = mgr->host()->setting(m.id, s.id);
            const QString pid = m.id, sid = s.id;
            QWidget *ed = nullptr;
            QWidget *field = nullptr;
            if (s.type == "bool") {
                // QCheckBox text cannot wrap. Keep its full label beside the indicator.
                field = new QWidget(m_detail);
                auto *row = new QHBoxLayout(field);
                row->setContentsMargins(0, 0, 0, 0);
                row->setSpacing(8);
                auto *cb = new QCheckBox(field);
                m_detailLay->removeWidget(lab);
                row->addWidget(cb, 0, Qt::AlignTop);
                row->addWidget(lab, 1);
                lab->setBuddy(cb);
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
            m_detailLay->addWidget(field ? field : ed);
        }
    }

    // actions
    auto *acts = new QWidget(m_detail);
    auto *col = new QVBoxLayout(acts);
    col->setContentsMargins(0, 12, 0, 0);
    col->setSpacing(8);
    auto *al = new ui::ActionRow(acts), *al2 = new ui::ActionRow(acts);
    col->addWidget(al);
    col->addWidget(al2);
    auto add = [&](const char *name, const QString &text, const QString &tip, std::function<void()> fn, bool primary = false, bool danger = false, bool second = false) {
        auto *b = new ui::WrappingButton(text, acts);
        b->setObjectName(QStringLiteral("hnPlugin_") + QLatin1String(name));
        b->setToolTip(tip);
        b->setMinimumHeight(32);
        if (primary) b->setProperty("hnRole", "primary");
        b->setProperty("hnDanger", danger);
        connect(b, &QPushButton::clicked, this, [fn] { fn(); });
        m_buttons.insert(name, b);
        (second ? al2 : al)->addButton(b);
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
        bool yes;
        if (m_c->plugins().hooks().confirm) yes = m_c->plugins().hooks().confirm(m.name, q);
        else {
            QMessageBox box(QMessageBox::Question, tr("Remove plugin"), tr("Remove plugin"), QMessageBox::Yes | QMessageBox::No, this);
            box.setTextFormat(Qt::PlainText);
            box.setInformativeText(q);
            if (auto *heading = box.findChild<QLabel *>("qt_msgbox_label")) heading->setProperty("hnRole", "heading");
            box.setDefaultButton(QMessageBox::No);
            yes = box.exec() == QMessageBox::Yes;
        }
        if (!yes) return;
        QString err;
        say(mgr->remove(id, &err) ? tr("Removed %1.").arg(m.name) : tr("Could not remove: %1").arg(err));
        m_sel.clear();
        refresh();
    }, false, true, true);
    m_detailLay->addWidget(acts);
    m_detailLay->addStretch(1);
    restyle();
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
    l->setContentsMargins(24, 16, 24, 16);
    auto *title = new QLabel(tr("Plugin audit log"), d);
    title->setProperty("hnRole", "heading");
    l->addWidget(title);
    auto *ed = new QPlainTextEdit(d);
    ed->setObjectName("hnAuditText");
    ed->setReadOnly(true);
    ed->setLineWrapMode(QPlainTextEdit::NoWrap);
    const auto restyle = [ed] {
        const auto &t = ui::theme();
        QFont mono(t.monoFamily.isEmpty() ? QStringLiteral("monospace") : t.monoFamily);
        mono.setPixelSize(t.baseSize);
        ed->setFont(mono);
        ed->document()->setDefaultFont(mono);
    };
    restyle();
    connect(ui::themeNotifier(), &ui::ThemeNotifier::changed, d, restyle);
    QStringList lines;
    for (const QString &ln : m_c->plugins().auditText().split('\n')) if (id.isEmpty() || ln.contains(id)) lines << ln;
    ed->setPlainText(lines.isEmpty() ? tr("No audit entries for this plugin.") : lines.join('\n'));
    l->addWidget(ed);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, d);
    connect(buttons, &QDialogButtonBox::rejected, d, &QDialog::reject);
    l->addWidget(buttons);
    d->show();
}

} // namespace hn::app
