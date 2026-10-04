#include "app_test_util.h"
#include "panel_dock.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>

static QString panelFixture(const QString &root, const QString &id, const QByteArray &lua) {
    const QString path = root + "/src-" + id;
    if (!QDir().mkpath(path)) qFatal("Cannot create panel fixture");
    const QJsonObject manifest{{"id", id}, {"name", id}, {"version", "1.0.0"},
        {"author", "tests"}, {"description", "Panel lifecycle fixture"}, {"api", 2},
        {"tier", "script"}, {"entry", "main.lua"}, {"min_app", "0.1.0"},
        {"permissions", QJsonArray{"ui.panel", "notes.read", "note.read", "ui"}}};
    QFile json(path + "/plugin.json"), script(path + "/main.lua");
    if (!json.open(QIODevice::WriteOnly) || !script.open(QIODevice::WriteOnly))
        qFatal("Cannot write panel fixture");
    const QByteArray bytes = QJsonDocument(manifest).toJson();
    if (json.write(bytes) != bytes.size() || script.write(lua) != lua.size())
        qFatal("Short panel fixture write");
    return path;
}

class AppPanelLifecycleTest : public QObject {
    Q_OBJECT
private slots:
    void retiredNoteCancelsRemainingPlugins_data() {
        QTest::addColumn<QString>("dispatch");
        for (const auto *name : {"pre-save", "link", "completion"}) QTest::newRow(name) << QString(name);
    }
    void retiredNoteCancelsRemainingPlugins() {
        QFETCH(QString, dispatch);
        apptest::Lib lib;
        lib.write("A.md", "# A\n"); lib.write("B.md", "# B\n");
        auto options = lib.opts();
        options.pluginHooks.consent = [](const hn::app::ConsentRequest &) { return true; };
        hn::app::AppController controller(options);
        controller.showOrganizer(); auto *origin = controller.openInOrganizer("A.md");
        for (const auto &id : {QString("a-open"), QString("z-obsolete")}) {
            const QString body = id == "a-open" ? "hn.notes.open('B.md')" : "hn.ui.notify('obsolete provider ran')";
            const QByteArray lua = (dispatch == "completion"
                ? "hn.complete{id='c',trigger='[[',items=function() " + body + " return {} end}"
                : "hn.on('" + QString(dispatch == "pre-save" ? "note.pre_save" : "link.activate") +
                    "',function(value) " + body + (dispatch == "pre-save" ? " return value end)" : " return false end)")).toUtf8();
            const auto source = panelFixture(lib.dir.path(), id, lua);
            QFile json(source + "/plugin.json");
            QVERIFY(json.open(QIODevice::ReadOnly));
            auto manifest = QJsonDocument::fromJson(json.readAll()).object(); json.close();
            auto permissions = manifest["permissions"].toArray();
            permissions.append("editor.links"); permissions.append("editor.complete"); manifest["permissions"] = permissions;
            QVERIFY(json.open(QIODevice::WriteOnly | QIODevice::Truncate)); json.write(QJsonDocument(manifest).toJson()); json.close();
            QString message;
            QVERIFY2(controller.plugins().install(source, nullptr, &message).ok, qPrintable(message));
        }
        auto *manager = controller.plugins().manager();
        auto *bridge = controller.plugins().bridgeFor(origin);
        if (dispatch == "pre-save") QCOMPARE(manager->preSave("unchanged", bridge), QString("unchanged"));
        else if (dispatch == "completion") QVERIFY(manager->complete("[[", "", bridge).isEmpty());
        else {
            hn::plugins::LinkActivation link; link.kind = "link"; link.target = "B";
            QVERIFY(!manager->activateLink(link, bridge));
        }
        QCOMPARE(controller.organizerNote(), QString("B.md"));
        QVERIFY(!controller.lastNotice().contains("obsolete provider ran"));
    }

