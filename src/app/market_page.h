#pragma once
// Settings > Plugins > Browse: the registry list. Nothing here touches the network (or even creates the client) until the user
// has read and acknowledged the first-use notice; after that the cached list shows at once and is revalidated in the background.
#include "controller.h"
#include "market_model.h"
#include <QHash>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListView;
class QPushButton;
class QStackedWidget;

namespace hn::app {

class MarketPage : public QWidget {
    Q_OBJECT
public:
    explicit MarketPage(AppController *c, QWidget *parent = nullptr);
    void activate();                       // the Browse tab became visible: notice, or cached list + one revalidation
    QListView *list() const { return m_list; }
    QWidget *notice() const { return m_notice; }
    QLabel *statusLabel() const { return m_status; }
    QLabel *detailLabel() const { return m_detail; }
    QPushButton *button(const QString &name) const { return m_buttons.value(name); }   // continue back refresh install
    QLineEdit *search() const { return m_search; }
    QComboBox *tagBox() const { return m_tag; }
    QCheckBox *hideDangerous() const { return m_hide; }
    MarketModel *model() const { return m_model; }
    void select(const QString &id);
    void startInstall();                   // the Install / Update button; refused for native entries and while busy
signals:
    void installed(const QString &pluginId);   // so the Installed tab refreshes
    void backRequested();                      // the first-use notice was declined
protected:
    bool eventFilter(QObject *, QEvent *) override;
    void resizeEvent(QResizeEvent *) override;
private:
    void refresh();                        // revalidate against the registry
    void showIndex(const hn::plugins::IndexResult &r);
    void reloadModel();
    void applyFilter();
    void updateDetail();
    void updateButtons();
    bool canInstall() const;
    void restyle();
    void say(const QString &m);
    int currentRow() const;
    AppController *m_c;
    QStackedWidget *m_stack;
    QWidget *m_notice;
    QLabel *m_status, *m_detail, *m_hint;
    QLineEdit *m_search;
    QComboBox *m_tag;
    QCheckBox *m_hide;
    QListView *m_list;
    MarketModel *m_model;
    QHash<QString, QPushButton *> m_buttons;
    QList<hn::plugins::MarketEntry> m_entries;
    bool m_busy = false, m_fetching = false, m_fetched = false, m_swept = false;
};

} // namespace hn::app
