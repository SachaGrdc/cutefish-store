#pragma once

#include <cutefish/store/identifiers.h>
#include <cutefish/store/installed.h>
#include <cutefish/store/plan.h>
#include <cutefish/store/result.h>
#include <cutefish/store/source.h>
#include <cutefish/store/sourcefingerprint.h>
#include <cutefish/store/version.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cutefish::store {

/// Receives the result of an asynchronous backend operation.
template <typename T>
using Completion = std::function<void(Result<T>)>;

/// What a backend can do. Static: it does not change while the backend
/// lives. Differences between backends stay visible (ARCHITECTURE §7.1):
/// the host reads them instead of assuming.
struct BackendCapabilities
{
    /// Installations the backend manages. PackageKit: system only.
    bool userScope = false;
    bool systemScope = false;
    /// permissions() answers; otherwise NotSupported (PackageKit).
    bool permissions = false;
    /// refresh() rereads the repositories' metadata.
    bool refresh = false;
    /// A running transaction may be cancelled at any moment. Otherwise only
    /// when the last Progress says `cancellable` (PackageKit's AllowCancel),
    /// and never immediately (SERVICE.md §6.5).
    bool cancelAnytime = false;
    /// A running transaction outlives the service and can be re-attached
    /// after a restart (PackageKit: it lives in packagekitd).
    bool reattach = false;
    /// The execution plan checked by execute() is the plan of the
    /// transaction that runs (Flatpak's second pass). When false
    /// (PackageKit), it is a new simulation, a separate transaction: the
    /// check narrows the window between plan and execution without closing
    /// it (D-017). Never presented as an absolute guarantee.
    bool verifiesExecutedPlan = false;
    /// changeSource() enables, disables and removes the sources of the user
    /// installation (H.5). Never those of the system installation (D-011),
    /// and never adds one. PackageKit: false, its repositories are listed
    /// read-only (D-039).
    bool userSourceChanges = false;
};

/// Phase of a running operation (ARCHITECTURE §5.3, property `Phase`).
enum class OperationPhase : std::uint8_t {
    WaitingForAuth,
    WaitingForLock,
    Downloading,
    Installing,
    Removing,
    Finishing,
};

/// "waiting-for-auth", "waiting-for-lock", "downloading", "installing",
/// "removing", "finishing".
std::string_view toString(OperationPhase phase) noexcept;

/// Progress of a running operation, as normalised by the backend.
struct Progress
{
    std::optional<OperationPhase> phase = {};
    /// 0 to 1, or -1 when unknown.
    double fraction = -1;
    std::uint64_t downloadedBytes = 0;
    std::uint64_t totalBytes = 0; ///< 0 when unknown.
    /// A cancellation request would be honoured now.
    bool cancellable = false;
    /// Package or ref being processed, for display; may be empty.
    std::string item = {};
};

/// Callbacks of a long operation. Each is optional. Like completions, they
/// are called on the host's main thread through `post`, in order, never
/// from inside the method that started the operation, and never after its
/// completion.
struct OperationEvents
{
    /// execute() only: the execution plan passed mayExecute() and the
    /// backend is about to change the system. Called at most once, before
    /// any progress of the execution. The host calls
    /// Transaction::verifyExecutionPlan() with this plan.
    std::function<void(const Plan&)> started = {};
    std::function<void(const Progress&)> progress = {};
    /// The backend's own handle of the operation, when it outlives the
    /// service (PackageKit transaction path): for the journal, so that a
    /// restart can re-attach (TransactionRecord::backendRef).
    std::function<void(const std::string&)> handle = {};
};

/// How an execution ended without an error.
struct ExecutionResult
{
    enum class Outcome : std::uint8_t {
        /// The plan was executed.
        Completed,
        /// The execution plan needed a new confirmation (mayExecute() was
        /// false): nothing was executed. `freshPlan` holds that plan.
        PlanChanged,
    };
    Outcome outcome;
    std::optional<Plan> freshPlan = {};
};

/// An update the backend offers for something installed (ARCHITECTURE
/// §5.4, `GetUpdates`). Applications only (D-010), plus the runtimes that
/// installed Flatpak applications use.
struct AvailableUpdate
{
    OfferId offer;
    std::optional<AppId> app = {};
    PlanItemKind kind = PlanItemKind::Application;
    std::optional<Version> fromVersion = {};
    std::optional<Version> toVersion = {};
    std::uint64_t downloadSize = 0;
};

/// A configured source with the files the indexer reads for it
/// (ARCHITECTURE §6.2, `catalogSources`).
struct SourceMetadata
{
    Source source;
    SourceFiles files;
};

