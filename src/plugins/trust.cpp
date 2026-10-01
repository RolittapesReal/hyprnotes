#include "hn/plugins/store.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>

namespace hn::plugins {

TrustStore::TrustStore(QString path) : path_(std::move(path)) {}

bool TrustStore::load() {
    recs_.clear();
    QFile f(path_);
    if (!f.exists()) return true;
    if (!f.open(QIODevice::ReadOnly)) return false;
    const auto doc = QJsonDocument::fromJson(f.read(16 * 1024 * 1024));
    if (!doc.isObject()) return false;
    const auto ps = doc.object().value(QLatin1String("plugins")).toObject();
    for (auto it = ps.begin(); it != ps.end(); ++it) {
        const auto o = it->toObject();
        TrustRecord r;
        r.hasConsent = o["hasConsent"].toBool();
        for (const auto &p : o["permissions"].toArray()) r.permissions << p.toString();
        r.hash = o["hash"].toString();
        r.enabled = o["enabled"].toBool() && r.hasConsent && !r.hash.isEmpty() && !o["needsReconsent"].toBool();
        r.needsReconsent = o["needsReconsent"].toBool();
        r.failures = o["failures"].toInt();
        r.consentedAt = o["consentedAt"].toString();
        recs_.insert(it.key(), r);
    }
    return true;
}

bool TrustStore::save() {
    QJsonObject ps;
    for (auto it = recs_.begin(); it != recs_.end(); ++it) {
        const auto &r = it.value();
        ps[it.key()] = QJsonObject{{"hasConsent", r.hasConsent}, {"permissions", QJsonArray::fromStringList(r.permissions)},
                                   {"hash", r.hash}, {"enabled", r.enabled}, {"needsReconsent", r.needsReconsent},
                                   {"failures", r.failures}, {"consentedAt", r.consentedAt}};
    }
    QDir().mkpath(QFileInfo(path_).absolutePath());
    QSaveFile f(path_);
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(QJsonDocument(QJsonObject{{"version", 1}, {"plugins", ps}}).toJson(QJsonDocument::Indented));
    return f.commit();
}

bool TrustStore::recordConsent(const QString &id, const QStringList &perms, const QString &hash) {
    if (hash.isEmpty()) return false;
    auto &r = recs_[id];
    r.hasConsent = true;
    r.permissions = perms;
    r.hash = hash;
    r.needsReconsent = false;
    r.failures = 0;
    r.consentedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    return save();
}

bool TrustStore::setEnabled(const QString &id, bool on, QString *err) {
    auto it = recs_.find(id);
    if (on && (it == recs_.end() || !it->hasConsent || it->needsReconsent)) {
        if (err) *err = QStringLiteral("plugin has no valid consent; review and accept its permissions first");
        return false;
    }
    if (it == recs_.end()) return true;
    it->enabled = on;
    if (on) it->failures = 0;
    return save();
}

bool TrustStore::verifyHash(const QString &id, const QString &cur) {
    auto it = recs_.find(id);
    if (it == recs_.end() || !it->hasConsent || it->hash == cur) return false;
    it->enabled = false;
    it->needsReconsent = true;
    save();
    return true;
}

int TrustStore::addFailure(const QString &id) {
    auto &r = recs_[id];
    ++r.failures;
    save();
    return r.failures;
}
void TrustStore::resetFailures(const QString &id) {
    auto it = recs_.find(id);
    if (it != recs_.end() && it->failures) { it->failures = 0; save(); }
}
void TrustStore::remove(const QString &id) { if (recs_.remove(id)) save(); }

AuditLog::AuditLog(QString path, qint64 maxBytes, int files) : path_(std::move(path)), maxBytes_(maxBytes), files_(files) {}

void AuditLog::log(const QString &event, const QString &pluginId, const QString &detail) {
    if (event == QLatin1String("denied") && ++denials_[pluginId] > 200) return;
    QDir().mkpath(QFileInfo(path_).absolutePath());
    if (QFileInfo(path_).size() >= maxBytes_) {
        QFile::remove(QStringLiteral("%1.%2").arg(path_).arg(files_ - 1));
        for (int i = files_ - 1; i >= 2; --i) QFile::rename(QStringLiteral("%1.%2").arg(path_).arg(i - 1), QStringLiteral("%1.%2").arg(path_).arg(i));
        QFile::rename(path_, path_ + QStringLiteral(".1"));
    }
    QFile f(path_);
    if (!f.open(QIODevice::Append)) return;
    QString d = detail.left(500);
    d.replace(QLatin1Char('\n'), QLatin1Char(' '));
    f.write(QJsonDocument(QJsonObject{{"ts", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}, {"event", event},
                                      {"plugin", pluginId}, {"detail", d}}).toJson(QJsonDocument::Compact) + '\n');
}

}  // namespace hn::plugins
