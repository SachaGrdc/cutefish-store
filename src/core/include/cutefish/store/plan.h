#pragma once

#include <cutefish/store/identifiers.h>
#include <cutefish/store/offer.h>
#include <cutefish/store/result.h>
#include <cutefish/store/source.h>
#include <cutefish/store/version.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cutefish::store {

/// What a transaction was asked to do (ARCHITECTURE §5.2). Refresh and
/// update-all are organised by the service and carry no plan of their own.
enum class TransactionKind : std::uint8_t {
    Install,
    Remove,
    Update,
};

/// What a plan does to one package or ref.
enum class PlanOperation : std::uint8_t {
    Install,
    Update,
    Downgrade,
    Remove,
};

/// What the package or ref is, as far as the plan is concerned.
enum class PlanItemKind : std::uint8_t {
    Application, ///< An application the store presents.
    Runtime,     ///< A runtime or extension needed by an application.
    Package,     ///< Any other package (a dependency of a deb, for example).
};

/// Stable lowercase names: "install", "remove", "update", "downgrade",
/// "application", "runtime", "package".
std::string_view toString(TransactionKind kind) noexcept;
std::string_view toString(PlanOperation operation) noexcept;
std::string_view toString(PlanItemKind kind) noexcept;
std::optional<TransactionKind> parseTransactionKind(std::string_view text) noexcept;
std::optional<PlanOperation> parsePlanOperation(std::string_view text) noexcept;
std::optional<PlanItemKind> parsePlanItemKind(std::string_view text) noexcept;

/// Input of one plan item. Filled by a backend from its own simulation
/// (PackageKit SIMULATE, the `ready` signal of a FlatpakTransaction); the
/// core never computes a plan itself.
struct PlanItemSpec
{
    PlanOperation operation;
    PlanItemKind kind;
    /// True for what the user asked for; false for what the package manager
    /// added on its own (dependency, runtime, cascaded removal).
    bool requested;
    /// Package name or full Flatpak ref. Unique within a plan.
    std::string package;
    /// Version before the operation (absent for an install) and after it
    /// (absent for a removal), when known.
    std::optional<Version> fromVersion = {};
    std::optional<Version> toVersion = {};
    /// Repository origin or remote the item comes from; empty when unknown
    /// or irrelevant (removal).
    std::string origin = {};
    /// Immutable build identifier, when the backend has one (a Flatpak
    /// commit); empty otherwise.
    std::string revision = {};
    std::uint64_t downloadSize = 0;
    /// Permissions after and before the operation, as stable strings chosen
    /// by the backend (for example `shared=network`). Order and duplicates
    /// do not matter: Plan::create sorts and deduplicates them.
    std::vector<std::string> permissions = {};
    std::vector<std::string> oldPermissions = {};
    /// The operation adds a source (a remote) to the system.
    bool addsSource = false;
    /// Non-empty: the item replaces this package or ref (end-of-life
    /// rebase).
    std::string rebaseFrom = {};
    /// False when the package manager reports the item as not
    /// authenticated.
    bool trusted = true;
};

/// One validated plan item. Immutable.
class PlanItem
{
public:
    PlanOperation operation() const noexcept { return m_spec.operation; }
    PlanItemKind kind() const noexcept { return m_spec.kind; }
    bool requested() const noexcept { return m_spec.requested; }
    const std::string& package() const noexcept { return m_spec.package; }
    const std::optional<Version>& fromVersion() const noexcept { return m_spec.fromVersion; }
    const std::optional<Version>& toVersion() const noexcept { return m_spec.toVersion; }
    const std::string& origin() const noexcept { return m_spec.origin; }
    const std::string& revision() const noexcept { return m_spec.revision; }
    std::uint64_t downloadSize() const noexcept { return m_spec.downloadSize; }
    /// Sorted, without duplicates.
    std::span<const std::string> permissions() const noexcept { return m_spec.permissions; }
    std::span<const std::string> oldPermissions() const noexcept { return m_spec.oldPermissions; }
    /// Permissions present after the operation and not before it; sorted.
    std::vector<std::string> addedPermissions() const;
    bool addsSource() const noexcept { return m_spec.addsSource; }
    const std::string& rebaseFrom() const noexcept { return m_spec.rebaseFrom; }
    bool trusted() const noexcept { return m_spec.trusted; }

private:
    friend class Plan;
    explicit PlanItem(PlanItemSpec spec) : m_spec(std::move(spec)) {}
    PlanItemSpec m_spec;
};

