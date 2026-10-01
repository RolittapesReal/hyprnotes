#include "hn/core/recovery_store.h"
#include "hn/core/fs_util.h"
#include <QDir>
#include <QDirIterator>
#include <QJsonDocument>
#include <QJsonObject>
#include <deque>
#include <mutex>

namespace hn::core {

struct RecoveryStore::Hist {
    std::mutex mu;
    bool scanned = false;
    qint64 total = 0;
    std::deque<std::pair<QString, qint64>> entries; // oldest first (path, bytes)
};

RecoveryStore::RecoveryStore(QString dir, qint64 cap) : m_hist(std::make_shared<Hist>()), m_dir(std::move(dir)), m_cap(cap) {}

static qint64 dirBytes(const QString &d);

QString RecoveryStore::entryDir(const QString &root, const QString &rel) const {
    return m_dir + "/unresolved/" + contentHash((root + QChar(0) + rel).toUtf8()).toHex();
}

bool RecoveryStore::writePending(const QString &root, const QString &rel, const QByteArray &local,
                                 const QByteArray *prev, quint64 baseRev, QString *err) const {
    QString d = entryDir(root, rel);
    if (!QDir().mkpath(d)) { if (err) *err = "cannot create " + d; return false; }
    bool conflict = false;
    QFile old(d + "/meta.json");
    if (old.open(QIODevice::ReadOnly)) conflict = QJsonDocument::fromJson(old.readAll()).object()["conflict"].toBool();
    QJsonObject m{{"root", root}, {"rel", rel}, {"baseRevision", qint64(baseRev)}, {"hadDisk", prev != nullptr},
                  {"conflict", conflict}, {"time", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}};
    // Order matters for crash safety: payload first, meta last.
    if (!atomicWrite(d + "/local", local, err)) return false;
    if (prev) { if (!atomicWrite(d + "/disk", *prev, err)) return false; }
    else QFile::remove(d + "/disk");
    return atomicWrite(d + "/meta.json", QJsonDocument(m).toJson(QJsonDocument::Compact), err);
}

bool RecoveryStore::writeExternal(const QString &root, const QString &rel, const QByteArray &ext, QString *err) const {
    QString d = entryDir(root, rel);
    if (!QDir().mkpath(d)) { if (err) *err = "cannot create " + d; return false; }
    if (!atomicWrite(d + "/external", ext, err)) return false;
    QFile f(d + "/meta.json");
    QJsonObject m;
    if (f.open(QIODevice::ReadOnly)) m = QJsonDocument::fromJson(f.readAll()).object();
    f.close();
    m["conflict"] = true;
    return atomicWrite(d + "/meta.json", QJsonDocument(m).toJson(QJsonDocument::Compact), err);
}

void RecoveryStore::resolve(const QString &root, const QString &rel) const {
    QString d = entryDir(root, rel);
    if (!QFileInfo::exists(d)) return;
    QDir().mkpath(m_dir + "/resolved");
    QString base = m_dir + "/resolved/" + QString::number(QDateTime::currentMSecsSinceEpoch()) + "-" + QFileInfo(d).fileName();
    QString target = base;
    for (int i = 1; QFileInfo::exists(target); ++i) target = base + "." + QString::number(i);
    if (!QDir().rename(d, target)) { QDir(d).removeRecursively(); return; } // never leave a stale unresolved entry
    {
        const qint64 n = dirBytes(target);
        std::lock_guard g(m_hist->mu);
        if (m_hist->scanned) { m_hist->entries.emplace_back(target, n); m_hist->total += n; }
    }
    prune();
}

QList<RecoveryEntry> RecoveryStore::unresolved(const QString &root) const {
    QList<RecoveryEntry> out;
    const auto dirs = QDir(m_dir + "/unresolved").entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const auto &di : dirs) {
        QFile mf(di.filePath() + "/meta.json");
        if (!mf.open(QIODevice::ReadOnly)) continue; // incomplete write: payload exists but no meta
        QJsonObject m = QJsonDocument::fromJson(mf.readAll()).object();
        if (m["rel"].toString().isEmpty() || (!root.isEmpty() && m["root"].toString() != root)) continue;
        RecoveryEntry e;
        e.root = m["root"].toString();
        e.relPath = m["rel"].toString();
        e.baseRevision = quint64(m["baseRevision"].toInteger());
        e.conflict = m["conflict"].toBool();
        e.hadDisk = m["hadDisk"].toBool();
        e.time = QDateTime::fromString(m["time"].toString(), Qt::ISODateWithMs);
        QString err;
        if (!readAll(di.filePath() + "/local", &e.local, &err)) continue;
        readAll(di.filePath() + "/disk", &e.previousDisk, &err);
        readAll(di.filePath() + "/external", &e.external, &err);
        out.append(e);
    }
    return out;
}

static qint64 dirBytes(const QString &d) {
    qint64 n = 0;
    QDirIterator it(d, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) { it.next(); n += it.fileInfo().size(); }
    return n;
}

qint64 RecoveryStore::resolvedBytes() const { return dirBytes(m_dir + "/resolved"); }

void RecoveryStore::prune() const {
    std::lock_guard g(m_hist->mu);
    Hist &h = *m_hist;
    if (!h.scanned) { // first use: one full walk. Names start with the epoch msec, so Name order == oldest first.
        h.total = 0;
        for (const auto &d : QDir(m_dir + "/resolved").entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
            h.entries.emplace_back(d.filePath(), dirBytes(d.filePath()));
            h.total += h.entries.back().second;
        }
        h.scanned = true;
    }
    while (!h.entries.empty() && h.total > m_cap) {
        QDir(h.entries.front().first).removeRecursively();
        h.total -= h.entries.front().second;
        h.entries.pop_front();
    }
}

} // namespace hn::core
