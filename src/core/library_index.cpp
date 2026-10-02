#include "hn/core/library_index.h"
#include "hn/core/frontmatter.h"
#include "hn/core/fs_util.h"
#include "hn/core/links.h"
#include "hn/core/paths.h"
#include "markdown_internal.h"
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QJsonObject>
#include <QMultiHash>
#include <QPointer>
#include <QRegularExpression>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>
#include <memory>

namespace hn::core {
namespace {

// ---------- meta extraction (MD4C callbacks, no AST) ----------
struct MetaCtx {
    int inCode = 0, inLink = 0, inHeading = 0;
    bool titleDone = false;
    QString title, block;
    QStringList tags;

    static bool tagChar(uint c) { return QChar::isLetterOrNumber(c) || QChar::isMark(c) || c == '_' || c == '-'; }

    void flush() {
        // A tag starts at the text start or after whitespace; '#' then 1+ tag chars. "# " never matches.
        const QList<uint> cp = block.toUcs4();
        block.clear();
        for (qsizetype i = 0; i < cp.size(); ++i) {
            if (cp[i] != '#' || (i > 0 && !QChar::isSpace(cp[i - 1]))) continue;
            qsizetype j = i + 1;
            while (j < cp.size() && tagChar(cp[j])) ++j;
            if (j == i + 1) continue;
            QString t = QString::fromUcs4(reinterpret_cast<const char32_t *>(cp.constData() + i + 1), j - i - 1).toLower();
            if (!tags.contains(t)) tags << t;
            i = j - 1;
        }
    }
};
constexpr QChar kHole(0x1); // code/link/entity text: not a boundary, never a tag char

int mEnterBlock(MD_BLOCKTYPE t, void *d, void *u) {
    auto *c = static_cast<MetaCtx *>(u);
    c->flush();
    if (t == MD_BLOCK_CODE) ++c->inCode;
    else if (t == MD_BLOCK_H && !c->titleDone) { c->inHeading = 1; (void)d; }
    return 0;
}
int mLeaveBlock(MD_BLOCKTYPE t, void *, void *u) {
    auto *c = static_cast<MetaCtx *>(u);
    c->flush();
    if (t == MD_BLOCK_CODE) --c->inCode;
    else if (t == MD_BLOCK_H && c->inHeading) { c->inHeading = 0; c->titleDone = true; }
    return 0;
}
int mEnterSpan(MD_SPANTYPE t, void *, void *u) {
    auto *c = static_cast<MetaCtx *>(u);
    if (t == MD_SPAN_CODE) { ++c->inCode; c->block += kHole; }
    else if (t == MD_SPAN_A || t == MD_SPAN_IMG) { ++c->inLink; c->block += kHole; }
    return 0;
}
int mLeaveSpan(MD_SPANTYPE t, void *, void *u) {
    auto *c = static_cast<MetaCtx *>(u);
    if (t == MD_SPAN_CODE) --c->inCode;
    else if (t == MD_SPAN_A || t == MD_SPAN_IMG) { --c->inLink; c->block += kHole; }
    return 0;
}
int mText(MD_TEXTTYPE t, const MD_CHAR *s, MD_SIZE n, void *u) {
    auto *c = static_cast<MetaCtx *>(u);
    QString txt;
    switch (t) {
    case MD_TEXT_SOFTBR: case MD_TEXT_BR: txt = "\n"; break;
    case MD_TEXT_NULLCHAR: txt = QString(QChar::ReplacementCharacter); break;
    case MD_TEXT_ENTITY: txt = detail::decodeEntity(s, n); break;
    case MD_TEXT_HTML: return 0;
    default: txt = detail::u8(s, n); break;
    }
    if (c->inHeading) c->title += txt;
    if (c->inCode || c->inLink) return 0;
    c->block += (t == MD_TEXT_ENTITY ? QString(kHole) : txt);
    return 0;
}
} // namespace

NoteMeta extractMeta(const QString &fileBaseName, const QByteArray &utf8) {
    MetaCtx ctx;
    Frontmatter fm = parseFrontmatter(utf8);
    qsizetype skip = fm.present ? fm.endOffset : (utf8.startsWith("\xEF\xBB\xBF") ? 3 : 0);
    MD_PARSER p{};
    p.flags = MD_FLAG_STRIKETHROUGH | MD_FLAG_TASKLISTS;
    p.enter_block = mEnterBlock; p.leave_block = mLeaveBlock; p.enter_span = mEnterSpan;
    p.leave_span = mLeaveSpan; p.text = mText;
    md_parse(utf8.constData() + skip, MD_SIZE(utf8.size() - skip), &p, &ctx);
    ctx.flush();
    NoteMeta m;
    m.title = !fm.title.isEmpty() ? fm.title : ctx.title.simplified();
    if (m.title.isEmpty()) m.title = fileBaseName;
    m.tags = fm.tags;
    for (const auto &t : ctx.tags) if (!m.tags.contains(t)) m.tags << t;
    m.aliases = fm.aliases;
    return m;
}

// ---------- name resolution (pure, over in-memory maps) ----------
namespace {
QString normKey(QString t) {
    t = t.trimmed();
    t.replace('\\', '/');
    while (t.startsWith('/')) t.remove(0, 1);
    if (t.endsWith(".md", Qt::CaseInsensitive)) t.chop(3);
    return t;
}
QString tailOf(const QString &key) { return key.mid(key.lastIndexOf('/') + 1).toLower(); }
QString dirOf(const QString &p) { int i = p.lastIndexOf('/'); return i < 0 ? QString() : p.left(i); }
QString noExt(QString p) { if (p.endsWith(".md", Qt::CaseInsensitive)) p.chop(3); return p; }
} // namespace

struct NameMaps {
    QHash<QString, qint64> idByPath;
    QHash<qint64, QString> pathById;
    QMultiHash<QString, qint64> byTail;   // lowercase basename without extension
    QMultiHash<QString, qint64> byAlias;  // lowercase alias
    QHash<qint64, QStringList> aliasesById; // lowercase
    void add(qint64 id, const QString &path, const QStringList &aliasLc) {
        idByPath.insert(path, id); pathById.insert(id, path);
        byTail.insert(tailOf(noExt(path)), id);
        for (const auto &a : aliasLc) byAlias.insert(a, id);
        aliasesById.insert(id, aliasLc);
    }
    void remove(qint64 id) {
        auto it = pathById.find(id);
        if (it == pathById.end()) return;
        idByPath.remove(it.value());
        byTail.remove(tailOf(noExt(it.value())), id);
        for (const auto &a : aliasesById.value(id)) byAlias.remove(a, id);
        aliasesById.remove(id);
        pathById.erase(it);
    }
    Resolution resolve(const QString &target, const QString &from) const {
        Resolution r;
        QString key = normKey(target);
        if (key.isEmpty()) return r;
        const bool rel = key.startsWith("./") || key.startsWith("../");
        QString exact = rel ? QDir::cleanPath(dirOf(from) + "/" + key) : QDir::cleanPath(key);
        if (!exact.startsWith("../") && exact != ".." ) {
            auto it = idByPath.constFind(exact + ".md");
            if (it != idByPath.constEnd()) { r.state = Resolution::Resolved; r.via = Resolution::Path; r.path = exact + ".md"; return r; }
            // target written with a different extension case, e.g. "A.MD"
        }
        if (rel) return r;
        const QString lkey = QDir::cleanPath(key).toLower();
        const bool hasSlash = lkey.contains('/');
        QStringList cands;
        for (qint64 id : byTail.values(tailOf(lkey))) {
            const QString &p = pathById[id];
            if (!hasSlash) cands << p;
            else { QString lp = noExt(p).toLower(); if (lp == lkey || lp.endsWith("/" + lkey)) cands << p; }
        }
        if (cands.isEmpty()) {
            QString a = target.trimmed().toLower();
            for (qint64 id : byAlias.values(a)) cands << pathById[id];
            if (cands.size() == 1) { r.state = Resolution::Resolved; r.via = Resolution::Alias; r.path = cands[0]; return r; }
        } else if (cands.size() == 1) { r.state = Resolution::Resolved; r.via = Resolution::Basename; r.path = cands[0]; return r; }
        if (cands.size() > 1) {
            cands.removeDuplicates();
            cands.sort();
            if (cands.size() > 20) cands = cands.mid(0, 20);
            r.state = Resolution::Ambiguous;
            r.candidates = cands;
            r.via = Resolution::None;
        }
        return r;
    }
};

// ---------- SQLite (worker thread only) ----------
struct LibraryIndex::Impl {
    QString root, dbPath, conn;
    bool opened = false;
    NameMaps maps;
    bool mapsLoaded = false;
    QSet<QString> pendTails, pendAliases;
    std::unique_ptr<QueryEngine> qe;

