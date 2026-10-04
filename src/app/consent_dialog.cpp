#include "consent_dialog.h"
#include "action_row.h"
#include "ui_common.h"
#include <QApplication>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollArea>
#include <QScreen>
#include <QScopedValueRollback>
#include <QVBoxLayout>
#include <QWindow>
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
    l->setTextFormat(Qt::PlainText);
    l->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    l->setFocusPolicy(Qt::TabFocus);
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
    setObjectName("hnConsent");
    setWindowTitle(tr("Review plugin"));
    setWindowIcon(ui::appIcon());
    setModal(true);
    setMinimumWidth(520);
    resize(600, 720);
    auto *root = new QVBoxLayout(this);
    // Screen bounds, not unbounded label hints, govern the top-level window.
    root->setSizeConstraint(QLayout::SetNoConstraint);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // ---- header band
    auto *head = new QWidget(this);
    m_head = head;
    head->setObjectName("hnConsentHead");
    auto *hl = new QVBoxLayout(head);
    hl->setContentsMargins(16, 8, 16, 8);
    hl->setSpacing(2);
    auto *kicker = new QLabel(r.reconsentReason.isEmpty() ? tr("Review plugin") : tr("Review plugin again"), head);
    kicker->setProperty("hnRole", "heading");
    kicker->setWordWrap(true);
    auto *name = new QLabel(r.name, head);
    name->setProperty("hnRole", "heading");
    name->setTextFormat(Qt::PlainText);
    name->setWordWrap(true);
    name->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    name->setFocusPolicy(Qt::TabFocus);
    auto *by = new QLabel(tr("Version %1 by %2").arg(r.version, r.author), head);
    by->setProperty("hnRole", "hint");
    by->setTextFormat(Qt::PlainText);
    by->setWordWrap(true);
    by->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    by->setFocusPolicy(Qt::TabFocus);
    hl->addWidget(kicker);
    root->addWidget(head);

    auto *body = new QWidget(this);
    auto *bl = new QVBoxLayout(body);
    bl->setContentsMargins(16, 8, 16, 8);
    bl->setSpacing(8);

    // ---- permanent warning panel (never collapsible, never scrolled away)
    m_warn = new QFrame(body);
    m_warn->setObjectName("hnConsentWarning");
    auto *wl = new QVBoxLayout(m_warn);
    wl->setContentsMargins(12, 8, 12, 8);
    wl->setSpacing(4);
    auto *wi = new QLabel(m_warn);
    m_warningIcon = wi;
    wi->setAlignment(Qt::AlignTop);
    wi->setStyleSheet("background: transparent; border: none;");
    auto *warningHeading = new QHBoxLayout;
    warningHeading->setSpacing(8);
    auto *wh = new QLabel(tr("SECURITY WARNING"), m_warn);
    wh->setProperty("hnRole", "danger");
    wh->setWordWrap(true);
    auto *wtext = wrapped(warningText(), m_warn);
    wtext->setObjectName("hnConsentWarningText");
    wtext->setStyleSheet("background: transparent; border: none; font-weight: 600;");
    warningHeading->addWidget(wi);
    warningHeading->addWidget(wh, 1);
    wl->addLayout(warningHeading);
    wl->addWidget(wtext);
    bl->addWidget(m_warn);

    m_details = new QScrollArea(body);
    m_details->setObjectName("hnConsentDetails");
    m_details->setAccessibleName(tr("Plugin details and permissions"));
    m_details->setWidgetResizable(true);
    m_details->setFrameShape(QFrame::NoFrame);
    m_details->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_details->setFocusPolicy(Qt::StrongFocus);
    auto *detail = new QWidget;
    auto *dl = new QVBoxLayout(detail);
    dl->setContentsMargins(8, 8, 8, 8);
    dl->setSpacing(8);
    m_details->setWidget(detail);
    bl->addWidget(m_details, 1);

    if (r.native) {   // solid danger fill: the loudest element of the dialog
        m_native = new QFrame(detail);
        m_native->setObjectName("hnConsentNative");
        auto *nl = new QHBoxLayout(m_native);
        nl->setContentsMargins(12, 10, 16, 10);
        nl->setSpacing(12);
        auto *ni = new QLabel(m_native);
        m_nativeIcon = ni;
        ni->setStyleSheet("background: transparent;");
        auto *ntext = wrapped(nativeWarningText(), m_native);
        m_nativeText = ntext;
        ntext->setObjectName("hnConsentNativeText");
        nl->addWidget(ni, 0, Qt::AlignTop);
        nl->addWidget(ntext, 1);
        dl->addWidget(m_native);
    }

    if (!r.reconsentReason.isEmpty()) {
        auto *rc = wrapped(r.reconsentReason + (r.newPermissions.isEmpty() ? QString() : tr(" New permissions: %1.").arg(r.newPermissions.join(", "))), detail);
        rc->setObjectName("hnConsentReason");
        rc->setProperty("hnRole", "accent");
        dl->addWidget(rc);
    }
    dl->addWidget(name);
    dl->addWidget(by);

    // ---- identity: source and SHA-256 (selectable)
    auto *grid = new QWidget(detail);
    auto *gl = new QVBoxLayout(grid);
    gl->setContentsMargins(0, 8, 0, 0);
    gl->setSpacing(4);
    auto field = [&](const QString &label, const QString &value, bool mono) {
        auto *l = new QLabel(label, grid);
        l->setProperty("hnRole", "section");
        l->setWordWrap(true);
        auto *v = wrapped(value, grid);
        v->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
        v->setFocusPolicy(Qt::TabFocus);
        v->setAccessibleName(label);
        if (mono) v->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
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
    dl->addWidget(grid);

    // ---- permissions
    auto *ph = new QLabel(r.permissions.isEmpty() ? tr("PERMISSIONS: NONE REQUESTED") : tr("THIS PLUGIN REQUESTS"), detail);
    ph->setProperty("hnRole", "section");
    ph->setWordWrap(true);
    dl->addWidget(ph);
    auto *pw = new QWidget(detail);
    auto *pl = new QVBoxLayout(pw);
    pl->setContentsMargins(0, 0, 0, 0);
    pl->setSpacing(0);
    QStringList ordered = r.permissions;   // retain dangerous-first ordering in the scrolling review
    std::stable_partition(ordered.begin(), ordered.end(), [](const QString &p) { return hn::plugins::isDangerousPermission(p); });
    for (const QString &p : std::as_const(ordered)) {
        const bool danger = hn::plugins::isDangerousPermission(p);
        auto *row = new QWidget(pw);
        row->setObjectName(danger ? "hnPermDanger" : "hnPermRow");
        auto *rl = new QHBoxLayout(row);
        rl->setContentsMargins(8, 8, 8, 8);
        rl->setSpacing(12);
        auto *ic = new QLabel(row);
        m_permIcons << ic;
        ic->setStyleSheet("background: transparent;");
        auto *label = new QLabel(row);
        label->setObjectName("hnPermLabel");
        label->setWordWrap(true);
        label->setTextFormat(Qt::PlainText);
        label->setText(QString("%1  %2").arg(p, hn::plugins::permissionDescription(p)));
        label->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
        label->setFocusPolicy(Qt::TabFocus);
        label->setProperty("permission", p);
        label->setAccessibleName(tr("%1 permission%2: %3").arg(p, danger ? tr(" (dangerous)") : QString(), hn::plugins::permissionDescription(p)));
        if (danger) label->setProperty("hnRole", "danger");
        rl->addWidget(ic, 0, Qt::AlignTop);
        rl->addWidget(label, 1);
        m_perms << label;
        pl->addWidget(row);
    }
    dl->addWidget(pw);
    dl->addStretch(1);
    root->addWidget(body, 1);

    // ---- buttons
    auto *bar = new QWidget(this);
    m_bar = bar;
    bar->setObjectName("hnConsentBar");
    auto *bh = new QVBoxLayout(bar);
    bh->setContentsMargins(16, 8, 16, 16);
    bh->setSpacing(8);
    m_wait = new QLabel(bar);
    m_wait->setProperty("hnRole", "hint");
    m_wait->setWordWrap(true);
    m_cancel = new ui::WrappingButton(tr("Cancel"), bar);
    m_cancel->setObjectName("hnConsentCancel");
    m_cancel->setDefault(true);
    m_cancel->setAutoDefault(true);
    m_cancel->setMinimumHeight(40);
    m_cancel->setAccessibleName(tr("Cancel, do not enable this plugin"));
    m_approve = new ui::WrappingButton(approveText(), bar);
    m_approve->setObjectName("hnConsentApprove");
    m_approve->setAutoDefault(false);
    m_approve->setMinimumHeight(40);
    m_approve->setAccessibleName(tr("I trust this plugin, enable it"));
    m_approve->setAccessibleDescription(tr("Available two seconds after this dialog opens"));
    m_approve->setProperty("hnRole", "primary");
    bh->addWidget(m_wait);
    // Both labels wrap within one footer row, reserving space for the warning.
    auto *actions = new QHBoxLayout;
    actions->setSpacing(8);
    actions->addWidget(m_cancel);
    actions->addWidget(m_approve, 1);
    bh->addLayout(actions);
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
    connect(ui::themeNotifier(), &ui::ThemeNotifier::changed, this, &ConsentDialog::restyle);
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget *, QWidget *now) {
        if (now && m_details->widget()->isAncestorOf(now)) m_details->ensureWidgetVisible(now, 0, 0);
    });
    restyle();
    m_layoutReady = true;
    updateBounds();
    m_cancel->setFocus();
}