/// A source as the backend has it configured now (H.5, `ListSources`),
/// enabled or not, with what the backend can tell about it. Unknown facts
/// stay unknown: nothing here is guessed.
struct BackendSource
{
    Source source;
    /// The catalogue sources it feeds, when their ids differ from
    /// source.id(): a Debian repository feeds one DEP-11 source per
    /// component. Empty: the catalogue source of the same id, if any.
    std::vector<SourceId> catalogueSources = {};
    /// Whether the package manager checks the signatures of what comes from
    /// it (Flatpak's gpg-verify). Unknown when the backend cannot tell
    /// (PackageKit does not report it).
    std::optional<bool> verifiesSignatures = {};
    /// How many installed items (applications and runtimes) came from it.
    /// Unknown when the backend cannot tell.
    std::optional<std::uint32_t> installedItems = {};
};

/// A change asked for one configured source (H.5). There is no addition:
/// adding a source is not part of the contract (D-040).
struct SourceRequest
{
    enum class Action : std::uint8_t {
        Enable,
        Disable,
        Remove,
    };
    SourceId source;
    Action action = Action::Disable;
};

/// "enable", "disable", "remove".
std::string_view toString(SourceRequest::Action action) noexcept;

/// What changed outside the store (an `apt` or `flatpak` command, another
/// tool), as far as the backend can tell.
struct BackendChanges
{
    bool installed = false;
    bool updates = false;
    bool sources = false;
    /// externalActivity() changed.
    bool activity = false;
};

/// What the package manager behind a backend is busy with for another
/// program (H.2): a system update from Cutefish Settings, `pkcon`, another
/// store. Never an operation of this store. The backend only observes it:
/// it never cancels it, never waits on a lock it holds, never forces one.
struct ExternalActivity
{
    enum class Kind : std::uint8_t {
        None,       ///< Idle, or nothing that concerns the store.
        Checking,   ///< Queries and simulations: nothing changes.
        Refreshing, ///< Repository metadata, downloads: no package changes.
        Changing,   ///< Installs, removals, updates, repository changes.
    };
    Kind kind = Kind::None;
    /// The package manager's name for it (`update-packages`); empty for None.
    std::string role = {};
    /// Its current step when it was seen (`download`, `install`); may be empty.
    std::string status = {};

    /// While true the backend starts nothing that changes the system: plan,
    /// execute and refresh complete at once with BackendFailure::Locked (a
    /// temporary failure: the same request may be made again once it is
    /// over), and nothing is sent to the package manager.
    bool blocksChanges() const noexcept { return kind == Kind::Changing; }

    bool operator==(const ExternalActivity&) const = default;
};

std::string_view toString(ExternalActivity::Kind kind) noexcept;

/// A package manager as seen by the core (ARCHITECTURE §7.1): PackageKit or
/// Flatpak. Implementations live outside the core (backend-packagekit,
/// backend-flatpak) and do their work on their own threads or connections.
/// Authorisation belongs to the package managers and polkit: nothing here
/// authenticates anyone.
///
/// Contract, the same for every operation:
/// - methods are called from the host's main thread, and return at once;
/// - the completion is called exactly once, on the host's main thread,
///   through the `post` function the host gave the backend (§2);
/// - it is never called from inside the method itself, so callers do not
///   have to handle re-entrancy; nor are the OperationEvents callbacks;
/// - failures are Errors, with ErrorCode::Backend and a BackendFailure when
///   the package manager is the cause: Interrupted when the backend lost
///   the operation, Unavailable when it cannot work at all. ErrorCode::
///   Cancelled after a cancellation, NotSupported for an operation the
///   capabilities exclude, Conflict for a second modifying operation;
/// - `operationId` is chosen by the host (the transaction id) and names the
///   operation for cancel(); unique among the backend's running operations.
///
/// Concurrency (P-009): at most one modifying operation (execute, refresh,
/// reattach) per backend at a time; another one completes with Conflict at
/// once. Plans and queries may be called at any time; the backend queues
/// them on its own (PackageKit runs one transaction at a time, queries
/// included).
///
/// Nothing a backend reports here is stored as the truth: the installed
/// state the host keeps (state.sqlite) is a cache of queryInstalled().
class Backend
{
public:
    virtual ~Backend() = default;

    virtual BackendKind kind() const noexcept = 0;
    virtual BackendCapabilities capabilities() const noexcept = 0;

    /// Success if the backend can be used; otherwise an Error with
    /// BackendFailure::Unavailable and the reason in its message.
    virtual void availability(Completion<void> done) = 0;

    // --- Queries (no change to the system) ---------------------------------

    /// What this backend has installed (all scopes it manages).
    virtual void queryInstalled(Completion<std::vector<InstalledItem>> done) = 0;

