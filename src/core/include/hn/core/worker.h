#pragma once
#include <QString>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <thread>

namespace hn::core {

// One shared worker thread. Saves run before index work; a queued task with the same
// coalesce key is replaced in place by a newer one (superseded work is dropped).
class Worker {
public:
    enum Priority { Save = 0, Index = 1 };
    Worker();
    ~Worker();
    static Worker &shared();

    void post(Priority p, std::function<void()> fn, const QString &coalesceKey = {});
    void waitIdle(); // queue empty and nothing running

    template <class F> auto callBlocking(Priority p, F f) -> decltype(f()) {
        if (std::this_thread::get_id() == m_thread.get_id()) return f();
        std::packaged_task<decltype(f())()> task(std::move(f));
        auto fut = task.get_future();
        auto sp = std::make_shared<decltype(task)>(std::move(task));
        post(p, [sp] { (*sp)(); });
        return fut.get();
    }

private:
    struct Task { QString key; std::function<void()> fn; };
    void run();
    std::mutex m_mu;
    std::condition_variable m_cv, m_idle;
    std::deque<Task> m_q[2];
    bool m_running = false, m_stop = false;
    std::thread m_thread;
};

} // namespace hn::core
