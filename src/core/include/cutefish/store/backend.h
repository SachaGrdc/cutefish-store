#pragma once

#include <cutefish/store/installed.h>
#include <cutefish/store/result.h>
#include <cutefish/store/source.h>

#include <functional>
#include <vector>

namespace cutefish::store {

/// Receives the result of an asynchronous backend operation.
template <typename T>
using Completion = std::function<void(Result<T>)>;

/// A package manager as seen by the core (ARCHITECTURE §7.1): PackageKit or
/// Flatpak. Implementations live outside the core (backend-packagekit,
/// backend-flatpak) and do their work on their own threads or connections.
///
/// Contract, the same for every operation:
/// - methods are called from the host's main thread, and return at once;
/// - the completion is called exactly once, on the host's main thread,
///   through the `post` function the host gave the backend (§2);
/// - it is never called from inside the method itself, so callers do not
///   have to handle re-entrancy;
/// - failures are Errors, with ErrorCode::Backend and a BackendFailure when
///   the package manager is the cause.
///
/// v0.1 covers what the installed state and the sources need. Planning,
/// execution, cancellation, updates and change notifications (§7.1) will be
/// added with the transactions, once their model exists.
class Backend
{
public:
    virtual ~Backend() = default;

    virtual BackendKind kind() const noexcept = 0;

    /// Success if the backend can be used; otherwise an Error with
    /// BackendFailure::Unavailable and the reason in its message.
    virtual void availability(Completion<void> done) = 0;

    /// What this backend has installed (all scopes it manages).
    virtual void queryInstalled(Completion<std::vector<InstalledItem>> done) = 0;

    /// Configured sources (repositories, remotes), read-only.
    virtual void sources(Completion<std::vector<Source>> done) = 0;

protected:
    Backend() = default;
    Backend(const Backend&) = default;
    Backend& operator=(const Backend&) = default;
};

} // namespace cutefish::store
