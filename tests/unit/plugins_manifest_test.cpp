#include "plugins_test_util.h"
#include <QRandomGenerator>

using namespace hn::plugins;
using namespace hn::plugins::test;

class ManifestTest : public QObject {
    Q_OBJECT
    static QJsonObject base() { return QJsonDocument::fromJson(manifestJson("good-id", {"ui", "network"}, {"api.example.com"})).object(); }
    static bool parse(const QJsonObject &o, QList<PluginError> *e = nullptr, Manifest *m = nullptr) {
        QList<PluginError> local;
        Manifest mm;
        return parseManifest(QJsonDocument(o).toJson(), "/tmp/x", m ? m : &mm, e ? e : &local);
    }
private slots:
    void valid() {
        Manifest m;
        QList<PluginError> e;
        QVERIFY(parse(base(), &e, &m));
        QVERIFY(e.isEmpty());
        QCOMPARE(m.id, QString("good-id"));
        QCOMPARE(m.tier, Tier::Script);
        QCOMPARE(m.permissions, (QStringList{"ui", "network"}));
        QCOMPARE(m.netHosts, QStringList{"api.example.com"});
    }
    void invalidFields_data() {
        QTest::addColumn<QString>("field");
        QTest::addColumn<QJsonValue>("value");
        QTest::addColumn<QString>("needle");
        QTest::newRow("id uppercase") << "id" << QJsonValue("Bad") << "id";
        QTest::newRow("id slash") << "id" << QJsonValue("a/b") << "id";
        QTest::newRow("id dotdot") << "id" << QJsonValue("a..b") << "id";
        QTest::newRow("id empty") << "id" << QJsonValue("") << "id";
        QTest::newRow("id too long") << "id" << QJsonValue(QString(65, 'a')) << "id";
        QTest::newRow("id number") << "id" << QJsonValue(5) << "id";
        QTest::newRow("name missing") << "name" << QJsonValue(QJsonValue::Undefined) << "name";
        QTest::newRow("version bad") << "version" << QJsonValue("1.2") << "version";
        QTest::newRow("version word") << "version" << QJsonValue("latest") << "version";
        QTest::newRow("author empty") << "author" << QJsonValue("  ") << "author";
        QTest::newRow("description ctl") << "description" << QJsonValue("a\x01z") << "description";
        QTest::newRow("homepage ftp") << "homepage" << QJsonValue("ftp://x.org") << "homepage";
        QTest::newRow("api 3") << "api" << QJsonValue(3) << "API";
        QTest::newRow("api 0") << "api" << QJsonValue(0) << "API";
        QTest::newRow("api string") << "api" << QJsonValue("1") << "api";
        QTest::newRow("api 1.5") << "api" << QJsonValue(1.5) << "api";
        QTest::newRow("tier") << "tier" << QJsonValue("wasm") << "tier";
        QTest::newRow("entry abs") << "entry" << QJsonValue("/etc/x.lua") << "entry";
        QTest::newRow("entry dotdot") << "entry" << QJsonValue("../x.lua") << "entry";
        QTest::newRow("entry not lua") << "entry" << QJsonValue("main.py") << "entry";
        QTest::newRow("entry backslash") << "entry" << QJsonValue("a\\b.lua") << "entry";
        QTest::newRow("perm unknown") << "permissions" << QJsonValue(QJsonArray{"root"}) << "unknown permission";
        QTest::newRow("perm dup") << "permissions" << QJsonValue(QJsonArray{"ui", "ui", "network"}) << "twice";
        QTest::newRow("perm native in script") << "permissions" << QJsonValue(QJsonArray{"native", "network"}) << "native";
        QTest::newRow("perm not array") << "permissions" << QJsonValue("ui") << "permissions";
        QTest::newRow("perm non-string") << "permissions" << QJsonValue(QJsonArray{5, "network"}) << "permission";
        QTest::newRow("hosts not array") << "net_hosts" << QJsonValue("api.example.com") << "net_hosts";
        QTest::newRow("hosts scheme") << "net_hosts" << QJsonValue(QJsonArray{"https://api.example.com"}) << "net_hosts";
        QTest::newRow("hosts port") << "net_hosts" << QJsonValue(QJsonArray{"api.example.com:443"}) << "net_hosts";
        QTest::newRow("hosts path") << "net_hosts" << QJsonValue(QJsonArray{"api.example.com/x"}) << "net_hosts";
        QTest::newRow("hosts wildcard") << "net_hosts" << QJsonValue(QJsonArray{"*.example.com"}) << "net_hosts";
        QTest::newRow("hosts ip") << "net_hosts" << QJsonValue(QJsonArray{"192.168.1.1"}) << "net_hosts";
        QTest::newRow("hosts localhost") << "net_hosts" << QJsonValue(QJsonArray{"localhost"}) << "net_hosts";
        QTest::newRow("hosts .local") << "net_hosts" << QJsonValue(QJsonArray{"printer.local"}) << "net_hosts";
        QTest::newRow("hosts upper") << "net_hosts" << QJsonValue(QJsonArray{"API.example.com"}) << "net_hosts";
        QTest::newRow("hosts dup") << "net_hosts" << QJsonValue(QJsonArray{"a.example.com", "a.example.com"}) << "twice";
        QTest::newRow("hosts empty with network") << "net_hosts" << QJsonValue(QJsonArray{}) << "network";
        QTest::newRow("min_app newer") << "min_app" << QJsonValue("9.0.0") << "requires Hyprnotes";
        QTest::newRow("min_app junk") << "min_app" << QJsonValue("soon") << "min_app";
    }
    void invalidFields() {
        QFETCH(QString, field);
        QFETCH(QJsonValue, value);
        QFETCH(QString, needle);
        auto o = base();
        if (value.isUndefined()) o.remove(field); else o[field] = value;
        QList<PluginError> e;
        QVERIFY2(!parse(o, &e), qPrintable(field));
        QVERIFY(!e.isEmpty());
        QStringList all;
        for (const auto &x : e) { QVERIFY(!x.message.isEmpty()); all << x.message; }
        QVERIFY2(all.join(" | ").contains(needle, Qt::CaseInsensitive), qPrintable(all.join(" | ")));
    }
    void networkNeedsHostsAndViceVersa() {
        auto o = base();
        o["permissions"] = QJsonArray{"ui"};  // hosts without network
        QList<PluginError> e;
        QVERIFY(!parse(o, &e));
        QVERIFY(e.first().message.contains("net_hosts"));
    }
    void nativeTier() {
        auto o = base();
        o["tier"] = "native"; o["entry"] = "libx.so"; o["permissions"] = QJsonArray{"native"}; o.remove("net_hosts");
        Manifest m;
        QVERIFY(parse(o, nullptr, &m));
        QCOMPARE(m.tier, Tier::Native);
        o["permissions"] = QJsonArray{"ui"};
        QList<PluginError> e;
        QVERIFY(!parse(o, &e));
        QVERIFY(e.first().message.contains("native"));
    }
    void notJson() {
        for (const QByteArray &b : {QByteArray(""), QByteArray("[]"), QByteArray("{"), QByteArray("null"), QByteArray(70000, ' ')}) {
            QList<PluginError> e;
            Manifest m;
            QVERIFY(!parseManifest(b, "/x/y", &m, &e));
            QVERIFY(!e.isEmpty());
        }
    }
    void versions() {
        QCOMPARE(compareVersions("0.1.0", "0.1.0"), 0);
        QCOMPARE(compareVersions("0.2.0", "0.10.0"), -1);
        QCOMPARE(compareVersions("1.0.0", "0.99.99"), 1);
        QCOMPARE(compareVersions("1.0.0-beta", "1.0.0"), 0);
    }
    void httpPolicy_data() {
        QTest::addColumn<QString>("url");
        QTest::addColumn<bool>("ok");
        const bool Y = true, N = false;
        QTest::newRow("plain https") << "https://api.example.com/x?y=1" << Y;
        QTest::newRow("upper scheme") << "HTTPS://api.example.com/x" << Y;
        QTest::newRow("upper host") << "https://API.Example.COM/x" << Y;
        QTest::newRow("explicit 443") << "https://api.example.com:443/x" << Y;
        QTest::newRow("http") << "http://api.example.com/x" << N;
        QTest::newRow("other port") << "https://api.example.com:8443/x" << N;
        QTest::newRow("other host") << "https://example.com/x" << N;
        QTest::newRow("suffix trick") << "https://api.example.com.evil.net/" << N;
        QTest::newRow("prefix trick") << "https://evilapi.example.com/" << N;
        QTest::newRow("userinfo trick") << "https://api.example.com@evil.net/" << N;
        QTest::newRow("userinfo") << "https://u:p@api.example.com/" << N;
        QTest::newRow("trailing dot") << "https://api.example.com./" << N;
        QTest::newRow("ip") << "https://127.0.0.1/" << N;
        QTest::newRow("file") << "file:///etc/passwd" << N;
        QTest::newRow("scheme relative") << "//api.example.com/x" << N;
        QTest::newRow("crlf") << "https://api.example.com/\r\nHost: x" << N;
        QTest::newRow("space") << "https://api.example.com/a b" << N;
        QTest::newRow("empty") << "" << N;
        QTest::newRow("too long") << ("https://api.example.com/" + QString(3000, 'a')) << N;
    }
    void httpPolicy() {
        QFETCH(QString, url);
        QFETCH(bool, ok);
        QString host, err;
        QCOMPARE(checkHttpUrl(url, {"api.example.com"}, &host, &err), ok);
        if (ok) QCOMPARE(host, QString("api.example.com")); else QVERIFY(!err.isEmpty());
    }
    void fuzz500() {
        QRandomGenerator rng(20261001);
        const QByteArray seed = manifestJson("fuzz-me", {"ui", "network", "storage"}, {"api.example.com"});
        const QList<QJsonValue> junk{QJsonValue(), QJsonValue(0), QJsonValue(-1), QJsonValue(1e300), QJsonValue(true), QJsonValue(""), QJsonValue(QString(100000, 'x')),
                                     QJsonValue(QJsonArray{}), QJsonValue(QJsonObject{}), QJsonValue(QJsonArray{QJsonArray{QJsonArray{}}}), QJsonValue("\xff\xfe"), QJsonValue("../../x")};
        const auto keys = QJsonDocument::fromJson(seed).object().keys();
        int accepted = 0, rejected = 0;
        for (int i = 0; i < 500; ++i) {
            QByteArray b = seed;
            switch (rng.bounded(6)) {
            case 0: for (int k = 0, n = 1 + rng.bounded(6); k < n; ++k) b[rng.bounded(b.size())] = char(rng.bounded(256)); break;
            case 1: b.truncate(rng.bounded(b.size())); break;
            case 2: b.remove(rng.bounded(b.size()), 1 + rng.bounded(20)); break;
            case 3: b.insert(rng.bounded(b.size()), char(rng.bounded(256))); break;
            case 4: {
                auto o = QJsonDocument::fromJson(seed).object();
                o[keys[rng.bounded(keys.size())]] = junk[rng.bounded(junk.size())];
                if (rng.bounded(2)) o.remove(keys[rng.bounded(keys.size())]);
                o[QString::number(rng.generate())] = junk[rng.bounded(junk.size())];
                b = QJsonDocument(o).toJson();
                break;
            }
            default: b = QByteArray(int(rng.bounded(300)), 0); for (auto &c : b) c = char(rng.bounded(256)); break;
            }
            QList<PluginError> e;
            Manifest m;
            const bool ok = parseManifest(b, "/some/dir", &m, &e);
            QCOMPARE(ok, e.isEmpty());
            if (ok) {
                ++accepted;
                QVERIFY(validPluginId(m.id));
                QCOMPARE(m.api, 1);
                QVERIFY(!m.has("native") || m.tier == Tier::Native);
                for (const auto &h : m.netHosts) QVERIFY(validHostName(h));
                QCOMPARE(m.netHosts.isEmpty(), !m.has("network"));
            } else {
                ++rejected;
                for (const auto &x : e) QVERIFY(!x.message.isEmpty());
            }
        }
        qInfo().noquote() << QString("fuzz: 500 mutated manifests, %1 accepted, %2 rejected, no crash").arg(accepted).arg(rejected);
        QVERIFY(rejected > 100);
    }
};
QTEST_APPLESS_MAIN(ManifestTest)
#include "plugins_manifest_test.moc"
