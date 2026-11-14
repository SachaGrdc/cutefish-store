#include "inventoryio.h"

namespace cutefish::store::service {

InventoryIo::InventoryIo(std::filesystem::path state, Options options)
    : m_shared(std::make_shared<Shared>(std::move(state), std::move(options)))
{
    m_stopped = m_shared->stopped.get_future().share();
    m_thread = std::thread(threadMain, m_shared);
}

InventoryIo::~InventoryIo()
{
    bool busy = false;
    {
        std::lock_guard lock(m_shared->mutex);
        m_shared->stopping = true;
        m_shared->queue.clear();
        busy = m_shared->busy;
    }
    m_shared->wake.notify_all();
    // Idle: the thread only waits on the condition variable and ends at
    // once. Busy: it may be inside SQLite (up to its busy timeout); it is
    // left to end on its own, holding what it needs (Shared).
    if (busy)
        m_thread.detach();
    else
        m_thread.join();
}

void InventoryIo::post(std::function<void()> task)
{
    {
        std::lock_guard lock(m_shared->mutex);
        if (m_shared->stopping)
            return;
        m_shared->queue.push_back(std::move(task));
    }
    m_shared->wake.notify_one();
}

void InventoryIo::threadMain(std::shared_ptr<Shared> shared)
{
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock lock(shared->mutex);
            shared->busy = false;
            shared->wake.wait(lock, [&] { return shared->stopping || !shared->queue.empty(); });
            if (shared->stopping)
                break;
            task = std::move(shared->queue.front());
            shared->queue.pop_front();
            shared->busy = true;
        }
        task();
    }
    // The connection belongs to this thread: closed here.
    shared->store.reset();
    shared->stopped.set_value();
}

} // namespace cutefish::store::service
