#pragma once

// The registry of the service's backends (H.1): one Backend and one
// Inventory per BackendKind, owned here. Not a global: a StoreService owns
// one instance. It gives a reliable lookup by BackendKind and answers
// whether all configured backends are unusable (D-E: the service stays ready
// as long as one backend works, and degrades only when all of them are).
//
// The inventories share one I/O thread and one connection to state.sqlite
// (inventoryio.h, H.2): each writes its own (backend, scope) rows, one write
// after the other. Destroying the registry never waits for that thread's
// I/O (inventoryio.h).
//
// The aggregation of H.2: updates() is what GetUpdates lists and what
// UpdateCount counts, so the two always agree.

#include "inventory.h"
#include "inventoryio.h"

#include <cutefish/store/backend.h>

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace cutefish::store::service {

/// One entry of the aggregated updates: the update and the backend that
/// offers it.
struct BackendUpdate
{
    BackendKind backend;
    const AvailableUpdate* update;
};

class Backends
{
public:
    /// `catalog` and `state`: the files every inventory links with and
    /// records into.
    Backends(std::filesystem::path catalog, std::filesystem::path state, InventoryIo::Options io = {});
    ~Backends();

    Backends(const Backends&) = delete;
    Backends& operator=(const Backends&) = delete;

    /// Creates the Inventory for `backend` and owns both. The caller wires
    /// the Inventory's callbacks (onAvailabilityChanged, onInstalledChanged,
    /// onUpdatesChanged) afterwards. One backend per kind: a second one of
    /// the same kind is refused (false).
    bool add(std::unique_ptr<Backend> backend);

    Backend* backend(BackendKind kind) const;
    Inventory* inventory(BackendKind kind) const;

    bool empty() const noexcept { return m_entries.empty(); }
    /// Sorted BackendKinds present.
    std::vector<BackendKind> kinds() const;
    /// True when at least one backend is configured and none of them is
    /// usable (D-E). False while an inventory has not answered yet.
    bool allUnavailable() const;

    /// The updates of every backend (H.2), sorted by offer id, one entry per
    /// offer. An offer id names its backend, so two backends never list the
    /// same offer: two updates of one application from two backends, two
    /// scopes or two remotes are two entries. A backend that is not usable
    /// has none (its inventory cleared them). Valid until the next change of
    /// an inventory.
    std::vector<BackendUpdate> updates() const;
    /// updates().size(): the UpdateCount property.
    std::uint32_t updateCount() const;

    const InventoryIo& io() const noexcept { return m_io; }
    /// InventoryIo::stop(): no inventory writes any more (shutdown).
    void stopWriting() { m_io.stop(); }

private:
    struct Entry
    {
        // The backend outlives its inventory (the inventory references it);
        // declared first, destroyed last.
        std::unique_ptr<Backend> backend;
        std::unique_ptr<Inventory> inventory;
    };

    std::filesystem::path m_catalog;
    // Outlives every inventory: declared before them, destroyed after.
    InventoryIo m_io;
    std::map<BackendKind, Entry> m_entries;
};

} // namespace cutefish::store::service