    void enclosingDispatchKeepsDetachedBridgeForRemainingHandlers_data() {
        QTest::addColumn<QString>("dispatch");
        for (const auto *name : {"event", "pre-save", "link"}) QTest::newRow(name) << QString(name);
    }
    void enclosingDispatchKeepsDetachedBridgeForRemainingHandlers() {
        QFETCH(QString, dispatch);
        apptest::Lib lib;
        lib.write("A.md", "# A\n"); lib.write("B.md", "# B\n");
        auto options = lib.opts();
        options.pluginHooks.consent = [](const hn::app::ConsentRequest &) { return true; };
        hn::app::AppController controller(options);
        controller.showOrganizer(); auto *origin = controller.openInOrganizer("A.md");
        const QString event = dispatch == "event" ? "note.saved" : dispatch == "pre-save" ? "note.pre_save" : "link.activate";
        const QByteArray lua = ("hn.on('" + event + "',function(value) hn.notes.open('B.md') " +
            (dispatch == "pre-save" ? "return value" : "return false") + " end)\n" +
            "hn.on('" + event + "',function(value) hn.ui.notify('second title=['..hn.note.title()..']') " +
            (dispatch == "pre-save" ? "return value" : "return true") + " end)").toUtf8();
        const auto source = panelFixture(lib.dir.path(), "multi-handler", lua);
        if (dispatch == "link") {
            QFile json(source + "/plugin.json");
            QVERIFY(json.open(QIODevice::ReadOnly));
            auto manifest = QJsonDocument::fromJson(json.readAll()).object(); json.close();
            auto permissions = manifest["permissions"].toArray(); permissions.append("editor.links"); manifest["permissions"] = permissions;
            QVERIFY(json.open(QIODevice::WriteOnly | QIODevice::Truncate)); json.write(QJsonDocument(manifest).toJson()); json.close();
        }
        QString message;
        QVERIFY2(controller.plugins().install(source, nullptr, &message).ok, qPrintable(message));
        auto *bridge = controller.plugins().bridgeFor(origin);
        if (dispatch == "event") controller.plugins().post(event, origin);
        else if (dispatch == "pre-save") QCOMPARE(controller.plugins().manager()->preSave("unchanged", bridge), QString("unchanged"));
        else {
            hn::plugins::LinkActivation link; link.kind = "link"; link.target = "B";
            QVERIFY(controller.plugins().manager()->activateLink(link, bridge));
        }
        QCOMPARE(controller.organizerNote(), QString("B.md"));
        QCOMPARE(controller.lastNotice(), QString("multi-handler: second title=[]"));
    }

    void incomingEventIsCancelledWhenPreliminaryFlushRetiresItsNote() {
        apptest::Lib lib;
        lib.write("A.md", "# A\n"); lib.write("B.md", "# B\n");
        auto options = lib.opts();
        options.pluginHooks.consent = [](const hn::app::ConsentRequest &) { return true; };
        hn::app::AppController controller(options);
        controller.showOrganizer(); auto *origin = controller.openInOrganizer("A.md");
        const auto source = panelFixture(lib.dir.path(), "incoming-event", R"LUA(
hn.on('note.changed',function() hn.notes.open('B.md') end)
hn.on('note.saved',function() hn.ui.notify('saved received') end)
)LUA");
        QString message;
        QVERIFY2(controller.plugins().install(source, nullptr, &message).ok, qPrintable(message));
        controller.plugins().post("note.changed", origin);
        controller.plugins().post("note.saved", origin);
        QCOMPARE(controller.organizerNote(), QString("B.md"));
        QVERIFY(!controller.lastNotice().contains("saved received"));
    }

    void activationOwnsItsPathAcrossSynchronousContentReplacement() {
        hn::app::PanelView view;
        hn::plugins::PanelBlock row;
        row.type = "item"; row.title = "B"; row.path = "B.md"; row.click = 1;
        view.setContent({row}, {});
        connect(&view, &hn::app::PanelView::activated, &view, [&] {
            hn::plugins::PanelBlock replacement;
            replacement.type = "item"; replacement.title = "C"; replacement.path = "C.md";
            view.setContent({replacement}, {});
        });
        QString observed;
        connect(&view, &hn::app::PanelView::activated, &view, [&](int, const QString &path, int) { observed = path; });
        view.setCurrentIndex(0);
        view.activateCurrent();
        QCOMPARE(observed, QString("B.md"));
    }

