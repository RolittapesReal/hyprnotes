// Float-without-rules fallback: with no compositor rule a new sticky maps tiled; the app floats + sizes it once
// (idempotent, verified from the next query, no polling). The organizer is only forced when windows.organizerFloating.
#include "app_test_util.h"
#include "platform_fake_hyprland.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

using namespace apptest;
using hn::platform::Action;

class FloatTest : public QObject {
    Q_OBJECT
    struct Compositor {
        FakeHyprland fake;
        QString stickyTok, orgTok;
        bool stickyFloating = false, orgFloating = false, ignoreFloat = false, refuseMoves = false;
        QPoint orgAt{0, 0};
        QSize orgSize{100, 100};
        Compositor() {
            fake.handler = [this](const QByteArray &cmd) -> QByteArray {
                if (cmd.contains("j/clients")) {
                    QJsonArray a;
                    auto add = [&](const QString &tok, const QString &addr, bool fl) {
                        if (tok.isEmpty()) return;
                        const QPoint at = addr == "0xbbb" ? orgAt : QPoint(0, 0);
                        const QSize size = addr == "0xbbb" ? orgSize : QSize(100, 100);
                        a.append(QJsonObject{{"address", addr}, {"mapped", true}, {"hidden", false}, {"at", QJsonArray{at.x(), at.y()}}, {"size", QJsonArray{size.width(), size.height()}},
                            {"workspace", QJsonObject{{"id", 2}, {"name", "2"}}}, {"floating", fl}, {"monitor", 0}, {"class", "hyprnotes"},
                            {"initialClass", "hyprnotes"}, {"title", "x"}, {"initialTitle", tok}, {"pid", double(QCoreApplication::applicationPid())},
                            {"xwayland", false}, {"pinned", false}});
                    };
                    add(stickyTok.isEmpty() ? QString() : "hyprnotes-sticky:" + stickyTok, "0xaaa", stickyFloating);
                    add(orgTok.isEmpty() ? QString() : "hyprnotes-organizer:" + orgTok, "0xbbb", orgFloating);
                    return QJsonDocument(a).toJson(QJsonDocument::Compact);
                }
                if (refuseMoves && (cmd.contains("window.move") || cmd.contains("window.resize"))) return "refused";
                if (cmd.contains("j/monitors")) return kMonitorsJson;
                if (cmd.contains("j/workspaces")) return R"([{"id":2,"name":"2","monitorID":0,"windows":1}])";
                if (cmd.contains("window.float") && cmd.contains("action='on'") && !ignoreFloat) {
                    if (cmd.contains("0xaaa")) stickyFloating = true;
                    if (cmd.contains("0xbbb")) orgFloating = true;
                }
                return "ok";
            };
        }
        int count(const QString &needle) const { return fake.received.filter(needle).size(); }
    };

private slots:
    void tiled_sticky_is_floated_and_sized_once() {
        Lib l; l.write("a.md", "# A\n");
        Compositor c;
        auto o = l.opts(); o.useHyprland = true; o.hyprPaths = c.fake.paths();
        AppController ctl(o);
        QTRY_VERIFY_WITH_TIMEOUT(ctl.ipc()->connected(), 5000);
        QVERIFY(ctl.openSticky("a.md"));
        c.stickyTok = ctl.stickyOf("a.md")->token();
        c.fake.emitEvent("openwindow>>aaa,2,hyprnotes,x\n");
        QTRY_VERIFY_WITH_TIMEOUT(c.count("window.float") >= 1 && c.count("exact=true") >= 1, 8000);
        const QString all = c.fake.received.join("\n");
        QVERIFY2(all.contains("0xaaa") && all.contains("x=360,y=300,exact=true"), qPrintable(all));
        QVERIFY(all.indexOf("window.float") < all.indexOf("window.resize"));   // float lands before size
        // verification pass sees it floating; further events must not re-send anything
        QTest::qWait(300);
        c.fake.emitEvent("movewindowv2>>aaa,2,2\n");
        QTest::qWait(400);
        QCOMPARE(c.count("window.float"), 1);
        QCOMPARE(c.count("window.resize"), 1);
        QVERIFY(c.count("0xbbb") == 0);   // organizer untouched
    }
    void user_retile_after_float_is_not_fought() {
        Lib l; l.write("a.md", "# A\n");
        Compositor c;
        auto o = l.opts(); o.useHyprland = true; o.hyprPaths = c.fake.paths();
        AppController ctl(o);
        QTRY_VERIFY_WITH_TIMEOUT(ctl.ipc()->connected(), 5000);
        ctl.openSticky("a.md");
        c.stickyTok = ctl.stickyOf("a.md")->token();
        c.fake.emitEvent("openwindow>>aaa,2,hyprnotes,x\n");
        QTRY_VERIFY_WITH_TIMEOUT(c.count("window.resize") >= 1, 8000);
        QTest::qWait(300);
        c.stickyFloating = false;   // user tiles it on purpose
        c.fake.emitEvent("changefloatingmode>>aaa,0\n");
        QTest::qWait(500);
        QCOMPARE(c.count("window.float"), 1);
    }
    void unapplied_float_is_retried_once_then_gives_up() {
        Lib l; l.write("a.md", "# A\n");
        Compositor c;
        c.ignoreFloat = true;   // compositor acks but never floats
        auto o = l.opts(); o.useHyprland = true; o.hyprPaths = c.fake.paths();
        AppController ctl(o);
        QTRY_VERIFY_WITH_TIMEOUT(ctl.ipc()->connected(), 5000);
        ctl.openSticky("a.md");
        c.stickyTok = ctl.stickyOf("a.md")->token();
        c.fake.emitEvent("openwindow>>aaa,2,hyprnotes,x\n");
        QTRY_VERIFY_WITH_TIMEOUT(c.count("window.float") >= 2, 8000);
        QTest::qWait(600);
        QCOMPARE(c.count("window.float"), 2);
    }
    void organizer_floated_only_when_configured() {
        for (bool want : {false, true}) {
            Lib l; l.write("a.md", "# A\n");
            QDir().mkpath(QFileInfo(l.cfg).absolutePath());
            QFile cf(l.cfg);
            QVERIFY(cf.open(QIODevice::WriteOnly));
            cf.write(want ? R"({"version":1,"windows":{"organizerFloating":true}})" : R"({"version":1})");
            cf.close();
            Compositor c;
            auto o = l.opts(); o.useHyprland = true; o.hyprPaths = c.fake.paths();
            AppController ctl(o);
            QTRY_VERIFY_WITH_TIMEOUT(ctl.ipc()->connected(), 5000);
            QTRY_VERIFY_WITH_TIMEOUT(!c.fake.evClients.isEmpty(), 3000);
            QCOMPARE(ctl.settings().organizerFloating, want);
            QVERIFY(ctl.handleAction(Action::ShowOrganizer));
            QVERIFY(ctl.organizer());
            c.orgTok = ctl.organizer()->token();
            c.fake.emitEvent("openwindow>>bbb,2,hyprnotes,x\n");
            if (want) {
                QTRY_VERIFY2_WITH_TIMEOUT(c.count("0xbbb") >= 3, qPrintable(c.fake.received.join("\n")), 8000);   // float, size, center
                QVERIFY(c.count("window.move") >= 1);
            } else {
                QTest::qWait(500);
                QCOMPARE(c.count("0xbbb"), 0);
            }
        }
    }
    void ruleFloatedOrganizerIsCenteredInLogicalWorkArea() {
        Lib l;
        Compositor c;
        c.orgFloating = true;
        c.orgAt = QPoint(503, 195);
        c.orgSize = QSize(960, 640);
        auto options = l.opts(); options.useHyprland = true; options.hyprPaths = c.fake.paths();
        AppController ctl(options);
        QTRY_VERIFY_WITH_TIMEOUT(ctl.ipc()->connected(), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!c.fake.evClients.isEmpty(), 3000);
        QVERIFY(!ctl.settings().organizerFloating);
        QVERIFY(ctl.handleAction(Action::ShowOrganizer));
        c.orgTok = ctl.organizer()->token();
        c.fake.emitEvent("openwindow>>bbb,2,hyprnotes,x\n");
        QTRY_VERIFY2_WITH_TIMEOUT(c.count("window.move") >= 1, qPrintable(c.fake.received.join("\n")), 5000);
        const auto move = c.fake.received.filter("window.move").last();
        QVERIFY2(move.contains("x=480") && move.contains("y=239"), qPrintable(move));
    }
    void userPlacedOrganizerIsNotRecenteredAfterFirstPlacement() {
        Lib l;
        Compositor c;
        c.orgFloating = true;
        c.orgAt = QPoint(503, 195);
        c.orgSize = QSize(960, 640);
        auto options = l.opts(); options.useHyprland = true; options.hyprPaths = c.fake.paths();
        AppController ctl(options);
        QTRY_VERIFY_WITH_TIMEOUT(ctl.ipc()->connected(), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!c.fake.evClients.isEmpty(), 3000);
        QVERIFY(ctl.handleAction(Action::ShowOrganizer));
        c.orgTok = ctl.organizer()->token();
        c.fake.emitEvent("openwindow>>bbb,2,hyprnotes,x\n");
        QTRY_VERIFY_WITH_TIMEOUT(c.count("window.move") >= 1, 5000);
        const int moves = c.count("window.move");
        // the user drags it so it sticks out of the work area but is still partly visible: leave it alone
        c.orgAt = QPoint(-300, 50);
        c.fake.emitEvent("movewindow>>bbb,2\n");
        QTest::qWait(800);
        c.fake.emitEvent("movewindow>>bbb,2\n");
        QTest::qWait(800);
        QCOMPARE(c.count("window.move"), moves);
    }
    void refusedOrganizerPlacementIsRetriedOnlyBoundedly() {
        Lib l;
        Compositor c;
        c.orgFloating = true;
        c.refuseMoves = true;
        c.orgAt = QPoint(6000, 4000);   // outside every work area: a refused move would be retried forever
        
        c.orgSize = QSize(960, 640);
        auto options = l.opts(); options.useHyprland = true; options.hyprPaths = c.fake.paths();
        AppController ctl(options);
        QTRY_VERIFY_WITH_TIMEOUT(ctl.ipc()->connected(), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!c.fake.evClients.isEmpty(), 3000);
        QVERIFY(ctl.handleAction(Action::ShowOrganizer));
        c.orgTok = ctl.organizer()->token();
        c.fake.emitEvent("openwindow>>bbb,2,hyprnotes,x\n");
        QTRY_VERIFY_WITH_TIMEOUT(c.count("window.move") >= 1, 5000);
        QTest::qWait(1500);   // a refusing compositor must not keep the app re-dispatching
        const int moves = c.count("window.move");
        QVERIFY2(moves <= 3, qPrintable(QString::number(moves)));
        QTest::qWait(500);
        QCOMPARE(c.count("window.move"), moves);
    }
};

QTEST_MAIN(FloatTest)
#include "app_float_test.moc"
