// Qt-only reproducer (no Hyprnotes code): destroy a QNetworkReply on the main thread while the QNetworkAccessManager helper
// thread is deleting the reply's QHttpThreadDelegate. Mirrors the shape of the failing market-client test: GET, a response, then
// delete the reply from its finished handling.
#include <QCoreApplication>
#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTcpServer>
#include <QTcpSocket>
#include <cstdio>

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const int rounds = argc > 1 ? atoi(argv[1]) : 50;
    QTcpServer server;
    if (!server.listen(QHostAddress::LocalHost, 0)) return 2;
    QObject::connect(&server, &QTcpServer::newConnection, [&] {
        while (QTcpSocket *s = server.nextPendingConnection()) {
            QObject::connect(s, &QTcpSocket::readyRead, s, [s] {
                s->readAll();
                s->write("HTTP/1.1 302 Found\r\nLocation: ftp://example.invalid/x\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                s->disconnectFromHost();
            });
            QObject::connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
        }
    });
    const QUrl url(QStringLiteral("http://127.0.0.1:%1/index.json").arg(server.serverPort()));
    for (int i = 0; i < rounds; ++i) {
        QNetworkAccessManager nam;
        QNetworkReply *reply = nam.get(QNetworkRequest(url));
        QEventLoop loop;
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec();
        delete reply;   // ~QNetworkReply writes the vptr that the helper thread's delegate teardown still reads
    }
    std::puts("done");
    return 0;
}
