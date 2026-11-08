#include "readpool.h"

#include <optional>

namespace cutefish::store::service {

Result<SqliteCatalog*> Readers::catalog()
{
    if (m_generation == 0)
        return Error::notFound("no catalogue in service yet");
    if (!m_catalog) {
        auto c = SqliteCatalog::open(m_catalogPath, {.locales = m_locales, .statePath = m_statePath});
        if (!c)
            return std::move(c).error();
        m_catalog = std::make_unique<SqliteCatalog>(std::move(c).value());
    }
    return m_catalog.get();
}

Result<SqliteInstalledState*> Readers::installed()
{
    if (!m_installed) {
        auto s = SqliteInstalledState::open(m_statePath);
        if (!s)
            return std::move(s).error();
        m_installed = std::make_unique<SqliteInstalledState>(std::move(s).value());
    }
    return m_installed.get();
}

Result<StateStore*> Readers::journal()
{
    if (!m_journal) {
        // Opening an existing, current database changes nothing: the main
        // thread created and migrated it before the pool started.
        auto s = StateStore::open(m_statePath, 0);
        if (!s)
            return std::move(s).error();
        m_journal = std::make_unique<StateStore>(std::move(s).value());
    }
    return m_journal.get();
}

Readers& ReadPool::threadReaders(const std::filesystem::path& catalog, const std::filesystem::path& state,
                                 const std::vector<std::string>& locales, std::uint64_t generation)
{
    // One set per pool thread; a new generation means a new catalogue file.
    thread_local std::optional<Readers> readers;
    if (!readers || readers->generation() != generation)
        readers.emplace(catalog, state, locales, generation);
    return *readers;
}

} // namespace cutefish::store::service
