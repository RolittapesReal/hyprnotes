#pragma once
#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>
#include <functional>

namespace hn::core {

struct FileInfo {
    QString relPath;
    qint64 size = 0;
    qint64 mtimeMs = 0;
    QString fileId; // "dev:inode", empty if unavailable
};

QString fileIdOf(const QString &absPath);
QByteArray contentHash(const QByteArray &bytes);
bool readAll(const QString &absPath, QByteArray *out, QString *err);
// QSaveFile, direct-write fallback disabled; open/write/commit all checked.
bool atomicWrite(const QString &absPath, const QByteArray &bytes, QString *err);
// Compare-and-swap write for notes. expect == nullptr: unconditional. expect empty: target must not exist.
// Otherwise the target must still hash to *expect at the instant of replacement (renameat2 RENAME_EXCHANGE,
// the swapped-out file is verified and swapped back on mismatch). Mismatch => *found holds the observed
// external bytes, the target is left as it was.
enum class CasResult { Ok, Mismatch, Error };
CasResult casWrite(const QString &absPath, const QByteArray &bytes, const QByteArray *expect, QByteArray *found, QString *err);
// Test seam: called by casWrite after the temp file is durable, immediately before the rename.
std::function<void(const QString &)> &beforeRenameHook();
// Remove temp files left by writers that no longer exist (killed saves) under root. Safe: only
// ".<name>.hn-tmp.<pid>.<n>" files whose pid is dead are removed.
void sweepStaleTemps(const QString &root);
// All *.md under root (hidden entries skipped); optionally the subdirectory list (relative).
QList<FileInfo> scanNotes(const QString &root, QStringList *folders = nullptr);

} // namespace hn::core