    void callbackGeneratedNoteOpenedCascadeIsBounded() {
        apptest::Lib lib;
        lib.write("A.md", "# A\n"); lib.write("B.md", "# B\n");
        auto options = lib.opts();
        options.pluginHooks.consent = [](const hn::app::ConsentRequest &) { return true; };
        hn::app::AppController controller(options);
        controller.showOrganizer(); controller.openInOrganizer("A.md");
        const QString source = panelFixture(lib.dir.path(), "ping-pong", R"LUA(
hn.on('note.opened', function()
  hn.notes.open(hn.note.path() == 'A.md' and 'B.md' or 'A.md')
end)
hn.panel{id='p',title='Ping',render=function()
  return {{type='item',title='Go',on_click=function() hn.notes.open('B.md') end}}
end}
)LUA");
        QString message;
        QVERIFY2(controller.plugins().install(source, nullptr, &message).ok, qPrintable(message));
        auto *dock = controller.organizer()->dock();
        dock->refresh();
        dock->view()->setCurrentIndex(0); dock->view()->activateCurrent();
        // The cascade must end: the event loop regains control and the queue drains.
        QTest::qWait(200);
        QVERIFY(!controller.organizerNote().isEmpty());
    }

    void coldLoadMainChunkNavigationKeepsIncomingBridge() {
        apptest::Lib lib;
        lib.write("A.md", "# A\n"); lib.write("B.md", "# B\n");
        auto options = lib.opts();
        options.pluginHooks.consent = [](const hn::app::ConsentRequest &) { return true; };
        hn::app::AppController controller(options);
        controller.showOrganizer(); controller.openInOrganizer("A.md");
        const QString source = panelFixture(lib.dir.path(), "cold-nav", R"LUA(
hn.notes.open('B.md')
hn.panel{id='p',title='Cold',render=function()
  return {{type='item',title='t=['..hn.note.title()..']'}}
end}
)LUA");
        QString message;
        QVERIFY2(controller.plugins().install(source, nullptr, &message).ok, qPrintable(message));
        controller.organizer()->dock()->refresh();
        QTest::qWait(100);
        QVERIFY(!controller.organizerNote().isEmpty());
    }

    void realRenderErrorsRemainVisible() {
        apptest::Lib lib;
        auto options = lib.opts();
        options.pluginHooks.consent = [](const hn::app::ConsentRequest &) { return true; };
        hn::app::AppController controller(options);
        controller.showOrganizer();
        const QString source = panelFixture(lib.dir.path(), "broken-render", R"LUA(
hn.panel{id='p',title='Broken render',render=function() error('render sentinel') end}
)LUA");
        QString message;
        controller.plugins().install(source, nullptr, &message);
        auto *hub = controller.plugins().hub();
        QVERIFY(hub);
        QTRY_VERIFY((hub->find("broken-render:p") && hub->find("broken-render:p")->error.contains("render sentinel")) ||
            controller.lastNotice().contains("render sentinel"));
    }

    void realClickErrorsRemainVisibleAndOpenNothing() {
        apptest::Lib lib;
        lib.write("A.md", "# A\n");
        auto options = lib.opts();
        options.pluginHooks.consent = [](const hn::app::ConsentRequest &) { return true; };
        hn::app::AppController controller(options);
        controller.showOrganizer(); controller.openInOrganizer("A.md");
        const QString source = panelFixture(lib.dir.path(), "broken-click", R"LUA(
hn.panel{id='p',title='Broken click',render=function()
  return {{type='item',title='Fail now',on_click=function() error('click sentinel') end}}
end}
)LUA");
        QString message;
        QVERIFY2(controller.plugins().install(source, nullptr, &message).ok, qPrintable(message));
        auto *view = controller.organizer()->dock()->view();
        QTRY_COMPARE(view->actionableCount(), 1);
        QSignalSpy opened(&controller, &hn::app::AppController::noteOpened);
        view->setCurrentIndex(0); view->activateCurrent();
        QTRY_VERIFY(controller.lastNotice().contains("click sentinel"));
        QCOMPARE(opened.count(), 0);
        QCOMPARE(controller.organizerNote(), QString("A.md"));
        QVERIFY(controller.plugins().auditText().contains("click sentinel"));
    }

