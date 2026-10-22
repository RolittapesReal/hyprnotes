// Race-positive control that runs under Qt frames: the destructor of a lambda capture (run by disconnect()) races a plain int written by
// another std::thread. This is an application race, not the Qt defect, and MUST still be reported with tsan.supp loaded.
#include <QCoreApplication>
#include <QObject>
#include <atomic>
#include <memory>
#include <thread>
#include <cstdio>

volatile int dtorHits = 0;
class Sender : public QObject {
    Q_OBJECT
signals:
    void ping();
};
struct Guard { ~Guard() { dtorHits = dtorHits + 1; } };

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const int rounds = argc > 1 ? atoi(argv[1]) : 2000;
    Sender s;
    std::atomic<bool> stop{false};
    std::thread t([&] { while (!stop.load(std::memory_order_relaxed)) dtorHits = dtorHits + 1; });
    for (int i = 0; i < rounds; ++i) {
        auto g = std::make_shared<Guard>();
        auto c = QObject::connect(&s, &Sender::ping, [g] {});
        g.reset();
        QObject::disconnect(c);
        QObject::disconnect(QObject::connect(&s, &Sender::ping, [] {}));
    }
    stop = true;
    t.join();
    std::printf("%d\n", int(dtorHits));
    return 0;
}
#include "app_lambda_race.moc"
