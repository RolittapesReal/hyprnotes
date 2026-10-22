#include "hn/core/worker.h"

#include <algorithm>
#include <utility>

namespace hn::core {

Worker::Worker() : m_thread([this] { run(); }) {}

Worker::~Worker() {
    { std::lock_guard l(m_mu); m_stop = true; }
    m_cv.notify_all();
    m_thread.join();
}

Worker &Worker::shared() { static Worker w; return w; }

void Worker::post(Priority p, std::function<void()> fn, const QString &key) {
    enqueue(p, std::move(fn), key, {});
}

void Worker::enqueue(Priority p, std::function<void()> fn, const QString &key,
                     std::shared_ptr<std::promise<void>> finished) {
    {
        std::lock_guard l(m_mu);
        auto &q = m_q[p];
        auto same = q.end();
        if (!key.isEmpty())
            same = std::find_if(q.begin(), q.end(), [&](const auto &t) { return t.key == key; });
        if (same != q.end()) {
            same->fn = std::move(fn);
            if (finished) same->finished = std::move(finished);
        } else {
            q.push_back({key, std::move(fn), std::move(finished)});
        }
    }
    m_cv.notify_one();
}

void Worker::waitIdle() {
    std::unique_lock l(m_mu);
    m_idle.wait(l, [this] { return !m_running && m_q[0].empty() && m_q[1].empty(); });
}

void Worker::setBeforeDequeueHookForTest(Priority p, QString keyPrefix, std::function<void()> hook) {
    std::lock_guard l(m_mu);
    m_beforeDequeueHook = BeforeDequeueHook{p, std::move(keyPrefix), std::move(hook)};
}

void Worker::clearBeforeDequeueHookForTest() {
    std::lock_guard l(m_mu);
    m_beforeDequeueHook.reset();
}

void Worker::run() {
    std::unique_lock l(m_mu);
    for (;;) {
        m_cv.wait(l, [this] { return m_stop || !m_q[0].empty() || !m_q[1].empty(); });
        if (m_stop) return;
        const Priority priority = m_q[Save].empty() ? Index : Save;
        auto &q = m_q[priority];
        if (m_beforeDequeueHook && m_beforeDequeueHook->priority == priority
            && q.front().key.startsWith(m_beforeDequeueHook->keyPrefix)) {
            auto hook = std::move(m_beforeDequeueHook->callback);
            m_beforeDequeueHook.reset(); // a replacement must not enter the one-shot gate again
            l.unlock();                  // let post() replace the still-queued task by coalesce key
            hook();
            l.lock();
            continue;                    // reselect/re-evaluate the queue after the gate
        }
        Task t = std::move(q.front());
        q.pop_front();
        m_running = true;
        l.unlock();
        t.fn();
        t.fn = {}; // release task captures before completing callBlocking callers
        l.lock();
        m_running = false;
        m_idle.notify_all();
        auto finished = std::move(t.finished);
        l.unlock();
        if (finished) finished->set_value();
        l.lock();
    }
}

} // namespace hn::core
