#pragma once
#include "hn/core/worker.h"
#include <QCache>
#include <QObject>
#include <QStringList>
#include <atomic>
#include <memory>

namespace hn::core {

struct NoteMeta {
    QString title;     // first heading, else filename without extension
    QStringList tags;  // lowercase, unique, in order of appearance
};
// Title + #tags (outside code and links) from raw note bytes. Pure; also used for dirty open docs.
NoteMeta extractMeta(const QString &fileBaseName, const QByteArray &utf8);

struct IndexRow {
    QString relPath, title, folder, snippet;
    QStringList tags;
    qint64 mtimeMs = 0, size = 0;
    bool dirty = false; // came from an unsaved open document
};
struct IndexQuery {
    QString text = {};       // empty => list by modification time
    QString folder = {};     // empty => any; otherwise that folder and its subfolders
    QString tag = {};        // empty => any
    int limit = 100;
    int offset = 0;
};
struct SearchPage {
    quint64 id = 0;
    QList<IndexRow> rows;
    bool hasMore = false;
};
struct DirtyDoc { QString relPath; QString text; };

// Rebuildable SQLite FTS5 cache. One connection, owned by the shared worker thread
// (4 MiB page cache, file-backed temp store). Results carry generation/query ids.
class LibraryIndex : public QObject {
    Q_OBJECT
public:
    explicit LibraryIndex(QString root, QString cacheDir = {}, QObject *parent = nullptr, Worker *worker = nullptr);
    ~LibraryIndex() override;

    quint64 sync();    // async incremental discovery: only new/changed/removed entries are touched
    quint64 rebuild(); // drop everything, then sync
    void updatePath(const QString &rel);  // async, coalesced per path
    void removePath(const QString &rel);
    void renamePath(const QString &oldRel, const QString &newRel);

    quint64 search(const IndexQuery &q);        // async; superseded queries are cancelled
    SearchPage searchNow(const IndexQuery &q);  // blocking (used by tests / small callers)
    QStringList folders();                      // blocking
    QList<QPair<QString, int>> tags();          // blocking, (tag, note count)
    int count();                                // blocking

    // Merge unsaved open documents into disk results (pure; GUI thread).
    static QList<IndexRow> mergeDirty(QList<IndexRow> disk, const QList<DirtyDoc> &dirty, const QString &text);

    struct Impl; // worker-owned DB state (opaque)
    const IndexRow *cachedRow(const QString &rel) const { return m_cache.object(rel); } // max 500 rows
    int cachedRows() const { return m_cache.size(); }
    QString databasePath() const { return m_dbPath; }

signals:
    void synced(quint64 generation, int added, int updated, int removed);
    void pathUpdated(const QString &relPath, quint64 generation);
    void searchFinished(const hn::core::SearchPage &page);

private:
    static void syncChunk(std::shared_ptr<void> ctx);
    std::shared_ptr<Impl> m_impl;
    QString m_root, m_dbPath;
    Worker *m_worker;
    std::shared_ptr<std::atomic<quint64>> m_gen, m_searchId;
    QCache<QString, IndexRow> m_cache{500};
};

} // namespace hn::core
Q_DECLARE_METATYPE(hn::core::SearchPage)
