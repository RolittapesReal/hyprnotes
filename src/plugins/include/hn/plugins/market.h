#pragma once
// Plugin marketplace, UI-free half: registry index model + parser, download client, install pre-flight.
#include "hn/plugins/store.h"
#include "hn/plugins/types.h"
#include <QDateTime>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <functional>

class QNetworkAccessManager;

namespace hn::plugins {

inline constexpr qint64 kMaxIndexBytes = 1 << 20;
inline constexpr int kMaxIndexEntries = 500;
inline constexpr qint64 kMaxMarketPackageBytes = 5 << 20;

struct MarketEntry {
    QString id, name, version, author, description, tier, minApp, url, sha256;
    QStringList tags, permissions, netHosts;
    int api = 0;
    qint64 size = 0;
    bool native() const { return tier == QLatin1String("native"); }
};

struct MarketIndex {
    QString updated;
    QList<MarketEntry> entries;
    int skipped = 0;   // entries dropped by validation
};

using UrlPolicy = std::function<bool(const QUrl &)>;
// https, non-empty host, no credentials, default port. The production policy.
bool httpsOnly(const QUrl &u);

// Index is untrusted input. Returns false (with *err) only when the whole document is unusable.
bool parseMarketIndex(const QByteArray &json, MarketIndex *out, QString *err, const UrlPolicy &policy = httpsOnly);

enum class MarketState { NotInstalled, Installed, UpdateAvailable, InstalledNewer };
// installedVersion empty = not installed.
MarketState marketState(const MarketEntry &e, const QString &installedVersion);

struct IndexResult {
    bool ok = false;
    bool fromCache = false;     // served from the cache (offline, error or 304)
    bool revalidated = false;   // server answered 304: the cache is current
    QDateTime cachedAt;         // mtime of the cache file when fromCache
    MarketIndex index;
    QString error;              // why the network path failed (also set with ok=true when falling back to the cache)
};
struct DownloadResult { bool ok = false; QString path; QString error; };

// Fetches the registry index and packages. Creates no network object until the first request.
class MarketClient : public QObject {
    Q_OBJECT
public:
    static constexpr int kMaxRedirects = 3;
    static QString defaultIndexUrl();
    // cacheDir: the app cache dir; files live in <cacheDir>/market/. policy: production passes none (https only).
    MarketClient(QString indexUrl, QString cacheDir, QObject *parent = nullptr, UrlPolicy policy = httpsOnly);
    void setTimeoutMs(int ms) { timeoutMs_ = ms; }
    void setTotalDeadlineMs(int ms) { totalMs_ = ms; }   // tests; 0 = defaults (60 s index, 120 s download)
    bool networkCreated() const { return nam_ != nullptr; }
    bool busy() const { return busy_; }
    IndexResult cached() const;                                    // cache only, never touches the network
    void fetchIndex(std::function<void(IndexResult)> done);        // always revalidates with If-None-Match
    // Downloads e.url, checks size and SHA-256 against e, writes destFile atomically. destFile is removed on every failure.
    void download(const MarketEntry &e, const QString &destFile, std::function<void(DownloadResult)> done);

private:
    struct Raw { int status = 0; QByteArray body, etag; QString error; bool network = false; };
    void run(const QUrl &url, const QByteArray &etag, qint64 cap, int hop, int totalMs, std::function<void(Raw)> done);
    QString cachePath(const char *name) const;
    QNetworkAccessManager *nam();
    QString indexUrl_, cacheDir_;
    UrlPolicy policy_;
    QNetworkAccessManager *nam_ = nullptr;
    int timeoutMs_ = 15000;
    int totalMs_ = 0;
    bool busy_ = false;
};

struct Preflight { bool ok = false; QString error; CheckResult check; };
// Compares the downloaded package with its index entry and with this app. Nothing is installed.
Preflight preflightPackage(const QString &file, const MarketEntry &e, const QString &appVersion = QString::fromLatin1(kAppVersion));

}  // namespace hn::plugins