void ConsentDialog::restyle() {
    const auto &t = ui::theme();
    m_head->setStyleSheet(QString("QWidget#hnConsentHead { background: %1; border-bottom: 1px solid %2; border-left: 6px solid %3; }").arg(t.surface.name(), t.border.name(), t.accent.name()));
    m_warn->setStyleSheet(QString("QFrame#hnConsentWarning { background: %1; border: 1px solid %2; border-left: 8px solid %2; }").arg(t.surface.name(), t.danger.name()));
    const int px = qMax(24, t.baseSize);
    m_warningIcon->setPixmap(warningIcon(t.danger, px).pixmap(px, px));
    if (m_native) {
        const QColor fg = readableOn(t.danger);
        m_native->setStyleSheet(QString("QFrame#hnConsentNative { background: %1; border: none; }").arg(t.danger.name()));
        m_nativeIcon->setPixmap(warningIcon(fg, px).pixmap(px, px));
        m_nativeText->setStyleSheet(QString("color: %1; background: transparent; font-weight: bold;").arg(fg.name()));
    }
    for (int i = 0; i < m_perms.size(); ++i) {
        auto *label = m_perms[i];
        const bool danger = hn::plugins::isDangerousPermission(label->property("permission").toString());
        auto *row = label->parentWidget();
        row->setStyleSheet(QString("QWidget#%1 { border-top: 1px solid %2; %3 }").arg(row->objectName(), t.border.name(), danger ? QString("background: %1;").arg(t.surface.name()) : QString()));
        auto *icon = m_permIcons[i];
        icon->setFixedSize(px, px);
        if (danger) icon->setPixmap(warningIcon(t.danger, px).pixmap(px, px));
        else {
            QPixmap pm(px, px); pm.fill(Qt::transparent);
            QPainter painter(&pm); painter.fillRect(px / 3, px / 3, px / 3, px / 3, t.muted); painter.end();
            icon->setPixmap(pm);
        }
    }
    QFont mono(t.monoFamily.isEmpty() ? QStringLiteral("monospace") : t.monoFamily);
    mono.setPixelSize(t.baseSize);
    m_hash->setFont(mono);
    m_bar->setStyleSheet(QString("QWidget#hnConsentBar { border-top: 1px solid %1; }").arg(t.border.name()));
}

