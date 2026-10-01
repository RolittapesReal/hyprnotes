#include "signal_bridge.h"
#include <QSocketNotifier>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <unistd.h>

namespace hn::app {

namespace {
constexpr int kSignals[] = {SIGTERM, SIGINT, SIGHUP};
volatile sig_atomic_t g_fd = -1;
void onSignal(int signo) {
    const int saved = errno;
    const unsigned char b = static_cast<unsigned char>(signo);
    if (g_fd >= 0) { ssize_t r = ::write(g_fd, &b, 1); (void)r; }   // pipe full => a quit is already queued
    errno = saved;
}
} // namespace

SignalBridge::SignalBridge(QObject *parent) : QObject(parent) {
    if (g_fd >= 0 || ::pipe2(m_pipe, O_CLOEXEC | O_NONBLOCK) != 0) return;
    g_fd = m_pipe[1];
    struct sigaction sa {};
    sa.sa_handler = onSignal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    for (int s : kSignals) sigaction(s, &sa, nullptr);
    m_notifier = new QSocketNotifier(m_pipe[0], QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated, this, [this] {
        unsigned char b;
        while (::read(m_pipe[0], &b, 1) == 1) emit quitSignal(b);
    });
    m_ok = true;
}

SignalBridge::~SignalBridge() {
    if (!m_ok) return;
    for (int s : kSignals) std::signal(s, SIG_DFL);
    g_fd = -1;
    ::close(m_pipe[0]);
    ::close(m_pipe[1]);
}

} // namespace hn::app
