#include "hn/plugins/market.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QTimer>
#include <memory>

namespace hn::plugins {

QString MarketClient::defaultIndexUrl() {
    return QStringLiteral("https://raw.githubusercontent.com/RolittapesReal/hyprnotes-plugins/main/index.json");
}

MarketClient::MarketClient(QString indexUrl, QString cacheDir, QObject *parent, UrlPolicy policy)
    : QObject(parent), indexUrl_(std::move(indexUrl)), cacheDir_(std::move(cacheDir)), policy_(std::move(policy)) {}

QString MarketClient::cachePath(const char *name) const {
    return cacheDir_ + QStringLiteral("/market/") + QLatin1String(name);
}

// Created on first use only: nothing in the app may touch the network before the user opens Browse.
QNetworkAccessManager *MarketClient::nam() {
    if (!nam_) nam_ = new QNetworkAccessManager(this);
    return nam_;
}

namespace {
bool writeAtomic(const QString &path, const QByteArray &data) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(data) == data.size() && f.commit();
}
}  // namespace

// One request, redirects followed by hand so the URL policy is re-checked on every hop.
void MarketClient::run(const QUrl &url, const QByteArray &etag, qint64 cap, int hop, int totalMs, std::function<void(Raw)> done) {
    if (!policy_(url)) {
        const QString msg = tr("This address is not allowed (only https:// URLs are used).");
        QTimer::singleShot(0, this, [done, msg] { done({0, {}, {}, msg, false}); });
        return;
    }
    QNetworkRequest rq(url);
    rq.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    rq.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    rq.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    rq.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    rq.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
    rq.setTransferTimeout(timeoutMs_);
    rq.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("hyprnotes"));
    if (!etag.isEmpty()) rq.setRawHeader("If-None-Match", etag);
    QNetworkReply *r = nam()->get(rq);
    // The transfer timeout is inactivity-based; a slow drip would never trip it, so cap the whole request too.
    auto timedOut = std::make_shared<bool>(false);
    QTimer::singleShot(totalMs_ > 0 ? totalMs_ : totalMs, r, [r, timedOut] { if (r->isRunning()) { *timedOut = true; r->abort(); } });
    auto body = std::make_shared<QByteArray>();
    auto over = std::make_shared<bool>(false);
    connect(r, &QNetworkReply::readyRead, r, [r, body, over, cap] {
        if (*over) return;
        body->append(r->readAll());
        if (body->size() > cap) { *over = true; r->abort(); }
    });
    connect(r, &QNetworkReply::finished, this, [=, this] {
        r->deleteLater();
        if (!*over) { body->append(r->readAll()); if (body->size() > cap) *over = true; }
        if (*timedOut) { done({0, {}, {}, tr("The server took too long to answer."), false}); return; }
        if (*over) { done({0, {}, {}, tr("The response is larger than the allowed limit."), false}); return; }
        const int st = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (st >= 300 && st < 400 && st != 304) {
            if (hop >= kMaxRedirects) { done({st, {}, {}, tr("Too many redirects."), false}); return; }
            const QUrl next = url.resolved(QUrl(QString::fromUtf8(r->rawHeader("Location"))));
            run(next, {}, cap, hop + 1, totalMs, done);   // policy re-checked at the top of run()
            return;
        }
        if (st == 0 || st >= 400) {
            done({st, {}, {}, st ? tr("The server answered HTTP %1.").arg(st) : tr("Could not reach the server: %1").arg(r->errorString()), st == 0});
            return;
        }
        done({st, *body, r->rawHeader("ETag"), {}, false});
    });
}