void ConsentDialog::showEvent(QShowEvent *e) {
    QDialog::showEvent(e);
    if (!m_approve->isEnabled()) m_timer.start();
    m_cancel->setFocus();
}

bool ConsentDialog::event(QEvent *event) {
    const bool handled = QDialog::event(event);
    if (event->type() == QEvent::LayoutRequest || event->type() == QEvent::Resize || event->type() == QEvent::Show
        || event->type() == QEvent::ScreenChangeInternal)
        updateBounds();
    return handled;
}

void ConsentDialog::updateBounds() {
    if (!m_layoutReady || m_updatingBounds || !screen()) return;
    const QScopedValueRollback<bool> updating(m_updatingBounds, true);
    if (m_screen != screen()) {
        disconnect(m_screenGeometryConnection);
        m_screen = screen();
        m_screenGeometryConnection = connect(screen(), &QScreen::availableGeometryChanged, this, &ConsentDialog::updateBounds);
    }
    QSize available = screen()->availableGeometry().size();
    if (auto *handle = windowHandle()) {
        const QMargins frame = handle->frameMargins();
        available -= QSize(frame.left() + frame.right(), frame.top() + frame.bottom());
    }
    available = available.expandedTo(QSize(1, 1));
    setMaximumSize(available);
    const int w = qMin(width(), available.width());
    const auto margins = m_warn->parentWidget()->layout()->contentsMargins();
    const int warningHeight = m_warn->heightForWidth(qMax(1, w - margins.left() - margins.right()));
    const int headHeight = m_head->heightForWidth(w), barHeight = m_bar->heightForWidth(w);
    m_warn->setMinimumHeight(warningHeight);
    m_head->setFixedHeight(headHeight);
    m_bar->setFixedHeight(barHeight);
    m_details->setMinimumHeight(qMax(32, fontMetrics().height() + 8));
    const int minimum = headHeight + warningHeight + barHeight + margins.top() + margins.bottom()
        + m_warn->parentWidget()->layout()->spacing() + m_details->minimumHeight();
    setMinimumSize(qMin(520, available.width()), qMin(minimum, available.height()));
}

bool ConsentDialog::run(const ConsentRequest &r, QWidget *parent) {
    ConsentDialog d(r, parent);
    return d.exec() == QDialog::Accepted;
}

} // namespace hn::app
