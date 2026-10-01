#pragma once
#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QString>
#include <memory>

namespace hn::core {

struct RecoveryEntry {
    QString root, relPath;
    QByteArray local;        // latest pending local snapshot
    QByteArray previousDisk; // disk version observed before that write (may be empty)
    bool hadDisk = false;
    QByteArray external;     // observed external version when a conflict happened
    bool conflict = false;
    quint64 baseRevision = 0;
    QDateTime time;
};

// Layout: <dir>/unresolved/<sha1(root\0rel)>/{meta.json,local,disk,external}
//         <dir>/resolved/<msec>-<id>/...   (history, capped; unresolved is never evicted)
// Stateless apart from paths, so it is safe to use from the worker and the GUI thread.
class RecoveryStore {
public:
    explicit RecoveryStore(QString dir, qint64 resolvedCap = 64LL << 20);
    bool writePending(const QString &root, const QString &rel, const QByteArray &local,
                      const QByteArray *previousDisk, quint64 baseRevision, QString *err) const;
    bool writeExternal(const QString &root, const QString &rel, const QByteArray &external, QString *err) const;
    void resolve(const QString &root, const QString &rel) const; // move to history + prune
    QList<RecoveryEntry> unresolved(const QString &root = {}) const;
    qint64 resolvedBytes() const;
    void prune() const; // amortised O(1): the history is walked once per store, then tracked incrementally
    QString dir() const { return m_dir; }

private:
    QString entryDir(const QString &root, const QString &rel) const;
    // Shared by copies (the repository hands copies to the worker): running size of resolved/.
    struct Hist;
    std::shared_ptr<Hist> m_hist;
    QString m_dir;
    qint64 m_cap;
};

} // namespace hn::core