/// Input of Plan::create.
struct PlanSpec
{
    TransactionKind kind;
    /// A plan belongs to one backend and one installation: a request that
    /// spans several is split by the service (SERVICE.md §4.4).
    BackendKind backend;
    Scope scope;
    std::vector<PlanItemSpec> items;
    /// Change of disk usage announced by the backend, when known.
    std::int64_t installedSizeChange = 0;
    /// The total to download, for a backend that knows only the total
    /// (PackageKit's DownloadSizeRemaining after a simulation, not split
    /// per package). Absent: the sum of the items' sizes.
    std::optional<std::uint64_t> downloadSize = {};
};

/// What a transaction will do, as computed by its backend before anything
/// is changed (P-008). Immutable; the order of the items is the backend's.
class Plan
{
public:
    /// Rules:
    /// - at least one item, and at least one requested item;
    /// - a requested item does what the transaction asks: install for
    ///   Install, remove for Remove, update for Update;
    /// - package names are unique, 1 to 255 printable ASCII bytes without
    ///   spaces; origin, revision and rebaseFrom, when present, too;
    /// - no target version for a removal, no previous version for an
    ///   install;
    /// - a PackageKit plan is system-wide with Debian versions; a Flatpak
    ///   plan has upstream versions;
    /// - permissions are valid UTF-8 without control characters, 1 to 1024
    ///   bytes each, at most 256 per list.
    static Result<Plan> create(PlanSpec spec);

    TransactionKind kind() const noexcept { return m_kind; }
    BackendKind backend() const noexcept { return m_backend; }
    Scope scope() const noexcept { return m_scope; }
    std::span<const PlanItem> items() const noexcept { return m_items; }
    /// The item for this package or ref, or nullptr.
    const PlanItem* find(std::string_view package) const noexcept;
    /// The backend's total when it gave one (PlanSpec::downloadSize),
    /// otherwise the sum of the items' download sizes.
    std::uint64_t downloadSize() const noexcept;
    /// The download size is the backend's total, not split per item.
    bool hasTotalDownloadSize() const noexcept { return m_downloadSize.has_value(); }
    std::int64_t installedSizeChange() const noexcept { return m_installedSizeChange; }

private:
    Plan(TransactionKind kind, BackendKind backend, Scope scope, std::vector<PlanItem> items,
         std::int64_t installedSizeChange, std::optional<std::uint64_t> downloadSize)
        : m_items(std::move(items)), m_installedSizeChange(installedSizeChange), m_downloadSize(downloadSize),
          m_kind(kind), m_backend(backend), m_scope(scope)
    {
    }

    std::vector<PlanItem> m_items;
    std::int64_t m_installedSizeChange;
    std::optional<std::uint64_t> m_downloadSize;
    TransactionKind m_kind;
    BackendKind m_backend;
    Scope m_scope;
};

/// Most offers one request may name (as many as a D-Bus array accepts,
/// ARCHITECTURE §5.2).
inline constexpr std::size_t kMaxRequestOffers = 200;

/// What a transaction asks a backend to plan and execute: one kind, one
/// backend, one installation (a request that spans several is split by the
/// service, SERVICE.md §4.4).
class PlanRequest
{
public:
    /// Rules: 1 to 200 offers, without duplicates, every id readable by
    /// decodeOfferId(), all of the same backend and installation.
    static Result<PlanRequest> create(TransactionKind kind, std::vector<OfferId> offers);

    TransactionKind kind() const noexcept { return m_kind; }
    BackendKind backend() const noexcept { return m_targets.front().backend; }
    Scope scope() const noexcept { return m_targets.front().scope; }
    std::span<const OfferId> offers() const noexcept { return m_offers; }
    /// The decoded offers, in the same order.
    std::span<const OfferAddress> targets() const noexcept { return m_targets; }

private:
    PlanRequest(TransactionKind kind, std::vector<OfferId> offers, std::vector<OfferAddress> targets)
        : m_offers(std::move(offers)), m_targets(std::move(targets)), m_kind(kind)
    {
    }
    std::vector<OfferId> m_offers;
    std::vector<OfferAddress> m_targets;
    TransactionKind m_kind;
};

/// Whether a plan computed by a backend answers the request: same kind,
/// backend and installation; every requested item is one of the requested
/// packages; for an install or a removal, every requested package is in the
/// plan. An update may leave out a package already up to date, but not all
/// of them. A plan that does not answer its request is a backend error:
/// the host refuses it (InvalidArgument) instead of showing it.
Result<void> checkPlanForRequest(const PlanRequest& request, const Plan& plan);

} // namespace cutefish::store
