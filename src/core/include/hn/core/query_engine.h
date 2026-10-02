#pragma once
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <atomic>

struct sqlite3;

namespace hn::core {

struct QueryResult {
    quint64 id = 0;
    bool ok = false;
    QString error;        // clear, user-facing validation/budget message when !ok
    QStringList columns;  // = the requested `select` (or the source's defaults)
    QList<QVariantList> rows; // bool for done/resolved, int64 for numbers, QStringList for `tag`
    bool hasMore = false;
    double ms = 0;
    QJsonObject toJson() const; // {ok,error?,columns,rows:[[..]],hasMore}
};

// Constrained query: {from:"notes"|"tasks"|"links", where:[{field,op,value}], select:[..], order:[{field,dir}],
// limit (1..500, default 100), offset}. Compiled to parameterized SQL (values are never spliced into
// SQL text) and run on its own READ-ONLY sqlite connection (SQLITE_OPEN_READONLY, query_only, an
// authorizer allowing SELECT on whitelisted columns of notes/tags/meta/tasks/links and the LIKE function
// only, SQL/expression limits, 100 ms progress-handler budget). Not thread-safe: use from one thread.
//   notes: path,title,folder,mtime,size,tag,meta.<key>      tasks: path,line,done,text
//   links: src,target,resolved,kind,line,dest
//   ops: = != < > <= >= like in contains  (bool fields: = != only; tag: = != in like contains)
class QueryEngine {
public:
    static constexpr int kMaxLimit = 500, kBudgetMs = 100;

    struct Compiled {
        bool ok = false;
        QString error;
        QString sql;
        QVariantList binds;
        QStringList columns;
        QList<char> kinds; // per column: 't' text, 'i' int, 'b' bool, 'l' tag list
        int limit = 100;
    };
    static Compiled compile(const QJsonObject &spec); // pure, validation only

    explicit QueryEngine(QString dbPath);
    ~QueryEngine();
    QueryEngine(const QueryEngine &) = delete;
    QueryEngine &operator=(const QueryEngine &) = delete;

    QueryResult run(const QJsonObject &spec, const std::atomic<quint64> *latest = nullptr, quint64 id = 0);
    QueryResult run(const QByteArray &json); // parse + run; size cap 64 KiB
    // Internal/test seam: run arbitrary SQL under the same read-only connection + authorizer + budget.
    QueryResult runAuthorizedSqlForTests(const QString &sql);
    void close();

private:
    bool ensureOpen(QString *err);
    QueryResult exec(const Compiled &c, const std::atomic<quint64> *latest, quint64 id);
    QString m_path;
    sqlite3 *m_db = nullptr;
};

} // namespace hn::core
Q_DECLARE_METATYPE(hn::core::QueryResult)
