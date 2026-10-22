// Launcher used by the startup-race test: acts like `hyprnotes --show-organizer`.
// Usage: platform_race_helper SOCKET [EXPECTED_CLIENTS]. The process that wins the race serves until EXPECTED_CLIENTS requests have
// been handled (then prints HANDLED n and exits) or 60 s pass. It must not stop after a fixed short time: on a loaded machine the other
// launchers may start seconds later, and a server that had already quit would turn a slow start into a failed (or second) server.
#include "hn/platform/instance_transport.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTimer>
#include <cstdio>
using namespace hn::platform;
int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  const QString path = argc > 1 ? argv[1] : QString();
  const int expected = argc > 2 ? QString(argv[2]).toInt() : 0;
  InstanceServer srv(path);
  int handled = 0;
  auto report = [&] { std::printf("HANDLED %d\n", handled); std::fflush(stdout); app.quit(); };
  srv.setHandler([&](Action) {
    ++handled;
    if (expected > 0 && handled == expected) QTimer::singleShot(500, &app, report);   // linger so the ack of this last request is flushed (socket notifiers run before timers)
    return true;
  });
  QString err;
  QElapsedTimer listenTimer;
  listenTimer.start();
  switch (srv.listen(&err)) {
  case InstanceServer::Result::Listening:
    std::printf("SERVER LISTEN_MS %lld\n", static_cast<long long>(listenTimer.elapsed())); std::fflush(stdout);
    QTimer::singleShot(expected > 0 ? 60000 : 3000, &app, report);
    return app.exec();
  case InstanceServer::Result::AlreadyRunning: {
    const auto r = sendAction(Action::ShowOrganizer, path, 20000);   // the ack deadline is tested elsewhere; here only the race matters
    std::printf("CLIENT %d LISTEN_MS %lld\n", int(r.status), static_cast<long long>(listenTimer.elapsed()));
    return r.status == ClientResult::Status::Acked ? 0 : 3;
  }
  default: std::printf("FAILED LISTEN_MS %lld %s\n", static_cast<long long>(listenTimer.elapsed()), qPrintable(err)); return 2;
  }
}
