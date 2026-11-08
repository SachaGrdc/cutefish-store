#pragma once

// The read pool (SERVICE.md §2.2, rule 2): SQLite reads never run on the
// main thread. Each pool thread keeps its own connections, opened on first
// use and reopened when the catalogue generation changes: a SqliteCatalog
// with state.sqlite attached (§8.4), a SqliteInstalledState, and a
// StateStore used only to read the journal and the refresh dates. The
// result comes back on the main loop.

#include <cutefish/store/result.h>
#include <cutefish/store/sqlitecatalog.h>
#include <cutefish/store/statestore.h>

#include <base/thread_pool.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace cutefish::store::service {

/// The connections of one pool thread, for one generation.
class Readers
{
public:
    Readers(const std::filesystem::path& catalog, const std::filesystem::path& state,
            const std::vector<std::string>& locales, std::uint64_t generation)
        : m_catalogPath(catalog), m_statePath(state), m_locales(locales), m_generation(generation)
    {
    }

    /// NotFound when there is no catalogue in service yet (generation 0).
    Result<SqliteCatalog*> catalog();
    Result<SqliteInstalledState*> installed();
    Result<StateStore*> journal();

    std::uint64_t generation() const noexcept { return m_generation; }

private:
    std::filesystem::path m_catalogPath;
    std::filesystem::path m_statePath;
    std::vector<std::string> m_locales;
    std::uint64_t m_generation;
    std::unique_ptr<SqliteCatalog> m_catalog;
    std::unique_ptr<SqliteInstalledState> m_installed;
    std::unique_ptr<StateStore> m_journal;
};

class ReadPool
{
public:
    ReadPool(std::filesystem::path catalog, std::filesystem::path state, std::vector<std::string> locales,
             int threads = 2)
        : m_catalog(std::move(catalog)), m_state(std::move(state)), m_locales(std::move(locales)), m_pool(threads)
    {
    }

    /// Main thread only. Readers of an older generation reopen their
    /// connections at their next work.
    void setGeneration(std::uint64_t generation) noexcept { m_generation = generation; }
    std::uint64_t generation() const noexcept { return m_generation; }

    /// Runs work(readers) on a pool thread, then done(result) on the calling
    /// thread's loop. The result is delivered even if the caller forgets the
    /// job; it is dropped if the loop is gone.
    template <typename R>
    void read(std::function<R(Readers&)> work, std::function<void(R)> done)
    {
        auto task = [catalog = m_catalog, state = m_state, locales = m_locales, generation = m_generation,
                     work = std::move(work)]() mutable -> R {
            return work(threadReaders(catalog, state, locales, generation));
        };
        m_pool.run(std::move(task), std::move(done)).detach();
    }

private:
    static Readers& threadReaders(const std::filesystem::path& catalog, const std::filesystem::path& state,
                                  const std::vector<std::string>& locales, std::uint64_t generation);

    std::filesystem::path m_catalog;
    std::filesystem::path m_state;
    std::vector<std::string> m_locales;
    std::uint64_t m_generation = 0;
    base::ThreadPool m_pool;
};

} // namespace cutefish::store::service