    /// Configured sources (repositories, remotes), enabled or not, as the
    /// package manager has them now.
    virtual void sources(Completion<std::vector<BackendSource>> done) = 0;

    /// Configured sources with their metadata files, for the indexer and
    /// for probeSource()/assessSources(). A source without metadata yet is
    /// listed with the path where its file will be.
    virtual void catalogSources(Completion<std::vector<SourceMetadata>> done) = 0;

    /// Updates available for what is installed, as the package manager sees
    /// them: every package with a newer version, identical versions left
    /// out. The backend does not know the catalogue; the host keeps the
    /// updates of applications and runtimes with applicationUpdates()
    /// (D-010, D-023).
    virtual void queryUpdates(Completion<std::vector<AvailableUpdate>> done) = 0;

    /// Permissions of an offer, as stable strings (`shared=network`).
    /// NotSupported without capabilities().permissions.
    virtual void permissions(const OfferId& offer, Completion<std::vector<std::string>> done) = 0;

    // --- Plans and execution (P-008, D-014, D-017) -------------------------

    /// Computes the plan of a request without changing anything (PackageKit
    /// simulation, first Flatpak pass). Its result is meant for
    /// checkPlanForRequest(), then Transaction::planReady(). Cancellable.
    virtual void plan(const std::string& operationId, const PlanRequest& request, Completion<Plan> done) = 0;

    /// Executes a request whose plan was accepted (by the user, or as is
    /// when it needed no confirmation). Before any change, the backend
    /// computes the plan it is about to execute and calls
    /// mayExecute(accepted, fresh):
    /// - false: it stops without changing anything and completes with
    ///   PlanChanged and the fresh plan (`started` is never called);
    /// - true: it calls `started` with the fresh plan, executes, reports
    ///   progress, and completes with Completed or an error.
    /// See BackendCapabilities::verifiesExecutedPlan for what the check
    /// covers on each backend.
    virtual void execute(const std::string& operationId, const PlanRequest& request, const Plan& accepted,
                         OperationEvents events, Completion<ExecutionResult> done) = 0;

    /// Follows again an execution that outlived the service, by the handle
    /// reported earlier (RecoveryAction::TryReattach). NotFound if the
    /// backend no longer knows it: the host then interrupts the
    /// transaction. NotSupported without capabilities().reattach.
    virtual void reattach(const std::string& operationId, const std::string& backendRef, OperationEvents events,
                          Completion<ExecutionResult> done) = 0;

    /// Rereads the repositories' metadata (D-015). NotSupported without
    /// capabilities().refresh. Modifying: one at a time with executions.
    virtual void refresh(const std::string& operationId, OperationEvents events, Completion<void> done) = 0;

    /// Enables, disables or removes a configured source (H.5). Modifying: one
    /// at a time with executions and refresh (P-009); not cancellable. The
    /// backend checks again what the host checked, against the package
    /// manager's state when it runs:
    /// - NotSupported without capabilities().userSourceChanges;
    /// - NotFound for a source it does not have;
    /// - NotAllowed for a source of the system installation (D-011), and for
    ///   removing a source something installed came from (D-041: nothing is
    ///   uninstalled with it);
    /// - Backend/Untrusted for enabling a source whose signatures are not
    ///   checked (D-044).
    /// Enabling an enabled source, or disabling a disabled one, succeeds and
    /// changes nothing.
    virtual void changeSource(const std::string& operationId, const SourceRequest& change, Completion<void> done) = 0;

    /// Asks to cancel a running plan, execution, reattach or refresh. Only a
    /// request: success means it was passed on, and the operation's own
    /// completion tells what happened (Cancelled, or success if it was too
    /// late). NotFound for an unknown operation; NotSupported when the
    /// operation cannot be cancelled now (an execution without
    /// cancelAnytime whose last Progress was not cancellable).
    virtual Result<void> cancel(const std::string& operationId) = 0;

    /// Registers the receiver of changes made outside the store (signals of
    /// packagekitd, Flatpak installation monitors). Called on the host's
    /// main thread; replaces any previous receiver; an empty function stops
    /// the notifications.
    virtual void setChangeListener(std::function<void(const BackendChanges&)> listener) = 0;

    /// What the package manager is busy with for another program, as last
    /// seen (H.2); reported through the change listener (`activity`). Only
    /// a backend whose package manager is shared with other programs and
    /// can tell (PackageKit) reports anything; the default is None.
    virtual ExternalActivity externalActivity() const { return {}; }

protected:
    Backend() = default;
    Backend(const Backend&) = default;
    Backend& operator=(const Backend&) = default;
};

} // namespace cutefish::store
