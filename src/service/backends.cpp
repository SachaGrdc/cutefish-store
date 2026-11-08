#include "backends.h"

namespace cutefish::store::service {

void Backends::add(std::unique_ptr<Backend> backend, std::filesystem::path catalog, std::filesystem::path state)
{
    const BackendKind kind = backend->kind();
    Entry entry;
    entry.backend = std::move(backend);
    entry.inventory = std::make_unique<Inventory>(*entry.backend, std::move(catalog), std::move(state));
    m_entries.emplace(kind, std::move(entry));
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

} // namespace cutefish::store::service
