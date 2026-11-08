#pragma once

// Plain data of the Flatpak backend (ARCHITECTURE §7.3): what the store
// reports and what the pure translation (translate.h) turns into the core's
// types. Nothing here knows GLib or libflatpak, so it is testable in
// isolation and stays out of the core (P-006: the core has no Flatpak
// dependency, this library does).

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cutefish::store::flatpak {

/// A Flatpak ref is `app|runtime/ID/arch/branch`. Applications and runtimes
/// are kept apart: the store never presents a runtime as an application.
enum class RefKind : std::uint8_t {
    App,
    Runtime,
};

/// A parsed ref. The full form is `app/ID/arch/branch` (or `runtime/…`).
struct FlatpakRef
{
    RefKind kind = RefKind::App;
    std::string name;   ///< the component id (`org.gimp.GIMP`).
    std::string arch;   ///< `x86_64`.
    std::string branch; ///< `stable`.

    /// The full ref string the id and the plan carry.
    std::string full() const;
    bool operator==(const FlatpakRef&) const = default;
};

/// A remote of one installation, as the store sees it.
struct RemoteInfo
{
    std::string name;
    std::string url;
    bool disabled = false;
    /// Whether signatures are verified (ARCHITECTURE §9.3).
    bool gpgVerify = true;
    /// AppStream catalogue of the remote, when known: the file the indexer
    /// reads and its icon directory. Empty when the remote has none yet.
    std::string metadataPath;
    std::string iconsPath;
};

/// An installed ref (an application or a runtime).
struct InstalledRef
{
    FlatpakRef ref;
    std::string origin; ///< remote it was installed from; empty when unknown.
};

/// A ref with an available update.
struct UpdateRef
{
    FlatpakRef ref;
    std::string origin;
    /// The commit the update would install; empty when unknown.
    std::string newCommit;
};

/// What the store knows about a remote ref before any change: version,
/// sizes and permissions, for the plan and the permission display
/// (ARCHITECTURE §7.3, `GetPermissions`).
struct RefMetadata
{
    std::optional<std::string> version;
    std::uint64_t downloadSize = 0;
    std::uint64_t installedSize = 0;
    /// Stable permission strings (`shared=network`), sorted and deduplicated.
    std::vector<std::string> permissions;
    bool eol = false;
    std::string eolRebase; ///< rebased-to ref, when end-of-life.
};

/// One operation a transaction resolved to (from the `ready` signal of a
/// FlatpakTransaction). Translated into a PlanItem by translate.h.
enum class OperationType : std::uint8_t {
    Install,
    Update,
    Uninstall,
};

struct FlatpakOp
{
    OperationType type = OperationType::Install;
    FlatpakRef ref;
    std::string remote;    ///< remote it comes from; empty for a removal.
    std::string commit;    ///< build identifier; empty when unknown.
    std::uint64_t downloadSize = 0;
    std::uint64_t installedSize = 0;
    std::vector<std::string> permissions;    ///< after the operation, sorted.
    std::vector<std::string> oldPermissions; ///< before, sorted.
    /// Whether the user asked for it (vs. a runtime flatpak added on its own).
    bool requested = false;
    bool addsSource = false; ///< the operation adds a remote (refused in v1).
    std::string rebaseFrom;  ///< end-of-life rebase source, when any.
};

/// A progress update of a running transaction.
struct FlatpakProgress
{
    /// 0 to 1, or -1 when unknown.
    double fraction = -1;
    /// A display status (`downloading`, `installing`); empty when unknown.
    std::string status;
    /// The ref being processed, for display; may be empty.
    std::string ref;
    /// A cancellation request would be honoured now (always true for Flatpak,
    /// §7.6).
    bool cancellable = true;
};

} // namespace cutefish::store::flatpak
