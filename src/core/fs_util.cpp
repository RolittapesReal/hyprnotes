#include "hn/core/fs_util.h"
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <fcntl.h>
#include <mutex>
#include <set>
#include <sys/stat.h>
#include <unistd.h>

namespace hn::core {

QString fileIdOf(const QString &absPath) {
    struct stat st;
    if (::stat(QFile::encodeName(absPath).constData(), &st) != 0) return {};
    return QString::number(quint64(st.st_dev)) + ":" + QString::number(quint64(st.st_ino));
}

QByteArray contentHash(const QByteArray &bytes) { return QCryptographicHash::hash(bytes, QCryptographicHash::Sha1); }

bool readAll(const QString &absPath, QByteArray *out, QString *err) {
    QFile f(absPath);
    if (!f.open(QIODevice::ReadOnly)) { if (err) *err = f.errorString(); return false; }
    *out = f.readAll();
    if (f.error() != QFileDevice::NoError) { if (err) *err = f.errorString(); return false; }
    return true;
}

static const QString kTmpTag = ".hn-tmp.";

// Temps we could not delete (e.g. directory went read-only): retried on the next write, so a live
// process does not leave orphans the pid-based sweep cannot identify.
static std::mutex g_leakMu;
static std::set<QString> g_leaked;
static void drop(const QString &tmp) {
    std::lock_guard g(g_leakMu);
    if (!QFile::remove(tmp) && QFileInfo::exists(tmp)) g_leaked.insert(tmp);
}
static void retryLeaked() {
    std::lock_guard g(g_leakMu);
    for (auto it = g_leaked.begin(); it != g_leaked.end();) it = (QFile::remove(*it) || !QFileInfo::exists(*it)) ? g_leaked.erase(it) : std::next(it);
}

// Writes a durable temp file next to the target and returns its path ("" on failure).
static QString writeTemp(const QString &abs, const QByteArray &bytes, QString *err) {
    static std::atomic<unsigned> n{0};
    QFileInfo fi(abs);
    QString tmp = fi.absolutePath() + "/." + fi.fileName() + kTmpTag + QString::number(::getpid()) + "." + QString::number(n++);
    QFile f(tmp);
    if (!f.open(QIODevice::WriteOnly | QIODevice::NewOnly)) { if (err) *err = f.errorString(); return {}; }
    bool ok = f.write(bytes) == bytes.size() && f.flush() && ::fsync(f.handle()) == 0;
    if (!ok) { if (err) *err = f.errorString(); f.close(); drop(tmp); return {}; }
    f.close();
    if (fi.exists()) QFile::setPermissions(tmp, QFile::permissions(abs)); // keep the note's mode
    return tmp;
}

std::function<void(const QString &)> &beforeRenameHook() { static std::function<void(const QString &)> h; return h; }

CasResult casWrite(const QString &abs, const QByteArray &bytes, const QByteArray *expect, QByteArray *found, QString *err) {
    retryLeaked();
    const QString tmp = writeTemp(abs, bytes, err);
    if (tmp.isEmpty()) return CasResult::Error;
    if (auto &h = beforeRenameHook()) h(abs);
    const QByteArray t = QFile::encodeName(tmp), a = QFile::encodeName(abs);
    auto fail = [&](const QString &m) -> CasResult { if (err) *err = m; drop(tmp); return CasResult::Error; };
    auto plain = [&]() -> CasResult { // fallback: re-check immediately before the rename, then replace
        if (expect) {
            QByteArray cur; QString e;
            bool ex = QFileInfo::exists(abs);
            if (ex && !readAll(abs, &cur, &e)) return fail(e);
            if (expect->isEmpty() ? ex : (!ex || contentHash(cur) != *expect)) { drop(tmp); if (found) *found = cur; return CasResult::Mismatch; }
        }
        if (::rename(t.constData(), a.constData()) != 0) return fail(QString::fromLocal8Bit(strerror(errno)));
        return CasResult::Ok;
    };
    if (!expect) return plain();
    if (expect->isEmpty()) {
        if (::renameat2(AT_FDCWD, t.constData(), AT_FDCWD, a.constData(), RENAME_NOREPLACE) == 0) return CasResult::Ok;
        if (errno == EEXIST) { drop(tmp); if (found) readAll(abs, found, nullptr); return CasResult::Mismatch; }
        return plain();
    }
    if (::renameat2(AT_FDCWD, t.constData(), AT_FDCWD, a.constData(), RENAME_EXCHANGE) != 0) return plain(); // ENOENT/EINVAL: re-check path
    // The target's previous content is now at tmp: verify it was the baseline we checked.
    QByteArray old; QString e;
    if (!readAll(tmp, &old, &e)) { ::renameat2(AT_FDCWD, t.constData(), AT_FDCWD, a.constData(), RENAME_EXCHANGE); return fail(e); }
    if (contentHash(old) == *expect) { drop(tmp); return CasResult::Ok; }
    ::renameat2(AT_FDCWD, t.constData(), AT_FDCWD, a.constData(), RENAME_EXCHANGE); // swap the external version back
    // If yet another external write replaced our copy inside this window, tmp now holds that newer
    // version: it must become the file again (the one we swapped back is older and goes to recovery).
    QByteArray cur;
    if (readAll(tmp, &cur, nullptr) && cur != bytes && ::rename(t.constData(), a.constData()) == 0) {}
    else drop(tmp);
    if (found) *found = old;
    return CasResult::Mismatch;
}

bool atomicWrite(const QString &absPath, const QByteArray &bytes, QString *err) {
    return casWrite(absPath, bytes, nullptr, nullptr, err) == CasResult::Ok;
}

void sweepStaleTemps(const QString &root) {
    const auto entries = QDir(root).entryInfoList(QDir::Files | QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot | QDir::NoSymLinks);
    for (const auto &fi : entries) {
        const QString n = fi.fileName();
        if (fi.isDir()) { if (!n.startsWith('.')) sweepStaleTemps(fi.filePath()); continue; } // hidden dirs (.git) are skipped
        int i = n.lastIndexOf(kTmpTag);
        if (!n.startsWith('.') || i < 0) continue;
        const QStringList parts = n.mid(i + kTmpTag.size()).split('.');
        bool ok = false;
        const int pid = parts.value(0).toInt(&ok);
        if (ok && pid > 0 && parts.size() == 2 && ::kill(pid, 0) != 0 && errno == ESRCH) QFile::remove(fi.filePath());
    }
}

QList<FileInfo> scanNotes(const QString &root, QStringList *folders) {
    QList<FileInfo> out;
    QDir rootDir(root);
    QDirIterator it(root, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        QFileInfo fi = it.fileInfo();
        QString rel = rootDir.relativeFilePath(fi.filePath());
        if (fi.isDir()) { if (folders) folders->append(rel); continue; }
        if (!rel.endsWith(".md", Qt::CaseInsensitive)) continue;
        out.append({rel, fi.size(), fi.lastModified().toMSecsSinceEpoch(), fileIdOf(fi.filePath())});
    }
    return out;
}

} // namespace hn::core
