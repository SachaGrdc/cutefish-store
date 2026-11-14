#pragma once

// The real Flatpak store over libflatpak. Compiled only when libflatpak is
// found (HAVE_LIBFLATPAK). Every method runs on the backend's worker thread,
// so the FlatpakInstallation objects are created, used and destroyed there
// and never shared (ARCHITECTURE §8.2). The two-pass transaction model
// (P-008, D-017) lives in the `ready` signal of FlatpakTransaction.

#include "flatpakstore.h"

#include <filesystem>
#include <optional>

namespace cutefish::store::flatpak {

class LibFlatpakStore final : public FlatpakStore
{
public:
    struct Options
    {
        /// Override the user installation path (tests, FLATPAK_USER_DIR).
        std::optional<std::filesystem::path> userPath;
        /// Override the system installation path (tests).
        std::optional<std::filesystem::path> systemPath;
        bool user = true;   ///< open the user installation.
        bool system = true; ///< open the system installation.
    };

    explicit LibFlatpakStore(Options options);
    ~LibFlatpakStore() override;

    std::vector<Scope> scopes() override;
    Result<std::vector<RemoteInfo>> remotes(Scope scope) override;
    Result<void> addRemoteFromFile(Scope scope, const std::string& name, const std::string& flatpakrepo) override;
    Result<void> removeRemote(Scope scope, const std::string& name) override;
    Result<void> setRemoteDisabled(Scope scope, const std::string& name, bool disabled) override;
    Result<std::vector<InstalledRef>> installed(Scope scope) override;
    Result<std::vector<UpdateRef>> updates(Scope scope) override;
    Result<RefMetadata> metadata(Scope scope, const std::string& remote, const FlatpakRef& ref) override;
    Result<std::vector<FlatpakOp>> resolve(const TransactionSpec& spec) override;
    Result<ExecutionOutcome> execute(const TransactionSpec& spec, ExecuteControl control) override;

private:
    Result<void*> installation(Scope scope); // FlatpakInstallation*, borrowed

    Options m_options;
    void* m_user = nullptr;   // FlatpakInstallation*
    void* m_system = nullptr; // FlatpakInstallation*
    bool m_userFailed = false;
    bool m_systemFailed = false;
};

} // namespace cutefish::store::flatpak
