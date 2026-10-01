#pragma once
#include <QByteArray>
#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <functional>
#include <memory>
#include <vector>

namespace hn::mods {

struct ModError { QString modId; QString message; };

struct CommandInfo { QString id; QString title; QString modId; };

struct Manifest {
    QString id, name, version, arch, library, entry = QStringLiteral("hn_mod_entry");
    int hostApiVersion = 0;
    QStringList activation;           // "on-startup", "on-note-open", "on-command:<id>"
    QList<CommandInfo> commands;      // optional static command declarations (for palettes before activation)
    QString dir;                      // absolute directory of the mod
};

// Validates mod.json. On failure returns false and appends to errs. Never touches any library.
bool parseManifest(const QByteArray &json, const QString &dir, Manifest *out, QList<ModError> *errs);
// Scans <modsDir>/<id>/mod.json only. Library files are never opened.
QList<Manifest> scanManifests(const QString &modsDir, QList<ModError> *errs);
// enabled.json: {"version":1,"enabled":["id",...]}. Missing or malformed file yields an empty list.
QStringList loadEnabled(const QString &path, QList<ModError> *errs = nullptr);
bool saveEnabled(const QString &path, const QStringList &ids);

// Implemented by the app/editor. Each window supplies its own bridge.
class DocumentBridge {
public:
    virtual ~DocumentBridge() = default;
    virtual QString selectionText() = 0;
    virtual void beginTransaction(const QString &name) = 0;
    virtual void replaceSelection(const QString &utf8) = 0;
    virtual void insertText(const QString &utf8) = 0;
    virtual void endTransaction() = 0;
};

enum class EventType { NoteOpened = 1, NoteSaved = 2, NoteClosed = 3, SelectionChanged = 4 };

struct Stats { int callbacks = 0; int slowCallbacks = 0; qint64 maxCallbackNs = 0; };

class ModHostPrivate;
class ModHost : public QObject {
    Q_OBJECT
public:
    explicit ModHost(QObject *parent = nullptr);
    ~ModHost() override;

    using Logger = std::function<void(int level, const QString &modId, const QString &msg)>;
    void setLogger(Logger l);
    void setSlowThresholdMs(int ms);     // default 4
    void setCoalesceMs(int ms);          // default 16

    // Reads manifests and the enabled list; loads no library. Activates "on-startup" mods of enabled set.
    void start(const QString &modsDir, const QString &enabledPath);
    QList<Manifest> installed() const;   // all valid manifests
    QStringList enabled() const;
    QList<ModError> errors() const;      // accumulated: manifest, load and runtime errors
    QList<CommandInfo> commands() const; // declared + registered, enabled mods only
    bool isLoaded(const QString &modId) const;
    Stats stats() const;

    // Activates the owning mod lazily if needed. Returns false on unknown command or activation failure.
    bool runCommand(const QString &commandId, DocumentBridge *doc);
    // Events are delivered without a bound document (mutating host calls return HN_ERR_NO_DOCUMENT).
    // Events: pure no-op (no allocation) when no loaded mod subscribed. note-opened also triggers "on-note-open" mods.
    void post(EventType t, const QString &noteId);
    void flushEvents();                  // deliver coalesced events now (normally timer driven)
    // Unloads libraries (application exit). Calls mod shutdown hooks, cancels scheduled work.
    void shutdown();

private:
    std::unique_ptr<ModHostPrivate> d;
};

} // namespace hn::mods
