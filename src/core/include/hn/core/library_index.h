#pragma once
#include "hn/core/query_engine.h"
#include "hn/core/worker.h"
#include <QCache>
#include <QObject>
#include <QStringList>
#include <atomic>
#include <memory>

namespace hn::core {

struct NoteMeta {
    QString title;     // first heading, else filename without extension
    QStringList tags;  // lowercase, unique, in order of appearance (frontmatter tags first)
    QStringList aliases; // frontmatter `aliases`
};
// Title (frontmatter `title`, else first heading, else file name) + tags (frontmatter `tags`, plus #tags
// outside code and links) + aliases from raw note bytes. Pure; also used for dirty open docs.
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

// ---- link graph / tasks (schema v2) ----
struct LinkRow {
    QString srcPath, target, alias, anchor, kind /* "link"|"embed" */, context;
    QString destPath;       // resolved note, empty if unresolved/ambiguous
    int line = 0;
    bool resolved = false;  // exactly one note
    bool ambiguous = false; // several candidates (see LibraryIndex::resolve)
    bool dirty = false;     // computed from unsaved text
};
struct LinkPage { quint64 id = 0; QList<LinkRow> rows; bool hasMore = false; };
struct TaskRow { QString path; int line = 0; bool done = false; QString text; bool dirty = false; };
struct TaskQuery {
    QString path = {};   // empty => any note
    int done = -1;       // -1 any, 0 open, 1 done
    QString text = {};   // case-insensitive substring
    int limit = 100;
    int offset = 0;
};
struct TaskPage { quint64 id = 0; QList<TaskRow> rows; bool hasMore = false; };
struct Resolution {
    enum State { Unresolved, Resolved, Ambiguous } state = Unresolved;
    enum Via { None, Path, Basename, Alias } via = None;
    QString path;            // when Resolved
    QStringList candidates;  // when Ambiguous (sorted, max 20)
};
// Full new file content for one note whose links must change when a note is renamed/moved.
struct FileEdit {
    QString path;       // relative path of the file AS IT IS NOW (i.e. before the rename is applied)
    QByteArray newBytes;
    int changedCount = 0;
};

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

    // ---- link graph, tasks, queries. Blocking calls run on the index worker (safe from any thread,
    // like tags()); dirty docs (unsaved text) replace the stored links/tasks of those notes.
    QList<LinkRow> linksFrom(const QString &rel, const QList<DirtyDoc> &dirty = {});
    LinkPage backlinks(const QString &rel, int limit = 100, int offset = 0, const QList<DirtyDoc> &dirty = {});
    LinkPage unresolvedLinks(int limit = 100, int offset = 0, const QList<DirtyDoc> &dirty = {});
    Resolution resolve(const QString &name, const QString &fromRel = {});
    TaskPage tasks(const TaskQuery &q = {}, const QList<DirtyDoc> &dirty = {});
    // Pure computation from the index and the files' current bytes; nothing is written.
    QList<FileEdit> rewriteLinksForRename(const QString &oldRel, const QString &newRel);
    // Constrained JSON query (see query_engine.h) on a read-only connection with a 100 ms budget.
    QueryResult query(const QJsonObject &spec);
    // Async variants: a newer call of the same kind cancels/supersedes the older one (its signal never fires).
    quint64 backlinksAsync(const QString &rel, int limit = 100, int offset = 0, const QList<DirtyDoc> &dirty = {});
    quint64 tasksAsync(const TaskQuery &q, const QList<DirtyDoc> &dirty = {});
    quint64 queryAsync(const QJsonObject &spec);

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
    void backlinksFinished(const hn::core::LinkPage &page);
    void tasksFinished(const hn::core::TaskPage &page);
    void queryFinished(const hn::core::QueryResult &result);

private:
    static void syncChunk(std::shared_ptr<void> ctx);
    std::shared_ptr<Impl> m_impl;
    QString m_root, m_dbPath;
    Worker *m_worker;
    std::shared_ptr<std::atomic<quint64>> m_gen, m_searchId, m_backId, m_taskId, m_queryId;
    QCache<QString, IndexRow> m_cache{500};
};

} // namespace hn::core
Q_DECLARE_METATYPE(hn::core::SearchPage)
Q_DECLARE_METATYPE(hn::core::LinkPage)
Q_DECLARE_METATYPE(hn::core::TaskPage)
