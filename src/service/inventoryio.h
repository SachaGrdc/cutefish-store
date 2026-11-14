#pragma once

// The one I/O thread and the one connection to state.sqlite that every
// Inventory of a service shares (H.2).
//
// Why one thread: the inventories of two backends write disjoint rows
// ((backend, scope) of `installed`, and their own `meta` keys), so SQLite
// alone keeps the data right; but two writers of the same process would
// wait on each other through the busy timeout. One thread serialises them
// by construction: in-process, no inventory write ever waits for another,
// and the writes happen in the order they were asked. The catalogue reads
// that link the inventory run here too.
//
// Stopping (H.2, final audit): stop() — called by the destructor, and by the
// service BEFORE it releases its bus name — makes this writer stop for good:
// jobs not started are dropped, a job not yet writing gives up, and a write
// already inside its SQLite transaction is rolled back by the connection's
// commit guard (StateStore::setCommitGuard), which runs while the write lock
// of the file is held. So once stop() has returned, either a write of this
// writer had already passed its guard — it holds the write lock, and any
// write of a new instance of the service, which can only start after the
// name is released, comes after it — or it never commits. An old
// inventory can never be committed over a newer one.
//
// What the main thread never does here (H.2 finalisation):
// - wait for a write: work and results go through a queue and the loop;
// - wait at destruction: an idle thread is joined (it is only waiting on a
//   condition variable, no I/O); a thread in the middle of a job is
//   detached instead, finishes that one job (bounded by its busy timeout)
//   without delivering it, closes the connection and ends. Jobs not yet
//   started are dropped.
// - retry: a failed write is not retried here; the Inventory schedules a
//   new attempt on its loop (inventory.cpp), so this thread never spins.
//
// The connection waits `busyTimeout` for another connection's lock (short:
// the main thread's journal has the priority, the retries do the rest), and
// its commits do not wait for the disk (synchronous=NORMAL): it only writes
// the installed state, a cache rebuilt from the backends (SERVICE.md §21.5).
// So the main thread, when its journal meets an inventory write, waits for a
// short write without fsync, never for this thread's fsync.

#include <cutefish/store/result.h>
#include <cutefish/store/statestore.h>

#include <base/event_loop.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <deque>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace cutefish::store::service {

/// A result on its way back to the loop. Destroyed or cancelled, the work
/// still runs to its end (or is dropped if it has not started), but its
/// result is never delivered. Kept by the object the result is for.
class IoJob
{
public:
    IoJob() = default;
    ~IoJob() { cancel(); }
    IoJob(IoJob&& o) noexcept : m_state(std::move(o.m_state)) {}
    IoJob& operator=(IoJob&& o) noexcept
    {
        if (this != &o) {
            cancel();
            m_state = std::move(o.m_state);
        }
        return *this;
    }
    IoJob(const IoJob&) = delete;
    IoJob& operator=(const IoJob&) = delete;

    bool isPending() const noexcept { return m_state && !m_state->cancelled; }
    void cancel() noexcept
    {
        if (m_state)
            m_state->cancelled = true;
        m_state.reset();
    }

    struct State
    {
        std::atomic<bool> cancelled = false;
    };

private:
    friend class InventoryIo;
    explicit IoJob(std::shared_ptr<State> s) : m_state(std::move(s)) {}
    std::shared_ptr<State> m_state;
};

/// How the shared I/O thread treats state.sqlite (InventoryIo::Options).
struct InventoryIoOptions
{
    /// How long a write waits for another connection's lock before it
    /// fails with Conflict (then the Inventory tries again later).
    std::chrono::milliseconds busyTimeout = std::chrono::milliseconds(250);
    /// The delays before each new attempt of a write that failed with
    /// a temporary error (Conflict); one attempt more than delays. After
    /// the last one the error is reported and kept until the next
    /// reconciliation (SERVICE.md §21.5).
    std::vector<std::chrono::milliseconds> retryDelays = {
            std::chrono::seconds(1),  std::chrono::seconds(2),  std::chrono::seconds(4),
            std::chrono::seconds(8),  std::chrono::seconds(16), std::chrono::seconds(30),
            std::chrono::seconds(60), std::chrono::seconds(60), std::chrono::seconds(60)};

    /// Observation points for the tests, called on the I/O thread: before
    /// each write job, before each commit of its connection (holding the
    /// write lock), and once the thread has closed its connection and is
    /// about to end. Empty in the service.
    struct Hooks
    {
        std::function<void()> beforeWrite;
        std::function<void()> beforeCommit;
        std::function<void()> threadEnded;
    };
    Hooks hooks = {};
};

