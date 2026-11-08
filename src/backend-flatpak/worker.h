#pragma once

// A single worker thread with a FIFO queue, for the Flatpak backend: libflatpak
// objects are created, used and destroyed on this thread, never shared across
// threads (ARCHITECTURE §8.2). One serial executor also enforces P-009 for the
// backend: one transaction at a time. The Flatpak backend is a v0 of the two
// GLib threads of the design; this single thread serialises queries and
// transactions, which is safe and keeps the main loop unblocked.

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

namespace cutefish::store::flatpak {

class SerialExecutor
{
public:
    SerialExecutor()
    {
        m_thread = std::thread([this] {
            for (;;) {
                std::function<void()> task;
                {
                    std::unique_lock lock(m_mutex);
                    m_ready.wait(lock, [this] { return m_stopped || !m_queue.empty(); });
                    if (m_stopped && m_queue.empty())
                        return;
                    task = std::move(m_queue.front());
                    m_queue.pop_front();
                }
                task();
            }
        });
    }

    ~SerialExecutor()
    {
        {
            std::lock_guard lock(m_mutex);
            m_stopped = true;
        }
        m_ready.notify_all();
        if (m_thread.joinable())
            m_thread.join();
    }

    SerialExecutor(const SerialExecutor&) = delete;
    SerialExecutor& operator=(const SerialExecutor&) = delete;

    /// Runs `task` on the worker thread. Called from the host's main loop.
    void run(std::function<void()> task)
    {
        {
            std::lock_guard lock(m_mutex);
            m_queue.push_back(std::move(task));
        }
        m_ready.notify_one();
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_ready;
    std::deque<std::function<void()>> m_queue;
    bool m_stopped = false;
    std::thread m_thread;
};

} // namespace cutefish::store::flatpak
