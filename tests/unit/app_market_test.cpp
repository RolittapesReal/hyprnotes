// Browse tab: first-use notice, nothing-before-consent, cached list + revalidation, filters, install / update flows through the
// real download, pre-flight and consent path against a loopback registry, layout, theme and idle behaviour.
#include "app_test_util.h"
#include "consent_dialog.h"
#include "hn/plugins/store.h"
#include "market_model.h"
#include "market_page.h"
#include "market_test_util.h"
#include "plugins_page.h"
#include "ui_polish_test_util.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDirIterator>
#include <QListWidget>
#include <QLabel>
#include <QNetworkReply>
#include <QLineEdit>
#include <QListView>
#include <QNetworkAccessManager>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QTabBar>
#include <QTimer>

using namespace apptest;
using namespace hn::plugins;
using markettest::FakeHttp;

namespace {
// One registry on loopback, a controller wired to it and a Plugins page. Entries: alpha (script, ui), beta (script, network),
// gamma (native).
struct Fx {
    PolishProfile profile;
    Lib lib;
    FakeHttp srv;
    QMap<QString, QByteArray> packages;
    QList<MarketEntry> entries;
    ConsentRequest seen;
    int consents = 0, confirms = 0;
    bool accept = true;
    std::unique_ptr<AppController> c;
    std::unique_ptr<PluginsPage> page;

    Fx() { QVERIFY(srv.start()); }
    QByteArray pack(const QString &id, const QString &version, const QStringList &perms, const QStringList &hosts = {}) {
        const QString dir = lib.dir.filePath("src-" + id + version);
        QDir().mkpath(dir);
        QJsonObject o{{"id", id}, {"name", id}, {"version", version}, {"author", "Test"}, {"description", "Test plugin."}, {"api", 2},
                      {"tier", "script"}, {"entry", "main.lua"}, {"permissions", QJsonArray::fromStringList(perms)}, {"min_app", "0.1.0"}};
        if (!hosts.isEmpty()) o["net_hosts"] = QJsonArray::fromStringList(hosts);
        QFile pj(dir + "/plugin.json"), ml(dir + "/main.lua");
        if (!pj.open(QIODevice::WriteOnly) || !ml.open(QIODevice::WriteOnly)) qFatal("cannot write plugin");
        pj.write(QJsonDocument(o).toJson());
        ml.write("hn.log('x')");
        pj.close(); ml.close();
        const QString file = lib.dir.filePath(id + version + ".hnplugin");
        QList<PluginError> errs;
        if (!packDirectory(dir, file, &errs)) qFatal("cannot pack");
        QFile f(file);
        f.open(QIODevice::ReadOnly);
        return f.readAll();
    }
    MarketEntry add(const QString &id, const QString &version, const QStringList &perms, const QStringList &hosts = {}, const QStringList &tags = {}) {
        const QByteArray pkg = pack(id, version, perms, hosts);
        const QString path = "/" + id + "-" + version + ".hnplugin";
        MarketEntry e = markettest::entryFor(id, version, pkg, srv.url(path));
        e.permissions = perms; e.netHosts = hosts; e.tags = tags;
        packages[id] = pkg;
        srv.routes[path].body = pkg;
        entries.removeIf([&](const MarketEntry &x) { return x.id == id; });
        entries << e;
        return e;
    }
    void publish() {
        QJsonArray a;
        for (const auto &e : std::as_const(entries))
            a.append(QJsonObject{{"id", e.id}, {"name", e.name}, {"version", e.version}, {"author", e.author}, {"description", e.description},
                                 {"tags", QJsonArray::fromStringList(e.tags)}, {"permissions", QJsonArray::fromStringList(e.permissions)},
                                 {"net_hosts", QJsonArray::fromStringList(e.netHosts)}, {"tier", e.tier}, {"api", e.api}, {"min_app", e.minApp},
                                 {"url", e.url}, {"sha256", e.sha256}, {"size", double(e.size)}});
        srv.routes["/index.json"].body = QJsonDocument(QJsonObject{{"schema", 1}, {"updated", "2026-10-04T12:00:00Z"}, {"plugins", a}}).toJson();
        srv.routes["/index.json"].headers["ETag"] = "\"v1\"";
    }
    void standard() {
        add("alpha", "1.0.0", {"ui"}, {}, {"text"});
        add("beta", "1.0.0", {"network"}, {"api.example.com"}, {"net"});
        MarketEntry g = markettest::entryFor("gamma", "1.0.0", "native", srv.url("/gamma-1.0.0.hnplugin"));
        g.tier = "native"; g.permissions = {"native"};
        entries << g;
        publish();
    }
    void up() {   // fresh controller and page on the same dirs (the "next launch")
        page.reset();
        c.reset();
        ControllerOptions o = lib.opts();
        o.marketIndexUrl = srv.url("/index.json");
        o.marketUrlPolicy = markettest::loopbackOnly;
        o.pluginHooks.consent = [this](const ConsentRequest &r) { seen = r; ++consents; return accept; };
        o.pluginHooks.confirm = [this](const QString &, const QString &) { ++confirms; return true; };
        c = std::make_unique<AppController>(o);
        page = std::make_unique<PluginsPage>(c.get());
        page->resize(900, 700);
        page->show();
    }
    MarketPage *browse() { page->showBrowse(); return page->browse(); }
    MarketPage *open() {   // acknowledge and wait for the list
        auto *m = browse();
        QTest::mouseClick(m->button("continue"), Qt::LeftButton);
        return m;
    }
    QStringList cacheFiles() const {
        QStringList out;
        QDirIterator it(lib.cache, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) out << it.next();
        return out;
    }
};
}  // namespace

