#include "hn/core/query_engine.h"
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <sqlite3.h>

namespace hn::core {
namespace {

enum class T { Text, Int, Bool };
struct Field {
    QString expr; // SQL expression (no binds)
    T type;
    bool nullable = false;
};
struct Source {
    QString from;
    QMap<QString, Field> fields;
    QStringList defaults, tiebreak;
};

const Source &source(const QString &name) {
    static const Source notes{
        "notes n", {{"path", {"n.path", T::Text}}, {"title", {"n.title", T::Text}}, {"folder", {"n.folder", T::Text}},
                    {"mtime", {"n.mtime", T::Int}}, {"size", {"n.size", T::Int}}},
        {"path", "title", "folder", "mtime", "size"}, {"n.path"}};
    static const Source tasks{
        "tasks k JOIN notes n ON n.id=k.note_id",
        {{"path", {"n.path", T::Text}}, {"line", {"k.line", T::Int}}, {"done", {"k.done", T::Bool}}, {"text", {"k.text", T::Text}}},
        {"path", "line", "done", "text"}, {"n.path", "k.line", "k.id"}};
    static const Source links{
        "links l JOIN notes s ON s.id=l.src_id LEFT JOIN notes d ON d.id=l.target_resolved",
        {{"src", {"s.path", T::Text}}, {"target", {"l.target_raw", T::Text}}, {"resolved", {"(l.target_resolved IS NOT NULL)", T::Bool}},
         {"kind", {"l.kind", T::Text}}, {"line", {"l.line", T::Int}}, {"dest", {"d.path", T::Text, true}}},
        {"src", "target", "resolved", "kind", "line"}, {"s.path", "l.line", "l.id"}};
    return name == "notes" ? notes : name == "tasks" ? tasks : links;
}

const QRegularExpression &metaKeyRe() {
    static const QRegularExpression re(R"(^[a-z0-9_][a-z0-9_ .\-]{0,63}$)");
    return re;
}

QString likeEscape(QString s) { return s.replace('\\', "\\\\").replace('%', "\\%").replace('_', "\\_"); }
QString typeName(T t) { return t == T::Text ? "a string" : t == T::Int ? "a number" : "true or false"; }

struct Builder {
    QString src;
    const Source *S;
    QString err;
    QVariantList binds;

    // Resolve a field into (expression-building info). kind: 0 plain, 1 tag, 2 meta.
    struct Ref { int kind = 0; Field f; QString metaKey; };
    bool ref(const QString &name, Ref *r) {
        if (name.size() > 80) { err = "field name too long"; return false; }
        if (src == "notes") {
            if (name == "tag") { r->kind = 1; r->f = {"", T::Text}; return true; }
            if (name.startsWith("meta.")) {
                QString k = name.mid(5).toLower();
                if (!metaKeyRe().match(k).hasMatch()) { err = QString("invalid meta key in field '%1'").arg(name.left(80)); return false; }
                r->kind = 2; r->metaKey = k; r->f = {"", T::Text}; return true;
            }
        }
        auto it = S->fields.constFind(name);
        if (it == S->fields.constEnd()) {
            err = QString("unknown field '%1' for source '%2' (allowed: %3%4)").arg(name.left(80), src, S->fields.keys().join(", "),
                                                                                src == "notes" ? ", tag, meta.<key>" : "");
            return false;
        }
        r->f = *it;
        return true;
    }

    QVariant bindValue(T t, const QJsonValue &v, const QString &field) {
        auto bad = [&] { err = QString("value for '%1' must be %2").arg(field, typeName(t)); return QVariant(); };
        switch (t) {
        case T::Text:
            if (!v.isString()) return bad();
            if (v.toString().size() > 512) { err = QString("value for '%1' is too long (max 512 characters)").arg(field); return QVariant(); }
            return v.toString();
        case T::Int:
            if (!v.isDouble()) return bad();
            return qint64(v.toDouble());
        case T::Bool:
            if (!v.isBool()) return bad();
            return v.toBool() ? 1 : 0;
        }
        return {};
    }

