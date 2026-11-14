#include "backends.h"

#include <algorithm>

namespace cutefish::store::service {

Backends::Backends(std::filesystem::path catalog, std::filesystem::path state)
    : m_catalog(std::move(catalog)), m_io(std::move(state))
{
}

Backends::~Backends()
{
    // Inventories first (they cancel their jobs and stop listening), then
    // their backends; m_io goes last and waits for the started work.
    for (auto& [kind, entry] : m_entries)
        entry.inventory.reset();
}

bool Backends::add(std::unique_ptr<Backend> backend)
{
    const BackendKind kind = backend->kind();
    if (m_entries.contains(kind))
        return false;
    Entry entry;
    entry.backend = std::move(backend);
    entry.inventory = std::make_unique<Inventory>(*entry.backend, m_catalog, m_io);
    m_entries.emplace(kind, std::move(entry));
    return true;
}

Backend* Backends::backend(BackendKind kind) const
{
    const auto it = m_entries.find(kind);
    return it == m_entries.end() ? nullptr : it->second.backend.get();
}

Inventory* Backends::inventory(BackendKind kind) const
{
    const auto it = m_entries.find(kind);
    return it == m_entries.end() ? nullptr : it->second.inventory.get();
}

std::vector<BackendKind> Backends::kinds() const
{
    std::vector<BackendKind> out;
    out.reserve(m_entries.size());
    for (const auto& [kind, entry] : m_entries)
        out.push_back(kind);
    return out;
}

bool Backends::allUnavailable() const
{
    if (m_entries.empty())
        return false; // no backend configured: the service is not degraded
    for (const auto& [kind, entry] : m_entries)
        if (entry.inventory->available() != std::optional<bool>(false))
            return false;
    return true;
}

std::vector<BackendUpdate> Backends::updates() const
{
    std::vector<BackendUpdate> out;
    for (const auto& [kind, entry] : m_entries)
        for (const AvailableUpdate& u : entry.inventory->updates())
            out.push_back({kind, &u});
    // Each inventory's list is already one entry per offer (normalizeUpdates)
    // and an offer id names its backend: the sort alone gives the order, and
    // the unique() below only guards that invariant.
    std::stable_sort(out.begin(), out.end(), [](const BackendUpdate& a, const BackendUpdate& b) {
        return a.update->offer.str() < b.update->offer.str();
    });
    out.erase(std::unique(out.begin(), out.end(),
                          [](const BackendUpdate& a, const BackendUpdate& b) {
                              return a.update->offer == b.update->offer;
                          }),
              out.end());
    return out;
}

std::uint32_t Backends::updateCount() const
{
    return static_cast<std::uint32_t>(updates().size());
}

} // namespace cutefish::store::service
