#pragma once
#include "controller.h"
#include <QDialog>

class QDragEnterEvent;
class QDropEvent;

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QSpinBox;
class QPushButton;
class QTableWidget;
class QTabWidget;

namespace hn::app {

class PluginsPage;

// First-run notes folder choice. Returns the chosen folder (suggested when accepted unchanged), or empty if cancelled.
QString runFirstRunDialog(const QString &suggested);

// Live-applying settings: every change is validated, saved atomically and applied through the controller.
class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    SettingsDialog(AppController *c, QWidget *parent = nullptr);
    QComboBox *schemeBox() const { return m_scheme; }
    QSpinBox *fontSpin() const { return m_font; }
    QCheckBox *trayBox() const { return m_tray; }
    QCheckBox *autostartBox() const { return m_autostart; }
    QCheckBox *motionBox() const { return m_motion; }
    static constexpr int kPluginsTab = 3;
    PluginsPage *pluginsPage() const { return m_pluginsPage; }
    QTabWidget *tabs() const { return m_tabs; }
    void showTab(int i);
    QTableWidget *keysTable() const { return m_keys; }
    QLineEdit *folderEdit() const { return m_folder; }
    QComboBox *themeBox() const { return m_theme; }
    QPushButton *removeThemeButton() const { return m_removeTheme; }
    QString note() const;
    bool importThemeFile(const QString &path);     // import + select + apply; result shown in the note line
    bool exportThemeTo(const QString &path);       // exports the selected theme
    bool removeSelectedTheme();                    // user themes only; falls back to modernist
protected:
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dropEvent(QDropEvent *e) override;
private:
    void apply();
    void refreshThemes(const QString &select);
    QWidget *tabAppearance(), *tabNotes(), *tabBehavior(), *tabKeys();
    AppController *m_c;
    QComboBox *m_scheme, *m_theme;
    QPushButton *m_removeTheme;
    QSpinBox *m_font;
    QCheckBox *m_motion, *m_tray, *m_autostart;
    QLineEdit *m_folder;
    QLabel *m_note;
    QTableWidget *m_keys;
    QTabWidget *m_tabs;
    PluginsPage *m_pluginsPage;
    bool m_loading = true;
};

} // namespace hn::app
