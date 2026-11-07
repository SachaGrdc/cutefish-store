#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace cutefish::store {

/// Broad category of an error. Callers branch on this value, never on the
/// message.
enum class ErrorCode : std::uint8_t {
    InvalidArgument, ///< Input rejected by validation (malformed, too long, inconsistent).
    NotFound,        ///< The designated resource does not exist.
    NotSupported,    ///< The operation has no meaning or no reliable answer here.
    Conflict,        ///< The request conflicts with the current state (already in progress, duplicate).
    Cancelled,       ///< The operation was cancelled.
    Backend,         ///< A package manager reported a failure; see BackendFailure.
    Internal,        ///< Bug or broken invariant in Cutefish Store itself.
};

/// Why a package manager failed. Meaningful only with ErrorCode::Backend.
/// The values follow the transaction error codes of ARCHITECTURE §5.6.
enum class BackendFailure : std::uint8_t {
    Unspecified,
    Unavailable,     ///< Backend absent or unusable (backend-unavailable).
    AuthDenied,      ///< Authorization refused (auth-denied).
    AuthCancelled,   ///< Authentication cancelled (auth-cancelled).
    Network,         ///< Download or repository access failed (network).
    NoSpace,         ///< Not enough disk space (no-space).
    Untrusted,       ///< Missing or invalid signature (untrusted).
    Dependency,      ///< Dependencies cannot be resolved (dependency).
    PackageConflict, ///< File or package conflict (conflict).
    Locked,          ///< Package database locked by another program (locked).
    Interrupted,     ///< The operation stopped without a final state (interrupted).
};

/// A structured error: a code, a backend failure reason for backend errors,
/// and a message for diagnostics. The message is for logs and the `details`
/// field of the D-Bus API; it is not translated and not meant to be parsed.
class Error
{
public:
    explicit Error(ErrorCode code, std::string message = {});
    explicit Error(BackendFailure failure, std::string message = {});

    static Error invalidArgument(std::string message) { return Error(ErrorCode::InvalidArgument, std::move(message)); }
    static Error notFound(std::string message) { return Error(ErrorCode::NotFound, std::move(message)); }
    static Error notSupported(std::string message) { return Error(ErrorCode::NotSupported, std::move(message)); }
    static Error conflict(std::string message) { return Error(ErrorCode::Conflict, std::move(message)); }
    static Error cancelled(std::string message = {}) { return Error(ErrorCode::Cancelled, std::move(message)); }
    static Error backend(BackendFailure failure, std::string message) { return Error(failure, std::move(message)); }
    static Error internal(std::string message) { return Error(ErrorCode::Internal, std::move(message)); }

    ErrorCode code() const noexcept { return m_code; }
    /// BackendFailure::Unspecified unless code() is ErrorCode::Backend.
    BackendFailure backendFailure() const noexcept { return m_failure; }
    const std::string& message() const noexcept { return m_message; }

    bool operator==(const Error&) const = default;

private:
    ErrorCode m_code;
    BackendFailure m_failure = BackendFailure::Unspecified;
    std::string m_message;
};

/// Stable lowercase names, e.g. "invalid-argument", "auth-denied".
std::string_view toString(ErrorCode code) noexcept;
std::string_view toString(BackendFailure failure) noexcept;

} // namespace cutefish::store