    // Returns SQL for one condition; appends binds in textual order.
    QString condition(const QJsonObject &c, int idx) {
        const QString where = QString("where[%1]").arg(idx);
        for (auto it = c.begin(); it != c.end(); ++it)
            if (it.key() != "field" && it.key() != "op" && it.key() != "value") { err = QString("%1: unknown key '%2'").arg(where, it.key().left(40)); return {}; }
        if (!c.value("field").isString() || !c.value("op").isString()) { err = where + ": 'field' and 'op' must be strings"; return {}; }
        const QString fname = c.value("field").toString(), op = c.value("op").toString();
        static const QSet<QString> ops{"=", "!=", "<", ">", "<=", ">=", "like", "in", "contains"};
        if (!ops.contains(op)) { err = QString("%1: unknown operator '%2' (allowed: = != < > <= >= like in contains)").arg(where, op.left(20)); return {}; }
        Ref r;
        if (!ref(fname, &r)) { err = where + ": " + err; return {}; }
        const QJsonValue val = c.value("value");
        const bool cmp = op == "<" || op == ">" || op == "<=" || op == ">=";
        T t = r.f.type;
        // nullable fields accept null with = / !=
        if (val.isNull() && r.f.nullable && (op == "=" || op == "!=")) return r.f.expr + (op == "=" ? " IS NULL" : " IS NOT NULL");
        if (t == T::Bool && op != "=" && op != "!=") { err = QString("%1: operator '%2' is not allowed for boolean field '%3'").arg(where, op, fname); return {}; }
        if ((op == "like" || op == "contains") && t != T::Text) { err = QString("%1: operator '%2' needs a text field, '%3' is not").arg(where, op, fname); return {}; }
        if (r.kind == 1 && cmp) { err = QString("%1: operator '%2' is not allowed for 'tag'").arg(where, op); return {}; }

        // value-type for meta: numbers compare numerically, strings textually
        QString col = r.kind == 1 ? "t.tag" : r.kind == 2 ? "m.value" : r.f.expr;
        QVariantList local;
        QString test;
        if (op == "in") {
            if (!val.isArray()) { err = QString("%1: 'in' needs an array value").arg(where); return {}; }
            QJsonArray a = val.toArray();
            if (a.isEmpty() || a.size() > 100) { err = QString("%1: 'in' needs 1 to 100 values").arg(where); return {}; }
            QStringList qs;
            for (const auto &e : a) {
                QVariant b;
                if (r.kind == 2 && e.isDouble()) b = e.toDouble();
                else {
                    b = bindValue(t, e, fname);
                    if (!b.isValid()) { err = where + ": " + err; return {}; }
                }
                if (r.kind == 1) b = b.toString().toLower();
                local << b; qs << "?";
            }
            test = (r.kind == 2 && a.first().isDouble() ? "CAST(" + col + " AS REAL)" : col) + " IN (" + qs.join(',') + ")";
        } else {
            if (val.isArray() || val.isObject()) { err = QString("%1: value must be a scalar (use 'in' for lists)").arg(where); return {}; }
            QVariant b;
            QString expr = col;
            if (r.kind == 2 && val.isDouble() && op != "like" && op != "contains") { b = val.toDouble(); expr = "CAST(" + col + " AS REAL)"; }
            else {
                b = bindValue(t, val, fname);
                if (!b.isValid()) { err = where + ": " + err; return {}; }
            }
            if (r.kind == 1) b = b.toString().toLower().remove(QRegularExpression("^#"));
            if (op == "like") test = col + " LIKE ? ESCAPE '\\'";
            else if (op == "contains") { test = col + " LIKE ? ESCAPE '\\'"; b = "%" + likeEscape(b.toString()) + "%"; }
            else test = expr + " " + (op == "!=" && r.kind == 0 ? "<>" : op == "!=" ? "=" : op) + " ?";
            local << b;
            if (op == "!=" && r.kind != 0) { // negated existence for tag/meta
                if (r.kind == 2) binds << r.metaKey;
                binds += local;
                return r.kind == 1 ? "NOT EXISTS(SELECT 1 FROM tags t WHERE t.note_id=n.id AND " + test + ")"
                                   : "NOT EXISTS(SELECT 1 FROM meta m WHERE m.note_id=n.id AND m.key=? AND " + test + ")";
            }
        }
        if (r.kind == 0) { binds += local; return test; }
        if (r.kind == 1) { binds += local; return "EXISTS(SELECT 1 FROM tags t WHERE t.note_id=n.id AND " + test + ")"; }
        binds << r.metaKey; // key first (appears first in text)
        binds += local;
        return "EXISTS(SELECT 1 FROM meta m WHERE m.note_id=n.id AND m.key=? AND " + test + ")";
    }
};

} // namespace

QueryEngine::Compiled QueryEngine::compile(const QJsonObject &spec) {
    Compiled out;
    auto fail = [&](const QString &m) { out.ok = false; out.error = m; return out; };
    static const QSet<QString> keys{"from", "where", "order", "select", "limit", "offset"};
    for (auto it = spec.begin(); it != spec.end(); ++it)
        if (!keys.contains(it.key())) return fail(QString("unknown key '%1' (allowed: from, where, order, select, limit, offset)").arg(it.key().left(40)));
    const QJsonValue from = spec.value("from");
    if (!from.isString() || !QStringList{"notes", "tasks", "links"}.contains(from.toString()))
        return fail("'from' must be one of \"notes\", \"tasks\", \"links\"");
    Builder b;
    b.src = from.toString();
    b.S = &source(b.src);

    // limit / offset
    int limit = 100;
    qint64 offset = 0;
    if (spec.contains("limit")) {
        if (!spec["limit"].isDouble()) return fail("'limit' must be a number");
        double l = spec["limit"].toDouble();
        if (!(l >= 1 && l <= kMaxLimit)) return fail(QString("'limit' must be between 1 and %1").arg(kMaxLimit));
        limit = int(l);
    }
    if (spec.contains("offset")) {
        if (!spec["offset"].isDouble()) return fail("'offset' must be a number");
        double o = spec["offset"].toDouble();
        if (!(o >= 0 && o <= 1e7)) return fail("'offset' must be between 0 and 10000000");
        offset = qint64(o);
    }
    // select
    QStringList sel;
    if (spec.contains("select")) {
        if (!spec["select"].isArray()) return fail("'select' must be an array of field names");
        for (const auto &v : spec["select"].toArray()) {
            if (!v.isString()) return fail("'select' entries must be strings");
            if (!sel.contains(v.toString())) sel << v.toString();
        }
        if (sel.isEmpty() || sel.size() > 16) return fail("'select' needs 1 to 16 fields");
    } else sel = b.S->defaults;
    QStringList cols;
    QString selSql;
    for (const QString &f : sel) {
        Builder::Ref r;
        if (!b.ref(f, &r)) return fail("select: " + b.err);
        if (!selSql.isEmpty()) selSql += ",";
        if (r.kind == 1) { selSql += "n.tags"; out.kinds << 'l'; }
        else if (r.kind == 2) {
            selSql += "(SELECT m.value FROM meta m WHERE m.note_id=n.id AND m.key=? ORDER BY m.id LIMIT 1)";
            b.binds << r.metaKey; out.kinds << 't';
        } else { selSql += r.f.expr; out.kinds << (r.f.type == T::Text ? 't' : r.f.type == T::Int ? 'i' : 'b'); }
        cols << f;
    }
    // where
    QStringList conds;
    if (spec.contains("where")) {
        if (!spec["where"].isArray()) return fail("'where' must be an array of {field, op, value}");
        QJsonArray w = spec["where"].toArray();
        if (w.size() > 16) return fail("'where' has too many conditions (max 16)");
        int i = 0;
        for (const auto &c : w) {
            if (!c.isObject()) return fail(QString("where[%1] must be an object").arg(i));
            QString s = b.condition(c.toObject(), i++);
            if (s.isEmpty()) return fail(b.err);
            conds << s;
        }
    }
    // order
    QStringList ord;
    if (spec.contains("order")) {
        if (!spec["order"].isArray()) return fail("'order' must be an array of {field, dir}");
        QJsonArray o = spec["order"].toArray();
        if (o.size() > 4) return fail("'order' has too many entries (max 4)");
        for (const auto &e : o) {
            if (!e.isObject()) return fail("'order' entries must be objects");
            QJsonObject eo = e.toObject();
            for (auto it = eo.begin(); it != eo.end(); ++it)
                if (it.key() != "field" && it.key() != "dir") return fail(QString("order: unknown key '%1'").arg(it.key().left(40)));
            if (!eo["field"].isString()) return fail("order: 'field' must be a string");
            QString dir = eo.contains("dir") ? eo["dir"].toString() : "asc";
            if (dir != "asc" && dir != "desc") return fail("order: 'dir' must be \"asc\" or \"desc\"");
            Builder::Ref r;
            if (!b.ref(eo["field"].toString(), &r)) return fail("order: " + b.err);
            QString d = dir == "asc" ? " ASC" : " DESC";
            if (r.kind == 1) return fail("order: 'tag' is multi-valued and cannot be ordered");
            if (r.kind == 2) {
                // order binds are collected after where binds (textual order below)
                // numbers sort numerically ("10" after "3"), then text; non-numeric values cast to 0
                const QString sub = "(SELECT m.value FROM meta m WHERE m.note_id=n.id AND m.key=? ORDER BY m.id LIMIT 1)";
                ord << "CAST(" + sub + " AS REAL)" + d << sub + d;
                out.binds << r.metaKey << r.metaKey; // stashed; moved after where binds below
            } else ord << r.f.expr + d;
        }
    }
    QVariantList orderBinds = out.binds;
    out.binds.clear();
    if (ord.isEmpty()) {
        if (b.src == "notes") ord << "n.mtime DESC";
    }
    for (const QString &t : b.S->tiebreak) ord << t;

    out.sql = "SELECT " + selSql + " FROM " + b.S->from + (conds.isEmpty() ? "" : " WHERE " + conds.join(" AND ")) +
              " ORDER BY " + ord.join(",") + " LIMIT ? OFFSET ?";
    // bind order = select metas, where binds, order metas, limit, offset
    out.binds = b.binds;
    // b.binds currently holds select binds followed by where binds (appended in that order) ✓
    out.binds += orderBinds;
    out.binds << (limit + 1) << offset;
    out.columns = cols;
    out.limit = limit;
    out.ok = true;
    return out;
}

// ---------------- execution ----------------
namespace {

struct Allowed { const char *table; const char *cols; };
constexpr Allowed kAllowed[] = {
    {"notes", ",id,path,title,folder,size,mtime,tags,"},
    {"tags", ",note_id,tag,"},
    {"meta", ",id,note_id,key,value,"},
    {"tasks", ",id,note_id,line,done,text,"},
    {"links", ",id,src_id,target_raw,target_resolved,kind,line,"},
};

int authorizer(void *, int action, const char *a1, const char *a2, const char *, const char *) {
    switch (action) {
    case SQLITE_SELECT: return SQLITE_OK;
    case SQLITE_READ: {
        if (!a1 || !a2) return SQLITE_DENY;
        for (const auto &t : kAllowed) {
            if (qstrcmp(a1, t.table) != 0) continue;
            if (!*a2) return SQLITE_OK; // table touched without a column (SELECT 1 / count)
            return QByteArray(t.cols).contains(QByteArray(",") + a2 + ",") ? SQLITE_OK : SQLITE_DENY;
        }
        return SQLITE_DENY;
    }
    case SQLITE_FUNCTION: return a2 && qstrcmp(a2, "like") == 0 ? SQLITE_OK : SQLITE_DENY;
    default: return SQLITE_DENY;
    }
}

struct Budget {
    QElapsedTimer timer;
    const std::atomic<quint64> *latest;
    quint64 id;
    bool cancelled = false, timedOut = false;
};
int progress(void *u) {
    auto *b = static_cast<Budget *>(u);
    if (b->latest && b->latest->load() != b->id) { b->cancelled = true; return 1; }
    if (b->timer.elapsed() > QueryEngine::kBudgetMs) { b->timedOut = true; return 1; }
    return 0;
}

} // namespace

QueryEngine::QueryEngine(QString dbPath) : m_path(std::move(dbPath)) {}
QueryEngine::~QueryEngine() { close(); }
void QueryEngine::close() { if (m_db) { sqlite3_close_v2(m_db); m_db = nullptr; } }

bool QueryEngine::ensureOpen(QString *err) {
    if (m_db) return true;
    if (!QFile::exists(m_path)) { *err = "index is not available yet"; return false; }
    if (sqlite3_open_v2(QFile::encodeName(m_path).constData(), &m_db, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, nullptr) != SQLITE_OK) {
        *err = "cannot open index read-only";
        close();
        return false;
    }
    sqlite3_busy_timeout(m_db, 0);
    sqlite3_exec(m_db, "PRAGMA query_only=ON", nullptr, nullptr, nullptr);
    sqlite3_db_config(m_db, SQLITE_DBCONFIG_DEFENSIVE, 1, nullptr);
    sqlite3_db_config(m_db, SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0, nullptr);
    sqlite3_db_config(m_db, SQLITE_DBCONFIG_ENABLE_TRIGGER, 0, nullptr);
    sqlite3_db_config(m_db, SQLITE_DBCONFIG_ENABLE_VIEW, 0, nullptr);
    sqlite3_limit(m_db, SQLITE_LIMIT_SQL_LENGTH, 32 * 1024);
    sqlite3_limit(m_db, SQLITE_LIMIT_EXPR_DEPTH, 40);
    sqlite3_limit(m_db, SQLITE_LIMIT_COMPOUND_SELECT, 1);
    sqlite3_limit(m_db, SQLITE_LIMIT_FUNCTION_ARG, 4);
    sqlite3_limit(m_db, SQLITE_LIMIT_VARIABLE_NUMBER, 512);
    sqlite3_limit(m_db, SQLITE_LIMIT_LIKE_PATTERN_LENGTH, 1100);
    sqlite3_limit(m_db, SQLITE_LIMIT_LENGTH, 1 << 20);
    sqlite3_set_authorizer(m_db, authorizer, nullptr);
    return true;
}

QueryResult QueryEngine::exec(const Compiled &c, const std::atomic<quint64> *latest, quint64 id) {
    QueryResult res;
    res.id = id;
    res.columns = c.columns;
    QElapsedTimer total;
    total.start();
    if (!ensureOpen(&res.error)) return res;
    Budget budget{{}, latest, id};
    budget.timer.start();
    sqlite3_progress_handler(m_db, 1000, progress, &budget);
    sqlite3_stmt *st = nullptr;
    const QByteArray sql = c.sql.toUtf8();
    const char *tail = nullptr;
    int rc = sqlite3_prepare_v2(m_db, sql.constData(), int(sql.size()), &st, &tail);
    if (rc == SQLITE_OK && tail && QByteArray(tail).trimmed().size() > 0) { sqlite3_finalize(st); st = nullptr; rc = SQLITE_MISUSE; }
    if (rc != SQLITE_OK) {
        res.error = rc == SQLITE_AUTH ? "query touches data that is not allowed" : rc == SQLITE_MISUSE ? QString("only a single statement is allowed") : QString("query rejected: %1").arg(QString::fromUtf8(sqlite3_errmsg(m_db)));
        sqlite3_progress_handler(m_db, 0, nullptr, nullptr);
        return res;
    }
    int i = 1;
    for (const QVariant &v : c.binds) {
        switch (v.typeId()) {
        case QMetaType::Int: case QMetaType::LongLong: case QMetaType::UInt: case QMetaType::ULongLong:
            sqlite3_bind_int64(st, i, v.toLongLong()); break;
        case QMetaType::Double: sqlite3_bind_double(st, i, v.toDouble()); break;
        default: { QByteArray u = v.toString().toUtf8(); sqlite3_bind_text(st, i, u.constData(), int(u.size()), SQLITE_TRANSIENT); }
        }
        ++i;
    }
    const int ncol = sqlite3_column_count(st);
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        if (res.rows.size() >= c.limit) { res.hasMore = true; break; }
        QVariantList row;
        for (int k = 0; k < ncol; ++k) {
            const char kind = k < c.kinds.size() ? c.kinds[k] : 't';
            if (sqlite3_column_type(st, k) == SQLITE_NULL) { row << QVariant(); continue; }
            switch (kind) {
            case 'i': row << QVariant::fromValue<qlonglong>(sqlite3_column_int64(st, k)); break;
            case 'b': row << (sqlite3_column_int(st, k) != 0); break;
            case 'l': row << QVariant(QString::fromUtf8(reinterpret_cast<const char *>(sqlite3_column_text(st, k))).split(' ', Qt::SkipEmptyParts)); break;
            default: row << QString::fromUtf8(reinterpret_cast<const char *>(sqlite3_column_text(st, k)), sqlite3_column_bytes(st, k)); break;
            }
        }
        res.rows.append(row);
    }
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
        if (budget.timedOut) res.error = QString("query exceeded the %1 ms budget").arg(kBudgetMs);
        else if (budget.cancelled) res.error = "query superseded";
        else res.error = QString("query failed: %1").arg(QString::fromUtf8(sqlite3_errmsg(m_db)));
        res.rows.clear();
        res.hasMore = false;
    } else res.ok = true;
    sqlite3_finalize(st);
    sqlite3_progress_handler(m_db, 0, nullptr, nullptr);
    res.ms = total.nsecsElapsed() / 1e6;
    return res;
}

