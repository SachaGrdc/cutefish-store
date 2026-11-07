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

} // namespace cutefish::store
