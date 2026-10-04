#pragma once
// Test helpers for the marketplace client: a tiny loopback HTTP server and index builders.
#include <QTimer>
#include "hn/plugins/market.h"
#include <QByteArray>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QTcpServer>
#include <QTcpSocket>

namespace markettest {
using hn::plugins::MarketEntry;

struct Resp { int status = 200; QByteArray body; QMap<QByteArray, QByteArray> headers; bool hang = false, drip = false; };   // drip: headers + 3 bytes, then 1 byte per 100 ms

class FakeHttp : public QTcpServer {   // minimal HTTP/1.1, Connection: close, loopback only
public:
    QMap<QString, Resp> routes;                           // path -> response
    QList<QMap<QByteArray, QByteArray>> seen;             // request headers per request (lower-cased names)
    QStringList paths;                                    // request paths in order
    bool start() {
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (auto *s = nextPendingConnection()) {
                connect(s, &QTcpSocket::readyRead, this, [this, s] { handle(s); });
                connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
            }
        });
        return listen(QHostAddress::LocalHost, 0);
    }
    QString url(const QString &path) const { return QStringLiteral("http://127.0.0.1:%1%2").arg(serverPort()).arg(path); }

private:
    void handle(QTcpSocket *s) {
        buf_[s].append(s->readAll());
        const int end = buf_[s].indexOf("\r\n\r\n");
        if (end < 0) return;
        const QList<QByteArray> lines = buf_.take(s).left(end).split('\n');
        const QList<QByteArray> first = lines.value(0).trimmed().split(' ');
        const QString path = QString::fromUtf8(first.value(1));
        QMap<QByteArray, QByteArray> h;
        for (int i = 1; i < lines.size(); ++i) {
            const int c = lines[i].indexOf(':');
            if (c > 0) h.insert(lines[i].left(c).trimmed().toLower(), lines[i].mid(c + 1).trimmed());
        }
        seen << h;
        paths << path;
        Resp r;
        r.status = 404;
        if (routes.contains(path)) r = routes.value(path);
        if (r.hang) return;
        if (r.drip) {
            s->write("HTTP/1.1 200 X\r\nContent-Length: 100000\r\nConnection: close\r\n\r\nabc");
            auto *t = new QTimer(s);
            connect(t, &QTimer::timeout, s, [s] { s->write("x"); });
            t->start(100);
            return;
        }
        QByteArray out = "HTTP/1.1 " + QByteArray::number(r.status) + " X\r\nContent-Length: " + QByteArray::number(r.body.size())
                         + "\r\nConnection: close\r\n";
        for (auto it = r.headers.cbegin(); it != r.headers.cend(); ++it) out += it.key() + ": " + it.value() + "\r\n";
        out += "\r\n" + r.body;
        s->write(out);
        s->disconnectFromHost();
    }
    QHash<QTcpSocket *, QByteArray> buf_;
};

inline bool loopbackOnly(const QUrl &u) { return u.scheme() == QLatin1String("http") && u.host() == QLatin1String("127.0.0.1"); }

inline MarketEntry entryFor(const QString &id, const QString &version, const QByteArray &package, const QString &url) {
    MarketEntry e;
    e.id = id; e.name = id; e.version = version; e.author = "Test"; e.description = "Test plugin.";
    e.tier = "script"; e.minApp = "0.1.0"; e.api = 2; e.url = url;
    e.size = package.size();
    e.sha256 = QString::fromLatin1(QCryptographicHash::hash(package, QCryptographicHash::Sha256).toHex());
    return e;
}

inline QByteArray makeIndex(const QList<MarketEntry> &entries) {
    QJsonArray a;
    for (const auto &e : entries)
        a.append(QJsonObject{{"id", e.id}, {"name", e.name}, {"version", e.version}, {"author", e.author}, {"description", e.description},
                             {"tags", QJsonArray{}}, {"permissions", QJsonArray{}}, {"net_hosts", QJsonArray{}}, {"tier", e.tier},
                             {"api", e.api}, {"min_app", e.minApp}, {"url", e.url}, {"sha256", e.sha256}, {"size", double(e.size)}});
    return QJsonDocument(QJsonObject{{"schema", 1}, {"updated", "2026-10-04T12:00:00Z"}, {"plugins", a}}).toJson();
}
}  // namespace markettest
