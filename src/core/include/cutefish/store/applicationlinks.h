#pragma once

// Links what a backend reports (installed items, available updates) to the
// applications of the catalogue. Backends do not know the catalogue: the
// host links their answers with these pure functions (D-010, owner's
// decision for step F). The result is for display only; nothing here is a
// source of truth about the system.

#include <cutefish/store/backend.h>
#include <cutefish/store/identifiers.h>
#include <cutefish/store/installed.h>

#include <span>
#include <utility>
#include <vector>

namespace cutefish::store {

/// An offer of the catalogue and the application it belongs to. The same
/// deb package may be an offer of several applications.
using OfferApplication = std::pair<OfferId, AppId>;

/// Sets the application of every installed item that has none and whose
/// offer is in the catalogue (the first application, by id, when several).
/// Items the backend already linked keep their link. Items without an
/// offer in the catalogue stay unlinked.
std::vector<InstalledItem> linkInstalled(std::vector<InstalledItem> items, std::span<const OfferApplication> links);

/// D-010: keeps the updates of applications the catalogue knows, with their
/// application set, and the runtimes (needed by installed Flatpak
/// applications). Everything else (libraries, the kernel, packages without
/// an application) is left out: the store does not manage system updates.
std::vector<AvailableUpdate> applicationUpdates(std::vector<AvailableUpdate> updates,
                                                std::span<const OfferApplication> links);

} // namespace cutefish::store
