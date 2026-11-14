#include <cutefish/store/backend.h>

namespace cutefish::store {

std::string_view toString(OperationPhase phase) noexcept
{
    switch (phase) {
    case OperationPhase::WaitingForAuth: return "waiting-for-auth";
    case OperationPhase::WaitingForLock: return "waiting-for-lock";
    case OperationPhase::Downloading: return "downloading";
    case OperationPhase::Installing: return "installing";
    case OperationPhase::Removing: return "removing";
    case OperationPhase::Finishing: return "finishing";
    }
    return "finishing";
}

std::string_view toString(SourceRequest::Action action) noexcept
{
    switch (action) {
    case SourceRequest::Action::Enable: return "enable";
    case SourceRequest::Action::Disable: return "disable";
    case SourceRequest::Action::Remove: return "remove";
    }
    return "disable";
}

std::string_view toString(ExternalActivity::Kind kind) noexcept
{
    switch (kind) {
    case ExternalActivity::Kind::None: return "none";
    case ExternalActivity::Kind::Checking: return "checking";
    case ExternalActivity::Kind::Refreshing: return "refreshing";
    case ExternalActivity::Kind::Changing: return "changing";
    }
    return "none";
}

} // namespace cutefish::store
