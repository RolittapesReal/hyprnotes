#pragma once
// The plugin consent dialog: shown on install, before the first enable and for re-consent (files changed / permissions grew).
// Security UI: a permanent warning panel, every requested permission in plain language (dangerous ones in the danger colour
// with an icon), the package SHA-256 as selectable text, Cancel is the default and the approve button stays disabled for
// two seconds so a reflex click cannot approve.
#include "hn/plugins/runtime.h"
#include <QDialog>
#include <QPointer>
#include <QTimer>

class QShowEvent;

class QPushButton;
class QLabel;
class QScrollArea;
class QScreen;

namespace hn::app {

struct ConsentRequest {
    QString id, name, author, version, description, source, sha256;
    QStringList permissions;          // as requested by the manifest
    bool native = false;
    QString reconsentReason;          // non-empty: shown as a notice (files changed since approval, permissions grew)
    QStringList newPermissions;       // permissions not covered by the earlier approval
};

class ConsentDialog : public QDialog {
    Q_OBJECT
public:
    static constexpr int kDelayMs = 2000;
    static QString warningText();         // the exact third-party warning
    static QString nativeWarningText();   // the extra native-code warning
    static QString approveText();         // "I trust this plugin - Enable"
    explicit ConsentDialog(const ConsentRequest &r, QWidget *parent = nullptr, int delayMs = kDelayMs);
    QPushButton *approveButton() const { return m_approve; }
    QPushButton *cancelButton() const { return m_cancel; }
    QLabel *hashLabel() const { return m_hash; }
    QWidget *warningPanel() const { return m_warn; }
    QWidget *nativePanel() const { return m_native; }
    QList<QLabel *> permissionLabels() const { return m_perms; }
    // Modal run; true only when the user pressed the approve button.
    static bool run(const ConsentRequest &r, QWidget *parent);
protected:
    void showEvent(QShowEvent *e) override;   // the two seconds start when the dialog actually appears
    bool event(QEvent *event) override;
private:
    void restyle();
    void updateBounds();
    QPushButton *m_approve, *m_cancel;
    QLabel *m_hash, *m_wait;
    QLabel *m_warningIcon, *m_nativeIcon = nullptr, *m_nativeText = nullptr;
    QWidget *m_head, *m_bar;
    QWidget *m_warn, *m_native = nullptr;
    QScrollArea *m_details;
    QPointer<QScreen> m_screen;
    QMetaObject::Connection m_screenGeometryConnection;
    bool m_layoutReady = false, m_updatingBounds = false;
    QList<QLabel *> m_perms, m_permIcons;
    QTimer m_timer;
};

QIcon warningIcon(const QColor &c, int px = 16);   // flat triangle with an exclamation mark

} // namespace hn::app
