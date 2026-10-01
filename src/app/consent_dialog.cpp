#include "consent_dialog.h"
#include "ui_common.h"
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>
#include <algorithm>

namespace hn::app {

namespace {
// Black or white, whichever reads better on `bg` (WCAG relative luminance).
QColor readableOn(const QColor &bg) {
    auto lin = [](qreal c) { return c <= 0.03928 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); };
    const qreal L = 0.2126 * lin(bg.redF()) + 0.7152 * lin(bg.greenF()) + 0.0722 * lin(bg.blueF());
    return (L + 0.05) / 0.05 > 1.05 / (L + 0.05) ? QColor(Qt::black) : QColor(Qt::white);
}
QLabel *wrapped(const QString &text, QWidget *p) {
    auto *l = new QLabel(text, p);
    l->setWordWrap(true);
    l->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return l;
}
}  // namespace

QIcon warningIcon(const QColor &c, int px) {
    QPixmap pm(px * 2, px * 2);   // 2x for crisp edges
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    const qreal s = px * 2;
    QPainterPath tri;
    tri.moveTo(s / 2, s * 0.06);
    tri.lineTo(s * 0.97, s * 0.92);
    tri.lineTo(s * 0.03, s * 0.92);
    tri.closeSubpath();
    p.fillPath(tri, c);
    const QColor fg = readableOn(c);
    p.fillRect(QRectF(s * 0.45, s * 0.34, s * 0.10, s * 0.32), fg);
    p.fillRect(QRectF(s * 0.45, s * 0.72, s * 0.10, s * 0.10), fg);
    pm.setDevicePixelRatio(2);
    return QIcon(pm);
}

QString ConsentDialog::warningText() {
    return QObject::tr("Third-party plugins are not reviewed by the Hyprnotes project. A plugin can read or change your notes and, depending on its permissions, send data over the network. Only install plugins from sources you trust.");
}
QString ConsentDialog::nativeWarningText() { return QObject::tr("This plugin runs native code with full access to your user account. It is not sandboxed."); }
QString ConsentDialog::approveText() { return QObject::tr("I trust this plugin - Enable"); }

