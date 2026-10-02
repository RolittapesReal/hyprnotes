// QueryEngine: semantics, validation messages, read-only authorizer, budget, 2000-spec hostile fuzz.
#include "hn/core/library_index.h"
#include "hn/core/query_engine.h"
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QtTest>
#include <random>
using namespace hn::core;

static void writeFile(const QString &p, const QByteArray &b) {
    QDir().mkpath(QFileInfo(p).absolutePath());
    QFile f(p);
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(b);
}
static QJsonObject cond(const QString &f, const QString &op, const QJsonValue &v) { return {{"field", f}, {"op", op}, {"value", v}}; }
static QJsonObject spec(const QString &from, QJsonArray where = {}, QJsonArray select = {}, QJsonArray order = {}) {
    QJsonObject o{{"from", from}};
    if (!where.isEmpty()) o["where"] = where;
    if (!select.isEmpty()) o["select"] = select;
    if (!order.isEmpty()) o["order"] = order;
    return o;
}
static QJsonObject ord(const QString &f, const QString &d = "asc") { return {{"field", f}, {"dir", d}}; }

class QueryTest : public QObject {
    Q_OBJECT
    QTemporaryDir m_root, m_cache;
    std::unique_ptr<LibraryIndex> m_idx;
    QString abs(const QString &r) { return m_root.path() + "/" + r; }
    QStringList col(const QueryResult &r, int c = 0) { QStringList o; for (auto &row : r.rows) o << row[c].toString(); return o; }
    QByteArray dbHash() {
        QFile f(m_idx->databasePath());
        if (!f.open(QIODevice::ReadOnly)) return {};
        QCryptographicHash h(QCryptographicHash::Sha1);
        h.addData(&f);
        return h.result();
    }
private slots:
    void initTestCase() {
        writeFile(abs("n1.md"), "---\ntitle: One\ntags: [alpha, beta]\nrating: 3\nstatus: open\n---\n# Heading One\n\nlinks [[n2]] and [[ghost]] and ![[n2]]\n\n- [ ] t1 first\n- [x] t2 second\n");
        writeFile(abs("n2.md"), "---\ntags: [beta]\nrating: 10\nstatus: done\n---\n# Two\n\nback [[n1]]\n");
        writeFile(abs("sub/n3.md"), "# Three #gamma\n\nplain, 100% sure_ish\n- [ ] t3 third\n");
        writeFile(abs("sub/deep/n4.md"), "---\ntags: [alpha]\nrating: 7\nstatus: open\n---\n# Four\n");
        m_idx = std::make_unique<LibraryIndex>(m_root.path(), m_cache.path());
        QSignalSpy sp(m_idx.get(), &LibraryIndex::synced);
        m_idx->sync();
        QVERIFY(sp.wait(30000));
    }
    void cleanupTestCase() { m_idx.reset(); }

