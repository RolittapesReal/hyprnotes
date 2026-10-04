#include "hn/plugins/market.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>

using namespace hn::plugins;

namespace {
QJsonObject entry(const QString &id = "journal", const QString &version = "1.0.0") {
    return QJsonObject{{"id", id}, {"name", "Journal"}, {"version", version}, {"author", "Hyprnotes"}, {"description", "Daily notes."},
                       {"tags", QJsonArray{"daily"}}, {"permissions", QJsonArray{"notes.write", "ui"}}, {"net_hosts", QJsonArray{}},
                       {"tier", "script"}, {"api", 2}, {"min_app", "0.1.0"},
                       {"url", "https://example.invalid/" + id + "-" + version + ".hnplugin"},
                       {"sha256", QString(64, 'a')}, {"size", 1234}};
}
QByteArray index(const QJsonArray &plugins, int schema = 1) {
    return QJsonDocument(QJsonObject{{"schema", schema}, {"updated", "2026-10-04T12:00:00Z"}, {"plugins", plugins}}).toJson();
}
}  // namespace

class MarketIndexTest : public QObject {
    Q_OBJECT
private slots:
    void valid_entry_roundtrips() {
        MarketIndex ix; QString err;
        QVERIFY2(parseMarketIndex(index({entry()}), &ix, &err), qPrintable(err));
        QCOMPARE(ix.entries.size(), 1);
        const auto &e = ix.entries[0];
        QCOMPARE(e.id, QString("journal"));
        QCOMPARE(e.size, qint64(1234));
        QCOMPARE(e.permissions, (QStringList{"notes.write", "ui"}));
        QVERIFY(!e.native());
        QCOMPARE(ix.skipped, 0);
    }
    void wrong_schema_rejected() {
        MarketIndex ix; QString err;
        QVERIFY(!parseMarketIndex(index({entry()}, 2), &ix, &err));
        QVERIFY(!err.isEmpty());
    }
    void oversized_document_rejected() {
        MarketIndex ix; QString err;
        QVERIFY(!parseMarketIndex(QByteArray(kMaxIndexBytes + 1, ' '), &ix, &err));
    }
    void truncated_or_garbage_json_rejected() {
        MarketIndex ix; QString err;
        QVERIFY(!parseMarketIndex(index({entry()}).left(40), &ix, &err));
        QVERIFY(!parseMarketIndex("not json", &ix, &err));
        QVERIFY(!parseMarketIndex("[]", &ix, &err));
        QVERIFY(!parseMarketIndex("", &ix, &err));
    }
    void bad_entries_are_skipped_and_counted() {
        auto bad = [&](const QString &key, const QJsonValue &v) { auto e = entry("p" + key); e[key] = v; return e; };
        QJsonArray a{entry("good"),
                     bad("url", "http://example.invalid/x.hnplugin"),            // not https
                     bad("url", "https://user:pw@example.invalid/x.hnplugin"),   // credentials
                     bad("url", "https://example.invalid:8443/x.hnplugin"),      // port
                     bad("sha256", "xyz"), bad("size", 0), bad("size", kMaxMarketPackageBytes + 1),
                     bad("version", "one"), bad("id", "Bad Id!"), bad("permissions", QJsonArray{"does.not.exist"}),
                     bad("tier", "wasm"), bad("api", 3), bad("name", ""), QJsonValue(5).toObject()};
        MarketIndex ix; QString err;
        QVERIFY2(parseMarketIndex(index(a), &ix, &err), qPrintable(err));
        QCOMPARE(ix.entries.size(), 1);
        QCOMPARE(ix.entries[0].id, QString("good"));
        QCOMPARE(ix.skipped, int(a.size()) - 1);
    }
    void duplicate_ids_keep_first() {
        MarketIndex ix; QString err;
        QVERIFY(parseMarketIndex(index({entry("a", "1.0.0"), entry("a", "2.0.0")}), &ix, &err));
        QCOMPARE(ix.entries.size(), 1);
        QCOMPARE(ix.entries[0].version, QString("1.0.0"));
        QCOMPARE(ix.skipped, 1);
    }
    void entry_cap_enforced() {
        QJsonArray a;
        for (int i = 0; i < kMaxIndexEntries + 20; ++i) a.append(entry(QString("p%1").arg(i)));
        MarketIndex ix; QString err;
        QVERIFY(parseMarketIndex(index(a), &ix, &err));
        QCOMPARE(ix.entries.size(), kMaxIndexEntries);
    }
    void unknown_fields_ignored_and_long_strings_trimmed_or_skipped() {
        auto e = entry(); e["future_field"] = "x";
        auto longDesc = entry("long"); longDesc["description"] = QString(5000, 'x');
        MarketIndex ix; QString err;
        QVERIFY(parseMarketIndex(index({e, longDesc}), &ix, &err));
        QCOMPARE(ix.entries.size(), 1);   // over-long description: entry skipped, not truncated silently
        QCOMPARE(ix.skipped, 1);
    }
    void native_entries_are_kept_and_flagged() {
        auto e = entry("native-thing"); e["tier"] = "native"; e["permissions"] = QJsonArray{};
        MarketIndex ix; QString err;
        QVERIFY(parseMarketIndex(index({e}), &ix, &err));
        QCOMPARE(ix.entries.size(), 1);
        QVERIFY(ix.entries[0].native());
    }
    void custom_policy_allows_loopback_http() {
        auto e = entry(); e["url"] = "http://127.0.0.1:9/x.hnplugin";
        MarketIndex ix; QString err;
        QVERIFY(parseMarketIndex(index({e}), &ix, &err));
        QVERIFY(ix.entries.isEmpty());
        QVERIFY(parseMarketIndex(index({e}), &ix, &err, [](const QUrl &u) { return u.host() == "127.0.0.1"; }));
        QCOMPARE(ix.entries.size(), 1);
    }
    void trailing_newline_and_control_chars_rejected() {
        auto with = [&](const QString &key, const QJsonValue &v) { auto e = entry("p"); e[key] = v; return e; };
        auto native = entry("n"); native["tier"] = "native"; native["permissions"] = QJsonArray{"bad\x1bperm"};
        QJsonArray a{with("version", "1.0.0\n"), with("min_app", "0.1.0\n"), with("sha256", QString(64, 'a') + "\n"),
                     with("name", "Jo\tb"), with("author", "A\x1b"), with("name", "x\x7f"), with("tags", QJsonArray{"t\tag"}),
                     with("description", "a\x01"), native, with("description", "line1\nline2")};
        MarketIndex ix; QString err;
        QVERIFY(parseMarketIndex(index(a), &ix, &err));
        QCOMPARE(ix.entries.size(), 1);   // only the multi-line description survives
        QCOMPARE(ix.entries[0].description, QString("line1\nline2"));
    }
    void non_integral_numbers_rejected() {
        auto with = [&](const QString &key, const QJsonValue &v) { auto e = entry("p"); e[key] = v; return e; };
        QJsonArray a{with("size", 1e300), with("size", 1.5), with("api", 2.5), with("api", "2")};
        MarketIndex ix; QString err;
        QVERIFY(parseMarketIndex(index(a), &ix, &err));
        QCOMPARE(ix.entries.size(), 0);
        QCOMPARE(ix.skipped, 4);
    }
    void state_compares_versions() {
        MarketEntry e; e.version = "1.2.0";
        QCOMPARE(marketState(e, ""), MarketState::NotInstalled);
        QCOMPARE(marketState(e, "1.2.0"), MarketState::Installed);
        QCOMPARE(marketState(e, "1.1.9"), MarketState::UpdateAvailable);
        QCOMPARE(marketState(e, "1.10.0"), MarketState::InstalledNewer);   // numeric, not lexical
        QCOMPARE(marketState(e, "1.2.0-beta.1"), MarketState::UpdateAvailable);   // pre-release sorts below the release
        e.version = "1.2.0-beta.1";
        QCOMPARE(marketState(e, "1.2.0"), MarketState::InstalledNewer);
    }
};

QTEST_APPLESS_MAIN(MarketIndexTest)
#include "plugins_market_index_test.moc"
