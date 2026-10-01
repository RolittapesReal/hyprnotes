#pragma once
// Fake Hyprland: request socket (.socket.sock) + event socket (.socket2.sock) in a temp dir.
#include "hn/platform/hyprland_ipc.h"
#include <QLocalServer>
#include <QLocalSocket>
#include <QTemporaryDir>
#include <functional>

class FakeHyprland : public QObject {
public:
  QTemporaryDir dir;
  QLocalServer req, ev;
  QList<QLocalSocket*> evClients;
  QStringList received;                 // raw requests, in order
  int closedRequests = 0;               // request connections that ended
  bool hang = false;                    // accept + read but never reply
  std::function<QByteArray(const QByteArray&)> handler;

  FakeHyprland() {
    QVERIFY_NOOP();
    startEvents();
    startRequests();
  }
  static void QVERIFY_NOOP() {}
  hn::platform::HyprPaths paths() const { return hn::platform::HyprPaths::inDir(dir.path()); }

  void startRequests() {
    QLocalServer::removeServer(paths().request);
    req.listen(paths().request);
    connect(&req, &QLocalServer::newConnection, this, [this] {
      while (auto* s = req.nextPendingConnection()) {
        connect(s, &QLocalSocket::readyRead, this, [this, s] {
          const QByteArray cmd = s->readAll();
          received << QString::fromUtf8(cmd);
          if (hang) return;
          s->write(handler ? handler(cmd) : QByteArray("ok"));
          s->disconnectFromServer();
        });
        connect(s, &QLocalSocket::disconnected, this, [this, s] { ++closedRequests; s->deleteLater(); });
      }
    });
  }
  void startEvents() {
    QLocalServer::removeServer(paths().event);
    if (!ev.listen(paths().event)) qWarning() << "fake ev listen failed" << ev.errorString();
    connect(&ev, &QLocalServer::newConnection, this, [this] {
      while (auto* s = ev.nextPendingConnection()) evClients << s;
    });
  }
  void emitEvent(const QByteArray& raw) { for (auto* c : evClients) { c->write(raw); c->flush(); } }
  // simulate compositor restart / event stream loss
  void dropEvents() {
    ev.close();
    for (auto* c : evClients) { c->abort(); c->deleteLater(); }
    evClients.clear();
  }
};

inline const char* kClientsJson = R"([{"address":"0x55738da49dd0","mapped":true,"hidden":false,"at":[336,-782],"size":[1248,702],
"workspace":{"id":2,"name":"2"},"floating":true,"monitor":0,"class":"hyprnotes","title":"My note","initialClass":"hyprnotes",
"initialTitle":"hyprnotes-sticky:00000000000000aa","pid":4242,"xwayland":false,"pinned":false},
{"address":"0x55738e03c8b0","mapped":true,"at":[12,50],"size":[938,1018],"workspace":{"id":3,"name":"3"},"floating":false,"monitor":0,
"class":"kitty","title":"t","initialClass":"kitty","initialTitle":"kitty","pid":1,"xwayland":false,"pinned":true}])";
inline const char* kMonitorsJson = R"([{"id":0,"name":"eDP-1","width":3840,"height":2160,"x":0,"y":0,"reserved":[0,38,0,0],"scale":2.0,
"activeWorkspace":{"id":2,"name":"2"}}])";
