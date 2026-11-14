#pragma once

#include <cutefish/store/identifiers.h>
#include <cutefish/store/installed.h>
#include <cutefish/store/installedstate.h>
#include <cutefish/store/result.h>
#include <cutefish/store/source.h>
#include <cutefish/store/transaction.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cutefish::store {

/// Seconds since the Unix epoch. The core has no clock (D-019): every date
/// comes from the host.
using UnixSeconds = std::int64_t;

/// Journal retention (D-018): the stricter of the two limits applies, and a
/// transaction that is not finished is never removed.
inline constexpr std::size_t kMaxFinishedTransactions = 200;
inline constexpr UnixSeconds kMaxTransactionAge = 90 * 24 * 60 * 60;

/// An offer a transaction acts on, as the client designated it.
struct TransactionOffer
{
    OfferId offer;
    std::optional<AppId> app = {};

    bool operator==(const TransactionOffer&) const = default;
};

/// One transaction as kept in the journal.
struct TransactionRecord
{
    /// Chosen by the host (a UUID), stable across restarts. 1 to 255
    /// printable ASCII bytes without spaces.
    std::string id;
    BackendKind backend;
    Scope scope;
    /// At least one, without duplicates.
    std::vector<TransactionOffer> offers;
    TransactionSnapshot snapshot;
    /// The backend's own handle, when it has one that outlives the service
    /// (the PackageKit transaction path, for re-attaching); empty otherwise.
    std::string backendRef = {};
    UnixSeconds created;
    UnixSeconds updated;
    /// Set exactly when the state is terminal.
    std::optional<UnixSeconds> finished = {};
};

/// What a restart does with each unfinished transaction (SERVICE.md §5.6).
enum class RecoveryAction : std::uint8_t {
    None,         ///< Already finished.
    Interrupt,    ///< queued, planning, running without a backend that kept it.
    Cancel,       ///< awaiting-confirmation: an old plan is never executed.
    TryReattach,  ///< running PackageKit transaction with a handle: the host
                  ///< checks whether packagekitd still has it.
};

RecoveryAction recoveryAction(const TransactionRecord& record) noexcept;

/// Result of StateStore::recoverAfterRestart().
struct RecoveryReport
{
    std::vector<std::string> interrupted;
    std::vector<std::string> cancelled;
    /// Left as they were: the host re-attaches them, or interrupts them if
    /// the backend no longer knows them.
    std::vector<TransactionRecord> toReattach;
    /// An interrupted transaction may have changed the system: the host must
    /// reconcile the installed state with the backends.
    bool reconcile = false;
};

/// state.sqlite: what the store knows about the system and its own work
/// (P-003, SERVICE.md §8). One writer: the host's main thread. Readers on
/// other threads use SqliteInstalledState.
///
/// What it is not: the source of truth of what is installed. PackageKit and
/// libflatpak are. The `installed` table is a copy of the last inventory of
/// each backend, kept so that the interface never waits for them; it is
/// replaced as a whole by each inventory and is never used to decide what to
/// install or remove (that comes from the backend's plan). It changes only
/// through a full inventory, never by an optimistic update after a
/// transaction (SERVICE.md §8.3).
///
/// Durability: WAL journal with synchronous=FULL, every change in one SQLite
/// transaction. After a crash or a power loss the file holds the last
/// committed state; a change is either entirely there or not at all.
class StateStore final : public InstalledState
{
public:
    /// Opens the file, creating it and its schema if needed, and migrating
    /// an older schema. Refuses, without modifying it, a file with a newer
    /// schema, a file that is not a state database, and a damaged file
    /// (the host may then move it aside: it only holds a cache and the
    /// journal, SERVICE.md §8).
    static Result<StateStore> open(const std::filesystem::path& path, UnixSeconds now);

    /// Errors of the methods below, by code (H.2): Conflict when another
    /// connection held the file past the busy timeout (SQLITE_BUSY,
    /// SQLITE_LOCKED: temporary, the same call may succeed later);
    /// InvalidArgument for a damaged file or data the schema refuses
    /// (permanent); Internal otherwise (I/O error, full disk, read-only
    /// file). A failed write changes nothing.

