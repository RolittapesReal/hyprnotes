#pragma once
// Settings > Plugins: warning, install, plugin list (name / version / author / tier / status chips) and a per-plugin card
// (description, permissions in plain language, settings from hn.setting, Enable/Disable, Reload, Remove, Open folder, audit log).
#include "controller.h"
#include <QHash>
#include <QWidget>

class QLabel;
class QListWidget;
class QPushButton;
class QVBoxLayout;
class QBoxLayout;
class QStackedWidget;
class QTabBar;

namespace hn::app {

class MarketPage;

class PluginsPage : public QWidget {
    Q_OBJECT
public:
    explicit PluginsPage(AppController *c, QWidget *parent = nullptr);
    void ensureLoaded();                         // creates the manager and fills the list (done on first show)
    QString selectedId() const { return m_sel; }
    void select(const QString &id);
    QListWidget *list() const { return m_list; }
    QLabel *warningLabel() const { return m_warning; }
    QLabel *noteLabel() const { return m_note; }
    QPushButton *button(const QString &name) const { return m_buttons.value(name); }   // install enable disable reload remove folder audit
    QList<QLabel *> permissionLabels() const { return m_permLabels; }
    QList<QLabel *> registrationLabels() const { return m_regLabels; }   // panels / completions / link handlers the plugin registered
    QList<QWidget *> settingEditors() const { return m_settingEditors; }
    QString statusText(const QString &id) const;   // chip text
    bool installPath(const QString &path);         // install + consent flow (shared by the dialog, drag and drop and tests)
    void showAudit(const QString &id);             // non-modal audit viewer
    void refresh();
    void showBrowse();                             // switch to the Browse tab (builds the page on first use)
    void showInstalled();
    MarketPage *browse() const { return m_browse; }
    QTabBar *tabs() const { return m_tabs; }
    static QString chipFor(hn::plugins::Status s);
protected:
    void showEvent(QShowEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    bool eventFilter(QObject *, QEvent *) override;
private:
    void rebuildDetail();
    void restyle();
    void arrange();
    void updateChips();
    void say(const QString &m);
    AppController *m_c;
    QLabel *m_warning, *m_note;
    QLabel *m_warningIcon, *m_chips = nullptr;
    QWidget *m_warningFrame;
    QTabBar *m_tabs;
    QStackedWidget *m_stack;
    MarketPage *m_browse = nullptr;
    QBoxLayout *m_row;
    QListWidget *m_list;
    QWidget *m_detail;
    QVBoxLayout *m_detailLay;
    QHash<QString, QPushButton *> m_buttons;
    QList<QLabel *> m_permLabels, m_regLabels;
    QList<QWidget *> m_settingEditors;
    QString m_sel;
    bool m_loaded = false;
    bool m_native = false;
    QString m_chip;
};

} // namespace hn::app
