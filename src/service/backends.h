#pragma once

// The registry of the service's backends (H.1): one Backend and one
// Inventory per BackendKind, owned here. Not a global: a StoreService owns
// one instance. It gives a reliable lookup by BackendKind and answers
// whether all configured backends are unusable (D-E: the service stays ready
// as long as one backend works, and degrades only when all of them are).
//
// Each Inventory writes its own (backend, scope) rows of state.sqlite, on its
// own I/O thread and connection, so two backends can reconcile concurrently
// (SQLite's busy timeout arbitrates the shared file).

#include "inventory.h"

#include <cutefish/store/backend.h>

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace cutefish::store::service {

class Backends
{
public:
    /// Creates the Inventory for `backend` and owns both. The caller wires
    /// the Inventory's callbacks (onAvailabilityChanged, onInstalledChanged,
    /// onUpdatesChanged) afterwards.
    void add(std::unique_ptr<Backend> backend, std::filesystem::path catalog, std::filesystem::path state);

    Backend* backend(BackendKind kind) const;
    Inventory* inventory(BackendKind kind) const;

    bool empty() const noexcept { return m_entries.empty(); }
    /// Sorted BackendKinds present.
    std::vector<BackendKind> kinds() const;
    /// True when at least one backend is configured and none of them is
    /// usable (D-E). False while an inventory has not answered yet.
    bool allUnavailable() const;

private:
    struct Entry
    {
        // The backend outlives its inventory (the inventory references it);
        // declared first, destroyed last.
        std::unique_ptr<Backend> backend;
        std::unique_ptr<Inventory> inventory;
    };

    std::map<BackendKind, Entry> m_entries;
};

} // namespace cutefish::store::service