class MarketUiTest : public QObject {
    Q_OBJECT
private slots:
    void nothing_exists_until_browse_is_acknowledged() {
        Fx f; f.standard(); f.up();
        QTest::qWait(100);
        QVERIFY(!f.c->marketCreated());
        QVERIFY(f.srv.paths.isEmpty());
        QVERIFY(!f.page->browse());
    }
    void hostile_text_is_never_rich_text() {
        Fx f; f.standard();
        MarketEntry &a = f.entries[0];
        a.name = "<b>n</b>"; a.description = "<b>x</b><img src=x>"; a.author = "<i>a</i>"; a.tags = {"<u>t</u>"};
        f.publish(); f.up();
        auto *m = f.open();
        QTRY_COMPARE(m->model()->rowCount(), 3);
        QVERIFY(m->model()->data(m->model()->index(0, 0), Qt::ToolTipRole).toString().isEmpty());
        m->list()->setCurrentIndex(m->model()->index(0, 0));
        for (QLabel *l : m->findChildren<QLabel *>()) {
            QVERIFY2(l->textFormat() == Qt::PlainText || l->textFormat() == Qt::MarkdownText, qPrintable(l->text()));
        }
    }
    void leftover_downloads_are_swept_once_browse_opens() {
        Fx f; f.standard(); f.up();
        const QString dir = f.lib.cache + "/market";
        QDir().mkpath(dir);
        QFile a(dir + "/dl-old.hnplugin"), b(dir + "/keep.txt");
        QVERIFY(a.open(QIODevice::WriteOnly) && b.open(QIODevice::WriteOnly));
        a.close(); b.close();
        f.open();
        QVERIFY(!QFileInfo::exists(dir + "/dl-old.hnplugin"));
        QVERIFY(QFileInfo::exists(dir + "/keep.txt"));
    }
    void opening_browse_shows_notice_and_does_not_fetch() {
        Fx f; f.standard(); f.up();
        auto *m = f.browse();
        QVERIFY(m && m->notice()->isVisible());
        QVERIFY(!m->list()->isVisible());
        QCOMPARE(m->findChild<QLabel *>("hnMarketNoticeText")->text(),
                 QString("Browse contacts 127.0.0.1 to list plugins. Nothing about you is sent. Nothing is fetched until you continue."));
        QTest::qWait(100);
        QVERIFY(f.srv.paths.isEmpty());
        QVERIFY(!f.c->marketCreated());
        QVERIFY(!f.c->marketNoticeAcknowledged());
        QVERIFY(f.page->warningLabel()->isVisible());   // the permanent warning stays above the tabs
    }
    void back_returns_without_fetch() {
        Fx f; f.standard(); f.up();
        auto *m = f.browse();
        QTest::mouseClick(m->button("back"), Qt::LeftButton);
        QCOMPARE(f.page->tabs()->currentIndex(), 0);
        QTest::qWait(100);
        QVERIFY(f.srv.paths.isEmpty());
        QVERIFY(!f.c->marketCreated());
        QVERIFY(!f.c->marketNoticeAcknowledged());
    }
    void continue_acknowledges_fetches_and_lists() {
        Fx f; f.standard(); f.up();
        auto *m = f.open();
        QTRY_COMPARE(m->model()->rowCount(), 3);
        QVERIFY(QFileInfo::exists(f.lib.state + "/market-notice-ack"));
        QCOMPARE(f.srv.paths, QStringList{"/index.json"});
        QVERIFY(m->list()->isVisible() && !m->notice()->isVisible());
        QTRY_VERIFY2(m->statusLabel()->text().contains("ready"), qPrintable(m->statusLabel()->text()));
    }
    void second_open_shows_cache_then_revalidates() {
        Fx f; f.standard(); f.up();
        QTRY_COMPARE(f.open()->model()->rowCount(), 3);
        f.srv.paths.clear(); f.srv.seen.clear();
        f.srv.routes["/index.json"].hang = true;   // the revalidation never completes: the list must come from the cache
        f.up();
        auto *m = f.browse();
        QCOMPARE(m->model()->rowCount(), 3);
        QTRY_COMPARE(f.srv.seen.size(), 1);
        QVERIFY(f.srv.seen[0].contains("if-none-match"));
        QCOMPARE(f.srv.seen[0]["if-none-match"], QByteArray("\"v1\""));
        QVERIFY(m->list()->isVisible());
    }
    void offline_with_cache_lists_and_says_cached() {
        Fx f; f.standard(); f.up();
        QTRY_COMPARE(f.open()->model()->rowCount(), 3);
        f.srv.close();
        f.up();
        auto *m = f.browse();
        QTRY_VERIFY2(m->statusLabel()->text().contains("cached"), qPrintable(m->statusLabel()->text()));
        QTRY_VERIFY(m->button("refresh")->isEnabled());
        QCOMPARE(m->model()->rowCount(), 3);
    }
    void offline_without_cache_shows_message_not_blank() {
        Fx f; f.standard(); f.up();
        f.srv.close();
        auto *m = f.open();
        QTRY_VERIFY2(m->statusLabel()->text().contains("cannot be reached"), qPrintable(m->statusLabel()->text()));
        QCOMPARE(m->model()->rowCount(), 0);
        QVERIFY(!m->statusLabel()->text().isEmpty());
    }
    void search_and_tag_and_hide_dangerous_filter_rows() {
        Fx f; f.standard(); f.up();
        auto *m = f.open();
        QTRY_COMPARE(m->model()->rowCount(), 3);
        QCOMPARE(m->model()->allTags(), QStringList({"net", "text"}));
        m->search()->setText("ALPH");
        QCOMPARE(m->model()->rowCount(), 1);
        m->search()->setText("net");   // matches beta by tag
        QCOMPARE(m->model()->rowCount(), 1);
        QCOMPARE(m->model()->entryAt(0)->id, QString("beta"));
        m->search()->clear();
        m->tagBox()->setCurrentIndex(m->tagBox()->findText("text"));
        QCOMPARE(m->model()->rowCount(), 1);
        QCOMPARE(m->model()->entryAt(0)->id, QString("alpha"));
        m->tagBox()->setCurrentIndex(0);
        QCOMPARE(m->model()->rowCount(), 3);
        m->hideDangerous()->setChecked(true);   // drops beta (network) and gamma (native)
        QCOMPARE(m->model()->rowCount(), 1);
        QCOMPARE(m->model()->entryAt(0)->id, QString("alpha"));
        QCOMPARE(m->model()->totalCount(), 3);
    }
    void dangerous_permission_chip_uses_danger_role() {
        Fx f; f.standard();
        const auto alpha = MarketDelegate::chipsFor(f.entries[0]);
        QCOMPARE(alpha.size(), 1);
        QCOMPARE(alpha[0].text, QString("ui"));
        QVERIFY(!alpha[0].dangerous);
        const auto beta = MarketDelegate::chipsFor(f.entries[1]);
        QCOMPARE(beta.size(), 1);
        QCOMPARE(beta[0].text, QString("network"));
        QVERIFY(beta[0].dangerous);
        QVERIFY(MarketDelegate::chipsFor(f.entries[2])[0].dangerous);   // native
    }
    void install_runs_download_preflight_consent_and_lists_installed() {
        Fx f; f.standard(); f.up();
        auto *m = f.open();
        QTRY_COMPARE(m->model()->rowCount(), 3);
        QSignalSpy installed(m, &MarketPage::installed);
        m->select("alpha");
        QVERIFY(m->button("install")->isEnabled());
        QTest::mouseClick(m->button("install"), Qt::LeftButton);
        QTRY_COMPARE(installed.count(), 1);
        QCOMPARE(installed[0][0].toString(), QString("alpha"));
        QCOMPARE(f.consents, 1);
        QCOMPARE(f.seen.permissions, QStringList{"ui"});
        QVERIFY(QFileInfo(f.lib.dir.filePath("plugins/alpha/plugin.json")).exists());
        QCOMPARE(f.c->plugins().managerIfActive()->info("alpha").status, Status::Enabled);   // same as the Manual path after consent
        QCOMPARE(f.c->plugins().sourceOf("alpha"), "marketplace: " + f.entries[0].url);
        QVERIFY(f.cacheFiles().filter("dl-alpha").isEmpty());
        QTRY_VERIFY(f.page->list()->count() == 1);   // the Installed tab refreshed
        QCOMPARE(f.page->selectedId(), QString("alpha"));
        QCOMPARE(m->model()->stateAt(m->list()->currentIndex().row()), MarketState::Installed);
        QVERIFY(!m->button("install")->isEnabled());
        QCOMPARE(f.confirms, 0);
    }
    void install_with_consent_declined_leaves_plugin_disabled() {
        Fx f; f.standard(); f.accept = false; f.up();
        auto *m = f.open();
        QTRY_COMPARE(m->model()->rowCount(), 3);
        QSignalSpy installed(m, &MarketPage::installed);
        m->select("alpha");
        QTest::mouseClick(m->button("install"), Qt::LeftButton);
        QTRY_COMPARE(installed.count(), 1);
        QVERIFY(QFileInfo(f.lib.dir.filePath("plugins/alpha/plugin.json")).exists());
        QVERIFY(f.c->plugins().managerIfActive()->info("alpha").status != Status::Enabled);
        QVERIFY(m->statusLabel()->text().contains("not enabled"));
        QVERIFY(f.cacheFiles().filter("dl-alpha").isEmpty());
    }
    void hash_tamper_installs_nothing() {
        Fx f; f.standard();
        QByteArray bad = f.packages["alpha"];
        bad[bad.size() / 2] = char(bad[bad.size() / 2] ^ 0x55);   // same size, different bytes
        f.srv.routes["/alpha-1.0.0.hnplugin"].body = bad;
        f.up();
        auto *m = f.open();
        QTRY_COMPARE(m->model()->rowCount(), 3);
        m->select("alpha");
        QTest::mouseClick(m->button("install"), Qt::LeftButton);
        QTRY_VERIFY2(m->statusLabel()->text().contains("SHA-256"), qPrintable(m->statusLabel()->text()));
        QVERIFY(!QFileInfo(f.lib.dir.filePath("plugins/alpha")).exists());
        QCOMPARE(f.consents, 0);
        QVERIFY2(f.cacheFiles().filter(".hnplugin").isEmpty(), qPrintable(f.cacheFiles().join(",")));
        QTRY_VERIFY(m->button("install")->isEnabled());   // usable again after the failure
    }
    void manifest_mismatch_installs_nothing() {
        Fx f; f.standard();
        // The package is genuine and matches its hash, but asks for a permission the index does not list.
        const QByteArray pkg = f.pack("alpha", "1.0.0", {"ui", "clipboard"});
        f.entries[0].sha256 = QString::fromLatin1(QCryptographicHash::hash(pkg, QCryptographicHash::Sha256).toHex());
        f.entries[0].size = pkg.size();
        f.srv.routes["/alpha-1.0.0.hnplugin"].body = pkg;
        f.publish();
        f.up();
        auto *m = f.open();
        QTRY_COMPARE(m->model()->rowCount(), 3);
        m->select("alpha");
        QTest::mouseClick(m->button("install"), Qt::LeftButton);
        QTRY_VERIFY2(m->statusLabel()->text().contains("permissions"), qPrintable(m->statusLabel()->text()));
        QVERIFY(!QFileInfo(f.lib.dir.filePath("plugins/alpha")).exists());
        QCOMPARE(f.consents, 0);
        QVERIFY(f.cacheFiles().filter("dl-alpha").isEmpty());
    }
    void native_entry_listed_but_install_disabled() {
        Fx f; f.standard(); f.up();
        auto *m = f.open();
        QTRY_COMPARE(m->model()->rowCount(), 3);
        m->select("gamma");
        QCOMPARE(m->model()->entryAt(m->list()->currentIndex().row())->id, QString("gamma"));
        QVERIFY(!m->button("install")->isEnabled());
        QVERIFY2(m->detailLabel()->text().contains("signed releases"), qPrintable(m->detailLabel()->text()));
        f.srv.paths.clear();
        m->startInstall();   // the code path refuses too
        QTest::qWait(150);
        QVERIFY(f.srv.paths.isEmpty());
    }
    void update_available_and_update_reconsents() {
        Fx f; f.standard(); f.up();
        auto *m = f.open();
        QTRY_COMPARE(m->model()->rowCount(), 3);
        m->select("alpha");
        QTest::mouseClick(m->button("install"), Qt::LeftButton);
        QTRY_COMPARE(f.consents, 1);
        QTRY_VERIFY(!m->button("install")->isEnabled());
        // The registry now offers 1.1.0 asking for one more permission.
        f.add("alpha", "1.1.0", {"ui", "clipboard"}, {}, {"text"});
        f.publish();
        QTest::mouseClick(m->button("refresh"), Qt::LeftButton);
        QTRY_VERIFY(m->model()->rowCount() == 3);
        m->select("alpha");
        QTRY_COMPARE(m->model()->stateAt(m->list()->currentIndex().row()), MarketState::UpdateAvailable);
        QCOMPARE(m->button("install")->text(), QString("Update"));
        QVERIFY(m->button("install")->isEnabled());
        QSignalSpy installed(m, &MarketPage::installed);
        QTest::mouseClick(m->button("install"), Qt::LeftButton);
        QTRY_COMPARE(installed.count(), 1);
        QCOMPARE(f.consents, 2);
        QCOMPARE(f.seen.newPermissions, QStringList{"clipboard"});
        QVERIFY(!f.seen.reconsentReason.isEmpty());
        QCOMPARE(f.confirms, 0);   // no "Replace plugin?" question
        QCOMPARE(f.c->plugins().managerIfActive()->info("alpha").manifest.version, QString("1.1.0"));
        QCOMPARE(f.c->plugins().sourceOf("alpha"), "marketplace: " + f.entries.last().url);
    }
    void installed_newer_than_index_offers_nothing() {
        Fx f; f.standard();
        // alpha 2.0.0 is installed by hand, the registry only has 1.0.0
        QString err;
        const QString file = f.lib.dir.filePath("alpha2.hnplugin");
        const QByteArray two = f.pack("alpha", "2.0.0", {"ui"});
        QFile out(file); QVERIFY(out.open(QIODevice::WriteOnly)); out.write(two); out.close();
        f.up();
        QVERIFY(f.c->plugins().install(file, nullptr, nullptr).ok);
        auto *m = f.open();
        QTRY_COMPARE(m->model()->rowCount(), 3);
        m->select("alpha");
        QCOMPARE(m->model()->stateAt(m->list()->currentIndex().row()), MarketState::InstalledNewer);
        QVERIFY(!m->button("install")->isEnabled());
    }
    void double_click_install_starts_one_download() {
        Fx f; f.standard(); f.up();
        auto *m = f.open();
        QTRY_COMPARE(m->model()->rowCount(), 3);
        m->select("alpha");
        QSignalSpy installed(m, &MarketPage::installed);
        QTest::mouseClick(m->button("install"), Qt::LeftButton);
        QTest::mouseClick(m->button("install"), Qt::LeftButton);
        m->startInstall();   // and the code path itself is guarded
        QTRY_COMPARE(installed.count(), 1);
        QCOMPARE(f.srv.paths.count("/alpha-1.0.0.hnplugin"), 1);
        QCOMPARE(f.consents, 1);
    }
    void keyboard_navigation() {
        Fx f; f.standard(); f.up();
        auto *m = f.open();
        QTRY_COMPARE(m->model()->rowCount(), 3);
        m->list()->setFocus();
        m->list()->setCurrentIndex(m->model()->index(0));
        QCOMPARE(m->detailLabel()->text().left(5), QString("alpha"));
        QTest::keyClick(m->list(), Qt::Key_Down);
        QCOMPARE(m->list()->currentIndex().row(), 1);
        QVERIFY(m->detailLabel()->text().startsWith("beta"));
        QTest::keyClick(m->list(), Qt::Key_PageDown);
        QCOMPARE(m->list()->currentIndex().row(), 2);
        QVERIFY(m->detailLabel()->text().startsWith("gamma"));
        QTest::keyClick(m->list(), Qt::Key_Up);
        QTest::keyClick(m->list(), Qt::Key_Up);
        QTest::keyClick(m->list(), Qt::Key_Return);   // activates: focus moves to the enabled Install button
        QTRY_VERIFY(m->button("install")->hasFocus());
        m->list()->setFocus();
        QTest::keyClick(m->list(), Qt::Key_Slash);
        QVERIFY(m->search()->hasFocus());
        QVERIFY(m->search()->text().isEmpty());   // the slash is not typed
    }
    void layout_at_520x460_and_32px_font() {
        Fx f; f.standard(); f.up();
        f.page->resize(520, 460);
        auto *m = f.open();
        QTRY_COMPARE(m->model()->rowCount(), 3);
        auto *outer = f.page->findChild<QScrollArea *>("hnPluginsScroll");
        QVERIFY(outer);
        for (int px : {8, 18, 32}) {
            auto s = f.c->settings(); s.fontSize = px;
            auto p = f.c->prefs(); p.fontSize = px;
            f.c->applySettings(s, p);
            QTRY_COMPARE(m->search()->font().pixelSize(), px);
            QTRY_COMPARE(f.page->size(), QSize(520, 460));
            m->select("alpha");
            QTRY_COMPARE(outer->horizontalScrollBar()->maximum(), 0);
            for (auto *w : std::initializer_list<QWidget *>{m->list(), m->button("install"), m->button("refresh"), m->search()}) {
                for (QWidget *p2 = w->parentWidget(); p2; p2 = p2->parentWidget())
                    if (auto *sa = qobject_cast<QScrollArea *>(p2)) sa->ensureWidgetVisible(w, 0, 0);
                QTRY_VERIFY2(f.page->rect().contains(inWidget(w, f.page.get()).center()), qPrintable(QString("px=%1 %2").arg(px).arg(w->objectName())));
                QTRY_VERIFY(inWidget(w, f.page.get()).right() <= f.page->width());
            }
            QVERIFY(m->button("install")->height() >= m->button("install")->fontMetrics().height() + 8);
            // Every row: chips fit the content width and never overlap the head or the state label; the real list agrees on height.
            const int vw = m->list()->viewport()->width();
            for (int r = 0; r < m->model()->rowCount(); ++r) {
                const auto g = MarketDelegate::geometry(*m->model()->entryAt(r), m->model()->stateAt(r), vw);
                QVERIFY(!g.chips.isEmpty());
                for (const QRect &c : g.chips) {
                    QVERIFY2(c.left() >= MarketDelegate::kMargin + 4 && c.right() <= MarketDelegate::kMargin + 4 + MarketDelegate::contentWidth(vw),
                             qPrintable(QString("px=%1 row=%2 chip=%3..%4 vw=%5").arg(px).arg(r).arg(c.left()).arg(c.right()).arg(vw)));
                    QVERIFY(!c.intersects(g.state));
                    QVERIFY(!c.intersects(g.head));
                    QVERIFY(c.top() > g.head.bottom());
                }
                QVERIFY(g.state.isNull() || !g.state.intersects(g.head));
                QVERIFY(g.height >= g.chips.last().bottom());
                QTRY_COMPARE(m->list()->visualRect(m->model()->index(r)).height(), g.height);
            }
        }
        QVERIFY(!m->list()->viewport()->grab().isNull());
    }
    void narrow_list_rows_agree_with_painted_chips() {
        // A real QListView narrower than the 320 px fallback: the delegate must be handed the viewport width, otherwise the row height
        // (chip lines) and the painted chip wrapping would disagree.
        struct Probe : MarketDelegate {
            using MarketDelegate::MarketDelegate;
            QSize sizeHint(const QStyleOptionViewItem &o, const QModelIndex &i) const override { widths << o.rect.width(); return MarketDelegate::sizeHint(o, i); }
            mutable QList<int> widths;
        };
        auto base = hn::app::ui::theme(); base.baseSize = 14; hn::app::ui::setTheme(base);   // earlier tests leave a large font behind
        MarketModel model;
        QList<MarketEntry> es;
        MarketEntry e = markettest::entryFor("many", "1.0.0", "x", "http://127.0.0.1/x");
        e.permissions = {"ui", "network", "clipboard", "storage"};
        e.tags = {"alpha", "beta"};
        es << e;
        model.setEntries(es, {});
        QListView list;
        auto *d = new Probe(&list);
        list.setModel(&model);
        list.setItemDelegate(d);
        list.setResizeMode(QListView::Adjust);
        list.setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        list.resize(250, 400);
        list.show();
        QTRY_VERIFY(list.viewport()->width() > 0 && list.viewport()->width() < 320);
        list.doItemsLayout();
        const int vw = list.viewport()->width();
        const auto g = MarketDelegate::geometry(e, MarketState::NotInstalled, vw);
        QVERIFY2(MarketDelegate::geometry(e, MarketState::NotInstalled, 320).height != g.height, "precondition: 320 and the viewport wrap differently");
        QVERIFY(!d->widths.isEmpty());
        QVERIFY(d->widths.contains(0));   // QListView does call sizeHint with an empty option.rect (observed), besides the layout pass with the viewport width
        QStyleOptionViewItem empty;
        empty.widget = &list;
        QCOMPARE(d->sizeHint(empty, model.index(0)).height(), g.height);   // so the delegate must read the viewport itself
        QCOMPARE(list.visualRect(model.index(0)).height(), g.height);
        QVERIFY(g.chips.last().top() > g.chips.first().top());   // wrapped at this width
        for (const QRect &c : g.chips) QVERIFY(c.right() <= MarketDelegate::kMargin + 4 + MarketDelegate::contentWidth(vw));
    }
    void download_failure_cleans_up_and_page_stays_usable() {
        Fx f; f.standard();
        f.srv.routes["/alpha-1.0.0.hnplugin"].status = 404;
        f.up();
        auto *m = f.open();
        QTRY_COMPARE(m->model()->rowCount(), 3);
        m->select("alpha");
        QTest::mouseClick(m->button("install"), Qt::LeftButton);
        QTRY_VERIFY2(m->statusLabel()->text().contains("404"), qPrintable(m->statusLabel()->text()));
        QVERIFY(f.cacheFiles().filter("dl-").isEmpty());
        QVERIFY(!QFileInfo(f.lib.dir.filePath("plugins/alpha")).exists());
        QCOMPARE(f.consents, 0);
        QTRY_VERIFY(m->button("install")->isEnabled());
    }
    void page_destroyed_mid_download_leaves_nothing() {
        Fx f; f.standard();
        f.srv.routes["/alpha-1.0.0.hnplugin"].hang = true;
        f.up();
        QPointer<MarketPage> m = f.open();
        QTRY_COMPARE(m->model()->rowCount(), 3);
        m->select("alpha");
        QTest::mouseClick(m->button("install"), Qt::LeftButton);
        QTRY_VERIFY(f.srv.paths.contains("/alpha-1.0.0.hnplugin"));
        f.page.reset();   // the page (and its callback target) goes while the download hangs
        QVERIFY(!m);
        for (auto *s : f.srv.findChildren<QTcpSocket *>()) s->abort();   // the connection dies
        f.srv.close();
        QTRY_VERIFY(!f.c->market().busy());
        QTest::qWait(100);
        QVERIFY(f.cacheFiles().filter("dl-").isEmpty());
        QVERIFY(!QFileInfo(f.lib.dir.filePath("plugins/alpha")).exists());
        QCOMPARE(f.consents, 0);
    }
    void start_install_refuses_installed_newer() {
        Fx f; f.standard();
        const QString file = f.lib.dir.filePath("alpha2.hnplugin");
        QFile out(file); QVERIFY(out.open(QIODevice::WriteOnly)); out.write(f.pack("alpha", "2.0.0", {"ui"})); out.close();
        f.up();
        QVERIFY(f.c->plugins().install(file, nullptr, nullptr).ok);
        auto *m = f.open();
        QTRY_COMPARE(m->model()->rowCount(), 3);
        m->select("alpha");
        QCOMPARE(m->model()->stateAt(m->list()->currentIndex().row()), MarketState::InstalledNewer);
        f.srv.paths.clear();
        m->startInstall();   // a direct call must not downgrade
        QTest::qWait(150);
        QVERIFY(f.srv.paths.isEmpty());
        QCOMPARE(f.c->plugins().managerIfActive()->info("alpha").manifest.version, QString("2.0.0"));
    }
    void unwritable_ack_marker_stays_on_notice_and_fetches_nothing() {
        Fx f; f.standard();
        QDir().mkpath(f.lib.state + "/market-notice-ack");   // a directory where the marker file should go
        f.up();
        auto *m = f.browse();
        QTest::mouseClick(m->button("continue"), Qt::LeftButton);
        QVERIFY(m->notice()->isVisible());
        QVERIFY(m->notice()->findChild<QLabel *>("hnMarketNoticeStatus")->text().contains("Could not save your choice"));
        QTest::qWait(100);
        QVERIFY(f.srv.paths.isEmpty());
        QVERIFY(!f.c->marketCreated());
        QVERIFY(!f.c->marketNoticeAcknowledged());
    }
    void live_theme_restyle() {
        Fx f; f.standard(); f.up();
        auto *m = f.open();
        QTRY_COMPARE(m->model()->rowCount(), 3);
        m->list()->clearSelection();
        m->list()->setCurrentIndex(QModelIndex());
        auto rowBg = [&] {
            QTest::qWait(30);
            const QImage img = m->list()->viewport()->grab().toImage();
            return img.pixelColor(img.width() - 4, 4);
        };
        auto scheme = [&](const QString &name) {
            auto s = f.c->settings(); s.colorScheme = name;
            f.c->applySettings(s, f.c->prefs());
        };
        scheme("dark");
        QTRY_VERIFY(hn::app::ui::theme().dark);
        const QColor dark = rowBg();
        QCOMPARE(dark.name(), hn::app::ui::theme().bg.name());
        scheme("light");
        QTRY_VERIFY(!hn::app::ui::theme().dark);
        QTRY_COMPARE(rowBg().name(), hn::app::ui::theme().bg.name());
        QVERIFY(rowBg().name() != dark.name());
    }
    void closing_browse_leaves_no_timers_or_requests() {
        Fx f; f.standard(); f.up();
        auto activeTimers = [&] { int n = 0; for (auto *t : f.page->findChildren<QTimer *>()) n += t->isActive(); return n; };
        const int base = activeTimers();
        auto *m = f.open();
        QTRY_COMPARE(m->model()->rowCount(), 3);
        QTRY_VERIFY(!f.c->market().busy());
        f.page->showInstalled();
        QTest::qWait(200);
        QCOMPARE(activeTimers(), base);
        QVERIFY(!f.c->market().busy());
        QCOMPARE(f.srv.paths.size(), 1);   // and it never asked again
        for (auto *nam : f.c->market().findChildren<QNetworkAccessManager *>()) QVERIFY(nam->findChildren<QNetworkReply *>().isEmpty());
    }
    void stress_500_entries_scroll_and_close() {
        if (qEnvironmentVariableIsEmpty("HN_STRESS")) QSKIP("set HN_STRESS=1 to run");
        Fx f;
        const QByteArray pkg = f.pack("p0", "1.0.0", {"ui"});
        for (int i = 0; i < 500; ++i) {
            MarketEntry e = markettest::entryFor(QString("plugin-%1").arg(i), "1.0.0", pkg, f.srv.url("/p.hnplugin"));
            e.permissions = {"ui"}; e.tags = {"t" + QString::number(i % 7)};
            e.description = "A small generated plugin used to exercise the list with many rows and long descriptions.";
            f.entries << e;
        }
        f.publish();
        f.up();
        auto *m = f.open();
        QTRY_COMPARE_WITH_TIMEOUT(m->model()->rowCount(), 500, 10000);
        m->list()->setFocus();
        m->list()->setCurrentIndex(m->model()->index(0));
        for (int i = 0; i < 200; ++i) QTest::keyClick(m->list(), Qt::Key_Down);
        QCOMPARE(m->list()->currentIndex().row(), 200);
        f.page->showInstalled();
        QTest::qWait(3000);
        int active = 0;
        for (auto *t : f.page->findChildren<QTimer *>()) active += t->isActive();
        QCOMPARE(active, 0);
        QVERIFY(!f.c->market().busy());
    }
};

QTEST_MAIN(MarketUiTest)
#include "app_market_test.moc"