    void notesSemantics() {
        auto q = [&](QJsonObject s) { auto r = m_idx->query(s); if (!r.ok) qWarning() << r.error; return r; };
        auto r = q(spec("notes", {cond("tag", "=", "alpha")}, {"path"}, {ord("path")}));
        QVERIFY(r.ok); QCOMPARE(col(r), (QStringList{"n1.md", "sub/deep/n4.md"}));
        QCOMPARE(col(q(spec("notes", {cond("tag", "=", "#ALPHA")}, {"path"}, {ord("path")}))), (QStringList{"n1.md", "sub/deep/n4.md"}));
        QCOMPARE(col(q(spec("notes", {cond("tag", "!=", "alpha")}, {"path"}, {ord("path")}))), (QStringList{"n2.md", "sub/n3.md"}));
        QCOMPARE(col(q(spec("notes", {cond("tag", "in", QJsonArray{"beta", "gamma"})}, {"path"}, {ord("path")}))), (QStringList{"n1.md", "n2.md", "sub/n3.md"}));
        QCOMPARE(col(q(spec("notes", {cond("tag", "like", "al%")}, {"path"}, {ord("path")}))), (QStringList{"n1.md", "sub/deep/n4.md"}));
        QCOMPARE(col(q(spec("notes", {cond("tag", "contains", "amm")}, {"path"}))), (QStringList{"sub/n3.md"}));
        // numeric compare (text compare would put "10" before "3")
        QCOMPARE(col(q(spec("notes", {cond("meta.rating", ">", 5)}, {"path"}, {ord("path")}))), (QStringList{"n2.md", "sub/deep/n4.md"}));
        QCOMPARE(col(q(spec("notes", {cond("meta.rating", "<=", 3)}, {"path"}))), (QStringList{"n1.md"}));
        QCOMPARE(col(q(spec("notes", {cond("meta.status", "=", "open")}, {"path"}, {ord("path")}))), (QStringList{"n1.md", "sub/deep/n4.md"}));
        QCOMPARE(col(q(spec("notes", {cond("meta.status", "!=", "open")}, {"path"}, {ord("path")}))), (QStringList{"n2.md", "sub/n3.md"}));
        QCOMPARE(col(q(spec("notes", {cond("meta.Status", "in", QJsonArray{"done", "x"})}, {"path"}))), (QStringList{"n2.md"}));
        QCOMPARE(col(q(spec("notes", {cond("meta.status", "contains", "on")}, {"path"}))), (QStringList{"n2.md"}));
        QCOMPARE(col(q(spec("notes", {cond("folder", "=", "sub")}, {"path"}))), (QStringList{"sub/n3.md"}));
        QCOMPARE(col(q(spec("notes", {cond("folder", "like", "sub%")}, {"path"}, {ord("path")}))), (QStringList{"sub/deep/n4.md", "sub/n3.md"}));
        QCOMPARE(col(q(spec("notes", {cond("title", "contains", "HRE")}, {"path"}))), (QStringList{"sub/n3.md"}));
        QCOMPARE(col(q(spec("notes", {cond("title", "=", "One"), cond("tag", "=", "beta")}, {"path"}))), (QStringList{"n1.md"}));
        QCOMPARE(col(q(spec("notes", {cond("path", "like", "n_.md")}, {"path"}, {ord("path")}))), (QStringList{"n1.md", "n2.md"}));
        QCOMPARE(col(q(spec("notes", {cond("path", "contains", "_")}, {"path"}))), (QStringList{}));   // '_' is literal in contains
        QCOMPARE(col(q(spec("notes", {cond("size", ">", 0)}, {"path"}, {ord("meta.rating", "desc"), ord("path")}))), (QStringList{"n2.md", "sub/deep/n4.md", "n1.md", "sub/n3.md"}));
        r = q(spec("notes", {}, {"path", "tag", "meta.status", "mtime", "size", "title", "folder"}, {ord("path")}));
        QVERIFY(r.ok);
        QCOMPARE(r.columns, (QStringList{"path", "tag", "meta.status", "mtime", "size", "title", "folder"}));
        QCOMPARE(r.rows[0][1].toStringList(), (QStringList{"alpha", "beta"}));
        QCOMPARE(r.rows[0][2].toString(), QString("open"));
        QVERIFY(!r.rows[3][2].isValid());                                    // sub/n3.md has no frontmatter
        QCOMPARE(r.rows[0][5].toString(), QString("One"));
        QVERIFY(r.rows[0][3].typeId() == QMetaType::LongLong);
        // default select
        r = q(spec("notes"));
        QCOMPARE(r.columns, (QStringList{"path", "title", "folder", "mtime", "size"}));
        QCOMPARE(r.rows.size(), 4);
        // paging
        QJsonObject s = spec("notes", {}, {"path"}, {ord("path")});
        s["limit"] = 2; s["offset"] = 1;
        r = q(s);
        QCOMPARE(col(r), (QStringList{"n2.md", "sub/deep/n4.md"})); QVERIFY(r.hasMore);
        s["offset"] = 2;
        r = q(s);
        QCOMPARE(r.rows.size(), 2); QVERIFY(!r.hasMore);
        // JSON form
        auto js = r.toJson();
        QVERIFY(js["ok"].toBool()); QCOMPARE(js["rows"].toArray().size(), 2);
        // text form
        QueryEngine direct(m_idx->databasePath());
        auto tr = direct.run(QJsonDocument(spec("notes", {}, {"path"})).toJson());
        QVERIFY(tr.ok);
        QVERIFY(!direct.run(QByteArray("[1,2]")).ok);
        QVERIFY(!direct.run(QByteArray("{bad")).ok);
        QVERIFY(!direct.run(QByteArray(70000, ' ')).ok);
    }
    void tasksAndLinks() {
        auto q = [&](QJsonObject s) { return m_idx->query(s); };
        auto r = q(spec("tasks", {cond("done", "=", false)}, {"path", "line", "text"}));
        QCOMPARE(r.rows.size(), 2);
        QCOMPARE(r.rows[0][0].toString(), QString("n1.md")); QCOMPARE(r.rows[0][1].toLongLong(), 11); QCOMPARE(r.rows[0][2].toString(), QString("t1 first"));
        r = q(spec("tasks", {cond("done", "=", true)}, {"done", "text"}));
        QCOMPARE(r.rows.size(), 1); QCOMPARE(r.rows[0][0].typeId(), int(QMetaType::Bool)); QVERIFY(r.rows[0][0].toBool());
        QCOMPARE(col(q(spec("tasks", {cond("text", "contains", "third")}, {"path"}))), (QStringList{"sub/n3.md"}));
        QCOMPARE(col(q(spec("tasks", {cond("line", ">=", 11), cond("path", "=", "n1.md")}, {"text"}))), (QStringList{"t1 first", "t2 second"}));
        QCOMPARE(col(q(spec("tasks", {cond("line", "in", QJsonArray{12})}, {"text"}))), (QStringList{"t2 second"}));
        r = q(spec("links", {cond("resolved", "=", false)}, {"src", "target", "kind", "line"}));
        QCOMPARE(r.rows.size(), 1); QCOMPARE(r.rows[0][1].toString(), QString("ghost")); QCOMPARE(r.rows[0][2].toString(), QString("link"));
        QCOMPARE(col(q(spec("links", {cond("dest", "=", QJsonValue::Null)}, {"target"}))), (QStringList{"ghost"}));
        QCOMPARE(col(q(spec("links", {cond("dest", "!=", QJsonValue::Null), cond("kind", "=", "embed")}, {"dest"}))), (QStringList{"n2.md"}));
        QCOMPARE(col(q(spec("links", {cond("dest", "=", "n1.md")}, {"src"}))), (QStringList{"n2.md"}));
        QCOMPARE(q(spec("links", {}, {"src"})).rows.size(), 4);
        QCOMPARE(q(spec("links", {cond("target", "in", QJsonArray{"n1", "n2"})}, {"src", "target"}, {ord("target", "desc"), ord("src")})).rows.size(), 3);
    }
    void validationMessages() {
        struct C { QByteArray json; const char *expect; };
        const QList<C> bad{
            {"{}", "'from' must be"}, {"{\"from\":\"fts\"}", "'from' must be"}, {"{\"from\":\"notes\",\"bogus\":1}", "unknown key 'bogus'"},
            {"{\"from\":\"notes\",\"where\":[{\"field\":\"nope\",\"op\":\"=\",\"value\":\"x\"}]}", "unknown field 'nope'"},
            {"{\"from\":\"notes\",\"where\":[{\"field\":\"path\",\"op\":\"; DROP\",\"value\":\"x\"}]}", "unknown operator"},
            {"{\"from\":\"notes\",\"where\":[{\"field\":\"mtime\",\"op\":\"like\",\"value\":\"x\"}]}", "needs a text field"},
            {"{\"from\":\"tasks\",\"where\":[{\"field\":\"done\",\"op\":\"<\",\"value\":true}]}", "not allowed for boolean"},
            {"{\"from\":\"tasks\",\"where\":[{\"field\":\"done\",\"op\":\"=\",\"value\":\"yes\"}]}", "must be true or false"},
            {"{\"from\":\"notes\",\"where\":[{\"field\":\"title\",\"op\":\"=\",\"value\":5}]}", "must be a string"},
            {"{\"from\":\"notes\",\"where\":[{\"field\":\"size\",\"op\":\"=\",\"value\":\"5\"}]}", "must be a number"},
            {"{\"from\":\"notes\",\"where\":[{\"field\":\"title\",\"op\":\"=\",\"value\":[\"a\"]}]}", "must be a scalar"},
            {"{\"from\":\"notes\",\"where\":[{\"field\":\"title\",\"op\":\"in\",\"value\":\"a\"}]}", "'in' needs an array"},
            {"{\"from\":\"notes\",\"where\":[{\"field\":\"title\",\"op\":\"in\",\"value\":[]}]}", "1 to 100"},
            {"{\"from\":\"notes\",\"where\":[{\"field\":\"tag\",\"op\":\"<\",\"value\":\"a\"}]}", "not allowed for 'tag'"},
            {"{\"from\":\"notes\",\"where\":[{\"field\":\"meta.a b'; --\",\"op\":\"=\",\"value\":\"x\"}]}", "invalid meta key"},
            {"{\"from\":\"notes\",\"where\":[{\"field\":\"title\",\"op\":\"=\",\"value\":\"x\",\"extra\":1}]}", "unknown key 'extra'"},
            {"{\"from\":\"notes\",\"where\":[1]}", "must be an object"}, {"{\"from\":\"notes\",\"where\":{}}", "'where' must be an array"},
            {"{\"from\":\"notes\",\"select\":[\"fileid\"]}", "unknown field 'fileid'"}, {"{\"from\":\"notes\",\"select\":[]}", "1 to 16"},
            {"{\"from\":\"links\",\"select\":[\"tag\"]}", "unknown field 'tag'"}, {"{\"from\":\"tasks\",\"select\":[\"meta.x\"]}", "unknown field 'meta.x'"},
            {"{\"from\":\"notes\",\"order\":[{\"field\":\"tag\"}]}", "cannot be ordered"}, {"{\"from\":\"notes\",\"order\":[{\"field\":\"path\",\"dir\":\"sideways\"}]}", "'dir' must be"},
            {"{\"from\":\"notes\",\"limit\":501}", "between 1 and 500"}, {"{\"from\":\"notes\",\"limit\":0}", "between 1 and 500"}, {"{\"from\":\"notes\",\"limit\":\"5\"}", "'limit' must be a number"},
            {"{\"from\":\"notes\",\"offset\":-1}", "'offset' must be"},
        };
        for (const auto &c : bad) {
            auto r = QueryEngine::compile(QJsonDocument::fromJson(c.json).object());
            QVERIFY2(!r.ok, c.json.constData());
            QVERIFY2(r.error.contains(c.expect), qPrintable(QString("%1 => %2").arg(QString(c.json), r.error)));
        }
        QJsonArray big;
        for (int i = 0; i < 17; ++i) big.append(cond("path", "=", "x"));
        QVERIFY(QueryEngine::compile(spec("notes", big)).error.contains("too many conditions"));
        QJsonObject lng = spec("notes", {cond("title", "=", QString(600, 'x'))});
        QVERIFY(QueryEngine::compile(lng).error.contains("too long"));
        // values never reach the SQL text
        auto c = QueryEngine::compile(spec("notes", {cond("title", "=", "x' OR 1=1 --")}));
        QVERIFY(c.ok); QVERIFY(!c.sql.contains("OR 1=1")); QVERIFY(c.binds.contains(QVariant(QString("x' OR 1=1 --"))));
        QCOMPARE(m_idx->query(spec("notes", {cond("path", "=", "x' OR '1'='1")})).rows.size(), 0);
        QCOMPARE(m_idx->query(spec("notes", {cond("path", "=", "n1.md\") OR (1=1")}, {"path"})).rows.size(), 0);
    }
    void authorizerBlocksEverythingElse() {
        QueryEngine e(m_idx->databasePath());
        const QByteArray before = dbHash();
        const quint64 n0 = m_idx->count();
        for (const char *sql : {"SELECT * FROM fts", "SELECT * FROM aliases", "SELECT * FROM folders", "SELECT name FROM sqlite_master", "SELECT fileid FROM notes",
                                "SELECT body FROM fts WHERE fts MATCH 'x'", "DELETE FROM notes", "INSERT INTO tags(note_id,tag) VALUES(1,'x')", "UPDATE notes SET title='x'",
                                "DROP TABLE notes", "CREATE TABLE evil(a)", "PRAGMA user_version=9", "PRAGMA query_only=OFF", "ATTACH DATABASE '/tmp/hn-evil.db' AS e",
                                "SELECT load_extension('x')", "SELECT readfile('/etc/passwd')", "SELECT lower(path) FROM notes", "SELECT count(*) FROM notes",
                                "SELECT sqlite_version()", "VACUUM", "BEGIN", "SELECT 1; DELETE FROM notes", "WITH RECURSIVE r(x) AS (SELECT 1 UNION SELECT x+1 FROM r) SELECT x FROM r",
                                "SELECT path FROM notes UNION SELECT name FROM sqlite_master", "SELECT path FROM notes WHERE path IN (SELECT tag FROM aliases)",
                                "REPLACE INTO notes(path) VALUES('x')", "CREATE TEMP TABLE t(a)", "CREATE TRIGGER x AFTER INSERT ON notes BEGIN SELECT 1; END", "REINDEX", "ANALYZE"}) {
            auto r = e.runAuthorizedSqlForTests(sql);
            QVERIFY2(!r.ok, sql);
        }
        QVERIFY(!QFileInfo::exists("/tmp/hn-evil.db"));
        auto ok = e.runAuthorizedSqlForTests("SELECT path,title FROM notes WHERE title LIKE 'O%' ORDER BY path");
        QVERIFY2(ok.ok, qPrintable(ok.error));
        QCOMPARE(ok.rows.size(), 1);
        QCOMPARE(m_idx->count(), n0);
        QCOMPARE(dbHash(), before); // nothing written
        // engine still usable after rejections
        QVERIFY(e.run(spec("notes")).ok);
    }
    void budgetInterruptsRunawayQueries() {
        // Cross-joins of the indexed tables are only reachable through the test seam; the compiler never emits them.
        QueryEngine e(m_idx->databasePath());
        QElapsedTimer t;
        t.start();
        QString from, sum;
        for (int i = 0; i < 14; ++i) { from += QString(i ? "," : "") + "notes t" + QString::number(i); sum += QString(i ? "+" : "") + "t" + QString::number(i) + ".size"; }
        auto r = e.runAuthorizedSqlForTests("SELECT t0.path FROM " + from + " WHERE " + sum + " < 0");
        const qint64 ms = t.elapsed();
        qInfo().noquote() << QString("MEASURED runaway 14-way cross join interrupted after %1 ms (budget %2 ms)").arg(ms).arg(QueryEngine::kBudgetMs);
        QVERIFY(!r.ok);
        QVERIFY2(r.error.contains("budget"), qPrintable(r.error));
        QVERIFY(ms < 1000);
        QVERIFY(e.run(spec("notes")).ok); // connection reusable after an interrupt
    }

