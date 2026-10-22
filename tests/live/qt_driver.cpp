#include "controller.h"
#include "consent_dialog.h"
#include "organizer_window.h"
#include "settings_dialog.h"
#include "signal_bridge.h"
#include "sticky_window.h"
#include <QApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSocketNotifier>
#include <QEventLoop>
#include <QTimer>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QAbstractScrollArea>
#include <QTextEdit>
#include <QPlainTextEdit>
#include <QPointer>
#include <QTextStream>
#include <QTabWidget>
#include <QTest>
#include <QThread>
#include <unistd.h>

using namespace hn::app;
static QPointer<QWidget> g_peer;
static QPointer<ConsentDialog> g_consent;
static QList<QPointer<QWidget>> g_stormWindows;
static int g_stormTotal = 0, g_stormCreated = 0, g_stormClosed = 0;
static QTimer *g_stormTimer = nullptr;
static bool g_stormActive = false, g_stormWaitingForRelease = false, g_stormBatchQueued = false;

static bool waitSaved(NoteSession *session) {
    if (!session) return false;
    if (session->settled()) return true;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(session, &NoteSession::statusChanged, &loop, [&] { if (session->settled()) loop.quit(); });
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    session->flush();
    timeout.start(10000);
    if (!session->settled()) loop.exec();
    return session->settled();
}

static QWidget *surfaceWidget(AppController &controller, const QString &surface, const QString &rel) {
    if (surface == "organizer") return controller.organizer();
    if (surface == "sticky") return controller.stickyOf(rel);
    if (surface == "settings") {
        for (QWidget *widget : QApplication::topLevelWidgets())
            if (qobject_cast<SettingsDialog *>(widget)) return widget;
    }
    if (surface == "consent") return g_consent;
    return nullptr;
}

static void appendWidgetGeometry(QJsonArray &rows, QWidget *parent, int depth = 0, bool scrollableContext = false) {
    if (depth >= 6) return;
    const bool insideScrollArea = scrollableContext || qobject_cast<QAbstractScrollArea *>(parent) != nullptr;
    for (QWidget *child : parent->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly)) {
        if (!child->isVisible() || child->isWindow()) continue;
        const QRect g = child->geometry(), p = parent->rect();
        rows.append(QJsonObject{{"class", QString::fromLatin1(child->metaObject()->className())},
                                {"name", child->objectName()}, {"x", g.x()}, {"y", g.y()},
                                {"width", g.width()}, {"height", g.height()},
                                {"parentWidth", p.width()}, {"parentHeight", p.height()},
                                {"scrollArea", insideScrollArea}});
        appendWidgetGeometry(rows, child, depth + 1, insideScrollArea);
    }
}

static void stormBatch() {
    if (!g_stormTimer || !g_stormActive || g_stormWaitingForRelease) return;
    g_stormBatchQueued = false;
    constexpr int batch = 20;
    for (int i = 0; i < batch && g_stormCreated + i < g_stormTotal; ++i) {
        auto *peer = new QWidget(nullptr, Qt::Window);
        peer->setWindowTitle(QStringLiteral("task6-storm-%1").arg(g_stormCreated + i));
        peer->resize(120, 90);
        peer->show();
        g_stormWindows << peer;
    }
    g_stormWaitingForRelease = true;
    g_stormCreated += g_stormWindows.size();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
}