    QSqlDatabase db() const { return QSqlDatabase::database(conn, false); }
    bool exec(const QString &sql) { QSqlQuery q(db()); return q.exec(sql); }

    bool open() {
        if (opened) return true;
        QDir().mkpath(QFileInfo(dbPath).absolutePath());
        for (int attempt = 0; attempt < 2; ++attempt) {
            {
                QSqlDatabase d = QSqlDatabase::contains(conn) ? QSqlDatabase::database(conn, false)
                                                              : QSqlDatabase::addDatabase("QSQLITE", conn);
                d.setDatabaseName(dbPath);
                bool ok = d.open();
                int ver = -1;
                if (ok) {
                    exec("PRAGMA cache_size=-4096");   // 4 MiB page cache
                    exec("PRAGMA temp_store=1");       // file-backed temp work
                    exec("PRAGMA synchronous=OFF");    // rebuildable cache
                    exec("PRAGMA journal_mode=MEMORY");
                    QSqlQuery q(d);
                    if (q.exec("PRAGMA user_version") && q.next()) ver = q.value(0).toInt();
                }
                if (ok && ver == kSchemaVersion) { opened = true; return true; }
                if (ok && ver == 0 && createSchema()) { opened = true; return true; }
                d.close();
            }
            QSqlDatabase::removeDatabase(conn);
            QFile::remove(dbPath); // corrupt or other schema version: cache is disposable => rebuilt by sync()
        }
        return false;
    }
    static constexpr int kSchemaVersion = 2;
    bool createSchema() {
        const char *stmts[] = {
            "CREATE TABLE notes(id INTEGER PRIMARY KEY, path TEXT UNIQUE NOT NULL, title TEXT, folder TEXT, size INTEGER, mtime INTEGER, fileid TEXT, tags TEXT)",
            "CREATE INDEX notes_mtime ON notes(mtime DESC)", "CREATE INDEX notes_folder ON notes(folder)",
            "CREATE TABLE tags(note_id INTEGER NOT NULL, tag TEXT NOT NULL)",
            "CREATE INDEX tags_tag ON tags(tag)", "CREATE INDEX tags_note ON tags(note_id)",
            "CREATE TABLE folders(path TEXT PRIMARY KEY)",
            "CREATE VIRTUAL TABLE fts USING fts5(title, body, tags, tokenize='unicode61 remove_diacritics 2', prefix='2 3')",
            // v2: link graph, frontmatter fields, tasks, aliases. target_resolved = destination note id (NULL: unresolved/ambiguous)
            "CREATE TABLE links(id INTEGER PRIMARY KEY, src_id INTEGER NOT NULL, target_raw TEXT NOT NULL, target_key TEXT, target_tail TEXT, target_lc TEXT,"
            " target_resolved INTEGER, ambiguous INTEGER NOT NULL DEFAULT 0, anchor TEXT, alias TEXT, kind TEXT NOT NULL, line INTEGER, context TEXT)",
            "CREATE INDEX links_src ON links(src_id)", "CREATE INDEX links_dest ON links(target_resolved)",
            "CREATE INDEX links_tail ON links(target_tail)", "CREATE INDEX links_lc ON links(target_lc)",
            "CREATE TABLE meta(id INTEGER PRIMARY KEY, note_id INTEGER NOT NULL, key TEXT NOT NULL, value TEXT)",
            "CREATE INDEX meta_note ON meta(note_id)", "CREATE INDEX meta_kv ON meta(key, value)",
            "CREATE TABLE tasks(id INTEGER PRIMARY KEY, note_id INTEGER NOT NULL, line INTEGER, done INTEGER, text TEXT)",
            "CREATE INDEX tasks_note ON tasks(note_id)", "CREATE INDEX tasks_done ON tasks(done)",
            "CREATE TABLE aliases(note_id INTEGER NOT NULL, alias TEXT NOT NULL, alias_lc TEXT NOT NULL)",
            "CREATE INDEX aliases_lc ON aliases(alias_lc)", "CREATE INDEX aliases_note ON aliases(note_id)",
            "PRAGMA user_version=2"};
        for (auto *s : stmts) if (!exec(QString::fromLatin1(s))) return false;
        return true;
    }
    void close() {
        qe.reset();
        mapsLoaded = false; maps = {}; pendTails.clear(); pendAliases.clear();
        if (!opened && !QSqlDatabase::contains(conn)) return;
        { QSqlDatabase d = db(); if (d.isOpen()) d.close(); }
        QSqlDatabase::removeDatabase(conn);
        opened = false;
    }
    void reset() { close(); QFile::remove(dbPath); }

    void ensureMaps() {
        if (mapsLoaded) return;
        maps = {};
        QSqlQuery q(db());
        q.setForwardOnly(true);
        QHash<qint64, QStringList> al;
        QSqlQuery a(db());
        a.setForwardOnly(true);
        a.exec("SELECT note_id,alias_lc FROM aliases");
        while (a.next()) al[a.value(0).toLongLong()] << a.value(1).toString();
        q.exec("SELECT id,path FROM notes");
        while (q.next()) maps.add(q.value(0).toLongLong(), q.value(1).toString(), al.value(q.value(0).toLongLong()));
        mapsLoaded = true;
    }
    void dirtyName(const QString &path, const QStringList &aliasLc) {
        pendTails.insert(tailOf(noExt(path)));
        for (const auto &a : aliasLc) pendAliases.insert(a);
    }

