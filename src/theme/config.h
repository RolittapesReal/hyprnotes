#pragma once
#include <QKeySequence>
#include <QMap>
#include <QObject>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QTimer>

class QFileSystemWatcher;

namespace hn::theme {

struct Settings {
    QString theme = "modernist";
    QString colorScheme = "system";            // system | light | dark
    QString notesFolder;                       // default: $XDG_DATA_HOME/hyprnotes/notes
    bool trayEnabled = true;                   // desktop design section 4: the tray is on unless the user turns it off
    int fontSize = 0;                          // px; 0 = theme default
    QMap<QString, QKeySequence> keybindings;   // action id -> sequence
    QStringList toolbar;                       // ordered action ids
    QSize stickySize{360, 300}, stickyMin{260, 180}, organizerSize{900, 640};
    bool reduceMotion = false;
    bool organizerFloating = false;            // windows.organizerFloating: force the organizer floating on Hyprland when no rule does
    bool operator==(const Settings &) const = default;
};

// Versioned JSON settings ({"version":1,...}). Never writes at load; save() writes atomically.
class Config : public QObject {
    Q_OBJECT
public:
    explicit Config(const QString &path = defaultPath(), QObject *parent = nullptr);
    static QString defaultPath();              // $XDG_CONFIG_HOME/hyprnotes/config.json
    static Settings defaults();

    const Settings &settings() const { return m_s; }
    bool reload();                             // false on malformed file: last valid settings kept, see lastError()
    bool save(const Settings &s);              // atomic; does not emit changed()
    QString lastError() const { return m_error; }
    void watch();                              // event-driven reload of the config dir; emits changed()
    QString path() const { return m_path; }

signals:
    void changed();

private:
    void rearm();
    void onFs();
    QString m_path, m_error;
    Settings m_s;
    QByteArray m_lastBytes;
    QFileSystemWatcher *m_w = nullptr;
    QTimer m_debounce;
};

QString configHome();                          // honours XDG_CONFIG_HOME

} // namespace hn::theme