IndexResult MarketClient::cached() const {
    IndexResult res;
    const QString path = cachePath("index.json");
    QFile f(path);
    if (!f.exists()) return res;
    QByteArray data;
    MarketIndex ix;
    QString err;
    // A cache that is oversized, unreadable or fails validation (e.g. a half-written file) is dropped, never shown.
    if (f.size() <= kMaxIndexBytes && f.open(QIODevice::ReadOnly)) data = f.readAll();
    f.close();
    if (data.isEmpty() || !parseMarketIndex(data, &ix, &err, policy_)) {
        QFile::remove(path);
        QFile::remove(cachePath("index.etag"));
        res.error = tr("The saved plugin list was damaged and has been discarded.");
        return res;
    }
    res.ok = true;
    res.fromCache = true;
    res.cachedAt = QFileInfo(path).lastModified();
    res.index = std::move(ix);
    return res;
}

void MarketClient::fetchIndex(std::function<void(IndexResult)> done) {
    if (busy_) {
        QTimer::singleShot(0, this, [done] { IndexResult r; r.error = tr("A refresh is already running."); done(r); });
        return;
    }
    busy_ = true;
    QByteArray etag;
    if (cached().ok) {   // an ETag without a usable cache would turn a 304 into an empty list
        QFile f(cachePath("index.etag"));
        if (f.open(QIODevice::ReadOnly)) etag = f.read(256).trimmed();
    }
    run(QUrl(indexUrl_), etag, kMaxIndexBytes, 0, 60000, [this, done](Raw raw) {
        busy_ = false;
        QString reason = raw.error;
        if (raw.status == 304) {
            IndexResult r = cached();
            if (r.ok) { r.revalidated = true; done(r); return; }
            reason = tr("The server said the list is unchanged, but nothing valid is cached.");
        } else if (reason.isEmpty() && raw.status == 200) {
            MarketIndex ix;
            QString err;
            if (parseMarketIndex(raw.body, &ix, &err, policy_)) {
                if (writeAtomic(cachePath("index.json"), raw.body)) {
                    if (raw.etag.isEmpty()) QFile::remove(cachePath("index.etag"));
                    else writeAtomic(cachePath("index.etag"), raw.etag);
                }
                IndexResult r;
                r.ok = true;
                r.index = std::move(ix);
                done(r);
                return;
            }
            reason = tr("The plugin list from the server was not usable: %1").arg(err);
        } else if (reason.isEmpty()) {
            reason = tr("The server answered HTTP %1.").arg(raw.status);
        }
        IndexResult r = cached();
        if (r.ok) r.error = reason;
        else r.error = raw.network ? tr("You appear to be offline and nothing is cached yet.") : reason;
        done(r);
    });
}

void MarketClient::download(const MarketEntry &e, const QString &destFile, std::function<void(DownloadResult)> done) {
    auto fail = [destFile](const QString &msg) {
        QFile::remove(destFile);
        DownloadResult r;
        r.error = msg;
        return r;
    };
    const QUrl url(e.url, QUrl::StrictMode);
    if (!policy_(url) || e.size < 1 || e.size > kMaxMarketPackageBytes) {
        const DownloadResult r = fail(!policy_(url) ? tr("This address is not allowed (only https:// URLs are used).")
                                                    : tr("The package size is not acceptable."));
        QTimer::singleShot(0, this, [done, r] { done(r); });
        return;
    }
    const MarketEntry entry = e;
    run(url, {}, entry.size, 0, 120000, [=](Raw raw) {
        if (!raw.error.isEmpty()) { done(fail(raw.error)); return; }
        if (raw.status != 200) { done(fail(tr("The server answered HTTP %1.").arg(raw.status))); return; }
        if (raw.body.size() != entry.size) { done(fail(tr("The downloaded file has the wrong size."))); return; }
        if (QCryptographicHash::hash(raw.body, QCryptographicHash::Sha256).toHex() != entry.sha256.toLatin1()) {
            done(fail(tr("The downloaded file does not match its SHA-256 checksum.")));
            return;
        }
        if (!writeAtomic(destFile, raw.body)) { done(fail(tr("Could not save the downloaded file."))); return; }
        DownloadResult r;
        r.ok = true;
        r.path = destFile;
        done(r);
    });
}

}  // namespace hn::plugins
