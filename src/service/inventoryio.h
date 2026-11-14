#pragma once

// The one I/O thread and the one connection to state.sqlite that every
// Inventory of a service shares (H.2). The inventories of two backends
// write disjoint rows ((backend, scope) of `installed`, and their own
// `meta` keys), so SQLite alone would keep the data right; but each write is
// an fsync'd transaction, and two writers of the same process would then
// wait on each other through the busy timeout (up to 5 s, after which the
// write is dropped) and compete with the journal writes of the main thread.
// One thread here serialises them by construction: in-process, no
// inventory write ever waits for another, the order of the writes is the
// order of the requests, and the main thread meets at most one inventory
// write at a time. The catalogue reads that link the inventory run here
// too, behind the writes.
//
// Lifetime: destroyed after every Inventory that uses it (Backends declares
// it first). The destructor waits for the work that has started; results
// still on their way to the loop are dropped by the inventories' Jobs.

#include <cutefish/store/result.h>
#include <cutefish/store/statestore.h>

#include <base/thread_pool.h>

#include <ctime>
#include <filesystem>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

namespace cutefish::store::service {

class InventoryIo
{
public:
    explicit InventoryIo(std::filesystem::path state) : m_disk(std::make_shared<Disk>(std::move(state))) {}

    InventoryIo(const InventoryIo&) = delete;
    InventoryIo& operator=(const InventoryIo&) = delete;

    const std::filesystem::path& statePath() const noexcept { return m_disk->path; }

    /// Runs work(store) on the I/O thread with its connection to
    /// state.sqlite, opened at the first use, then done(result) on the
    /// calling thread's loop. `work` returns a Result; an error to open the
    /// file is given to `done` instead.
    template <typename Work, typename Done>
    base::Job write(Work work, Done done)
    {
        using R = std::invoke_result_t<Work&, StateStore&>;
        return m_pool.run(
                [disk = m_disk, work = std::move(work)]() mutable -> R {
                    if (!disk->store) {
                        auto s = StateStore::open(disk->path, static_cast<UnixSeconds>(std::time(nullptr)));
                        if (!s)
                            return std::move(s).error();
                        disk->store.emplace(std::move(s).value());
                    }
                    return work(*disk->store);
                },
                std::move(done));
    }

    /// Runs work() on the I/O thread, without the state connection.
    template <typename Work, typename Done>
    base::Job run(Work work, Done done)
    {
        return m_pool.run(std::move(work), std::move(done));
    }

private:
    struct Disk
    {
        explicit Disk(std::filesystem::path p) : path(std::move(p)) {}
        std::filesystem::path path;
        /// Used on the I/O thread only.
        std::optional<StateStore> store;
    };

    // The pool is destroyed first (it waits for the started work), then the
    // connection, if no started work still holds it.
    std::shared_ptr<Disk> m_disk;
    base::ThreadPool m_pool{1};
};

} // namespace cutefish::store::service