ConsentDialog::ConsentDialog(const ConsentRequest &r, QWidget *parent, int delayMs) : QDialog(parent) {
    const auto &t = ui::theme();
    setObjectName("hnConsent");
    setWindowTitle(tr("Review plugin"));
    setWindowIcon(ui::appIcon());
    setModal(true);
    setFixedWidth(600);
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // ---- header band
    auto *head = new QWidget(this);
    head->setObjectName("hnConsentHead");
    head->setStyleSheet(QString("QWidget#hnConsentHead { background: %1; border-bottom: 1px solid %2; border-left: 6px solid %3; }").arg(t.surface.name(), t.border.name(), t.accent.name()));
    auto *hl = new QVBoxLayout(head);
    hl->setContentsMargins(24, 16, 24, 16);
    hl->setSpacing(2);
    auto *kicker = new QLabel(r.reconsentReason.isEmpty() ? tr("REVIEW PLUGIN BEFORE ENABLING") : tr("APPROVE AGAIN"), head);
    kicker->setFont(ui::labelFont(10));
    kicker->setStyleSheet(QString("color: %1; background: transparent;").arg(t.muted.name()));
    auto *name = new QLabel(r.name, head);
    name->setFont(ui::uiFont(22, QFont::Bold));
    name->setStyleSheet("background: transparent;");
    name->setWordWrap(true);
    auto *by = new QLabel(tr("Version %1 by %2").arg(r.version, r.author), head);
    by->setStyleSheet(QString("color: %1; background: transparent;").arg(t.muted.name()));
    by->setTextInteractionFlags(Qt::TextSelectableByMouse);
    hl->addWidget(kicker);
    hl->addWidget(name);
    hl->addWidget(by);
    root->addWidget(head);

    auto *body = new QWidget(this);
    auto *bl = new QVBoxLayout(body);
    bl->setContentsMargins(24, 16, 24, 8);
    bl->setSpacing(8);

    // ---- permanent warning panel (never collapsible, never scrolled away)
    m_warn = new QFrame(body);
    m_warn->setObjectName("hnConsentWarning");
    m_warn->setStyleSheet(QString("QFrame#hnConsentWarning { background: %1; border: 2px solid %2; border-left: 8px solid %2; }").arg(t.surface.name(), t.danger.name()));
    auto *wl = new QHBoxLayout(m_warn);
    wl->setContentsMargins(12, 12, 16, 12);
    wl->setSpacing(12);
    auto *wi = new QLabel(m_warn);
    wi->setPixmap(warningIcon(t.danger, 24).pixmap(24, 24));
    wi->setAlignment(Qt::AlignTop);
    wi->setStyleSheet("background: transparent; border: none;");
    auto *wt = new QVBoxLayout;
    wt->setSpacing(4);
    auto *wh = new QLabel(tr("SECURITY WARNING"), m_warn);
    wh->setFont(ui::labelFont(10));
    wh->setStyleSheet(QString("color: %1; background: transparent; border: none;").arg(t.danger.name()));
    auto *wtext = wrapped(warningText(), m_warn);
    wtext->setObjectName("hnConsentWarningText");
    wtext->setStyleSheet("background: transparent; border: none;");
    wtext->setFont(ui::uiFont(13, QFont::DemiBold));
    wt->addWidget(wh);
    wt->addWidget(wtext);
    wl->addWidget(wi);
    wl->addLayout(wt, 1);
    bl->addWidget(m_warn);

    if (r.native) {   // solid danger fill: the loudest element of the dialog
        m_native = new QFrame(body);
        m_native->setObjectName("hnConsentNative");
        const QColor fg = readableOn(t.danger);
        m_native->setStyleSheet(QString("QFrame#hnConsentNative { background: %1; border: none; }").arg(t.danger.name()));
        auto *nl = new QHBoxLayout(m_native);
        nl->setContentsMargins(12, 10, 16, 10);
        nl->setSpacing(12);
        auto *ni = new QLabel(m_native);
        ni->setPixmap(warningIcon(fg, 20).pixmap(20, 20));
        ni->setStyleSheet("background: transparent;");
        auto *ntext = wrapped(nativeWarningText(), m_native);
        ntext->setObjectName("hnConsentNativeText");
        ntext->setFont(ui::uiFont(13, QFont::Bold));
        ntext->setStyleSheet(QString("color: %1; background: transparent;").arg(fg.name()));
        nl->addWidget(ni, 0, Qt::AlignTop);
        nl->addWidget(ntext, 1);
        bl->addWidget(m_native);
    }

    if (!r.reconsentReason.isEmpty()) {
        auto *rc = wrapped(r.reconsentReason + (r.newPermissions.isEmpty() ? QString() : tr(" New permissions: %1.").arg(r.newPermissions.join(", "))), body);
        rc->setObjectName("hnConsentReason");
        rc->setStyleSheet(QString("background: transparent; color: %1; font-weight: 600;").arg(t.accent.name()));
        bl->addWidget(rc);
    }

    // ---- identity: source and SHA-256 (selectable)
    auto *grid = new QWidget(body);
    auto *gl = new QVBoxLayout(grid);
    gl->setContentsMargins(0, 8, 0, 0);
    gl->setSpacing(4);
    auto field = [&](const QString &label, const QString &value, bool mono) {
        auto *l = new QLabel(label, grid);
        l->setFont(ui::labelFont(9));
        l->setStyleSheet(QString("color: %1;").arg(t.muted.name()));
        auto *v = wrapped(value, grid);
        v->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
        v->setFocusPolicy(Qt::TabFocus);
        v->setAccessibleName(label);
        if (mono) { QFont f = QFontDatabase::systemFont(QFontDatabase::FixedFont); f.setPixelSize(12); v->setFont(f); v->setWordWrap(true); }
        gl->addWidget(l);
        gl->addWidget(v);
        return v;
    };
    if (!r.description.isEmpty()) {
        auto *d = wrapped(r.description, grid);
        gl->addWidget(d);
        gl->addSpacing(4);
    }
    auto *src = field(tr("SOURCE"), r.source, false);
    src->setObjectName("hnConsentSource");
    m_hash = field(tr("SHA-256 OF THE PACKAGE"), r.sha256, true);
    m_hash->setObjectName("hnConsentHash");
    bl->addWidget(grid);

    // ---- permissions
    auto *ph = new QLabel(r.permissions.isEmpty() ? tr("PERMISSIONS: NONE REQUESTED") : tr("THIS PLUGIN REQUESTS"), body);
    ph->setFont(ui::labelFont(10));
    ph->setStyleSheet(QString("color: %1; margin-top: 8px;").arg(t.muted.name()));
    bl->addWidget(ph);
    auto *pw = new QWidget;
    auto *pl = new QVBoxLayout(pw);
    pl->setContentsMargins(0, 0, 0, 0);
    pl->setSpacing(0);
    QStringList ordered = r.permissions;   // dangerous permissions first: they are never hidden below the fold
    std::stable_partition(ordered.begin(), ordered.end(), [](const QString &p) { return hn::plugins::isDangerousPermission(p); });
    for (const QString &p : std::as_const(ordered)) {
        const bool danger = hn::plugins::isDangerousPermission(p);
        auto *row = new QWidget(pw);
        row->setObjectName(danger ? "hnPermDanger" : "hnPermRow");
        row->setStyleSheet(QString("QWidget#%1 { border-top: 1px solid %2; %3 }").arg(row->objectName(), t.border.name(), danger ? QString("background: %1;").arg(t.surface.name()) : QString()));
        auto *rl = new QHBoxLayout(row);
        rl->setContentsMargins(8, 8, 8, 8);
        rl->setSpacing(12);
        auto *ic = new QLabel(row);
        ic->setFixedSize(20, 20);
        if (danger) ic->setPixmap(warningIcon(t.danger, 18).pixmap(18, 18));
        else { QPixmap pm(20, 20); pm.fill(Qt::transparent); QPainter pp(&pm); pp.fillRect(7, 7, 6, 6, t.muted); ic->setPixmap(pm); }
        ic->setStyleSheet("background: transparent;");
        auto *label = new QLabel(row);
        label->setObjectName("hnPermLabel");
        label->setWordWrap(true);
        label->setTextFormat(Qt::PlainText);
        label->setText(QString("%1  %2").arg(p, hn::plugins::permissionDescription(p)));
        label->setProperty("permission", p);
        label->setAccessibleName(tr("%1 permission%2: %3").arg(p, danger ? tr(" (dangerous)") : QString(), hn::plugins::permissionDescription(p)));
        label->setStyleSheet(danger ? QString("color: %1; font-weight: 700; background: transparent;").arg(t.danger.name()) : "background: transparent;");
        rl->addWidget(ic, 0, Qt::AlignTop);
        rl->addWidget(label, 1);
        m_perms << label;
        pl->addWidget(row);
    }
    auto *scroll = new QScrollArea(body);
    scroll->setWidget(pw);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setMinimumHeight(qMin(5, qMax(1, int(r.permissions.size()))) * 38);
    scroll->setMaximumHeight(6 * 38 + 4);   // more than six rows scroll; dangerous ones are listed first
    scroll->setStyleSheet("QScrollArea { background: transparent; } QScrollArea > QWidget > QWidget { background: transparent; }");
    if (!r.permissions.isEmpty()) bl->addWidget(scroll);
    root->addWidget(body, 1);

    // ---- buttons
    auto *bar = new QWidget(this);
    bar->setObjectName("hnConsentBar");
    bar->setStyleSheet(QString("QWidget#hnConsentBar { border-top: 1px solid %1; }").arg(t.border.name()));
    auto *bh = new QHBoxLayout(bar);
    bh->setContentsMargins(24, 16, 24, 16);
    bh->setSpacing(8);
    m_wait = new QLabel(bar);
    m_wait->setStyleSheet(QString("color: %1;").arg(t.muted.name()));
    m_cancel = new QPushButton(tr("Cancel"), bar);
    m_cancel->setObjectName("hnConsentCancel");
    m_cancel->setDefault(true);
    m_cancel->setAutoDefault(true);
    m_cancel->setMinimumHeight(40);
    m_cancel->setAccessibleName(tr("Cancel, do not enable this plugin"));
    m_approve = new QPushButton(approveText(), bar);
    m_approve->setObjectName("hnConsentApprove");
    m_approve->setAutoDefault(false);
    m_approve->setMinimumHeight(40);
    m_approve->setAccessibleName(tr("I trust this plugin, enable it"));
    m_approve->setAccessibleDescription(tr("Available two seconds after this dialog opens"));
    m_approve->setStyleSheet(QString("QPushButton { background: %1; color: %2; border: 2px solid %1; padding: 0 16px; font-weight: 700; }"
                                     "QPushButton:hover { background: %3; color: %4; border-color: %3; }"
                                     "QPushButton:focus { border: 2px solid %3; }"
                                     "QPushButton:disabled { background: transparent; color: %5; border: 2px solid %6; }")
                                 .arg(t.accent.name(), t.accentText.name(), t.text.name(), t.bg.name(), t.muted.name(), t.border.name()));
    bh->addWidget(m_wait, 1);
    bh->addWidget(m_cancel);
    bh->addWidget(m_approve);
    root->addWidget(bar);
    setTabOrder(m_cancel, m_approve);

    connect(m_cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_approve, &QPushButton::clicked, this, &QDialog::accept);
    if (delayMs > 0) {
        m_approve->setEnabled(false);
        m_wait->setText(tr("You can approve in %1 seconds").arg(qMax(1, (delayMs + 999) / 1000)));
        m_timer.setSingleShot(true);
        m_timer.setInterval(delayMs);
        connect(&m_timer, &QTimer::timeout, this, [this] { m_approve->setEnabled(true); m_wait->clear(); });
        m_timer.start();   // started when the dialog is created; it is shown immediately by run()
    }
    // Word-wrapped labels need height-for-width: size the dialog for its fixed width so the warning can never be clipped.
    layout()->activate();
    const int h = layout()->totalHeightForWidth(600);
    setMinimumHeight(h);
    resize(600, h);
    m_cancel->setFocus();
}

void ConsentDialog::showEvent(QShowEvent *e) {
    QDialog::showEvent(e);
    if (!m_approve->isEnabled()) m_timer.start();
    m_cancel->setFocus();
}

bool ConsentDialog::run(const ConsentRequest &r, QWidget *parent) {
    ConsentDialog d(r, parent);
    return d.exec() == QDialog::Accepted;
}

} // namespace hn::app
