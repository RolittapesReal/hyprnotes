#pragma once
// Trust/consent state, audit log and package install/author tooling (UI-free).
#include "hn/plugins/types.h"

namespace hn::plugins {

// SHA-256 (hex) over the sorted relative file list and contents of a plugin directory (hidden entries ignored,
// symlinks never followed). Empty string + *err on failure.
QString hashDirectory(const QString &dir, QString *err = nullptr);

struct TrustRecord {
    bool hasConsent = false;
    QStringList permissions;  // consented
    QString hash;             // package hash at consent
    bool enabled = false;
    bool needsReconsent = false;
    int failures = 0;         // consecutive callback failures
    QString consentedAt;
};

// $XDG_STATE_HOME/hyprnotes/plugins.json, atomic writes. Every mutation is persisted immediately.
class TrustStore {
public:
    explicit TrustStore(QString path);
    bool load();  // missing/corrupt file => empty store (corrupt => everything disabled)
    TrustRecord record(const QString &id) const { return recs_.value(id); }
    bool known(const QString &id) const { return recs_.contains(id); }
    bool recordConsent(const QString &id, const QStringList &perms, const QString &hash);
    bool setEnabled(const QString &id, bool on, QString *err = nullptr);  // enabling requires a valid consent
    // Compares the current hash with the consented one. A change disables the plugin and sets needsReconsent. Returns true if invalidated.
    bool verifyHash(const QString &id, const QString &currentHash);
    int addFailure(const QString &id);
    void resetFailures(const QString &id);
    void remove(const QString &id);
    QString path() const { return path_; }
private:
    bool save();
    QString path_;
    QHash<QString, TrustRecord> recs_;
};

// Append-only JSONL, rotated at maxBytes keeping `files` files (current + .1 + .2).
class AuditLog {
public:
    explicit AuditLog(QString path, qint64 maxBytes = 1024 * 1024, int files = 3);
    void log(const QString &event, const QString &pluginId, const QString &detail = QString());
    QString path() const { return path_; }
private:
    QString path_;
    qint64 maxBytes_;
    int files_;
    QHash<QString, int> denials_;  // per plugin cap so a loop cannot flood the log
};

struct CheckResult {
    bool ok = false;
    Manifest manifest;
    QList<PluginError> errors;
    QStringList warnings;
    QString hash;
    int fileCount = 0;
    qint64 totalBytes = 0;
};
// Validates a plugin folder exactly as the installer would (manifest, caps, symlinks, UTF-8, native libs, Lua syntax).
CheckResult checkDirectory(const QString &dir, const QString &appVersion = QString::fromLatin1(kAppVersion));

struct InstallResult {
    bool ok = false;
    QString id;
    QList<PluginError> errors;
    QString hash;
    bool upgraded = false;
    bool consentKept = false;  // upgrade with byte-identical content
};

class PackageInstaller {
public:
    PackageInstaller(QString pluginsDir, TrustStore *trust, AuditLog *audit, QString appVersion = QString::fromLatin1(kAppVersion));
    // source: a folder or a .hnplugin (zip/tar) archive. Staged in a temp dir under pluginsDir, then atomically renamed.
    // An existing plugin with the same id is only replaced when allowUpgrade is set (and its installed manifest id matches).
    InstallResult install(const QString &source, bool allowUpgrade = false);
    bool uninstall(const QString &id, QString *err = nullptr);
private:
    QString dir_, appVersion_;
    TrustStore *trust_;
    AuditLog *audit_;
};

// Authoring helpers (hyprnotes --pack-plugin / --new-plugin).
bool packDirectory(const QString &dir, const QString &outFile, QList<PluginError> *errs);
bool createTemplate(const QString &dir, const QString &name, QString *err);

}  // namespace hn::plugins
