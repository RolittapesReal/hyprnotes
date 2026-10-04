#include "market_test_util.h"
#include <QCoreApplication>
#include <QEventLoop>
#include <QFile>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

using namespace hn::plugins;
using namespace markettest;

namespace {
IndexResult fetch(MarketClient &c) {
    IndexResult out;
    QEventLoop loop;
    QTimer::singleShot(5000, &loop, &QEventLoop::quit);
    bool fired = false;
    c.fetchIndex([&](IndexResult r) { out = r; fired = true; loop.quit(); });
    if (!fired) loop.exec();
    return out;
}
DownloadResult get(MarketClient &c, const MarketEntry &e, const QString &dest) {
    DownloadResult out;
    QEventLoop loop;
    QTimer::singleShot(5000, &loop, &QEventLoop::quit);
    bool fired = false;
    c.download(e, dest, [&](DownloadResult r) { out = r; fired = true; loop.quit(); });
    if (!fired) loop.exec();
    return out;
}
QByteArray slurp(const QString &p) { QFile f(p); return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray(); }
}  // namespace

class MarketClientTest : public QObject {
    Q_OBJECT
    QByteArray goodIndex() const { return makeIndex({entryFor("journal", "1.0.0", "pkg", "http://127.0.0.1:1/j.hnplugin")}); }
private slots:
    void initTestCase() {
        FakeHttp probe;
        if (!probe.start()) QSKIP("loopback listen denied");
    }
    void first_fetch_parses_and_caches() {
        QTemporaryDir tmp; FakeHttp srv; QVERIFY(srv.start());
        srv.routes["/index.json"] = {200, goodIndex(), {{"ETag", "\"v1\""}}};
        MarketClient c(srv.url("/index.json"), tmp.path(), nullptr, loopbackOnly);
        const auto r = fetch(c);
        QVERIFY2(r.ok, qPrintable(r.error));
        QVERIFY(!r.fromCache);
        QCOMPARE(r.index.entries.size(), 1);
        QVERIFY(QFile::exists(tmp.path() + "/market/index.json"));
        QVERIFY(QFile::exists(tmp.path() + "/market/index.etag"));
    }
    void second_fetch_sends_etag_and_uses_304() {
        QTemporaryDir tmp; FakeHttp srv; QVERIFY(srv.start());
        srv.routes["/index.json"] = {200, goodIndex(), {{"ETag", "\"v1\""}}};
        MarketClient c(srv.url("/index.json"), tmp.path(), nullptr, loopbackOnly);
        QVERIFY(fetch(c).ok);
        srv.routes["/index.json"] = {304, {}, {}};
        const auto r = fetch(c);
        QVERIFY2(r.ok, qPrintable(r.error));
        QVERIFY(r.fromCache && r.revalidated);
        QCOMPARE(srv.seen.last().value("if-none-match"), QByteArray("\"v1\""));
    }
    void network_down_falls_back_to_cache() {
        QTemporaryDir tmp; FakeHttp srv; QVERIFY(srv.start());
        srv.routes["/index.json"] = {200, goodIndex(), {{"ETag", "\"v1\""}}};
        MarketClient c(srv.url("/index.json"), tmp.path(), nullptr, loopbackOnly);
        QVERIFY(fetch(c).ok);
        srv.close();
        const auto r = fetch(c);
        QVERIFY(r.ok && r.fromCache && !r.revalidated);
        QVERIFY(!r.error.isEmpty());
        QVERIFY(r.cachedAt.isValid());
    }
    void no_cache_and_no_network_is_a_clear_error() {
        QTemporaryDir tmp; FakeHttp srv; QVERIFY(srv.start());
        const QString u = srv.url("/index.json");
        srv.close();
        MarketClient c(u, tmp.path(), nullptr, loopbackOnly);
        const auto r = fetch(c);
        QVERIFY(!r.ok);
        QVERIFY2(r.error.contains("offline") || r.error.contains("reach"), qPrintable(r.error));
    }
    void corrupt_cache_is_ignored_and_removed() {
        QTemporaryDir tmp;
        QDir().mkpath(tmp.path() + "/market");
        QFile f(tmp.path() + "/market/index.json");
        QVERIFY(f.open(QIODevice::WriteOnly)); f.write("garbage\x01{{"); f.close();
        MarketClient c("http://127.0.0.1:1/x", tmp.path(), nullptr, loopbackOnly);
        QVERIFY(!c.cached().ok);
        QVERIFY(!QFile::exists(f.fileName()));
    }
    void invalid_index_does_not_replace_cache() {
        QTemporaryDir tmp; FakeHttp srv; QVERIFY(srv.start());
        srv.routes["/index.json"] = {200, goodIndex(), {{"ETag", "\"v1\""}}};
        MarketClient c(srv.url("/index.json"), tmp.path(), nullptr, loopbackOnly);
        QVERIFY(fetch(c).ok);
        const QByteArray before = slurp(tmp.path() + "/market/index.json");
        srv.routes["/index.json"] = {200, "{bad", {{"ETag", "\"v2\""}}};
        const auto r = fetch(c);
        QVERIFY(r.ok && r.fromCache);
        QVERIFY(!r.error.isEmpty());
        QCOMPARE(slurp(tmp.path() + "/market/index.json"), before);
        QCOMPARE(slurp(tmp.path() + "/market/index.etag"), QByteArray("\"v1\""));
    }
    void index_over_cap_is_aborted() {
        QTemporaryDir tmp; FakeHttp srv; QVERIFY(srv.start());
        srv.routes["/index.json"] = {200, QByteArray(kMaxIndexBytes + 1, ' '), {}};
        MarketClient c(srv.url("/index.json"), tmp.path(), nullptr, loopbackOnly);
        const auto r = fetch(c);
        QVERIFY(!r.ok);
        QVERIFY2(r.error.contains("larger"), qPrintable(r.error));
    }
    void redirect_to_http_is_refused_by_https_policy() {
        QTemporaryDir tmp; FakeHttp srv; QVERIFY(srv.start());
        srv.routes["/index.json"] = {200, goodIndex(), {}};
        MarketClient c(srv.url("/index.json"), tmp.path());   // default httpsOnly
        const auto r = fetch(c);
        QVERIFY(!r.ok);
        QVERIFY(srv.paths.isEmpty());
    }
    void redirect_to_other_scheme_refused() {
        QTemporaryDir tmp; FakeHttp srv; QVERIFY(srv.start());
        srv.routes["/index.json"] = {302, {}, {{"Location", "ftp://127.0.0.1/x"}}};
        MarketClient c(srv.url("/index.json"), tmp.path(), nullptr, loopbackOnly);
        QVERIFY(!fetch(c).ok);
    }
    void redirect_chain_limit() {
        QTemporaryDir tmp; FakeHttp srv; QVERIFY(srv.start());
        srv.routes["/index.json"] = {302, {}, {{"Location", "/r1"}}};
        srv.routes["/r1"] = {302, {}, {{"Location", "/r2"}}};
        srv.routes["/r2"] = {302, {}, {{"Location", "/r3"}}};
        srv.routes["/r3"] = {302, {}, {{"Location", "/r4"}}};
        srv.routes["/r4"] = {200, goodIndex(), {}};
        MarketClient c(srv.url("/index.json"), tmp.path(), nullptr, loopbackOnly);
        QVERIFY(!fetch(c).ok);   // 4 redirects
        srv.routes["/r3"] = {200, goodIndex(), {}};
        const auto r = fetch(c);   // 3 redirects
        QVERIFY2(r.ok, qPrintable(r.error));
    }
    void hang_times_out() {
        QTemporaryDir tmp; FakeHttp srv; QVERIFY(srv.start());
        srv.routes["/index.json"] = {200, {}, {}, true};
        MarketClient c(srv.url("/index.json"), tmp.path(), nullptr, loopbackOnly);
        c.setTimeoutMs(300);
        QElapsedTimer t; t.start();
        QVERIFY(!fetch(c).ok);
        QVERIFY(t.elapsed() < 3000);
    }
    void slow_drip_hits_total_deadline() {
        QTemporaryDir tmp; FakeHttp srv; QVERIFY(srv.start());
        srv.routes["/index.json"] = {200, {}, {}, false, true};
        const QByteArray pkg(100000, 'x');
        srv.routes["/p.hnplugin"] = {200, {}, {}, false, true};
        MarketClient c(srv.url("/index.json"), tmp.path(), nullptr, loopbackOnly);
        c.setTimeoutMs(5000);
        c.setTotalDeadlineMs(500);
        QElapsedTimer t; t.start();
        auto r = fetch(c);
        QVERIFY(!r.ok);
        QVERIFY2(r.error.contains("too long"), qPrintable(r.error));
        QVERIFY(t.elapsed() < 3000);
        QVERIFY(!c.busy());
        srv.routes["/index.json"] = {200, goodIndex(), {}};
        QVERIFY2(fetch(c).ok, "a later fetch must work");
        t.restart();
        const auto d = get(c, entryFor("p", "1.0.0", pkg, srv.url("/p.hnplugin")), tmp.path() + "/p.hnplugin");
        QVERIFY(!d.ok);
        QVERIFY2(d.error.contains("too long"), qPrintable(d.error));
        QVERIFY(t.elapsed() < 3000);
    }
    void cookies_not_stored() {
        QTemporaryDir tmp; FakeHttp srv; QVERIFY(srv.start());
        srv.routes["/index.json"] = {200, goodIndex(), {{"Set-Cookie", "a=b"}}};
        MarketClient c(srv.url("/index.json"), tmp.path(), nullptr, loopbackOnly);
        QVERIFY(fetch(c).ok);
        QVERIFY(fetch(c).ok);
        QCOMPARE(srv.seen.size(), 2);
        QVERIFY(!srv.seen.last().contains("cookie"));
    }
    void download_verifies_hash_and_writes_file() {
        QTemporaryDir tmp; FakeHttp srv; QVERIFY(srv.start());
        const QByteArray pkg = "package-bytes";
        srv.routes["/p.hnplugin"] = {200, pkg, {}};
        MarketClient c(srv.url("/index.json"), tmp.path(), nullptr, loopbackOnly);
        const QString dest = tmp.path() + "/dl/p.hnplugin";
        const auto r = get(c, entryFor("p", "1.0.0", pkg, srv.url("/p.hnplugin")), dest);
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.path, dest);
        QCOMPARE(slurp(dest), pkg);
        QCOMPARE(QDir(tmp.path() + "/dl").entryList(QDir::Files).size(), 1);
    }
    void download_hash_mismatch_removes_file() {
        QTemporaryDir tmp; FakeHttp srv; QVERIFY(srv.start());
        srv.routes["/p.hnplugin"] = {200, "package-byteX", {}};
        MarketClient c(srv.url("/index.json"), tmp.path(), nullptr, loopbackOnly);
        const QString dest = tmp.path() + "/p.hnplugin";
        QFile f(dest); QVERIFY(f.open(QIODevice::WriteOnly)); f.write("stale"); f.close();
        const auto r = get(c, entryFor("p", "1.0.0", "package-bytes", srv.url("/p.hnplugin")), dest);
        QVERIFY(!r.ok);
        QVERIFY2(r.error.contains("SHA-256"), qPrintable(r.error));
        QVERIFY(!QFile::exists(dest));
    }
    void download_size_mismatch_and_overrun() {
        QTemporaryDir tmp; FakeHttp srv; QVERIFY(srv.start());
        MarketClient c(srv.url("/index.json"), tmp.path(), nullptr, loopbackOnly);
        const QString dest = tmp.path() + "/p.hnplugin";
        const auto e = entryFor("p", "1.0.0", "package-bytes", srv.url("/p.hnplugin"));
        srv.routes["/p.hnplugin"] = {200, "short", {}};
        QVERIFY(!get(c, e, dest).ok);
        QVERIFY(!QFile::exists(dest));
        srv.routes["/p.hnplugin"] = {200, "package-bytes-and-much-more", {}};
        QVERIFY(!get(c, e, dest).ok);
        QVERIFY(!QFile::exists(dest));
    }
    void download_blocked_url_policy() {
        QTemporaryDir tmp; FakeHttp srv; QVERIFY(srv.start());
        MarketClient c(srv.url("/index.json"), tmp.path(), nullptr, loopbackOnly);
        const auto r = get(c, entryFor("p", "1.0.0", "x", "https://example.invalid/p.hnplugin"), tmp.path() + "/p.hnplugin");
        QVERIFY(!r.ok);
        QVERIFY2(r.error.contains("not allowed"), qPrintable(r.error));
        QVERIFY(srv.paths.isEmpty());
    }
    void no_manager_before_first_use() {
        QTemporaryDir tmp; FakeHttp srv; QVERIFY(srv.start());
        srv.routes["/index.json"] = {200, goodIndex(), {}};
        MarketClient c(srv.url("/index.json"), tmp.path(), nullptr, loopbackOnly);
        QVERIFY(!c.networkCreated());
        QVERIFY(!c.cached().ok);
        QVERIFY(!c.networkCreated());
        fetch(c);
        QVERIFY(c.networkCreated());
    }
    void busy_rejects_parallel_fetch() {
        QTemporaryDir tmp; FakeHttp srv; QVERIFY(srv.start());
        srv.routes["/index.json"] = {200, goodIndex(), {}};
        MarketClient c(srv.url("/index.json"), tmp.path(), nullptr, loopbackOnly);
        QList<IndexResult> got;
        QEventLoop loop;
        QTimer::singleShot(5000, &loop, &QEventLoop::quit);
        auto cb = [&](IndexResult r) { got << r; if (got.size() == 2) loop.quit(); };
        c.fetchIndex(cb);
        c.fetchIndex(cb);
        loop.exec();
        QCOMPARE(got.size(), 2);
        QVERIFY(!got[0].ok);   // the rejection is delivered first
        QVERIFY2(got[0].error.contains("already running"), qPrintable(got[0].error));
        QVERIFY(got[1].ok);
    }
};

QTEST_MAIN(MarketClientTest)
#include "plugins_market_client_test.moc"
