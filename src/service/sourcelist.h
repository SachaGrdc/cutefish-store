#pragma once

// What ListSources answers (H.5, SERVICE.md §25): the sources each backend
// has configured now, enabled or not, completed by what the catalogue knows
// of them (offers, last refresh), and the catalogue's sources no backend
// reports (the sources file, a backend that is not usable). The rules that
// say what may be changed are here too, so that ListSources and the methods
// that change a source never disagree. Pure: no bus, no thread.

#include <cutefish/store/backend.h>
#include <cutefish/store/sourcefingerprint.h>
#include <cutefish/store/statestore.h>

#include <base/dbus_types.h>

#include <optional>
#include <string>
#include <vector>

namespace cutefish::store::service {

/// What one backend reports of its sources, and whether it changes them.
struct BackendSources
{
    BackendKind kind;
    bool userSourceChanges = false;
    std::vector<BackendSource> sources;
};

/// A source of the catalogue in service, with its last successful refresh.
struct CatalogueSource
{
    RecordedSource recorded;
    std::optional<UnixSeconds> lastRefresh;
};

/// Why the service refuses this request on this source before any
/// transaction (H.5), as an Error (empty when it may go on):
/// - NotSupported: the backend changes no source (PackageKit, D-039);
/// - NotAllowed: a source of the system installation (D-011); enabling an
///   enabled source or disabling a disabled one; removing a source
///   something installed came from, or of which the backend cannot tell
///   (D-041).
/// Enabling a source whose signatures are not checked is not refused here:
/// its transaction fails as untrusted (D-044), see untrustedEnable().
std::optional<Error> sourceRequestRefusal(const BackendSource& source, bool userSourceChanges,
                                          SourceRequest::Action action);

/// D-044: enabling this source would let something come from a source
/// whose signatures are not checked, or of which the backend cannot tell.
bool untrustedEnable(const BackendSource& source);

/// The entries of ListSources, sorted by id. A configured source:
/// `id`, `backend`, `name`, `origin`, `url`, `scope`, `kind`, `enabled`,
/// `configured` (true), `can-toggle`, `can-remove`, `suggested` (false:
/// nothing is suggested before P-011), `offers` (u, those of the catalogue
/// sources it feeds), `last-refresh` (t, the oldest of them, absent when
/// none was refreshed), `gpg-verify` (b) and `installed-count` (u) when the
/// backend can tell. A catalogue source no backend reports: the same keys
/// from the catalogue, `configured` false, nothing it allows to change.
std::vector<base::dbus::VariantMap> sourceList(const std::vector<BackendSources>& backends,
                                               const std::vector<CatalogueSource>& catalogue);

} // namespace cutefish::store::service
