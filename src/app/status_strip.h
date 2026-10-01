#pragma once
#include "note_session.h"
#include "ui_common.h"
#include <QPointer>
#include <QTimer>
#include <QWidget>

class QPushButton;

namespace hn::app {

class AppController;

// Slim, non-modal save status: Saved / Unsaved / Saving / Save failed / Conflict / Deleted, the editor's
// source-mode reason, and (only in problem states) a second row with the resolve actions.
class StatusStrip : public QWidget {
    Q_OBJECT
public:
    static constexpr int kRow = 28;
    explicit StatusStrip(AppController *c, QWidget *parent = nullptr);
    void setSession(NoteSession *s);
    void flash(const QString &message);          // transient info/error line (6 s, single-shot timer)
    QString labelText() const { return m_label->text(); }
    QString detailText() const { return m_detail->text(); }
    bool actionsVisible() const { return m_actions->isVisible(); }
    QPushButton *button(const QString &name) const;   // "retry" "reload" "copy" "replace" "discard" "recreate"
    void refresh();
protected:
    void paintEvent(QPaintEvent *) override;
private:
    AppController *m_c;
    QPointer<NoteSession> m_s;
    QList<QMetaObject::Connection> m_conns;
    QWidget *m_dot;
    QLabel *m_label;
    ui::ElidedLabel *m_detail;
    QLabel *m_chip, *m_hint;
    QWidget *m_actions;
    QHash<QString, QPushButton *> m_buttons;
    QTimer m_flashTimer;
    QString m_flash;
};

} // namespace hn::app