    void fuzz2000HostileSpecs() {
        std::mt19937 rng(20261002);
        auto pick = [&](const QStringList &l) { return l[rng() % l.size()]; };
        const QStringList sources{"notes", "tasks", "links", "fts", "NOTES", "", "notes; DROP TABLE notes", "sqlite_master", "notes n, fts f"};
        const QStringList fields{"path", "title", "folder", "tag", "mtime", "size", "meta.rating", "meta.status", "meta.", "meta.a'b", "meta.\"x\"", "line", "done", "text", "src", "target", "resolved", "kind", "dest",
                                 "fileid", "tags", "id", "rowid", "body", "fts", "n.path", "path; DROP TABLE notes", "path) OR (1=1", "1", "*", "", "(SELECT 1)", "sqlite_master.name", "path --", "\\", "%", "\xE2\x80\x8F"};
        const QStringList ops{"=", "!=", "<", ">", "<=", ">=", "like", "in", "contains", "LIKE", "IN", "== ", "<>", "OR 1=1", "=; DROP", "", "between", "match", "glob", "is", "regexp"};
        const QStringList hostile{"' OR 1=1 --", "'); DROP TABLE notes; --", "\" OR \"\"=\"", "%", "_", "\\", "\\%", "x' UNION SELECT name FROM sqlite_master --", "1; ATTACH DATABASE '/tmp/hn-fuzz.db' AS x",
                                  "n1.md", "alpha", "open", "", " ", "\xF0\x9F\x98\x80", "\xE2\x80\xAE", "NULL", "null", "0x10", "1e400", "-1", "\x01\x02", "randomblob(1000000000)"};
        std::function<QJsonValue(int)> val = [&](int depth) -> QJsonValue {
            switch (rng() % 12) {
            case 0: case 1: case 2: return pick(hostile);
            case 3: return QString(1 + rng() % 20000, QChar(char('a' + rng() % 26)));
            case 4: return double(int(rng() % 20)) - 5;
            case 5: return 1e308;
            case 6: return -1e308;
            case 7: return bool(rng() % 2);
            case 8: return QJsonValue::Null;
            case 9: { QJsonArray a; for (int i = 0, n = rng() % 150; i < n; ++i) a.append(depth > 0 ? val(depth - 1) : QJsonValue(pick(hostile))); return a; }
            case 10: { QJsonObject o; o["a"] = depth > 0 ? val(depth - 1) : QJsonValue(1); return o; }
            default: return double(rng() % 1000000) * 1e6;
            }
        };
        auto deep = [&](int d) { QJsonValue v = "leaf"; for (int i = 0; i < d; ++i) { QJsonArray a; a.append(v); v = a; } return v; };
        // plausible conditions per source: valid field/op/value shapes, so a good share of specs compile and execute
        const QMap<QString, QStringList> validFields{{"notes", {"path", "title", "folder", "tag", "mtime", "size", "meta.rating", "meta.status"}},
                                                     {"tasks", {"path", "line", "done", "text"}}, {"links", {"src", "target", "resolved", "kind", "line", "dest"}}};
        auto validCond = [&](const QString &src) -> QJsonValue {
            QString f = pick(validFields[src]);
            bool num = f == "mtime" || f == "size" || f == "line" || f == "meta.rating";
            bool bl = f == "done" || f == "resolved";
            QString op = bl ? pick({"=", "!="}) : num ? pick({"=", "!=", "<", ">", "<=", ">=", "in"}) : pick({"=", "!=", "like", "contains", "in"});
            if (f == "tag" && (op == "<" || op == ">")) op = "=";
            QJsonValue v = bl ? QJsonValue(bool(rng() % 2)) : num ? QJsonValue(double(rng() % 12)) : QJsonValue(pick(hostile));
            if (op == "in") { QJsonArray a; for (int i = 0, n = 1 + rng() % 5; i < n; ++i) a.append(num ? QJsonValue(double(rng() % 12)) : QJsonValue(pick(hostile))); v = a; }
            return QJsonObject{{"field", f}, {"op", op}, {"value", v}};
        };
        auto randCond = [&](const QString &src) -> QJsonValue {
            switch (rng() % 10) {
            case 0: return QJsonValue(pick(hostile));
            case 1: return deep(150);
            case 2: { QJsonObject o{{"field", pick(fields)}}; return o; }
            case 3: case 4: {
                QJsonObject o{{"field", pick(fields)}, {"op", pick(ops)}, {"value", val(rng() % 3 == 0 ? 3 : 0)}};
                if (rng() % 15 == 0) o[pick(hostile)] = 1;
                return o;
            }
            default: return validCond(src);
            }
        };
        QueryEngine e(m_idx->databasePath());
        const QByteArray before = dbHash();
        const QRegularExpression tableRe("(?:FROM|JOIN)\\s+(\\w+)");
        const QSet<QString> okTables{"notes", "tags", "meta", "tasks", "links"};
        int accepted = 0, rejected = 0, interrupted = 0;
        QElapsedTimer total;
        total.start();
        for (int i = 0; i < 2000; ++i) {
            QJsonObject s;
            const QString src = pick({"notes", "tasks", "links"});
            if (rng() % 20) s["from"] = rng() % 8 ? QJsonValue(src) : QJsonValue(pick(sources));
            if (rng() % 3) { QJsonArray w; for (int k = 0, n = rng() % (rng() % 10 ? 4 : 20); k < n; ++k) w.append(randCond(src)); s["where"] = w; }
            if (rng() % 3) { QJsonArray a; for (int k = 0, n = 1 + rng() % 5; k < n; ++k) a.append(rng() % 6 ? QJsonValue(pick(validFields[src])) : rng() % 4 ? QJsonValue(pick(fields)) : val(1)); s["select"] = a; }
            if (rng() % 3) { QJsonArray a; for (int k = 0, n = rng() % 3; k < n; ++k) a.append(rng() % 5 ? QJsonValue(QJsonObject{{"field", rng() % 4 ? pick(validFields[src]) : pick(fields)}, {"dir", rng() % 4 ? (rng() % 2 ? "asc" : "desc") : "DESC; --"}}) : randCond(src)); s["order"] = a; }
            if (rng() % 2) s["limit"] = rng() % 3 ? QJsonValue(double(int(rng() % 520)) - 10) : val(0);
            if (rng() % 2) s["offset"] = rng() % 3 ? QJsonValue(double(rng() % 100)) : val(0);
            if (rng() % 25 == 0) s[pick(hostile)] = 1;
            auto c = QueryEngine::compile(s);
            if (c.ok) {
                // SQL text: only whitelisted tables, no spliced values (the only quote is the constant ESCAPE '\')
                QString sql = c.sql;
                sql.remove("ESCAPE '\\'");
                QVERIFY2(!sql.contains('\'') && !sql.contains('"') && !sql.contains(';') && !sql.contains("--"), qPrintable(c.sql));
                for (auto m = tableRe.globalMatch(sql); m.hasNext();) QVERIFY2(okTables.contains(m.next().captured(1)), qPrintable(c.sql));
                QVERIFY(c.binds.size() <= 512);
            }
            auto r = e.run(s);
            if (r.ok) {
                ++accepted;
                QVERIFY(r.rows.size() <= 500);
                for (auto &row : r.rows) QCOMPARE(row.size(), r.columns.size());
                for (const QString &cn : r.columns) QVERIFY2(!cn.contains(';') && !cn.contains('\''), qPrintable(cn));
            } else {
                ++rejected;
                QVERIFY2(!r.error.isEmpty(), "failed queries always carry a message");
                if (r.error.contains("budget")) ++interrupted;
                QVERIFY2(!r.error.contains("touches data") && !r.error.contains("query rejected"), qPrintable(r.error)); // authorizer must never be the thing that rejects compiled SQL
            }
        }
        qInfo().noquote() << QString("MEASURED fuzz: 2000 hostile specs in %1 ms: %2 accepted, %3 rejected (%4 budget interrupts)").arg(total.elapsed()).arg(accepted).arg(rejected).arg(interrupted);
        QVERIFY(accepted > 100 && rejected > 500);
        QVERIFY(!QFileInfo::exists("/tmp/hn-fuzz.db"));
        QCOMPARE(dbHash(), before); // not one byte written
        // the index is still fully usable
        QCOMPARE(m_idx->count(), 4);
        QCOMPARE(m_idx->backlinks("n1.md").rows.size(), 1);
    }
};
QTEST_MAIN(QueryTest)
#include "core_query_test.moc"
