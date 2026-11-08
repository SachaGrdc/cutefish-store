#pragma once

// Pure translation of the Flatpak backend (ARCHITECTURE §7.3): ref parsing,
// remote classification, error mapping, permissions and the plan built from
// the operations a FlatpakTransaction resolved. No GLib, no libflatpak, no
// Cutefish; unit-testable in isolation. The core stays independent of
// Flatpak (P-006): this library links it, the core does not.

#include "flatpaktypes.h"

#include <cutefish/store/error.h>
#include <cutefish/store/plan.h>
#include <cutefish/store/result.h>
#include <cutefish/store/source.h>

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cutefish::store::flatpak {

/// Parses `app/ID/arch/branch` or `runtime/ID/arch/branch`. Anything else is
/// InvalidArgument (no guessed metadata).
Result<FlatpakRef> parseRef(std::string_view ref);

/// The `SourceKind` of a remote: Flathub by its URL, anything else
/// third-party (P-011, D-024). `gpgVerify` is reported on the source's URL
/// side but does not change the kind.
SourceKind remoteKind(std::string_view name, std::string_view url);

/// A remote turned into the core's Source: id `flatpak:<scope>:<name>`, the
/// origin is the remote name, the label empty (the interface shows the name).
Result<Source> sourceForRemote(Scope scope, const RemoteInfo& remote);

/// A backend failure mapped from a Flatpak or GIO error (ARCHITECTURE §7.3
/// "Erreurs"). `domain` is `flatpak`, `g-io` or `g-dbus`.
Error mapFlatpakError(std::string_view domain, int code, std::string_view message);

/// Sorts and deduplicates permission strings (`shared=network`), so that two
/// orders or a duplicate never change a plan.
std::vector<std::string> normalisePermissions(std::vector<std::string> permissions);

/// Builds the core Plan of a transaction from the operations a
/// FlatpakTransaction resolved in `ready` (ARCHITECTURE §7.3). `requestedRefs`
/// are the full refs the user asked for; the rest were added by flatpak
/// (runtimes, related refs). One item per operation; a Flatpak plan carries
/// upstream-absent versions and per-item sizes, and permissions per item.
Result<Plan> buildPlan(TransactionKind kind, Scope scope, std::span<const FlatpakOp> ops,
                       std::span<const std::string> requestedRefs);

} // namespace cutefish::store::flatpak
