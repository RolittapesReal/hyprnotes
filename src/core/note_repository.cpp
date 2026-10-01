#include "hn/core/note_repository.h"
#include "hn/core/paths.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QRegularExpression>

namespace hn::core {

NoteRepository::NoteRepository(QString root, QString recoveryDir, QObject *parent, Worker *worker)
    : QObject(parent), m_root(QDir::cleanPath(QFileInfo(root).absoluteFilePath())),
      m_recovery(recoveryDir.isEmpty() ? paths::stateDir() + "/recovery" : recoveryDir),
      m_worker(worker ? worker : &Worker::shared()) {
    m_timer.setSingleShot(true);
    m_timer.setInterval(40);
    connect(&m_timer, &QTimer::timeout, this, &NoteRepository::onFsEvent);
    connect(&m_fsw, &QFileSystemWatcher::fileChanged, this, &NoteRepository::kick);
    connect(&m_fsw, &QFileSystemWatcher::directoryChanged, this, [this](const QString &d) {
        m_dirtyDirs.insert(d);
        kick();
    });
    sweepStaleTemps(m_root); // orphans of killed saves
    sweepStaleTemps(m_recovery.dir());
}

// Debounce with a cap: a continuous writer must still be reported within kMaxWaitMs.
void NoteRepository::kick() {
    constexpr int kMaxWaitMs = 500;
    if (!m_burst.isValid()) m_burst.start();
    if (!m_timer.isActive() || m_burst.elapsed() < kMaxWaitMs) m_timer.start();
}

NoteRepository::~NoteRepository() { m_worker->waitIdle(); }

QString NoteRepository::absolutePath(const QString &rel) const {
    if (rel.isEmpty() || QDir::isAbsolutePath(rel)) return {};
    QString abs = QDir::cleanPath(m_root + "/" + rel);
    return abs.startsWith(m_root + "/") ? abs : QString();
}

QString NoteRepository::relOf(const QString &abs) const { return QDir(m_root).relativeFilePath(abs); }

QList<NoteEntry> NoteRepository::list() const {
    QList<NoteEntry> out;
    for (const auto &f : scanNotes(m_root))
        out.append({f.relPath, f.size, QDateTime::fromMSecsSinceEpoch(f.mtimeMs), f.fileId});
    return out;
}

QStringList NoteRepository::folders() const {
    QStringList f;
    scanNotes(m_root, &f);
    return f;
}

NoteSnapshot NoteRepository::read(const QString &rel) {
    NoteSnapshot s;
    s.relPath = rel;
    QString abs = absolutePath(rel);
    if (abs.isEmpty()) { s.error = "invalid path"; return s; }
    if (!readAll(abs, &s.bytes, &s.error)) return s;
    QFileInfo fi(abs);
    s.ok = true;
    s.mtime = fi.lastModified();
    s.fileId = fileIdOf(abs);
    s.revision = ++m_nextRev;
    State &st = m_state[rel];
    st.hasBaseline = true;
    st.baseHash = contentHash(s.bytes);
    st.baseRev = s.revision;
    st.conflict = st.removed = false;
    st.notifiedHash.clear();
    return s;
}

bool NoteRepository::isSaving(const QString &rel) const { return m_state.value(rel).inflight; }
bool NoteRepository::hasPending(const QString &rel) const { return m_state.value(rel).pending.has_value(); }

quint64 NoteRepository::save(const QString &rel, const QByteArray &bytes, quint64 baseRev, bool force) {
    Job j{bytes, ++m_nextRev, baseRev, force};
    State &st = m_state[rel];
    st.failed.reset();
    if (st.inflight) st.pending = j; // replaces any older pending snapshot
    else start(rel, st, j);
    return j.rev;
}

void NoteRepository::retry(const QString &rel) {
    State &st = m_state[rel];
    if (!st.failed) return;
    Job j = *st.failed;
    st.failed.reset();
    if (st.inflight) { if (!st.pending) st.pending = j; }
    else start(rel, st, j);
}

void NoteRepository::start(const QString &rel, State &st, Job job) {
    st.inflight = true;
    st.running = job;
    QString abs = absolutePath(rel);
    // Everything the task needs is copied: the worker never touches this object.
    struct In { QString root, rel, abs; Job job; bool hasBase; QByteArray baseHash; RecoveryStore store; };
    In in{m_root, rel, abs, job, st.hasBaseline, st.baseHash, m_recovery};
    QPointer<NoteRepository> self(this);
    m_worker->post(Worker::Save, [self, in] {
        Result r;
        QByteArray disk;
        bool exists = !in.abs.isEmpty() && QFileInfo::exists(in.abs);
        QString err;
        if (in.abs.isEmpty()) { r.error = "invalid path"; }
        else if (exists && !readAll(in.abs, &disk, &err)) { r.error = "cannot read current file: " + err; }
        else if (!in.store.writePending(in.root, in.rel, in.job.bytes, exists ? &disk : nullptr, in.job.baseRev, &err)) {
            r.error = "recovery copy failed: " + err; // never write without a recovery copy
        } else if (!in.job.force && !exists && in.hasBase) {
            r.kind = Result::Removed;
            r.error = "file was removed";
        } else if (!in.job.force && exists && (!in.hasBase || contentHash(disk) != in.baseHash)) {
            r.kind = Result::Conflict;
            r.external = disk;
            if (!in.store.writeExternal(in.root, in.rel, disk, &err)) { r.kind = Result::Failed; r.error = "recovery copy failed: " + err; }
        } else {
            // Compare-and-swap at the rename itself: the check above used bytes read before the recovery writes.
            QByteArray none, ext;
            const QByteArray *expect = in.job.force ? nullptr : (exists ? &in.baseHash : &none);
            switch (casWrite(in.abs, in.job.bytes, expect, &ext, &err)) {
            case CasResult::Ok: r.kind = Result::Saved; break;
            case CasResult::Mismatch:
                r.kind = Result::Conflict;
                r.external = ext;
                if (!in.store.writeExternal(in.root, in.rel, ext, &err)) { r.kind = Result::Failed; r.error = "recovery copy failed: " + err; }
                break;
            case CasResult::Error: r.error = err.isEmpty() ? "write failed" : err; break;
            }
        }
        if (r.kind == Result::Saved) {
            r.newHash = contentHash(in.job.bytes);
            in.store.resolve(in.root, in.rel);
        }
        if (QPointer<NoteRepository> s = self; s)
            QMetaObject::invokeMethod(s.data(), [s, in, r] { if (s) s->finish(in.rel, in.job, r); }, Qt::QueuedConnection);
    });
}

void NoteRepository::finish(const QString &rel, const Job &job, const Result &r) {
    auto it = m_state.find(rel);
    if (it == m_state.end()) return; // removed/renamed meanwhile
    State &st = *it;
    st.inflight = false;
    st.running = Job{}; // release the saved bytes: only pending/failed jobs need to be kept
    switch (r.kind) {
    case Result::Saved:
        st.hasBaseline = true; st.baseHash = r.newHash; st.baseRev = job.rev;
        st.conflict = st.removed = false; st.notifiedHash.clear();
        emit saved(rel, job.rev);
        break;
    case Result::Conflict:
        st.conflict = true;
        emit conflict(rel, r.external, job.bytes, job.rev);
        break;
    case Result::Removed: {
        bool first = !st.removed;
        st.removed = true;
        if (first) emit fileRemoved(rel);
        emit saveFailed(rel, r.error, job.rev);
        break;
    }
    case Result::Failed:
        st.failed = job;
        emit saveFailed(rel, r.error, job.rev);
        break;
    }
    // Re-look up: a slot may have mutated the map.
    State &s2 = m_state[rel];
    if (s2.pending) { Job p = *s2.pending; s2.pending.reset(); s2.failed.reset(); start(rel, s2, p); }
    else if (s2.recheck) { s2.recheck = false; m_timer.start(); }
}

QString NoteRepository::create(const QString &folder, const QString &title, const QByteArray &initial, QString *err) {
    QString base = title;
    base.replace(QRegularExpression("[/\\\\\\x00-\\x1f]"), " ");
    base = base.trimmed();
    while (base.startsWith('.')) base.remove(0, 1);
    base = base.trimmed();
    if (base.isEmpty()) base = "Untitled";
    base = base.left(120);
    QString dirRel = QDir::cleanPath(folder);
    if (dirRel == ".") dirRel.clear();
    QString dirAbs = dirRel.isEmpty() ? m_root : absolutePath(dirRel);
    if (dirAbs.isEmpty()) { if (err) *err = "invalid folder"; return {}; }
    if (!QDir().mkpath(dirAbs)) { if (err) *err = "cannot create folder"; return {}; }
    for (int i = 1; i < 10000; ++i) {
        QString name = i == 1 ? base + ".md" : base + " " + QString::number(i) + ".md";
        QString rel = dirRel.isEmpty() ? name : dirRel + "/" + name;
        QFile f(dirAbs + "/" + name);
        if (QFileInfo::exists(f.fileName())) continue;
        if (!f.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
            if (QFileInfo::exists(f.fileName())) continue; // lost a race: next suffix
            if (err) *err = f.errorString();
            return {};
        }
        if (f.write(initial) != initial.size() || !f.flush()) { if (err) *err = f.errorString(); f.close(); f.remove(); return {}; }
        f.close();
        State &st = m_state[rel];
        st = State{};
        st.hasBaseline = true; st.baseHash = contentHash(initial); st.baseRev = ++m_nextRev;
        return rel;
    }
    if (err) *err = "no free filename";
    return {};
}

bool NoteRepository::rename(const QString &rel, const QString &newRel, QString *err) {
    QString a = absolutePath(rel), b = absolutePath(newRel);
    if (a.isEmpty() || b.isEmpty()) { if (err) *err = "invalid path"; return false; }
    if (m_state.value(rel).inflight || m_state.value(rel).pending) { if (err) *err = "save in progress"; return false; }
    if (QFileInfo::exists(b)) { if (err) *err = "destination exists"; return false; }
    QDir().mkpath(QFileInfo(b).absolutePath());
    QFile f(a);
    if (!f.rename(b)) { if (err) *err = f.errorString(); return false; }
    bool wasWatched = m_watched.contains(rel);
    if (m_state.contains(rel)) m_state[newRel] = m_state.take(rel); // before unwatch(): unwatch drops idle state
    unwatch(rel);
    if (wasWatched) watch(newRel);
    emit renamed(rel, newRel);
    return true;
}

bool NoteRepository::remove(const QString &rel, QString *err) {
    QString a = absolutePath(rel);
    if (a.isEmpty()) { if (err) *err = "invalid path"; return false; }
    if (m_state.value(rel).inflight) { if (err) *err = "save in progress"; return false; }
    unwatch(rel);
    QFile f(a);
    if (!f.remove()) { if (err) *err = f.errorString(); return false; }
    m_state.remove(rel);
    return true;
}

void NoteRepository::watch(const QString &rel) {
    QString abs = absolutePath(rel);
    if (abs.isEmpty()) return;
    m_watched.insert(rel);
    if (QFileInfo::exists(abs)) m_fsw.addPath(abs);
    m_fsw.addPath(QFileInfo(abs).absolutePath());
}

void NoteRepository::unwatch(const QString &rel) {
    QString abs = absolutePath(rel);
    m_watched.remove(rel);
    // Closed and idle: drop per-note state so closed notes retain nothing (read() re-establishes the baseline).
    if (auto it = m_state.find(rel); it != m_state.end() && !it->inflight && !it->pending && !it->failed && !it->conflict && !it->removed)
        m_state.erase(it);
    if (abs.isEmpty()) return;
    m_fsw.removePath(abs);
    QString dir = QFileInfo(abs).absolutePath();
    for (const auto &w : std::as_const(m_watched))
        if (QFileInfo(absolutePath(w)).absolutePath() == dir) return;
    if (!m_libWatch) m_fsw.removePath(dir);
}

void NoteRepository::watchLibrary(bool on) {
    m_libWatch = on;
    if (!on) return;
    m_fsw.addPath(m_root);
    for (const auto &f : folders()) m_fsw.addPath(m_root + "/" + f);
}

bool NoteRepository::isArmed(const QString &rel) const { return m_fsw.files().contains(absolutePath(rel)); }

void NoteRepository::reconcileNow() { m_dirtyDirs.clear(); onFsEvent(); }

// Coalesced handler: re-arm replaced files, then compare each watched note with its baseline.
void NoteRepository::onFsEvent() {
    m_burst.invalidate();
    const auto dirty = std::exchange(m_dirtyDirs, {});
    if (m_libWatch)
        for (const auto &d : dirty) {
            if (QDir(d).exists()) {
                QString r = relOf(d);
                for (const auto &sub : QDir(d).entryList(QDir::Dirs | QDir::NoDotAndDotDot))
                    m_fsw.addPath(d + "/" + sub);
                emit libraryChanged(r == "." ? QString() : r);
            }
        }
    const auto watched = m_watched; // copy: slots may unwatch
    for (const auto &rel : watched) {
        if (!m_watched.contains(rel)) continue;
        QString abs = absolutePath(rel);
        bool exists = QFileInfo::exists(abs);
        if (exists && !m_fsw.files().contains(abs)) m_fsw.addPath(abs); // re-arm after atomic replace
        if (!m_fsw.directories().contains(QFileInfo(abs).absolutePath())) m_fsw.addPath(QFileInfo(abs).absolutePath());
        State &st = m_state[rel];
        if (st.inflight) { st.recheck = true; continue; } // our own write may be landing
        if (!exists) {
            if (!st.removed) { st.removed = true; emit fileRemoved(rel); }
            continue;
        }
        st.removed = false;
        if (!st.hasBaseline) continue;
        QByteArray bytes;
        if (!readAll(abs, &bytes, nullptr)) continue;
        QByteArray h = contentHash(bytes);
        if (h == st.baseHash) { st.notifiedHash.clear(); continue; }
        if (h != st.notifiedHash) { st.notifiedHash = h; emit externalChange(rel); }
    }
}

QList<RecoveryEntry> NoteRepository::recoverableDrafts() {
    QList<RecoveryEntry> out;
    for (auto &e : m_recovery.unresolved(m_root)) {
        QByteArray disk;
        QString abs = absolutePath(e.relPath);
        bool exists = !abs.isEmpty() && readAll(abs, &disk, nullptr);
        if (exists && disk == e.local && !e.conflict) { m_recovery.resolve(m_root, e.relPath); continue; }
        out.append(e);
    }
    return out;
}

void NoteRepository::resolveRecovery(const QString &rel) { m_recovery.resolve(m_root, rel); }

} // namespace hn::core
