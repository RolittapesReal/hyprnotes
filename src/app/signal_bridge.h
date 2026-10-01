#pragma once
// SIGTERM / SIGINT / SIGHUP -> Qt signal through a self-pipe (async-signal-safe handler, no polling, no timers).
#include <QObject>

class QSocketNotifier;

namespace hn::app {

class SignalBridge : public QObject {
    Q_OBJECT
public:
    explicit SignalBridge(QObject *parent = nullptr);   // at most one live instance per process
    ~SignalBridge() override;                           // restores default dispositions
    bool ok() const { return m_ok; }
signals:
    void quitSignal(int signo);
private:
    QSocketNotifier *m_notifier = nullptr;
    int m_pipe[2] = {-1, -1};
    bool m_ok = false;
};

} // namespace hn::app