    void removeRowById(const QVariant &id) {
        for (auto *sql : {"DELETE FROM fts WHERE rowid=?", "DELETE FROM tags WHERE note_id=?", "DELETE FROM notes WHERE id=?",
                          "DELETE FROM links WHERE src_id=?", "DELETE FROM tasks WHERE note_id=?", "DELETE FROM meta WHERE note_id=?",
                          "DELETE FROM aliases WHERE note_id=?"}) {
            QSqlQuery d(db());
            d.prepare(sql);
            d.addBindValue(id);
            d.exec();
        }
    }
    void removeRow(const QString &path) {
        ensureMaps();
        auto it = maps.idByPath.constFind(path);
        if (it == maps.idByPath.constEnd()) return;
        qint64 id = it.value();
        dirtyName(path, maps.aliasesById.value(id));
        removeRowById(id);
        QSqlQuery u(db());
        u.prepare("UPDATE links SET target_resolved=NULL, ambiguous=0 WHERE target_resolved=?");
        u.addBindValue(id);
        u.exec();
        maps.remove(id);
    }

    // Returns 1 if the row was new, 0 if updated, -1 if the file vanished/unreadable.
    int upsert(const FileInfo &fi) {
        ensureMaps();
        QByteArray bytes;
        if (!readAll(root + "/" + fi.relPath, &bytes, nullptr)) { removeRow(fi.relPath); return -1; }
        NoteMeta m = extractMeta(QFileInfo(fi.relPath).completeBaseName(), bytes);
        NoteGraph g = analyzeNote(bytes);
        Frontmatter fm = parseFrontmatter(bytes);
        QString body = QString::fromUtf8(bytes.left(2 << 20));
        QString folder = QFileInfo(fi.relPath).path();
        if (folder == ".") folder.clear();
        auto old = maps.idByPath.constFind(fi.relPath);
        bool isNew = old == maps.idByPath.constEnd();
        QVariant id = isNew ? QVariant() : QVariant(old.value());
        QStringList oldAliases;
        if (!isNew) { oldAliases = maps.aliasesById.value(old.value()); removeRowById(id); maps.remove(old.value()); }
        QSqlQuery ins(db());
        ins.prepare("INSERT INTO notes(id,path,title,folder,size,mtime,fileid,tags) VALUES(?,?,?,?,?,?,?,?)");
        for (const QVariant &v : {id, QVariant(fi.relPath), QVariant(m.title), QVariant(folder), QVariant(fi.size),
                                  QVariant(fi.mtimeMs), QVariant(fi.fileId), QVariant(m.tags.join(' '))})
            ins.addBindValue(v);
        if (!ins.exec()) return -1;
        QVariant nid = ins.lastInsertId();
        QSqlQuery f(db());
        f.prepare("INSERT INTO fts(rowid,title,body,tags) VALUES(?,?,?,?)");
        for (const QVariant &v : {nid, QVariant(m.title), QVariant(body), QVariant(m.tags.join(' '))}) f.addBindValue(v);
        f.exec();
        if (!m.tags.isEmpty()) {
            QSqlQuery tq(db());
            tq.prepare("INSERT INTO tags(note_id,tag) VALUES(?,?)");
            for (const auto &t : m.tags) { tq.addBindValue(nid); tq.addBindValue(t); tq.exec(); }
        }
        // aliases
        QStringList aliasLc;
        {
            QSqlQuery aq(db());
            aq.prepare("INSERT INTO aliases(note_id,alias,alias_lc) VALUES(?,?,?)");
            for (const auto &a : m.aliases) {
                QString lc = a.toLower();
                if (aliasLc.contains(lc)) continue;
                aliasLc << lc;
                aq.addBindValue(nid); aq.addBindValue(a); aq.addBindValue(lc); aq.exec();
            }
        }
        // frontmatter fields (one row per scalar / list item), capped
        if (fm.present) {
            QSqlQuery mq(db());
            mq.prepare("INSERT INTO meta(note_id,key,value) VALUES(?,?,?)");
            int rows = 0;
            for (auto it = fm.fields.cbegin(); it != fm.fields.cend() && rows < 128; ++it)
                for (const QString &v : it->items) {
                    if (rows++ >= 128) break;
                    mq.addBindValue(nid); mq.addBindValue(it.key()); mq.addBindValue(v.left(1024)); mq.exec();
                }
        }
        qint64 idn = nid.toLongLong();
        maps.add(idn, fi.relPath, aliasLc);
        if (isNew || oldAliases != aliasLc) { dirtyName(fi.relPath, aliasLc); for (const auto &a : oldAliases) pendAliases.insert(a); }
        // links (resolved against everything known so far; flushResolve() repairs the rest)
        if (!g.links.isEmpty()) {
            QSqlQuery lq(db());
            lq.prepare("INSERT INTO links(src_id,target_raw,target_key,target_tail,target_lc,target_resolved,ambiguous,anchor,alias,kind,line,context)"
                       " VALUES(?,?,?,?,?,?,?,?,?,?,?,?)");
            for (const LinkRef &l : g.links) {
                Resolution r = maps.resolve(l.target, fi.relPath);
                QString key = normKey(l.target);
                lq.addBindValue(nid); lq.addBindValue(l.target); lq.addBindValue(key); lq.addBindValue(tailOf(key));
                lq.addBindValue(l.target.trimmed().toLower());
                lq.addBindValue(r.state == Resolution::Resolved ? QVariant(maps.idByPath.value(r.path)) : QVariant());
                lq.addBindValue(r.state == Resolution::Ambiguous ? 1 : 0);
                lq.addBindValue(l.anchor); lq.addBindValue(l.alias); lq.addBindValue(l.kindName()); lq.addBindValue(l.line); lq.addBindValue(l.context);
                lq.exec();
            }
        }
        if (!g.tasks.isEmpty()) {
            QSqlQuery tq(db());
            tq.prepare("INSERT INTO tasks(note_id,line,done,text) VALUES(?,?,?,?)");
            for (const TaskItem &t : g.tasks) { tq.addBindValue(nid); tq.addBindValue(t.line); tq.addBindValue(t.done ? 1 : 0); tq.addBindValue(t.text.left(2000)); tq.exec(); }
        }
        return isNew ? 1 : 0;
    }

