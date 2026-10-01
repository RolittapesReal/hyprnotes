#pragma once
#include "hn/core/fs_util.h"
#include "hn/core/recovery_store.h"
#include "hn/core/worker.h"
#include <QByteArray>
#include <QDateTime>
#include <QFileSystemWatcher>
#include <QHash>
#include <QElapsedTimer>
#include <QObject>
#include <QSet>
#include <QTimer>
#include <optional>

namespace hn::core {

struct NoteSnapshot {
    bool ok = false;
    QString error;
    QString relPath;
    QByteArray bytes;
    quint64 revision = 0; // repository-assigned id of this disk version (also the new baseline)
    QDateTime mtime;
    QString fileId;
};

struct NoteEntry {
    QString relPath;
    qint64 size = 0;
    QDateTime mtime;
    QString fileId;
};

// Library root ownership: listing, versioned reads, async atomic saves with conflict detection,
// recovery copies, create/rename/delete and file watching. All methods run on the owner's (GUI)
// thread; only disk work for saves runs on the shared Worker with self-contained inputs.
class NoteRepository : public QObject {
    Q_OBJECT
public:
    explicit NoteRepository(QString root, QString recoveryDir = {}, QObject *parent = nullptr,
                            Worker *worker = nullptr);
    ~NoteRepository() override;

    QString root() const { return m_root; }
    QString absolutePath(const QString &rel) const; // empty when rel escapes the root

    QList<NoteEntry> list() const;
    QStringList folders() const;
    NoteSnapshot read(const QString &rel); // sets the external-change baseline

    // Returns the revision id that saved()/saveFailed()/conflict() will report for this snapshot.
    // baseRevision is recorded in the recovery copy. force=true skips the baseline check
    // (explicit "replace disk" / re-create after delete).
    quint64 save(const QString &rel, const QByteArray &bytes, quint64 baseRevision = 0, bool force = false);
    void retry(const QString &rel); // re-submit the last failed snapshot
    bool isSaving(const QString &rel) const;
    bool hasPending(const QString &rel) const;

    QString create(const QString &folder, const QString &title, const QByteArray &initial = {}, QString *err = nullptr);
    bool rename(const QString &rel, const QString &newRel, QString *err = nullptr);
    bool remove(const QString &rel, QString *err = nullptr);

    void watch(const QString &rel);
    void unwatch(const QString &rel);
    void watchLibrary(bool on = true);
    bool isArmed(const QString &rel) const; // file watch currently active
    void setDebounceMs(int ms) { m_timer.setInterval(ms); }
    void reconcileNow(); // startup / resume / watch failure

    RecoveryStore &recovery() { return m_recovery; }
    QList<RecoveryEntry> recoverableDrafts(); // unresolved drafts whose content differs from disk
    void resolveRecovery(const QString &rel);

signals:
    void saved(const QString &relPath, quint64 revision);
    void saveFailed(const QString &relPath, const QString &error, quint64 revision);
    void conflict(const QString &relPath, const QByteArray &externalBytes, const QByteArray &localBytes, quint64 revision);
    void externalChange(const QString &relPath);
    void fileRemoved(const QString &relPath);
    void renamed(const QString &oldRel, const QString &newRel);
    void libraryChanged(const QString &relDir);

private:
    struct Job { QByteArray bytes; quint64 rev = 0, baseRev = 0; bool force = false; };
    struct State {
        bool hasBaseline = false, inflight = false, conflict = false, removed = false, recheck = false;
        QByteArray baseHash, notifiedHash;
        quint64 baseRev = 0;
        Job running;
        std::optional<Job> pending, failed;
    };
    struct Result {
        enum Kind { Saved, Conflict, Removed, Failed } kind = Failed;
        QString error;
        QByteArray external, newHash;
    };
    void start(const QString &rel, State &st, Job job);
    void finish(const QString &rel, const Job &job, const Result &r);
    void onFsEvent();
    void kick();
    QString relOf(const QString &abs) const;

    QString m_root;
    RecoveryStore m_recovery;
    Worker *m_worker;
    QHash<QString, State> m_state;
    QSet<QString> m_watched;
    bool m_libWatch = false;
    QSet<QString> m_dirtyDirs;
    QFileSystemWatcher m_fsw;
    QTimer m_timer;
    QElapsedTimer m_burst;
    quint64 m_nextRev = 0;
};

} // namespace hn::core
