#include "hn/core/library_index.h"
#include "hn/core/fs_util.h"
#include "hn/core/paths.h"
#include "markdown_internal.h"
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QPointer>
#include <QRegularExpression>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

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
    MD_PARSER p{};
    p.flags = MD_FLAG_STRIKETHROUGH | MD_FLAG_TASKLISTS;
    p.enter_block = mEnterBlock; p.leave_block = mLeaveBlock; p.enter_span = mEnterSpan;
    p.leave_span = mLeaveSpan; p.text = mText;
    md_parse(utf8.constData(), MD_SIZE(utf8.size()), &p, &ctx);
    ctx.flush();
    NoteMeta m;
    m.title = ctx.title.simplified();
    if (m.title.isEmpty()) m.title = fileBaseName;
    m.tags = ctx.tags;
    return m;
}

// ---------- SQLite (worker thread only) ----------
struct LibraryIndex::Impl {
    QString root, dbPath, conn;
    bool opened = false;

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
                if (ok && ver == 1) { opened = true; return true; }
                if (ok && ver == 0 && createSchema()) { opened = true; return true; }
                d.close();
            }
            QSqlDatabase::removeDatabase(conn);
            QFile::remove(dbPath); // corrupt or different schema: cache is disposable
        }
        return false;
    }
    bool createSchema() {
        const char *stmts[] = {
            "CREATE TABLE notes(id INTEGER PRIMARY KEY, path TEXT UNIQUE NOT NULL, title TEXT, folder TEXT, size INTEGER, mtime INTEGER, fileid TEXT, tags TEXT)",
            "CREATE INDEX notes_mtime ON notes(mtime DESC)",
            "CREATE TABLE tags(note_id INTEGER NOT NULL, tag TEXT NOT NULL)",
            "CREATE INDEX tags_tag ON tags(tag)", "CREATE INDEX tags_note ON tags(note_id)",
            "CREATE TABLE folders(path TEXT PRIMARY KEY)",
            "CREATE VIRTUAL TABLE fts USING fts5(title, body, tags, tokenize='unicode61 remove_diacritics 2', prefix='2 3')",
            "PRAGMA user_version=1"};
        for (auto *s : stmts) if (!exec(QString::fromLatin1(s))) return false;
        return true;
    }
    void close() {
        if (!opened && !QSqlDatabase::contains(conn)) return;
        { QSqlDatabase d = db(); if (d.isOpen()) d.close(); }
        QSqlDatabase::removeDatabase(conn);
        opened = false;
    }
    void reset() { close(); QFile::remove(dbPath); }

    void removeRow(const QString &path) {
        QSqlQuery q(db());
        q.prepare("SELECT id FROM notes WHERE path=?");
        q.addBindValue(path);
        if (!q.exec() || !q.next()) return;
        QVariant id = q.value(0);
        for (auto *sql : {"DELETE FROM fts WHERE rowid=?", "DELETE FROM tags WHERE note_id=?", "DELETE FROM notes WHERE id=?"}) {
            QSqlQuery d(db());
            d.prepare(sql);
            d.addBindValue(id);
            d.exec();
        }
    }
    // Returns 1 if the row was new, 0 if updated, -1 if the file vanished/unreadable.
    int upsert(const FileInfo &fi) {
        QByteArray bytes;
        if (!readAll(root + "/" + fi.relPath, &bytes, nullptr)) { removeRow(fi.relPath); return -1; }
        NoteMeta m = extractMeta(QFileInfo(fi.relPath).completeBaseName(), bytes);
        QString body = QString::fromUtf8(bytes.left(2 << 20));
        QString folder = QFileInfo(fi.relPath).path();
        if (folder == ".") folder.clear();
        QSqlQuery q(db());
        q.prepare("SELECT id FROM notes WHERE path=?");
        q.addBindValue(fi.relPath);
        q.exec();
        bool isNew = !q.next();
        QVariant id = isNew ? QVariant() : q.value(0);
        if (!isNew) removeRowById(id);
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
        for (const auto &t : m.tags) {
            QSqlQuery tq(db());
            tq.prepare("INSERT INTO tags(note_id,tag) VALUES(?,?)");
            tq.addBindValue(nid);
            tq.addBindValue(t);
            tq.exec();
        }
        return isNew ? 1 : 0;
    }
    void removeRowById(const QVariant &id) {
        for (auto *sql : {"DELETE FROM fts WHERE rowid=?", "DELETE FROM tags WHERE note_id=?", "DELETE FROM notes WHERE id=?"}) {
            QSqlQuery d(db());
            d.prepare(sql);
            d.addBindValue(id);
            d.exec();
        }
    }
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
    c->impl->exec("COMMIT");
    if (job.pos < job.todo.size()) { c->w->post(Worker::Index, [c] { syncChunk(c); }); return; }
    QPointer<LibraryIndex> s = c->self;
    quint64 gen = job.gen;
    int a = job.added, u = job.updated, r = job.removed;
    if (s) QMetaObject::invokeMethod(s.data(), [s, gen, a, u, r] { if (s) { s->m_cache.clear(); emit s->synced(gen, a, u, r); } }, Qt::QueuedConnection);
}

LibraryIndex::LibraryIndex(QString root, QString cacheDir, QObject *parent, Worker *worker)
    : QObject(parent), m_impl(std::make_shared<Impl>()), m_root(QDir::cleanPath(QFileInfo(root).absoluteFilePath())),
      m_worker(worker ? worker : &Worker::shared()), m_gen(std::make_shared<std::atomic<quint64>>(0)),
      m_searchId(std::make_shared<std::atomic<quint64>>(0)) {
    qRegisterMetaType<hn::core::SearchPage>();
    if (cacheDir.isEmpty()) cacheDir = paths::cacheDir();
    m_dbPath = cacheDir + "/index.sqlite3";
    m_impl->root = m_root;
    m_impl->dbPath = m_dbPath;
    m_impl->conn = "hn-index-" + QString::number(quintptr(m_impl.get()), 16);
}

LibraryIndex::~LibraryIndex() {
    ++*m_gen;
    ++*m_searchId;
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
        QSqlQuery q(impl->db());
        q.prepare("UPDATE notes SET path=?, folder=? WHERE path=?");
        QString f = QFileInfo(newRel).path();
        q.addBindValue(newRel);
        q.addBindValue(f == "." ? QString() : f);
        q.addBindValue(oldRel);
        q.exec();
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

} // namespace hn::core