    void callbackCanReadDetachedOriginAfterOpeningDestination() {
        apptest::Lib lib;
        lib.write("A.md", "# A\n"); lib.write("B.md", "# B\n");
        auto options = lib.opts();
        options.pluginHooks.consent = [](const hn::app::ConsentRequest &) { return true; };
        hn::app::AppController controller(options);
        controller.showOrganizer(); controller.openInOrganizer("A.md");
        const QString source = panelFixture(lib.dir.path(), "read-after-open", R"LUA(
hn.panel{id='p',title='Read after open',render=function()
  return {{type='item',title='Open B',on_click=function()
    hn.notes.open('B.md')
    hn.ui.notify('origin title=['..hn.note.title()..']')
  end}}
end}
)LUA");
        QString message;
        QVERIFY2(controller.plugins().install(source, nullptr, &message).ok, qPrintable(message));
        auto *dock = controller.organizer()->dock();
        dock->refresh(); // Isolate bridge retirement from first-publication invalidation.
        dock->view()->setCurrentIndex(0); dock->view()->activateCurrent();
        QCOMPARE(controller.organizerNote(), QString("B.md"));
        QCOMPARE(controller.lastNotice(), QString("read-after-open: origin title=[]"));
    }

    void nestedCallbacksKeepDetachedBridgeUntilFinalHolderReturns() {
        apptest::Lib lib;
        lib.write("A.md", "# A\n"); lib.write("B.md", "# B\n");
        auto options = lib.opts();
        options.pluginHooks.consent = [](const hn::app::ConsentRequest &) { return true; };
        hn::app::AppController *active = nullptr;
        hn::app::NoteSession *origin = nullptr;
        bool innerSucceeded = false;
        QString innerError;
        options.pluginHooks.prompt = [&](const QString &, const QString &, const QString &, const QString &) -> std::optional<QString> {
            innerSucceeded = active->plugins().runQualified("nested-inner:go", origin, &innerError);
            return QString();
        };
        hn::app::AppController controller(options);
        active = &controller;
        controller.showOrganizer(); origin = controller.openInOrganizer("A.md");
        QString message;
        const auto inner = panelFixture(lib.dir.path(), "nested-inner", R"LUA(
hn.command{id='go',title='Go',run=function()
  hn.notes.open('B.md')
  hn.ui.notify('inner title=['..hn.note.title()..']')
end}
)LUA");
        QVERIFY2(controller.plugins().install(inner, nullptr, &message).ok, qPrintable(message));
        const auto outer = panelFixture(lib.dir.path(), "nested-outer", R"LUA(
hn.panel{id='p',title='Nested',render=function()
  return {{type='item',title='Open B',on_click=function()
    hn.ui.prompt('Nested','Run','')
    hn.ui.notify('outer title=['..hn.note.title()..']')
  end}}
end}
)LUA");
        QVERIFY2(controller.plugins().install(outer, nullptr, &message).ok, qPrintable(message));
        auto *view = controller.organizer()->dock()->view();
        view->setCurrentIndex(0); view->activateCurrent();
        QVERIFY2(innerSucceeded, qPrintable(innerError));
        QCOMPARE(controller.organizerNote(), QString("B.md"));
        QCOMPARE(controller.lastNotice(), QString("nested-outer: outer title=[]"));
    }