static QJsonObject result(AppController &controller, const QJsonObject &request) {
    const QString op = request.value("op").toString();
    const QString rel = request.value("rel").toString();
    if (op == "new") {
        const QString created = controller.newNote({}, false);
        return {{"ok", !created.isEmpty()}, {"rel", created}};
    }
    if (op == "sticky") {
        auto *s = controller.openSticky(rel, true);
        return {{"ok", s != nullptr}, {"rel", rel}};
    }
    if (op == "organizer") {
        controller.showOrganizer(true);
        return {{"ok", controller.organizer() != nullptr}};
    }
    if (op == "peer") {
        if (!g_peer) {
            g_peer = new QWidget(nullptr, Qt::Window);
            g_peer->setObjectName(QStringLiteral("task6-peer"));
            g_peer->setWindowTitle(QStringLiteral("task6-peer"));
            g_peer->resize(320, 240);
        }
        g_peer->show();
        g_peer->raise();
        g_peer->activateWindow();
        return {{"ok", true}, {"title", g_peer->windowTitle()}};
    }
    if (op == "close-peer") {
        if (g_peer) g_peer->close();
        QCoreApplication::processEvents();
        return {{"ok", !g_peer || !g_peer->isVisible()}};
    }
    if (op == "workspace-mode") {
        if (!controller.stickyOf(rel)) return {{"ok", false}, {"error", "sticky is not open"}};
        const QString mode = request.value("mode").toString();
        if (mode != "this" && mode != "all") return {{"ok", false}, {"error", "mode must be this or all"}};
        controller.setWorkspaceMode(rel, mode == "all" ? hn::platform::WorkspaceMode::AllWorkspaces
                                                         : hn::platform::WorkspaceMode::ThisWorkspace);
        return {{"ok", true}, {"mode", mode}};
    }
    if (op == "theme") {
        auto settings = controller.settings();
        const QString scheme = request.value("scheme").toString();
        if (scheme != "light" && scheme != "dark") return {{"ok", false}, {"error", "scheme must be light or dark"}};
        settings.colorScheme = scheme;
        controller.applySettings(settings, controller.prefs());
        return {{"ok", true}, {"scheme", scheme}};
    }
    if (op == "consent") {
        if (g_consent) g_consent->close();
        ConsentRequest request;
        request.id = QStringLiteral("task6-preview");
        request.name = QStringLiteral("Task 6 test plugin");
        request.author = QStringLiteral("Hyprnotes test harness");
        request.version = QStringLiteral("1.0.0");
        request.description = QStringLiteral("A test-only preview of the plugin consent surface.");
        request.source = QStringLiteral("scratch-only fixture");
        request.sha256 = QString(64, QLatin1Char('a'));
        request.permissions = {QStringLiteral("notes.read"), QStringLiteral("network")};
        g_consent = new ConsentDialog(request, nullptr, 0);
        g_consent->setAttribute(Qt::WA_DeleteOnClose);
        g_consent->show();
        return {{"ok", true}};
    }
    if (op == "dismiss-dialog") {
        if (QWidget *dialog = surfaceWidget(controller, request.value("surface").toString(), rel)) dialog->close();
        return {{"ok", true}};
    }
    if (op == "latency") {
        NoteSession *session = controller.session(rel);
        if (!session) return {{"ok", false}, {"error", "note is not open"}};
        QElapsedTimer timer;
        timer.start();
        QTest::keyClick(session->editor()->activeEdit(), Qt::Key_X);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        return {{"ok", true}, {"milliseconds", timer.elapsed()}};
    }
    if (op == "storm-start") {
        const int count = request.value("count").toInt();
        if (count < 1 || count > 1000 || g_stormActive)
            return {{"ok", false}, {"error", "count must be 1..1000 and no storm may be active"}};
        g_stormTotal = count; g_stormCreated = 0; g_stormClosed = 0;
        g_stormWaitingForRelease = false; g_stormBatchQueued = true;
        if (!g_stormTimer) { g_stormTimer = new QTimer(qApp); g_stormTimer->setSingleShot(true); }
        g_stormActive = true;
        QTimer::singleShot(0, g_stormTimer, stormBatch);
        return {{"ok", true}, {"count", count}};
    }
    if (op == "storm-progress")
        return {{"ok", true}, {"created", g_stormCreated}, {"closed", g_stormClosed},
                {"active", g_stormActive}, {"waitingForRelease", g_stormWaitingForRelease}, {"batchQueued", g_stormBatchQueued},
                {"batchSize", g_stormWindows.size()}};
    if (op == "storm-release") {
        if (!g_stormActive || !g_stormWaitingForRelease) return {{"ok", false}, {"error", "no mapped batch is awaiting release"}};
        const int count = g_stormWindows.size();
        g_stormWaitingForRelease = false;
        g_stormBatchQueued = true;
        for (const auto &window : std::as_const(g_stormWindows)) if (window) { window->close(); window->deleteLater(); }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        g_stormWindows.clear();
        g_stormClosed += count;
        g_stormWaitingForRelease = false;
        g_stormBatchQueued = false;
        if (g_stormClosed >= g_stormTotal) { g_stormActive = false; g_stormTimer->stop(); }
        return {{"ok", true}, {"closed", count}};
    }
    if (op == "storm-next") {
        if (!g_stormActive || g_stormWaitingForRelease || g_stormCreated >= g_stormTotal)
            return {{"ok", false}, {"error", "storm cannot advance"}};
        if (g_stormBatchQueued) return {{"ok", true}};
        g_stormBatchQueued = true;
        QTimer::singleShot(0, g_stormTimer, stormBatch);
        return {{"ok", true}};
    }
    if (op == "edit") {
        auto *s = controller.session(rel);
        if (!s) return {{"ok", false}, {"error", "note is not open"}};
        auto *edit = s->editor()->activeEdit();
        edit->setFocus();
        QTest::keyClicks(edit, request.value("text").toString());
        QString text;
        if (auto *visual = s->editor()->visualEdit()) text = visual->toPlainText();
        else if (auto *source = s->editor()->sourceEdit()) text = source->toPlainText();
        return {{"ok", true}, {"text", text}};
    }
    if (op == "pop-in") return {{"ok", controller.popIn(rel)}};
    if (op == "pop-out") return {{"ok", controller.popOut(rel)}};
    if (op == "close") {
        controller.closeNote(rel, true);
        QCoreApplication::processEvents();
        return {{"ok", controller.session(rel) == nullptr}};
    }
    if (op == "save-note") return {{"ok", waitSaved(controller.session(rel))}, {"state", controller.session(rel) ? controller.session(rel)->stateLabel() : QString("closed")}};
    if (op == "flush") {
        controller.flushSessionFile();
        return {{"ok", QFile::exists(controller.statePath("session.json"))}};
    }
    if (op == "screenshot") {
        QWidget *target = surfaceWidget(controller, request.value("surface").toString(), rel);
        const QString path = request.value("path").toString();
        if (!target || !target->isVisible()) return {{"ok", false}, {"error", "surface is not visible"}};
        QImage image = target->grab().toImage();
        if (image.size() != target->size()) image = image.scaled(target->size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        image.setDevicePixelRatio(1);
        return {{"ok", image.save(path)}, {"path", path}, {"width", image.width()}, {"height", image.height()},
                {"dpr", target->devicePixelRatioF()}};
    }
    if (op == "geometry") {
        QWidget *target = surfaceWidget(controller, request.value("surface").toString(), rel);
        if (!target) return {{"ok", false}, {"error", "surface is not open"}};
        const QRect g = target->geometry();
        const QRect available = target->screen() ? target->screen()->availableGeometry() : QRect();
        QJsonArray children;
        appendWidgetGeometry(children, target);
        return {{"ok", true}, {"x", g.x()}, {"y", g.y()}, {"width", g.width()}, {"height", g.height()},
                {"screen", target->screen() ? target->screen()->name() : QString()}, {"screenX", available.x()},
                {"screenY", available.y()}, {"screenWidth", available.width()}, {"screenHeight", available.height()},
                {"dpr", target->devicePixelRatioF()}, {"children", children}};
    }
    if (op == "state") {
        QJsonArray sessions, windows;
        for (const QString &name : controller.openNotes()) sessions.append(name);
        for (auto *sticky : controller.stickies()) if (sticky && sticky->session())
            windows.append(QJsonObject{{"rel", sticky->session()->rel()}, {"role", "sticky"}, {"token", sticky->token()}});
        if (controller.organizer() && controller.organizer()->currentSession())
            windows.append(QJsonObject{{"rel", controller.organizer()->currentSession()->rel()}, {"role", "organizer"},
                                       {"token", controller.organizer()->token()}});
        return {{"ok", true}, {"sessions", sessions}, {"windows", windows}};
    }
    if (op == "settings") {
        controller.showOrganizer(true);
        controller.showSettings(controller.organizer());
        return {{"ok", true}};
    }
    if (op == "settings-tab") {
        SettingsDialog *dialog = nullptr;
        for (QWidget *widget : QApplication::topLevelWidgets())
            if ((dialog = qobject_cast<SettingsDialog *>(widget))) break;
        if (!dialog) {
            controller.showOrganizer(true);
            controller.showSettings(controller.organizer());
            QCoreApplication::processEvents();
            for (QWidget *widget : QApplication::topLevelWidgets())
                if ((dialog = qobject_cast<SettingsDialog *>(widget))) break;
        }
        const int index = request.value("index").toInt(-1);
        if (!dialog || index < 0 || index >= dialog->tabs()->count()) return {{"ok", false}, {"error", "settings tab missing"}};
        dialog->showTab(index);
        QCoreApplication::processEvents();
        return {{"ok", true}, {"index", index}, {"count", dialog->tabs()->count()}};
    }
    if (op == "quit") return {{"ok", true}, {"quit", true}};
    return {{"ok", false}, {"error", "unknown operation"}};
}

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("hyprnotes"));
    app.setDesktopFileName(QStringLiteral("hyprnotes"));
    app.setQuitOnLastWindowClosed(false);
    ControllerOptions options;
    options.notesDir = qEnvironmentVariable("HN_NOTES_DIR");
    options.stateDir = qEnvironmentVariable("HN_STATE_DIR");
    options.cacheDir = qEnvironmentVariable("HN_CACHE_DIR");
    options.configPath = qEnvironmentVariable("HN_CONFIG_DIR") + "/config.json";
    options.modsDir = qEnvironmentVariable("HN_DATA_DIR") + "/mods";
    options.modsEnabledPath = qEnvironmentVariable("HN_CONFIG_DIR") + "/mods-enabled.json";
    options.pluginsDir = qEnvironmentVariable("HN_DATA_DIR") + "/plugins";
    if (qEnvironmentVariableIntValue("HN_LIVE_NO_HYPRLAND")) options.useHyprland = false;
    const QString fakeIpcDir = qEnvironmentVariable("HN_LIVE_HYPR_IPC_DIR");
    if (!fakeIpcDir.isEmpty()) options.hyprPaths = hn::platform::HyprPaths::inDir(fakeIpcDir);
    options.useTray = qEnvironmentVariableIntValue("HN_LIVE_ENABLE_TRAY") != 0;
    options.recoveryPrompt = [](const QList<hn::core::RecoveryEntry> &) { return RecoveryChoice::Later; };
    options.confirm = [](const QString &, const QString &) { return true; };
    AppController controller(options);
    controller.setTrayUsableOverride(true);
    controller.start(hn::platform::Action::Background);
    controller.beginInteractive();
    SignalBridge signalBridge;
    QObject::connect(&signalBridge, &SignalBridge::quitSignal, &controller, [&controller](int) { controller.requestQuit(); });
    QObject::connect(&controller, &AppController::exitRequested, &app, &QCoreApplication::quit);
    QSocketNotifier input(STDIN_FILENO, QSocketNotifier::Read);
    QByteArray pending;
    QObject::connect(&input, &QSocketNotifier::activated, &app, [&] {
        char bytes[65536];
        const ssize_t count = ::read(STDIN_FILENO, bytes, sizeof(bytes));
        if (count <= 0) { app.quit(); return; }
        pending.append(bytes, count);
        int newline = -1;
        while ((newline = pending.indexOf('\n')) >= 0) {
            const QByteArray row = pending.left(newline);
            pending.remove(0, newline + 1);
            if (row.trimmed().isEmpty()) continue;
            QJsonParseError error;
            const auto doc = QJsonDocument::fromJson(row, &error);
            QJsonObject response;
            if (error.error != QJsonParseError::NoError || !doc.isObject()) response = {{"ok", false}, {"error", "invalid JSON request"}};
            else response = result(controller, doc.object());
            if (error.error == QJsonParseError::NoError && doc.isObject() && doc.object().contains("id"))
                response.insert("id", doc.object().value("id"));
            QTextStream out(stdout);
            out << QJsonDocument(response).toJson(QJsonDocument::Compact) << Qt::endl;
            if (response.value("quit").toBool()) app.quit();
        }
    });
    return app.exec();
}
