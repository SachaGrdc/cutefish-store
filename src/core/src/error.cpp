#include <cutefish/store/error.h>

namespace cutefish::store {

Error::Error(ErrorCode code, std::string message)
    : m_code(code)
    , m_message(std::move(message))
{
}

Error::Error(BackendFailure failure, std::string message)
    : m_code(ErrorCode::Backend)
    , m_failure(failure)
    , m_message(std::move(message))
{
}

std::string_view toString(ErrorCode code) noexcept
{
    switch (code) {
    case ErrorCode::InvalidArgument: return "invalid-argument";
    case ErrorCode::NotFound: return "not-found";
    case ErrorCode::NotSupported: return "not-supported";
    case ErrorCode::Conflict: return "conflict";
    case ErrorCode::Cancelled: return "cancelled";
    case ErrorCode::Backend: return "backend";
    case ErrorCode::Internal: return "internal";
    }
    return "internal";
}

std::string_view toString(BackendFailure failure) noexcept
{
    switch (failure) {
    case BackendFailure::Unspecified: return "unspecified";
    case BackendFailure::Unavailable: return "backend-unavailable";
    case BackendFailure::AuthDenied: return "auth-denied";
    case BackendFailure::AuthCancelled: return "auth-cancelled";
    case BackendFailure::Network: return "network";
    case BackendFailure::NoSpace: return "no-space";
    case BackendFailure::Untrusted: return "untrusted";
    case BackendFailure::Dependency: return "dependency";
    case BackendFailure::PackageConflict: return "conflict";
    case BackendFailure::Locked: return "locked";
    case BackendFailure::Interrupted: return "interrupted";
    }
    return "unspecified";
}

std::optional<ErrorCode> parseErrorCode(std::string_view text) noexcept
{
    for (const ErrorCode c : {ErrorCode::InvalidArgument, ErrorCode::NotFound, ErrorCode::NotSupported,
                              ErrorCode::Conflict, ErrorCode::Cancelled, ErrorCode::Backend, ErrorCode::Internal})
        if (toString(c) == text)
            return c;
    return std::nullopt;
}

std::optional<BackendFailure> parseBackendFailure(std::string_view text) noexcept
{
    for (const BackendFailure f :
         {BackendFailure::Unspecified, BackendFailure::Unavailable, BackendFailure::AuthDenied,
          BackendFailure::AuthCancelled, BackendFailure::Network, BackendFailure::NoSpace, BackendFailure::Untrusted,
          BackendFailure::Dependency, BackendFailure::PackageConflict, BackendFailure::Locked,
          BackendFailure::Interrupted})
        if (toString(f) == text)
            return f;
    return std::nullopt;
}

} // namespace cutefish::store