    // Re-resolve stored links whose target may have changed meaning (notes added/removed/renamed, aliases changed).
    void flushResolve() {
        if (pendTails.isEmpty() && pendAliases.isEmpty()) return;
        ensureMaps();
        struct Row { qint64 id, src, dest; int amb; QString raw; };
        QList<Row> rows;
        auto collect = [&](QSqlQuery &q) {
            while (q.next()) rows.append({q.value(0).toLongLong(), q.value(1).toLongLong(), q.value(3).isNull() ? -1 : q.value(3).toLongLong(), q.value(4).toInt(), q.value(2).toString()});
        };
        const char *cols = "SELECT id,src_id,target_raw,target_resolved,ambiguous FROM links";
        if (pendTails.size() + pendAliases.size() > 200) {
            QSqlQuery q(db());
            q.setForwardOnly(true);
            q.exec(cols);
            collect(q);
        } else {
            auto run = [&](const char *col, const QStringList &keys) {
                for (qsizetype i = 0; i < keys.size(); i += 100) {
                    QStringList part = keys.mid(i, 100);
                    QSqlQuery q(db());
                    q.setForwardOnly(true);
                    q.prepare(QString("%1 WHERE %2 IN (%3)").arg(cols, col, QString("?,").repeated(part.size()).chopped(1)));
                    for (const auto &k : part) q.addBindValue(k);
                    q.exec();
                    collect(q);
                }
            };
            run("target_tail", QStringList(pendTails.cbegin(), pendTails.cend()));
            run("target_lc", QStringList(pendAliases.cbegin(), pendAliases.cend()));
        }
        pendTails.clear(); pendAliases.clear();
        QSqlQuery up(db());
        up.prepare("UPDATE links SET target_resolved=?, ambiguous=? WHERE id=?");
        QSet<qint64> seen;
        for (const Row &r : rows) {
            if (seen.contains(r.id)) continue;
            seen.insert(r.id);
            Resolution res = maps.resolve(r.raw, maps.pathById.value(r.src));
            qint64 dest = res.state == Resolution::Resolved ? maps.idByPath.value(res.path) : -1;
            int amb = res.state == Resolution::Ambiguous ? 1 : 0;
            if (dest == r.dest && amb == r.amb) continue;
            up.addBindValue(dest < 0 ? QVariant() : QVariant(dest)); up.addBindValue(amb); up.addBindValue(r.id);
            up.exec();
        }
    }
    QueryEngine &engine() { if (!qe) qe = std::make_unique<QueryEngine>(dbPath); return *qe; }
};

namespace {
QStringList ftsTerms(const QString &text) {
    static const QRegularExpression sep(R"([^\p{L}\p{N}\p{M}]+)");
    return text.split(sep, Qt::SkipEmptyParts);
}

QString fold(const QString &s) {
    QString n = s.normalized(QString::NormalizationForm_D).toCaseFolded(), o;
    for (QChar c : n) if (!c.isMark()) o += c;
    return o;
}

IndexRow rowFrom(QSqlQuery &q) {
    IndexRow r;
    r.relPath = q.value(0).toString(); r.title = q.value(1).toString(); r.folder = q.value(2).toString();
    r.size = q.value(3).toLongLong(); r.mtimeMs = q.value(4).toLongLong();
    r.tags = q.value(5).toString().split(' ', Qt::SkipEmptyParts);
    r.snippet = q.value(6).toString();
    return r;
}

// nullopt => cancelled
std::optional<SearchPage> doSearch(LibraryIndex::Impl &im, const IndexQuery &iq, quint64 id, const std::atomic<quint64> *latest) {
    SearchPage page;
    page.id = id;
    if (!im.open()) return page;
    QStringList conds;
    QVariantList binds, orderBinds;
    QString from, order, snip = "''";
    QString text = iq.text.trimmed();
    if (!text.isEmpty()) {
        // Every whitespace-separated word becomes one quoted FTS phrase of its alphanumeric tokens
        // ("C++" -> "C", "a:b" -> "a b", "foo-bar" -> "foo bar"); the last gets a prefix star. No user
        // character reaches FTS syntax: tokens contain only letters/digits/marks.
        QStringList words;
        for (const auto &w : text.split(QRegularExpression(R"(\s+)"), Qt::SkipEmptyParts)) {
            QStringList t = ftsTerms(w);
            if (!t.isEmpty()) words << "\"" + t.join(' ') + "\"";
        }
        if (words.isEmpty()) return page;
        words.last() += "*";
        QString match = words.join(' ');
        from = "fts JOIN notes n ON n.id=fts.rowid";
        conds << "fts MATCH ?";
        binds << match;
        // Prefix matches on short words ("C", "and") hit most notes: notes containing the text exactly as typed come first.
        order = "ORDER BY (instr(fts.body,?)>0 OR instr(fts.title,?)>0) DESC, fts.rank";
        orderBinds << text << text;
        snip = "snippet(fts,1,'[',']','...',12)";
    } else {
        from = "notes n";
        order = "ORDER BY n.mtime DESC, n.path";
    }
    if (!iq.folder.isEmpty()) { conds << "(n.folder=? OR n.folder LIKE ? ESCAPE '\\')"; binds << iq.folder << QString(iq.folder).replace('\\', "\\\\").replace('%', "\\%").replace('_', "\\_") + "/%"; }
    if (!iq.tag.isEmpty()) { conds << "EXISTS(SELECT 1 FROM tags t WHERE t.note_id=n.id AND t.tag=?)"; binds << iq.tag.toLower(); }
    QString sql = "SELECT n.path,n.title,n.folder,n.size,n.mtime,n.tags," + snip + " FROM " + from +
                  (conds.isEmpty() ? "" : " WHERE " + conds.join(" AND ")) + " " + order + " LIMIT ? OFFSET ?";
    QSqlQuery q(im.db());
    q.setForwardOnly(true);
    q.prepare(sql);
    for (auto &b : binds) q.addBindValue(b);
    for (auto &b : orderBinds) q.addBindValue(b);
    q.addBindValue(iq.limit + 1);
    q.addBindValue(iq.offset);
    if (!q.exec()) return page;
    int n = 0;
    while (q.next()) {
        if (latest && (++n & 15) == 0 && latest->load() != id) return std::nullopt;
        page.rows.append(rowFrom(q));
    }
    if (page.rows.size() > iq.limit) { page.rows.removeLast(); page.hasMore = true; }
    if (latest && latest->load() != id) return std::nullopt;
    return page;
}

struct SyncJob {
    QList<FileInfo> todo;
    QSet<QString> isNew;
    int pos = 0, added = 0, updated = 0, removed = 0;
    quint64 gen = 0;
};
struct SyncCtx {
    std::shared_ptr<LibraryIndex::Impl> impl;
    std::shared_ptr<std::atomic<quint64>> genp;
    std::shared_ptr<SyncJob> job;
    QPointer<LibraryIndex> self;
    Worker *w;
};
} // namespace

// Apply up to 32 entries per worker task, re-posting at Index priority so saves interleave.
void LibraryIndex::syncChunk(std::shared_ptr<void> vctx) {
    auto c = std::static_pointer_cast<SyncCtx>(vctx);
    auto &job = *c->job;
    if (c->genp->load() != job.gen) return; // superseded by a newer sync/rebuild
    c->impl->exec("BEGIN");
    for (int n = 0; n < 32 && job.pos < job.todo.size(); ++n, ++job.pos) {
        int r = c->impl->upsert(job.todo[job.pos]);
        if (r == 1) ++job.added; else if (r == 0) ++job.updated;
    }
    const bool last = job.pos >= job.todo.size();
    if (last) c->impl->flushResolve();
    c->impl->exec("COMMIT");
    if (!last) { c->w->post(Worker::Index, [c] { syncChunk(c); }); return; }
    QPointer<LibraryIndex> s = c->self;
    quint64 gen = job.gen;
    int a = job.added, u = job.updated, r = job.removed;
    if (s) QMetaObject::invokeMethod(s.data(), [s, gen, a, u, r] { if (s) { s->m_cache.clear(); emit s->synced(gen, a, u, r); } }, Qt::QueuedConnection);
}

LibraryIndex::LibraryIndex(QString root, QString cacheDir, QObject *parent, Worker *worker)
    : QObject(parent), m_impl(std::make_shared<Impl>()), m_root(QDir::cleanPath(QFileInfo(root).absoluteFilePath())),
      m_worker(worker ? worker : &Worker::shared()), m_gen(std::make_shared<std::atomic<quint64>>(0)),
      m_searchId(std::make_shared<std::atomic<quint64>>(0)), m_backId(std::make_shared<std::atomic<quint64>>(0)),
      m_taskId(std::make_shared<std::atomic<quint64>>(0)), m_queryId(std::make_shared<std::atomic<quint64>>(0)) {
    qRegisterMetaType<hn::core::SearchPage>();
    qRegisterMetaType<hn::core::LinkPage>();
    qRegisterMetaType<hn::core::TaskPage>();
    qRegisterMetaType<hn::core::QueryResult>();
    if (cacheDir.isEmpty()) cacheDir = paths::cacheDir();
    m_dbPath = cacheDir + "/index.sqlite3";
    m_impl->root = m_root;
    m_impl->dbPath = m_dbPath;
    m_impl->conn = "hn-index-" + QString::number(quintptr(m_impl.get()), 16);
}

LibraryIndex::~LibraryIndex() {
    ++*m_gen;
    ++*m_searchId; ++*m_backId; ++*m_taskId; ++*m_queryId;
    auto im = m_impl;
    m_worker->callBlocking(Worker::Index, [im] { im->close(); });
}

quint64 LibraryIndex::sync() {
    quint64 gen = ++*m_gen;
    auto impl = m_impl;
    auto genp = m_gen;
    QPointer<LibraryIndex> self(this);
    Worker *w = m_worker;
    QString key = "sync:" + QString::number(quintptr(this), 16);
    w->post(Worker::Index, [=] {
        if (genp->load() != gen) return;
        impl->open();
        if (!impl->opened) return;
        // Scan + diff against the DB, then apply in small chunks so saves can interleave.
        auto job = std::make_shared<SyncJob>();
        job->gen = gen;
        QStringList folders;
        QList<FileInfo> disk = scanNotes(impl->root, &folders);
        QHash<QString, FileInfo> known;
        {
            QSqlQuery q(impl->db());
            q.exec("SELECT path,size,mtime,fileid FROM notes");
            while (q.next()) known.insert(q.value(0).toString(), {q.value(0).toString(), q.value(1).toLongLong(), q.value(2).toLongLong(), q.value(3).toString()});
        }
        impl->exec("BEGIN");
        QSet<QString> present;
        for (const auto &f : disk) {
            present.insert(f.relPath);
            auto it = known.constFind(f.relPath);
            if (it == known.constEnd()) { job->todo << f; job->isNew.insert(f.relPath); }
            else if (it->size != f.size || it->mtimeMs != f.mtimeMs || it->fileId != f.fileId) job->todo << f;
        }
        for (auto it = known.cbegin(); it != known.cend(); ++it)
            if (!present.contains(it.key())) { impl->removeRow(it.key()); ++job->removed; }
        impl->exec("DELETE FROM folders");
        for (const auto &f : folders) {
            QSqlQuery q(impl->db());
            q.prepare("INSERT OR IGNORE INTO folders(path) VALUES(?)");
            q.addBindValue(f);
            q.exec();
        }
        impl->exec("COMMIT");
        syncChunk(std::make_shared<SyncCtx>(SyncCtx{impl, genp, job, self, w}));
    }, key);
    return gen;
}

quint64 LibraryIndex::rebuild() {
    auto impl = m_impl;
    ++*m_gen; // abort a running sync; sync() below bumps again
    m_worker->post(Worker::Index, [impl] { impl->reset(); }, "reset:" + QString::number(quintptr(this), 16));
    return sync();
}

void LibraryIndex::updatePath(const QString &rel) {
    auto impl = m_impl;
    auto genp = m_gen;
    QPointer<LibraryIndex> self(this);
    m_worker->post(Worker::Index, [=] {
        if (!impl->open()) return;
        QFileInfo fi(impl->root + "/" + rel);
        impl->exec("BEGIN");
        if (!fi.isFile()) impl->removeRow(rel);
        else impl->upsert({rel, fi.size(), fi.lastModified().toMSecsSinceEpoch(), fileIdOf(fi.filePath())});
        impl->flushResolve();
        impl->exec("COMMIT");
        quint64 g = genp->load();
        if (QPointer<LibraryIndex> s = self; s)
            QMetaObject::invokeMethod(s.data(), [s, rel, g] { if (s) { s->m_cache.remove(rel); emit s->pathUpdated(rel, g); } }, Qt::QueuedConnection);
    }, "upd:" + QString::number(quintptr(this), 16) + ":" + rel);
}

void LibraryIndex::removePath(const QString &rel) { updatePath(rel); } // missing file => row removed

void LibraryIndex::renamePath(const QString &oldRel, const QString &newRel) {
    auto impl = m_impl;
    m_worker->post(Worker::Index, [impl, oldRel, newRel] {
        if (!impl->open()) return;
        impl->ensureMaps();
        auto it = impl->maps.idByPath.constFind(oldRel);
        impl->exec("BEGIN");
        QSqlQuery q(impl->db());
        q.prepare("UPDATE notes SET path=?, folder=? WHERE path=?");
        QString f = QFileInfo(newRel).path();
        q.addBindValue(newRel);
        q.addBindValue(f == "." ? QString() : f);
        q.addBindValue(oldRel);
        q.exec();
        if (it != impl->maps.idByPath.constEnd()) {
            qint64 id = it.value();
            QStringList al = impl->maps.aliasesById.value(id);
            impl->dirtyName(oldRel, al);
            impl->maps.remove(id);
            impl->maps.add(id, newRel, al);
            impl->dirtyName(newRel, al);
            impl->flushResolve();
        }
        impl->exec("COMMIT");
    });
    updatePath(newRel); // refresh title/size/mtime (filename may be the title)
}

quint64 LibraryIndex::search(const IndexQuery &q) {
    quint64 id = ++*m_searchId;
    auto impl = m_impl;
    auto latest = m_searchId;
    QPointer<LibraryIndex> self(this);
    m_worker->post(Worker::Index, [=] {
        if (latest->load() != id) return; // superseded before it ran
        auto page = doSearch(*impl, q, id, latest.get());
        if (!page) return;
        if (QPointer<LibraryIndex> s = self; s)
            QMetaObject::invokeMethod(s.data(), [s, p = *page] {
                if (!s) return;
                if (s->m_searchId->load() != p.id) return; // stale
                for (const auto &r : p.rows) s->m_cache.insert(r.relPath, new IndexRow(r));
                emit s->searchFinished(p);
            }, Qt::QueuedConnection);
    }, "search:" + QString::number(quintptr(this), 16));
    return id;
}

SearchPage LibraryIndex::searchNow(const IndexQuery &q) {
    auto impl = m_impl;
    return m_worker->callBlocking(Worker::Index, [impl, q] { return doSearch(*impl, q, 0, nullptr).value_or(SearchPage{}); });
}

QStringList LibraryIndex::folders() {
    auto impl = m_impl;
    return m_worker->callBlocking(Worker::Index, [impl] {
        QStringList out;
        if (!impl->open()) return out;
        QSqlQuery q(impl->db());
        q.exec("SELECT path FROM folders ORDER BY path");
        while (q.next()) out << q.value(0).toString();
        return out;
    });
}

QList<QPair<QString, int>> LibraryIndex::tags() {
    auto impl = m_impl;
    return m_worker->callBlocking(Worker::Index, [impl] {
        QList<QPair<QString, int>> out;
        if (!impl->open()) return out;
        QSqlQuery q(impl->db());
        q.exec("SELECT tag,COUNT(*) FROM tags GROUP BY tag ORDER BY tag");
        while (q.next()) out.append({q.value(0).toString(), q.value(1).toInt()});
        return out;
    });
}

int LibraryIndex::count() {
    auto impl = m_impl;
    return m_worker->callBlocking(Worker::Index, [impl] {
        if (!impl->open()) return 0;
        QSqlQuery q(impl->db());
        return q.exec("SELECT COUNT(*) FROM notes") && q.next() ? q.value(0).toInt() : 0;
    });
}

QList<IndexRow> LibraryIndex::mergeDirty(QList<IndexRow> rows, const QList<DirtyDoc> &dirty, const QString &text) {
    QStringList terms;
    for (const auto &t : ftsTerms(text.trimmed())) terms << fold(t);
    for (const auto &d : dirty) {
        NoteMeta m = extractMeta(QFileInfo(d.relPath).completeBaseName(), d.text.toUtf8());
        qsizetype idx = -1;
        for (qsizetype i = 0; i < rows.size(); ++i) if (rows[i].relPath == d.relPath) { idx = i; break; }
        IndexRow r = idx >= 0 ? rows[idx] : IndexRow{};
        r.relPath = d.relPath; r.title = m.title; r.tags = m.tags; r.dirty = true;
        QString f = QFileInfo(d.relPath).path();
        r.folder = f == "." ? QString() : f;
        if (terms.isEmpty()) { if (idx >= 0) rows[idx] = r; continue; }
        QStringList words;
        for (const auto &w : ftsTerms(m.title + " " + d.text + " " + m.tags.join(' '))) words << fold(w);
        bool all = true;
        for (qsizetype i = 0; i < terms.size() && all; ++i) {
            bool last = i == terms.size() - 1, found = false;
            for (const auto &w : words) if (last ? w.startsWith(terms[i]) : w == terms[i]) { found = true; break; }
            all = found;
        }
        if (!all) { if (idx >= 0) rows.removeAt(idx); continue; }
        qsizetype pos = d.text.indexOf(terms.last(), 0, Qt::CaseInsensitive);
        r.snippet = pos < 0 ? QString() : d.text.mid(qMax<qsizetype>(0, pos - 40), 100).simplified();
        if (idx >= 0) rows[idx] = r; else rows.prepend(r);
    }
    return rows;
}

// ---------- link graph / tasks / queries ----------
namespace {
constexpr int kMaxRows = 100000;

LinkRow rowFromLink(QSqlQuery &q) {
    LinkRow r;
    r.srcPath = q.value(0).toString(); r.target = q.value(1).toString(); r.alias = q.value(2).toString();
    r.anchor = q.value(3).toString(); r.kind = q.value(4).toString(); r.context = q.value(5).toString();
    r.destPath = q.value(6).toString(); r.line = q.value(7).toInt(); r.resolved = q.value(8).toInt() != 0;
    r.ambiguous = q.value(9).toInt() != 0;
    return r;
}
const char *kLinkSelect = "SELECT s.path,l.target_raw,l.alias,l.anchor,l.kind,l.context,d.path,l.line,l.target_resolved IS NOT NULL,l.ambiguous"
                          " FROM links l JOIN notes s ON s.id=l.src_id LEFT JOIN notes d ON d.id=l.target_resolved ";

// nullopt => cancelled
std::optional<QList<LinkRow>> selectLinks(LibraryIndex::Impl &im, const QString &where, const QVariantList &binds, int limit, int offset,
                                          const std::atomic<quint64> *latest, quint64 id) {
    QList<LinkRow> out;
    QSqlQuery q(im.db());
    q.setForwardOnly(true);
    q.prepare(QString(kLinkSelect) + where + " ORDER BY s.path,l.line,l.id" + (limit >= 0 ? " LIMIT ? OFFSET ?" : ""));
    for (const auto &b : binds) q.addBindValue(b);
    if (limit >= 0) { q.addBindValue(limit); q.addBindValue(offset); }
    if (!q.exec()) return out;
    int n = 0;
    while (q.next()) {
        if (latest && (++n & 63) == 0 && latest->load() != id) return std::nullopt;
        out.append(rowFromLink(q));
    }
    return out;
}

QList<LinkRow> dirtyLinkRows(LibraryIndex::Impl &im, const DirtyDoc &d) {
    QList<LinkRow> out;
    for (const LinkRef &l : analyzeNote(d.text.toUtf8()).links) {
        Resolution r = im.maps.resolve(l.target, d.relPath);
        LinkRow row;
        row.srcPath = d.relPath; row.target = l.target; row.alias = l.alias; row.anchor = l.anchor; row.kind = l.kindName();
        row.context = l.context; row.line = l.line; row.dirty = true;
        row.resolved = r.state == Resolution::Resolved;
        row.ambiguous = r.state == Resolution::Ambiguous;
        if (row.resolved) row.destPath = r.path;
        out.append(row);
    }
    return out;
}

QString inList(qsizetype n) { return QString("?,").repeated(n).chopped(1); }
QVariantList dirtyPaths(const QList<DirtyDoc> &dirty) { QVariantList v; for (const auto &d : dirty) v << d.relPath; return v; }

bool rowLess(const LinkRow &a, const LinkRow &b) { return a.srcPath != b.srcPath ? a.srcPath < b.srcPath : a.line < b.line; }

template <class Row> void slicePage(QList<Row> &rows, int limit, int offset, bool *hasMore) {
    if (offset > 0) rows = rows.mid(offset);
    *hasMore = rows.size() > limit;
    if (*hasMore) rows.resize(limit);
}

// kind: 0 = backlinks to `rel`, 1 = unresolved links, 2 = links from `rel`
std::optional<LinkPage> doLinkPage(LibraryIndex::Impl &im, int kind, const QString &rel, int limit, int offset, const QList<DirtyDoc> &dirty,
                                   quint64 id, const std::atomic<quint64> *latest) {
    LinkPage page;
    page.id = id;
    limit = qBound(1, limit, 5000);
    offset = qMax(0, offset);
    if (!im.open()) return page;
    im.ensureMaps();
    qint64 dest = im.maps.idByPath.value(rel, -1);
    QString where;
    QVariantList binds;
    if (kind == 0) { if (dest < 0 && dirty.isEmpty()) return page; where = "WHERE l.target_resolved=?"; binds << dest; }
    else if (kind == 1) where = "WHERE l.target_resolved IS NULL";
    else { if (dest < 0) { where = "WHERE 0"; } else { where = "WHERE l.src_id=?"; binds << dest; } }
    QList<DirtyDoc> relevant;
    for (const auto &d : dirty) if (kind != 2 || d.relPath == rel) relevant << d;
    if (kind == 0 && dest < 0) where = "WHERE 0";
    if (relevant.isEmpty()) {
        auto rows = selectLinks(im, where, binds, limit + 1, offset, latest, id);
        if (!rows) return std::nullopt;
        page.rows = *rows;
        if (page.rows.size() > limit) { page.rows.removeLast(); page.hasMore = true; }
        return page;
    }
    // dirty docs: their stored rows are replaced by rows computed from the unsaved text
    where += " AND s.path NOT IN (" + inList(relevant.size()) + ")";
    binds += dirtyPaths(relevant);
    auto disk = selectLinks(im, where, binds, kMaxRows, 0, latest, id);
    if (!disk) return std::nullopt;
    QList<LinkRow> rows = *disk;
    for (const auto &d : relevant)
        for (const LinkRow &r : dirtyLinkRows(im, d)) {
            if (kind == 0 && !(r.resolved && r.destPath == rel)) continue;
            if (kind == 1 && r.resolved) continue;
            rows.append(r);
        }
    std::stable_sort(rows.begin(), rows.end(), rowLess);
    slicePage(rows, limit, offset, &page.hasMore);
    page.rows = rows;
    return page;
}

std::optional<TaskPage> doTasks(LibraryIndex::Impl &im, const TaskQuery &tq, const QList<DirtyDoc> &dirty, quint64 id, const std::atomic<quint64> *latest) {
    TaskPage page;
    page.id = id;
    int limit = qBound(1, tq.limit, 5000), offset = qMax(0, tq.offset);
    if (!im.open()) return page;
    QStringList conds;
    QVariantList binds;
    if (!tq.path.isEmpty()) { conds << "n.path=?"; binds << tq.path; }
    if (tq.done == 0 || tq.done == 1) { conds << "k.done=?"; binds << tq.done; }
    if (!tq.text.isEmpty()) { conds << "k.text LIKE ? ESCAPE '\\'"; binds << "%" + QString(tq.text).replace('\\', "\\\\").replace('%', "\\%").replace('_', "\\_") + "%"; }
    QList<DirtyDoc> relevant;
    for (const auto &d : dirty) if (tq.path.isEmpty() || d.relPath == tq.path) relevant << d;
    if (!relevant.isEmpty()) { conds << "n.path NOT IN (" + inList(relevant.size()) + ")"; binds += dirtyPaths(relevant); }
    QSqlQuery q(im.db());
    q.setForwardOnly(true);
    q.prepare("SELECT n.path,k.line,k.done,k.text FROM tasks k JOIN notes n ON n.id=k.note_id" + (conds.isEmpty() ? QString() : " WHERE " + conds.join(" AND ")) +
              " ORDER BY n.path,k.line LIMIT ? OFFSET ?");
    for (const auto &b : binds) q.addBindValue(b);
    q.addBindValue(relevant.isEmpty() ? limit + 1 : kMaxRows);
    q.addBindValue(relevant.isEmpty() ? offset : 0);
    if (!q.exec()) return page;
    int n = 0;
    while (q.next()) {
        if (latest && (++n & 63) == 0 && latest->load() != id) return std::nullopt;
        page.rows.append({q.value(0).toString(), q.value(1).toInt(), q.value(2).toInt() != 0, q.value(3).toString(), false});
    }
    if (relevant.isEmpty()) {
        if (page.rows.size() > limit) { page.rows.removeLast(); page.hasMore = true; }
        return page;
    }
    for (const auto &d : relevant)
        for (const TaskItem &t : analyzeNote(d.text.toUtf8()).tasks) {
            if (tq.done == 0 && t.done) continue;
            if (tq.done == 1 && !t.done) continue;
            if (!tq.text.isEmpty() && !t.text.contains(tq.text, Qt::CaseInsensitive)) continue;
            page.rows.append({d.relPath, t.line, t.done, t.text, true});
        }
    std::stable_sort(page.rows.begin(), page.rows.end(), [](const TaskRow &a, const TaskRow &b) { return a.path != b.path ? a.path < b.path : a.line < b.line; });
    slicePage(page.rows, limit, offset, &page.hasMore);
    return page;
}

QByteArray utf8Of(const QString &s) { return s.toUtf8(); }

// New spelling of a link target that pointed at `oldRel`, preserving the author's style. Empty => unchanged.
QString renamedTarget(const QString &orig, const QString &src, const QString &oldRel, const QString &newRel, bool basenameSafe) {
    Q_UNUSED(oldRel);
    QString t = orig.trimmed();
    t.replace('\\', '/');
    const bool md = t.endsWith(".md", Qt::CaseInsensitive);
    QString out;
    if (t.startsWith("./") || t.startsWith("../")) {
        out = noExt(QDir(dirOf(src).isEmpty() ? QStringLiteral("/") : "/" + dirOf(src)).relativeFilePath("/" + newRel));
        if (!out.startsWith("../")) out = "./" + out;
    } else if (t.contains('/') || !basenameSafe) out = noExt(newRel);
    else {
        out = noExt(QFileInfo(newRel).fileName());
        if (QString::compare(out, noExt(t), Qt::CaseInsensitive) == 0) return {}; // still resolves the same way
    }
    if (md) out += ".md";
    return out == orig.trimmed() ? QString() : out;
}

QList<FileEdit> doRewrite(LibraryIndex::Impl &im, const QString &oldRel, const QString &newRel) {
    QList<FileEdit> edits;
    if (!im.open() || oldRel == newRel) return edits;
    im.ensureMaps();
    auto oit = im.maps.idByPath.constFind(oldRel);
    if (oit == im.maps.idByPath.constEnd()) return edits;
    const qint64 oldId = oit.value();
    QStringList srcs;
    {
        QSqlQuery q(im.db());
        q.setForwardOnly(true);
        q.prepare("SELECT DISTINCT s.path FROM links l JOIN notes s ON s.id=l.src_id WHERE l.target_resolved=? ORDER BY s.path");
        q.addBindValue(oldId);
        q.exec();
        while (q.next()) srcs << q.value(0).toString();
    }
    bool basenameSafe = true;
    for (qint64 id : im.maps.byTail.values(tailOf(noExt(newRel)))) if (id != oldId) basenameSafe = false;
    for (const QString &src : srcs) {
        QByteArray bytes;
        if (!readAll(im.root + "/" + src, &bytes, nullptr)) continue;
        QList<LinkRef> links = analyzeNote(bytes).links;
        QByteArray out = bytes;
        int changed = 0;
        for (qsizetype i = links.size() - 1; i >= 0; --i) { // back to front keeps earlier offsets valid
            const LinkRef &l = links[i];
            Resolution r = im.maps.resolve(l.target, src);
            if (r.state != Resolution::Resolved || r.via == Resolution::Alias || r.path != oldRel) continue;
            QString nt = renamedTarget(l.target, src, oldRel, newRel, basenameSafe);
            if (nt.isEmpty()) continue;
            // The stored span is the trimmed target; verify it still matches the file bytes.
            if (l.targetEnd > out.size() || out.mid(l.targetStart, l.targetEnd - l.targetStart) != l.target.toUtf8()) continue;
            out.replace(l.targetStart, l.targetEnd - l.targetStart, utf8Of(nt));
            ++changed;
        }
        if (changed) edits.append({src, out, changed});
    }
    return edits;
}
} // namespace

QList<LinkRow> LibraryIndex::linksFrom(const QString &rel, const QList<DirtyDoc> &dirty) {
    auto impl = m_impl;
    return m_worker->callBlocking(Worker::Index, [impl, rel, dirty] {
        auto p = doLinkPage(*impl, 2, rel, 5000, 0, dirty, 0, nullptr);
        return p ? p->rows : QList<LinkRow>{};
    });
}
LinkPage LibraryIndex::backlinks(const QString &rel, int limit, int offset, const QList<DirtyDoc> &dirty) {
    auto impl = m_impl;
    return m_worker->callBlocking(Worker::Index, [impl, rel, limit, offset, dirty] {
        return doLinkPage(*impl, 0, rel, limit, offset, dirty, 0, nullptr).value_or(LinkPage{});
    });
}
LinkPage LibraryIndex::unresolvedLinks(int limit, int offset, const QList<DirtyDoc> &dirty) {
    auto impl = m_impl;
    return m_worker->callBlocking(Worker::Index, [impl, limit, offset, dirty] {
        return doLinkPage(*impl, 1, {}, limit, offset, dirty, 0, nullptr).value_or(LinkPage{});
    });
}
Resolution LibraryIndex::resolve(const QString &name, const QString &fromRel) {
    auto impl = m_impl;
    return m_worker->callBlocking(Worker::Index, [impl, name, fromRel] {
        if (!impl->open()) return Resolution{};
        impl->ensureMaps();
        return impl->maps.resolve(name, fromRel);
    });
}
TaskPage LibraryIndex::tasks(const TaskQuery &q, const QList<DirtyDoc> &dirty) {
    auto impl = m_impl;
    return m_worker->callBlocking(Worker::Index, [impl, q, dirty] { return doTasks(*impl, q, dirty, 0, nullptr).value_or(TaskPage{}); });
}
QList<FileEdit> LibraryIndex::rewriteLinksForRename(const QString &oldRel, const QString &newRel) {
    auto impl = m_impl;
    return m_worker->callBlocking(Worker::Index, [impl, oldRel, newRel] { return doRewrite(*impl, oldRel, newRel); });
}
QueryResult LibraryIndex::query(const QJsonObject &spec) {
    auto impl = m_impl;
    return m_worker->callBlocking(Worker::Index, [impl, spec] {
        if (!impl->open()) { QueryResult r; r.error = "index unavailable"; return r; }
        return impl->engine().run(spec);
    });
}

quint64 LibraryIndex::backlinksAsync(const QString &rel, int limit, int offset, const QList<DirtyDoc> &dirty) {
    quint64 id = ++*m_backId;
    auto impl = m_impl; auto latest = m_backId;
    QPointer<LibraryIndex> self(this);
    m_worker->post(Worker::Index, [=] {
        if (latest->load() != id) return;
        auto page = doLinkPage(*impl, 0, rel, limit, offset, dirty, id, latest.get());
        if (!page) return;
        if (QPointer<LibraryIndex> s = self; s)
            QMetaObject::invokeMethod(s.data(), [s, p = *page] { if (s && s->m_backId->load() == p.id) emit s->backlinksFinished(p); }, Qt::QueuedConnection);
    }, "backlinks:" + QString::number(quintptr(this), 16));
    return id;
}
quint64 LibraryIndex::tasksAsync(const TaskQuery &q, const QList<DirtyDoc> &dirty) {
    quint64 id = ++*m_taskId;
    auto impl = m_impl; auto latest = m_taskId;
    QPointer<LibraryIndex> self(this);
    m_worker->post(Worker::Index, [=] {
        if (latest->load() != id) return;
        auto page = doTasks(*impl, q, dirty, id, latest.get());
        if (!page) return;
        if (QPointer<LibraryIndex> s = self; s)
            QMetaObject::invokeMethod(s.data(), [s, p = *page] { if (s && s->m_taskId->load() == p.id) emit s->tasksFinished(p); }, Qt::QueuedConnection);
    }, "tasks:" + QString::number(quintptr(this), 16));
    return id;
}
quint64 LibraryIndex::queryAsync(const QJsonObject &spec) {
    quint64 id = ++*m_queryId;
    auto impl = m_impl; auto latest = m_queryId;
    QPointer<LibraryIndex> self(this);
    m_worker->post(Worker::Index, [=] {
        if (latest->load() != id) return;
        QueryResult r;
        if (!impl->open()) r.error = "index unavailable";
        else r = impl->engine().run(spec, latest.get(), id);
        r.id = id;
        if (latest->load() != id) return; // superseded: never reported
        if (QPointer<LibraryIndex> s = self; s)
            QMetaObject::invokeMethod(s.data(), [s, r] { if (s && s->m_queryId->load() == r.id) emit s->queryFinished(r); }, Qt::QueuedConnection);
    }, "query:" + QString::number(quintptr(this), 16));
    return id;
}

} // namespace hn::core