QueryResult QueryEngine::run(const QJsonObject &spec, const std::atomic<quint64> *latest, quint64 id) {
    try {
        Compiled c = compile(spec);
        if (!c.ok) { QueryResult r; r.id = id; r.error = c.error; return r; }
        return exec(c, latest, id);
    } catch (...) {
        QueryResult r;
        r.error = "internal error";
        return r;
    }
}

QueryResult QueryEngine::run(const QByteArray &json) {
    QueryResult r;
    if (json.size() > 64 * 1024) { r.error = "query is too large (max 64 KiB)"; return r; }
    QJsonParseError pe;
    QJsonDocument d = QJsonDocument::fromJson(json, &pe);
    if (pe.error != QJsonParseError::NoError || !d.isObject()) { r.error = "query must be a JSON object"; return r; }
    return run(d.object());
}

QueryResult QueryEngine::runAuthorizedSqlForTests(const QString &sql) {
    Compiled c;
    c.ok = true;
    c.sql = sql;
    c.limit = 1000000;
    return exec(c, nullptr, 0);
}

QJsonObject QueryResult::toJson() const {
    QJsonObject o{{"ok", ok}};
    if (!ok) { o["error"] = error; return o; }
    o["columns"] = QJsonArray::fromStringList(columns);
    QJsonArray rs;
    for (const auto &r : rows) {
        QJsonArray ra;
        for (const QVariant &v : r) {
            if (!v.isValid()) ra.append(QJsonValue::Null);
            else if (v.typeId() == QMetaType::QStringList) ra.append(QJsonArray::fromStringList(v.toStringList()));
            else if (v.typeId() == QMetaType::Bool) ra.append(v.toBool());
            else if (v.typeId() == QMetaType::LongLong) ra.append(double(v.toLongLong()));
            else ra.append(v.toString());
        }
        rs.append(ra);
    }
    o["rows"] = rs;
    o["hasMore"] = hasMore;
    return o;
}

} // namespace hn::core
