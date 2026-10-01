// Launcher used by the startup-race test: acts like `hyprnotes --show-organizer`.
#include "hn/platform/instance_transport.h"
#include <QCoreApplication>
#include <QTimer>
#include <cstdio>
using namespace hn::platform;
int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  const QString path = argc > 1 ? argv[1] : QString();
  InstanceServer srv(path);
  int handled = 0;
  srv.setHandler([&](Action) { ++handled; return true; });
  QString err;
  switch (srv.listen(&err)) {
  case InstanceServer::Result::Listening:
    std::printf("SERVER\n"); std::fflush(stdout);
    QTimer::singleShot(3000, &app, [&] { std::printf("HANDLED %d\n", handled); app.quit(); });
    return app.exec();
  case InstanceServer::Result::AlreadyRunning: {
    const auto r = sendAction(Action::ShowOrganizer, path);
    std::printf("CLIENT %d\n", int(r.status));
    return r.status == ClientResult::Status::Acked ? 0 : 3;
  }
  default: std::printf("FAILED %s\n", qPrintable(err)); return 2;
  }
}