    void backlinksRepeatedMouseAndKeyboardNavigation_data() {
        QTest::addColumn<bool>("cached");
        QTest::newRow("fresh-install") << false;
        QTest::newRow("cached-restart") << true;
    }
    void backlinksRepeatedMouseAndKeyboardNavigation() {
        QFETCH(bool, cached);
        apptest::Lib lib;
        lib.write("A.md", "# A\n\nSee [[B]].\n"); lib.write("B.md", "# B\n\nSee [[A]].\n");
        auto options = lib.opts();
        options.pluginHooks.consent = [](const hn::app::ConsentRequest &) { return true; };
        QString message;
        if (cached) {
            hn::app::AppController first(options);
            QVERIFY2(first.plugins().install(QString(HN_SOURCE_DIR) + "/examples/plugins/backlinks-panel", nullptr, &message).ok, qPrintable(message));
        }
        hn::app::AppController controller(options);
        controller.start(hn::platform::Action::Background);
        controller.showOrganizer(); controller.openInOrganizer("A.md");
        QTRY_COMPARE(controller.index()->count(), 2);
        if (!cached) QVERIFY2(controller.plugins().install(QString(HN_SOURCE_DIR) + "/examples/plugins/backlinks-panel", nullptr, &message).ok, qPrintable(message));
        auto *hub = controller.plugins().hub();
        QVERIFY(hub);
        QVERIFY(controller.organizer()->dock());
        const QString qid("backlinks-panel:backlinks");
        QStringList errors;
        QObject collector;
        connect(hub, &hn::app::PanelHub::panelUpdated, &collector, [&](const QString &id) {
            if (id == qid && hub->find(id) && !hub->find(id)->error.isEmpty()) errors << hub->find(id)->error;
        });
        auto *view = controller.organizer()->dock()->view();
        QSignalSpy opened(&controller, &hn::app::AppController::noteOpened);
        for (int i = 0; i < 6; ++i) {
            const QString target = i % 2 ? "A" : "B";
            QTRY_COMPARE(view->rowText(1), target);
            if (i % 2) { view->setCurrentIndex(0); QTest::keyClick(view, Qt::Key_Return); }
            else QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, view->rowRect(1).center());
            QTRY_COMPARE(controller.organizerNote(), target + ".md");
            QCOMPARE(opened.count(), i + 1);
            QVERIFY2(errors.isEmpty(), qPrintable(errors.join('\n')));
            QVERIFY(!controller.lastNotice().contains("stale", Qt::CaseInsensitive));
        }
        QCOMPARE(controller.plugins().manager()->info("backlinks-panel").status, hn::plugins::Status::Enabled);
    }

    void sharedVisibleHostsSurviveOneHostClosing() {
        apptest::Lib lib;
        lib.write("A.md", "# A\n\nSee [[B]].\n"); lib.write("B.md", "# B\n\nSee [[A]].\n");
        auto options = lib.opts();
        options.pluginHooks.consent = [](const hn::app::ConsentRequest &) { return true; };
        hn::app::AppController controller(options);
        controller.showOrganizer(); auto *a = controller.openInOrganizer("A.md");
        QTRY_COMPARE(controller.index()->count(), 2);
        QString message;
        QVERIFY2(controller.plugins().install(QString(HN_SOURCE_DIR) + "/examples/plugins/backlinks-panel", nullptr, &message).ok, qPrintable(message));
        auto second = std::make_unique<hn::app::PanelDock>(&controller);
        second->setSession(a); second->resize(320, 360); second->show();
        auto *first = controller.organizer()->dock();
        QTRY_COMPARE(second->view()->rowText(1), QString("B"));
        first->view()->setCurrentIndex(0); first->view()->activateCurrent();
        QTRY_COMPARE(controller.organizerNote(), QString("B.md"));
        QTRY_COMPARE(first->view()->rowText(1), QString("A"));
        QTRY_COMPARE(second->view()->rowText(1), QString("A"));
        second.reset();
        first->view()->setCurrentIndex(0); QTest::keyClick(first->view(), Qt::Key_Return);
        QTRY_COMPARE(controller.organizerNote(), QString("A.md"));
        QTRY_COMPARE(first->view()->rowText(1), QString("B"));
        QVERIFY(controller.plugins().hub()->find("backlinks-panel:backlinks")->error.isEmpty());
    }

    void backlinksStickyOriginAndAlreadyOpenDestination_data() {
        QTest::addColumn<bool>("alreadyOpen");
        QTest::newRow("sticky-origin") << false;
        QTest::newRow("existing-sticky-destination") << true;
    }
    void backlinksStickyOriginAndAlreadyOpenDestination() {
        QFETCH(bool, alreadyOpen);
        apptest::Lib lib;
        lib.write("A.md", "# A\n"); lib.write("B.md", "# B\n\nSee [[A]].\n");
        auto options = lib.opts();
        options.pluginHooks.consent = [](const hn::app::ConsentRequest &) { return true; };
        hn::app::AppController controller(options);
        hn::app::NoteSession *existing = nullptr;
        if (alreadyOpen) { existing = controller.openSticky("B.md", false); controller.showOrganizer(); controller.openInOrganizer("A.md"); }
        else controller.openSticky("A.md", false);
        QTRY_COMPARE(controller.index()->count(), 2);
        QString message;
        QVERIFY2(controller.plugins().install(QString(HN_SOURCE_DIR) + "/examples/plugins/backlinks-panel", nullptr, &message).ok, qPrintable(message));
        auto *dock = alreadyOpen ? controller.organizer()->dock() : controller.stickyOf("A.md")->openPanels();
        QVERIFY(dock);
        auto *hub = controller.plugins().hub();
        QStringList errors;
        QObject collector;
        connect(hub, &hn::app::PanelHub::panelUpdated, &collector, [&](const QString &id) {
            if (hub->find(id) && !hub->find(id)->error.isEmpty()) errors << hub->find(id)->error;
        });
        QTRY_COMPARE(dock->view()->rowText(1), QString("B"));
        QSignalSpy opened(&controller, &hn::app::AppController::noteOpened);
        QTest::mouseClick(dock->view()->viewport(), Qt::LeftButton, Qt::NoModifier, dock->view()->rowRect(1).center());
        if (alreadyOpen) { QCOMPARE(controller.session("B.md"), existing); QCOMPARE(opened.count(), 0); }
        else { QTRY_COMPARE(controller.organizerNote(), QString("B.md")); QCOMPARE(opened.count(), 1); }
        QCoreApplication::processEvents();
        QVERIFY2(errors.isEmpty(), qPrintable(errors.join('\n')));
        QVERIFY(!controller.lastNotice().contains("stale", Qt::CaseInsensitive));
        QCOMPARE(controller.plugins().manager()->info("backlinks-panel").status, hn::plugins::Status::Enabled);
    }

    void callbackMayDestroyItsOriginatingDock() {
        apptest::Lib lib;
        lib.write("A.md", "# A\n"); lib.write("B.md", "# B\n");
        auto options = lib.opts();
        options.pluginHooks.consent = [](const hn::app::ConsentRequest &) { return true; };
        QPointer<hn::app::PanelDock> origin;
        options.pluginHooks.prompt = [&](const QString &, const QString &, const QString &, const QString &) -> std::optional<QString> {
            delete origin.data(); return QString();
        };
        hn::app::AppController controller(options);
        auto *a = controller.openSticky("A.md", false);
        const auto source = panelFixture(lib.dir.path(), "destroy-dock", R"LUA(
hn.panel{id='p',title='Destroy dock',refresh_on={'note.opened'},render=function(ctx)
  return {{type='item',title=ctx.path or 'none',on_click=function()
    hn.notes.open('B.md')
    hn.ui.prompt('Close','Close origin','')
  end}}
end}
)LUA");
        QString message;
        QVERIFY2(controller.plugins().install(source, nullptr, &message).ok, qPrintable(message));
        origin = new hn::app::PanelDock(&controller);
        origin->setSession(a); origin->resize(320, 360); origin->show();
        origin->view()->setCurrentIndex(0); origin->view()->activateCurrent();
        QVERIFY(origin.isNull());
        QTRY_COMPARE(controller.organizerNote(), QString("B.md"));
        QTRY_COMPARE(controller.organizer()->dock()->view()->rowText(0), QString("B.md"));
        QVERIFY(controller.plugins().hub()->find("destroy-dock:p")->error.isEmpty());
    }

    void firstPublishedClickSurvivesDiscoveryAndOpensWithoutPanelError() {
        apptest::Lib lib;
        lib.write("A.md", "# A\n");
        lib.write("B.md", "# B\n");
        auto options = lib.opts();
        options.pluginHooks.consent = [](const hn::app::ConsentRequest &) { return true; };
        hn::app::AppController controller(options);
        controller.showOrganizer();
        controller.openInOrganizer("A.md");
        const QString source = panelFixture(lib.dir.path(), "panel-cycle", R"LUA(
    hn.panel{
      id='p', title='Panel cycle', refresh_on={'note.opened'},
      render=function(ctx)
        local target = ctx.path == 'B.md' and 'A.md' or 'B.md'
        return {{type='item', title=target, path=target,
          on_click=function() hn.notes.open(target) end}}
      end
    }
    )LUA");
        QString message;
        QVERIFY2(controller.plugins().install(source, nullptr, &message).ok, qPrintable(message));
        auto *hub = controller.plugins().hub();
        const QString qid("panel-cycle:p");
        QTRY_VERIFY(hub && hub->find(qid) && hub->find(qid)->rendered);
        auto *dock = controller.organizer()->dock();
        QVERIFY(dock);
        QTRY_COMPARE(dock->view()->actionableCount(), 1);
        QStringList errors;
        QObject errorCollector; // Destroy the connection before its captured errors list.
        connect(hub, &hn::app::PanelHub::panelUpdated, &errorCollector, [&](const QString &id) {
            if (id == qid && hub->find(id) && !hub->find(id)->error.isEmpty())
                errors << hub->find(id)->error;
        });
        QSignalSpy opened(&controller, &hn::app::AppController::noteOpened);
        dock->view()->setCurrentIndex(0);
        dock->view()->activateCurrent(); // Do not refresh after install to make this pass.
        QTRY_COMPARE(controller.organizerNote(), QString("B.md"));
        QTRY_VERIFY(hub->find(qid) && hub->find(qid)->error.isEmpty());
        QVERIFY2(errors.isEmpty(), qPrintable(errors.join('\n')));
        QVERIFY(!controller.lastNotice().contains("stale", Qt::CaseInsensitive));
        QCOMPARE(opened.count(), 1);
        QTRY_COMPARE(dock->view()->rowText(0), QString("A.md"));
        dock->view()->setCurrentIndex(0);
        QTest::keyClick(dock->view(), Qt::Key_Return);
        QTRY_COMPARE(controller.organizerNote(), QString("A.md"));
        QTRY_COMPARE(dock->view()->rowText(0), QString("B.md"));
        QTRY_VERIFY(hub->find(qid) && hub->find(qid)->error.isEmpty());
        QVERIFY2(errors.isEmpty(), qPrintable(errors.join('\n')));
    }
    void refreshedClickOpensWithoutReentrantPanelError() {
        apptest::Lib lib;
        lib.write("A.md", "# A\n");
        lib.write("B.md", "# B\n");
        auto options = lib.opts();
        options.pluginHooks.consent = [](const hn::app::ConsentRequest &) { return true; };
        hn::app::AppController controller(options);
        controller.showOrganizer();
        controller.openInOrganizer("A.md");
        const QString source = panelFixture(lib.dir.path(), "panel-cycle", R"LUA(
    hn.panel{
      id='p', title='Panel cycle', refresh_on={'note.opened'},
      render=function(ctx)
        local target = ctx.path == 'B.md' and 'A.md' or 'B.md'
        return {{type='item', title=target, path=target,
          on_click=function() hn.notes.open(target) end}}
      end
    }
    )LUA");
        QString message;
        QVERIFY2(controller.plugins().install(source, nullptr, &message).ok, qPrintable(message));
        auto *hub = controller.plugins().hub();
        const QString qid("panel-cycle:p");
        QTRY_VERIFY(hub && hub->find(qid) && hub->find(qid)->rendered);
        auto *dock = controller.organizer()->dock();
        QVERIFY(dock);
        QTRY_COMPARE(dock->view()->actionableCount(), 1);
        dock->refresh(); // Isolate callback-time reentrancy from discovery invalidation.
        QStringList errors;
        QObject errorCollector; // Destroy the connection before its captured errors list.
        connect(hub, &hn::app::PanelHub::panelUpdated, &errorCollector, [&](const QString &id) {
            if (id == qid && hub->find(id) && !hub->find(id)->error.isEmpty())
                errors << hub->find(id)->error;
        });
        QSignalSpy opened(&controller, &hn::app::AppController::noteOpened);
        dock->view()->setCurrentIndex(0);
        dock->view()->activateCurrent();
        QTRY_COMPARE(controller.organizerNote(), QString("B.md"));
        QTRY_VERIFY(hub->find(qid) && hub->find(qid)->error.isEmpty());
        QVERIFY2(errors.isEmpty(), qPrintable(errors.join('\n')));
        QVERIFY(!controller.lastNotice().contains("stale", Qt::CaseInsensitive));
        QCOMPARE(opened.count(), 1);
        QTRY_COMPARE(dock->view()->rowText(0), QString("A.md"));
        dock->view()->setCurrentIndex(0);
        QTest::keyClick(dock->view(), Qt::Key_Return);
        QTRY_COMPARE(controller.organizerNote(), QString("A.md"));
        QTRY_COMPARE(dock->view()->rowText(0), QString("B.md"));
        QTRY_VERIFY(hub->find(qid) && hub->find(qid)->error.isEmpty());
        QVERIFY2(errors.isEmpty(), qPrintable(errors.join('\n')));
    }
    void backlinksMouseClickOpensOnceAndRefreshesForDestination() {
        apptest::Lib lib;
        lib.write("A.md", "# A\n");
        lib.write("B.md", "# B\n\nSee [[A]].\n");
        auto options = lib.opts();
        options.pluginHooks.consent = [](const hn::app::ConsentRequest &) { return true; };
        hn::app::AppController controller(options);
        controller.showOrganizer();
        controller.openInOrganizer("A.md");
        QTRY_COMPARE_WITH_TIMEOUT(controller.index()->count(), 2, 15000);
        QString message;
        QVERIFY2(controller.plugins().install(QString(HN_SOURCE_DIR) + "/examples/plugins/backlinks-panel", nullptr, &message).ok, qPrintable(message));
        auto *hub = controller.plugins().hub();
        const QString qid("backlinks-panel:backlinks");
        QTRY_VERIFY(hub && hub->find(qid) && hub->find(qid)->rendered);
        auto *dock = controller.organizer()->dock();
        QVERIFY(dock);
        auto *view = dock->view();
        QTRY_COMPARE(view->actionableCount(), 1);
        QTRY_COMPARE(view->rowText(1), QString("B"));
        const auto *panel = hub->find(qid);
        QCOMPARE(panel->blocks.size(), 2);
        QCOMPARE(panel->blocks[1].items.size(), 1);
        QCOMPARE(panel->blocks[1].items[0].path, QString("B.md"));
        QStringList errors;
        QObject errorCollector;
        connect(hub, &hn::app::PanelHub::panelUpdated, &errorCollector, [&](const QString &id) {
            if (id == qid && hub->find(id) && !hub->find(id)->error.isEmpty())
                errors << hub->find(id)->error;
        });
        QSignalSpy opened(&controller, &hn::app::AppController::noteOpened);
        const QRect firstRow = view->rowRect(1);
        QVERIFY(firstRow.isValid());
        QVERIFY(view->viewport()->rect().contains(firstRow.center()));
        QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, firstRow.center());
        QTRY_COMPARE(controller.organizerNote(), QString("B.md"));
        QCOMPARE(opened.count(), 1);
        QCOMPARE(opened.at(0).at(0).toString(), QString("B.md"));
        QTRY_VERIFY(hub->find(qid) && hub->find(qid)->error.isEmpty());
        QVERIFY2(errors.isEmpty(), qPrintable(errors.join('\n')));
        QVERIFY(!controller.lastNotice().contains("stale", Qt::CaseInsensitive));
        QTRY_COMPARE(view->actionableCount(), 0);
        QTRY_COMPARE(view->rowText(0), QString("Nothing links to B yet."));

        // Reopen A through the controller, then exercise the newly published token.
        controller.openInOrganizer("A.md");
        QTRY_COMPARE(view->rowText(1), QString("B"));
        opened.clear();
        QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, view->rowRect(1).center());
        QTRY_COMPARE(controller.organizerNote(), QString("B.md"));
        QCOMPARE(opened.count(), 1);
        QTRY_COMPARE(view->rowText(0), QString("Nothing links to B yet."));
        QTRY_VERIFY(hub->find(qid) && hub->find(qid)->error.isEmpty());
        QVERIFY2(errors.isEmpty(), qPrintable(errors.join('\n')));
        QVERIFY(!controller.lastNotice().contains("stale", Qt::CaseInsensitive));
    }
};

QTEST_MAIN(AppPanelLifecycleTest)
#include "app_panel_lifecycle_test.moc"
