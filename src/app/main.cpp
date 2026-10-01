#include "cli.h"
#include "controller.h"
#include "hn/core/paths.h"
#include "hn/platform/instance_transport.h"
#include "signal_bridge.h"
#include "ui_common.h"
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QTextStream>

using namespace hn;
using hn::platform::Action;
using Status = hn::platform::ClientResult::Status;

int main(int argc, char **argv) {
    QStringList args;
    for (int i = 1; i < argc; ++i) args << QString::fromLocal8Bit(argv[i]);
    const app::CliOptions cli = app::parseCli(args);
    QTextStream out(stdout), err(stderr);
    if (!cli.error.isEmpty()) { err << "hyprnotes: " << cli.error << "\n"; return 2; }
    if (cli.help) { out << app::helpText(); return 0; }
    if (cli.printRules) {
        QString why;
        const QString t = app::hyprlandRulesText(&why);
        if (t.isEmpty()) { err << "hyprnotes: " << why << "\n"; return 1; }
        out << t;
        return 0;
    }
    if (!cli.importTheme.isEmpty()) return app::runImportTheme(cli.importTheme, out, err);
    if (cli.pluginAction()) {   // headless: no display, no running instance
        QCoreApplication core(argc, argv);
        QTextStream in(stdin);
        return app::runPluginCli(cli, in, out, err);
    }
    if (cli.version) { out << app::versionText() << "\n"; return 0; }

    QApplication qapp(argc, argv);
    QApplication::setApplicationName("hyprnotes");
    // No setApplicationDisplayName(): Qt would append " — Hyprnotes" to every window title, and Hyprland would record that
    // as initialTitle, breaking the token match (window identity) that placement and session save rely on.
    QApplication::setDesktopFileName("hyprnotes");
    QApplication::setApplicationVersion(HN_VERSION);
    QApplication::setWindowIcon(app::ui::appIcon());
    QApplication::setQuitOnLastWindowClosed(false);   // the controller decides, per spec 4.3 / desktop design 4

    // Forward to a running instance when there is one; never start a second persistent process.
    for (int attempt = 0; attempt < 2; ++attempt) {
        const auto r = platform::sendAction(cli.action);
        if (r.status == Status::Acked) return 0;
        if (r.status != Status::NoInstance) { err << "hyprnotes: " << r.message << "\n"; return 1; }
        platform::InstanceServer server;
        QString why;
        const auto res = server.listen(&why);
        if (res == platform::InstanceServer::Result::AlreadyRunning) continue;   // lost the startup race: forward instead
        if (res == platform::InstanceServer::Result::Failed) {
            err << "hyprnotes: cannot start the single-instance endpoint: " << why << "\n";
            return 1;
        }
        app::ControllerOptions opt;
        opt.notesDir = qEnvironmentVariable("HN_NOTES_DIR");
        opt.background = cli.action == Action::Background;
        app::AppController controller(opt);
        server.setHandler([&](Action a) { return controller.handleAction(a); });
        QObject::connect(&controller, &app::AppController::exitRequested, &qapp, &QApplication::quit);
        // SIGTERM/SIGINT/SIGHUP take the same save-aware path as the tray's Quit; if a note cannot be saved the app stays up.
        app::SignalBridge sigBridge;
        QObject::connect(&sigBridge, &app::SignalBridge::quitSignal, &controller, [&](int) { controller.requestQuit(); });
        QObject::connect(&controller, &app::AppController::quitAborted, &controller, [&](const QString &why) {
            err << "hyprnotes: not quitting, a note could not be saved (" << why << "). Fix it in the open window and quit again.\n";
            err.flush();
        });
        controller.start(cli.action);
        return qapp.exec();
    }
    err << "hyprnotes: could not reach or become the running instance.\n";
    return 1;
}
