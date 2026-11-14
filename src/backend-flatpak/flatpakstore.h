#pragma once

// The narrow seam between the generic Flatpak backend (flatpakbackend.h,
// which implements the core's Backend contract) and libflatpak. The backend
// drives this interface on a worker thread of its own; the real
// implementation (libflatpakstore.h) uses libflatpak, tests use a double
// (tests/backend-flatpak/fakestore.h). Every method is synchronous and runs
// on that worker thread, so it may block; the backend posts its results back
// to the host's main loop (P-006, ARCHITECTURE §2).

#include "flatpaktypes.h"

#include <cutefish/store/plan.h>
#include <cutefish/store/result.h>
#include <cutefish/store/source.h>

#include <functional>
#include <string>
#include <vector>

namespace cutefish::store::flatpak {

/// What a transaction acts on: one kind, one installation, one or more refs.
struct RefRequest
{
    FlatpakRef ref;
    std::string remote; ///< origin remote (install); empty for a removal.
};

struct TransactionSpec
{
    TransactionKind kind = TransactionKind::Install;
    Scope scope = Scope::User;
    std::vector<RefRequest> refs;
};

/// How an execution reports progress and is cancelled.
struct RunControl
{
    std::function<void(const FlatpakProgress&)> progress;
    /// Returns true once the host asked to cancel. The store then stops at
    /// its next interruptible step and reports Cancelled.
    std::function<bool()> cancelled;
};

/// How the second pass lets the host decide, in the `ready` signal of the
/// transaction that is about to run (D-017): the host builds the plan from
/// the resolved operations, compares it with the accepted plan, emits the
/// `started` event, and returns true to run or false to stop without any
/// change.
struct ExecuteControl
{
    /// Returns true to execute, false to abort (a changed plan).
    std::function<bool(const std::vector<FlatpakOp>& ops)> onReady;
    std::function<void(const FlatpakProgress&)> progress;
    std::function<bool()> cancelled;
};

/// The outcome of an execution that did not complete.
struct ExecutionOutcome
{
    enum class Type : std::uint8_t {
        Completed,   ///< the transaction ran to the end.
        PlanChanged, ///< the second pass differed: nothing was executed.
    };
    Type type = Type::Completed;
    /// The operations of the second pass, when PlanChanged.
    std::vector<FlatpakOp> ops;
};

class FlatpakStore
{
public:
    virtual ~FlatpakStore() = default;

    /// The installations that can be used (opened successfully). Empty means
    /// the backend is unusable.
    virtual std::vector<Scope> scopes() = 0;

    // --- Remotes (P-011, D-011) --------------------------------------------
    virtual Result<std::vector<RemoteInfo>> remotes(Scope scope) = 0;
    /// Adds a remote from the contents of a `.flatpakrepo`, which must carry
    /// its GPG key; signatures are always verified (D-040). Never from a URL
    /// alone. The caller obtained an explicit consent first: nothing in the
    /// service calls it yet (H.5d waits for P-011).
    virtual Result<void> addRemoteFromFile(Scope scope, const std::string& name, const std::string& flatpakrepo) = 0;
    virtual Result<void> removeRemote(Scope scope, const std::string& name) = 0;
    virtual Result<void> setRemoteDisabled(Scope scope, const std::string& name, bool disabled) = 0;

    // --- Installed state and updates ---------------------------------------
    /// Applications and runtimes installed in this installation.
    virtual Result<std::vector<InstalledRef>> installed(Scope scope) = 0;
    /// The refs with an available update, as flatpak sees them.
    virtual Result<std::vector<UpdateRef>> updates(Scope scope) = 0;
    /// Metadata of a remote ref: version, sizes, permissions (for a plan and
    /// for GetPermissions).
    virtual Result<RefMetadata> metadata(Scope scope, const std::string& remote, const FlatpakRef& ref) = 0;

    // --- Transactions (P-008, D-017: two passes) ---------------------------
    /// First pass: resolves the operations without running anything (`ready`
    /// returns false). The result feeds Transaction::planReady().
    virtual Result<std::vector<FlatpakOp>> resolve(const TransactionSpec& spec) = 0;
    /// Second pass: resolves again and, through `control.onReady`, asks the
    /// host whether to run (the plan it is about to execute is compared with
    /// the accepted one). Completed, or PlanChanged with the fresh operations
    /// when the host said no (nothing was executed).
    virtual Result<ExecutionOutcome> execute(const TransactionSpec& spec, ExecuteControl control) = 0;
};

} // namespace cutefish::store::flatpak
