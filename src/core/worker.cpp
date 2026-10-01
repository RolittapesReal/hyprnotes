#include "hn/core/worker.h"

namespace hn::core {

Worker::Worker() : m_thread([this] { run(); }) {}

Worker::~Worker() {
    { std::lock_guard l(m_mu); m_stop = true; }
    m_cv.notify_all();
    m_thread.join();
}

Worker &Worker::shared() { static Worker w; return w; }

void Worker::post(Priority p, std::function<void()> fn, const QString &key) {
    {
        std::lock_guard l(m_mu);
        auto &q = m_q[p];
        bool replaced = false;
        if (!key.isEmpty())
            for (auto &t : q) if (t.key == key) { t.fn = std::move(fn); replaced = true; break; }
        if (!replaced) q.push_back({key, std::move(fn)});
    }
    m_cv.notify_one();
}

void Worker::waitIdle() {
    std::unique_lock l(m_mu);
    m_idle.wait(l, [this] { return !m_running && m_q[0].empty() && m_q[1].empty(); });
}

void Worker::run() {
    std::unique_lock l(m_mu);
    for (;;) {
        m_cv.wait(l, [this] { return m_stop || !m_q[0].empty() || !m_q[1].empty(); });
        if (m_stop) return;
        auto &q = m_q[0].empty() ? m_q[1] : m_q[0];
        Task t = std::move(q.front());
        q.pop_front();
        m_running = true;
        l.unlock();
        t.fn();
        l.lock();
        m_running = false;
        m_idle.notify_all();
    }
}

} // namespace hn::core