class InventoryIo
{
public:
    using Options = InventoryIoOptions;

    explicit InventoryIo(std::filesystem::path state, Options options = {});
    /// Never waits for I/O (see above). Stops first.
    ~InventoryIo();

    /// Stops writing for good (see above). Never waits; idempotent. Jobs
    /// asked afterwards are dropped (their results never come).
    void stop();
    bool stopping() const noexcept { return m_shared->stopping; }

    InventoryIo(const InventoryIo&) = delete;
    InventoryIo& operator=(const InventoryIo&) = delete;

    const std::filesystem::path& statePath() const noexcept { return m_shared->path; }
    const Options& options() const noexcept { return m_shared->options; }

    /// Runs work(store) on the I/O thread with its connection to
    /// state.sqlite (opened at the first use, and again after a failed
    /// open), then done(result) on the calling thread's loop. `work` returns
    /// a Result; an error to open the file is given to `done` instead.
    template <typename Work, typename Done>
    IoJob write(Work work, Done done)
    {
        using R = std::invoke_result_t<Work&, StateStore&>;
        std::shared_ptr<Shared> shared = m_shared;
        return enqueue(
                [shared, work = std::move(work)]() mutable -> R {
                    ++shared->writes;
                    if (shared->options.hooks.beforeWrite)
                        shared->options.hooks.beforeWrite();
                    // Stopped meanwhile: nothing more is written.
                    if (shared->stopping)
                        return Error::cancelled("the inventory writer has stopped");
                    if (!shared->store) {
                        auto s = StateStore::open(shared->path, static_cast<UnixSeconds>(std::time(nullptr)));
                        if (!s)
                            return std::move(s).error();
                        s->setBusyTimeout(shared->options.busyTimeout);
                        if (auto ok = s->setDurableCommits(false); !ok)
                            return std::move(ok).error();
                        // The store lives inside Shared: a raw pointer, no
                        // cycle.
                        Shared* raw = shared.get();
                        s->setCommitGuard([raw] {
                            if (raw->options.hooks.beforeCommit)
                                raw->options.hooks.beforeCommit();
                            return !raw->stopping;
                        });
                        shared->store.emplace(std::move(s).value());
                    }
                    return work(*shared->store);
                },
                std::move(done));
    }

    /// Runs work() on the I/O thread, without the state connection.
    template <typename Work, typename Done>
    IoJob run(Work work, Done done)
    {
        return enqueue(std::move(work), std::move(done));
    }

    /// Write jobs started so far (attempts included), for the tests.
    int writes() const noexcept { return m_shared->writes; }
    /// Ready once the I/O thread has ended, after destruction.
    std::shared_future<void> stopped() const { return m_stopped; }

private:
    struct Shared
    {
        Shared(std::filesystem::path p, Options o) : path(std::move(p)), options(std::move(o)) {}
        const std::filesystem::path path;
        const Options options;
        std::mutex mutex;
        std::condition_variable wake;
        std::deque<std::function<void()>> queue;
        /// Set by stop(); read by the I/O thread without the mutex.
        std::atomic<bool> stopping = false;
        bool busy = false;
        std::atomic<int> writes = 0;
        /// Used on the I/O thread only.
        std::optional<StateStore> store;
        std::promise<void> stopped;
    };

    template <typename Work, typename Done>
    IoJob enqueue(Work work, Done done)
    {
        auto state = std::make_shared<IoJob::State>();
        auto callables = std::make_shared<std::pair<Work, Done>>(std::move(work), std::move(done));
        base::LoopHandle loop = base::EventLoop::current()->handle();
        post([state, callables, loop] {
            if (state->cancelled)
                return; // not started: nothing to do, nothing to deliver
            if constexpr (std::is_void_v<std::invoke_result_t<Work&>>) {
                callables->first();
                loop.post([state, callables] {
                    if (!state->cancelled)
                        callables->second();
                });
            } else {
                using Result = std::invoke_result_t<Work&>;
                auto result = std::make_shared<Result>(callables->first());
                // False once the loop is gone: the result is dropped.
                loop.post([state, callables, result] {
                    if (!state->cancelled)
                        callables->second(std::move(*result));
                });
            }
        });
        return IoJob(std::move(state));
    }

    void post(std::function<void()> task);
    static void threadMain(std::shared_ptr<Shared> shared);

    std::shared_ptr<Shared> m_shared;
    std::shared_future<void> m_stopped;
    std::thread m_thread;
};

} // namespace cutefish::store::service