    /// How long this connection waits for another one's lock before a
    /// Conflict (5 s by default).
    void setBusyTimeout(std::chrono::milliseconds timeout) noexcept;
    /// Whether each commit of this connection waits for the disk
    /// (synchronous=FULL, the default) or not (NORMAL: in WAL mode the
    /// file stays consistent after a crash, but the last commits of this
    /// connection may be lost at a power cut). For a connection that only
    /// writes what can be rebuilt, such as the installed state (a cache).
    Result<void> setDurableCommits(bool durable);

    StateStore(StateStore&&) noexcept;
    StateStore& operator=(StateStore&&) noexcept;
    ~StateStore() override;

    // --- Installed state (a cache, see above) -------------------------------

    /// Replaces everything recorded for one backend and one installation by
    /// a new inventory, atomically. Every item must belong to that backend
    /// and installation; offers are unique.
    Result<void> replaceInstalled(BackendKind backend, Scope scope, std::span<const InstalledItem> items,
                                  UnixSeconds now);
    /// The same for several installations of one backend, in one SQLite
    /// transaction (H.2): every listed scope is replaced, or none is. Each
    /// item must belong to `backend` and to one of `scopes`.
    Result<void> replaceInstalled(BackendKind backend, std::span<const Scope> scopes,
                                  std::span<const InstalledItem> items, UnixSeconds now);
    /// When replaceInstalled() last ran for this backend and installation.
    Result<std::optional<UnixSeconds>> lastReconciled(BackendKind backend, Scope scope) const;

    Result<std::vector<InstalledItem>> items(std::optional<BackendKind> backend) const override;
    Result<std::optional<InstalledItem>> find(const OfferId& offer) const override;
    Result<std::vector<InstalledItem>> forApplication(const AppId& app) const override;

    // --- Catalogue refresh (D-015) -----------------------------------------

    /// Records a successful refresh, put in service: every listed source and
    /// the global date. A failed refresh is never recorded.
    Result<void> recordRefresh(std::span<const SourceId> sources, UnixSeconds now);
    /// Date of the last successful refresh (LastRefresh), if any.
    Result<std::optional<UnixSeconds>> lastRefresh() const;
    Result<std::optional<UnixSeconds>> lastRefresh(const SourceId& source) const;

    // --- Transaction journal (D-017, D-018) --------------------------------

    /// Inserts or replaces a transaction. When it is finished, the
    /// retention of D-018 is applied at its `finished` date.
    Result<void> saveTransaction(const TransactionRecord& record);
    Result<std::optional<TransactionRecord>> transaction(std::string_view id) const;
    /// Unfinished transactions, oldest first.
    Result<std::vector<TransactionRecord>> openTransactions() const;
    /// Finished transactions, most recently finished first.
    Result<std::vector<TransactionRecord>> history(std::size_t limit) const;
    /// The unfinished transaction acting on this offer, if any (Busy).
    Result<std::optional<std::string>> openTransactionFor(const OfferId& offer) const;
    /// Applies D-018 at `now`; returns how many transactions were removed.
    Result<std::size_t> purgeHistory(UnixSeconds now);
    /// Applies recoveryAction() to every unfinished transaction, in one
    /// SQLite transaction. Interrupted and cancelled ones are finished at
    /// `now` (and the retention applied); the others are returned.
    Result<RecoveryReport> recoverAfterRestart(UnixSeconds now);

private:
    struct Impl;
    explicit StateStore(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> d;
};

/// Read-only view of the installed state of state.sqlite, for the host's
/// read threads: one instance per thread, each with its own connection. Every
/// call is a single query, so it sees one committed state, never a half
/// replaced inventory (WAL snapshot isolation). The file must have been
/// created by StateStore::open.
class SqliteInstalledState final : public InstalledState
{
public:
    static Result<SqliteInstalledState> open(const std::filesystem::path& path);

    SqliteInstalledState(SqliteInstalledState&&) noexcept;
    SqliteInstalledState& operator=(SqliteInstalledState&&) noexcept;
    ~SqliteInstalledState() override;

    Result<std::vector<InstalledItem>> items(std::optional<BackendKind> backend) const override;
    Result<std::optional<InstalledItem>> find(const OfferId& offer) const override;
    Result<std::vector<InstalledItem>> forApplication(const AppId& app) const override;

private:
    struct Impl;
    explicit SqliteInstalledState(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> d;
};

} // namespace cutefish::store
