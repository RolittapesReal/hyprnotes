// Qt-only reproducer (no Hyprnotes code, Qt 6.12.0): QObject::connect() growing a sender's signal vector while another thread emits an
// unrelated signal of the same sender. QObjectPrivate::maybeSignalConnected() relaxed-loads the vector that resizeSignalVector() replaces.
#include <QCoreApplication>
#include <QObject>
#include <thread>
#include <atomic>
#include <cstdio>

class Sender : public QObject {
    Q_OBJECT
public:
    explicit Sender(QObject *p = nullptr) : QObject(p) {}
signals:
    void s0(); void s1(); void s2(); void s3(); void s4(); void s5(); void s6(); void s7();
    void s8(); void s9(); void s10(); void s11(); void s12(); void s13(); void s14(); void s15();
    void s16(); void s17(); void s18(); void s19(); void s20(); void s21(); void s22(); void s23();
    void s24(); void s25(); void s26(); void s27(); void s28(); void s29(); void s30(); void s31();
public:
    void emitFirst() { emit s0(); }
};
class Sink : public QObject {
    Q_OBJECT
public slots:
    void hit() {}
};

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const int rounds = argc > 1 ? atoi(argv[1]) : 3000;
    for (int r = 0; r < rounds; ++r) {
        Sender sender;
        Sink sink;
        std::atomic<bool> stop{false};
        std::thread emitter([&] { while (!stop.load(std::memory_order_acquire)) sender.emitFirst(); });
        auto c1 = QObject::connect(&sender, &Sender::s8, &sink, &Sink::hit, Qt::DirectConnection);
        auto c2 = QObject::connect(&sender, &Sender::s16, &sink, &Sink::hit, Qt::DirectConnection);
        auto c3 = QObject::connect(&sender, &Sender::s31, &sink, &Sink::hit, Qt::DirectConnection);
        QObject::disconnect(c1); QObject::disconnect(c2); QObject::disconnect(c3);
        stop.store(true, std::memory_order_release);
        emitter.join();
    }
    std::puts("done");
    return 0;
}
#include "qt_connection_race.moc"
