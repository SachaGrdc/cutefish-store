#pragma once

// A deterministic double of FlatpakStore, for the tests of the Flatpak
// backend (like fakepackagekit.h is for PackageKit). It holds two
// installations, a few remotes, installed refs and updates, and resolves
// transactions into operations; its execution can be told to report a
// changed plan, fail, or wait for a cancellation.

#include "flatpakstore.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace cutefish::store::flatpak {

class FakeStore final : public FlatpakStore
{
public:
    std::vector<Scope> scopes_ = {Scope::User, Scope::System};
    std::map<Scope, std::vector<RemoteInfo>> remotes_;
    std::map<Scope, std::vector<InstalledRef>> installed_;
    std::map<Scope, std::vector<UpdateRef>> updates_;
    RefMetadata metadata_;
    /// When set, the second pass differs (one extra runtime is removed).
    bool planChanged = false;
    /// When set, the execution fails with this error after `onReady`.
    std::optional<Error> runError;
    /// When set, the second pass fails with this error before `onReady`
    /// (for example a runtime that is no longer available).
    std::optional<Error> executeError;
    /// When set, a transaction would add a remote: it is refused (P-011).
    bool remoteAddAttempted = false;
    /// When set, these are the operations the second pass resolves to
    /// (overrides the default), to script a specific plan change.
    std::optional<std::vector<FlatpakOp>> executeOps;

    int resolves = 0;
    int executes = 0;
    int cancels = 0;
    std::atomic<bool> executing{false};
    std::vector<std::string> progressRefs;

    std::vector<Scope> scopes() override { return scopes_; }

    Result<std::vector<RemoteInfo>> remotes(Scope scope) override
    {
        const auto it = remotes_.find(scope);
        return it == remotes_.end() ? std::vector<RemoteInfo>{} : it->second;
    }

    Result<void> addRemote(Scope scope, const std::string& name, const std::string& url) override
    {
        RemoteInfo info{.name = name, .url = url, .disabled = false, .gpgVerify = true, .metadataPath = {}, .iconsPath = {}};
        remotes_[scope].push_back(std::move(info));
        return Result<void>::success();
    }

    Result<void> removeRemote(Scope scope, const std::string& name) override
    {
        auto& list = remotes_[scope];
        list.erase(std::remove_if(list.begin(), list.end(), [&](const RemoteInfo& r) { return r.name == name; }),
                   list.end());
        return Result<void>::success();
    }

    Result<void> setRemoteDisabled(Scope scope, const std::string& name, bool disabled) override
    {
        for (RemoteInfo& r : remotes_[scope])
            if (r.name == name)
                r.disabled = disabled;
        return Result<void>::success();
    }

    Result<std::vector<InstalledRef>> installed(Scope scope) override
    {
        const auto it = installed_.find(scope);
        return it == installed_.end() ? std::vector<InstalledRef>{} : it->second;
    }

    Result<std::vector<UpdateRef>> updates(Scope scope) override
    {
        const auto it = updates_.find(scope);
        return it == updates_.end() ? std::vector<UpdateRef>{} : it->second;
    }

    Result<RefMetadata> metadata(Scope, const std::string&, const FlatpakRef&) override { return metadata_; }

    Result<std::vector<FlatpakOp>> resolve(const TransactionSpec& spec) override
    {
        ++resolves;
        if (remoteAddAttempted)
            return Error::notAllowed("the transaction would add the remote 'flathub', which the store only adds "
                                     "with explicit consent");
        return opsFor(spec);
    }

    Result<ExecutionOutcome> execute(const TransactionSpec& spec, ExecuteControl control) override
    {
        ++executes;
        if (remoteAddAttempted)
            return Error::notAllowed("the transaction would add the remote 'flathub', which the store only adds "
                                     "with explicit consent");
        if (executeError)
            return *executeError;
        std::vector<FlatpakOp> ops = executeOps ? *executeOps : opsFor(spec);
        if (planChanged) {
            FlatpakOp extra;
            extra.type = OperationType::Uninstall;
            extra.ref = FlatpakRef{RefKind::Runtime, "org.example.Extra", "x86_64", "24.08"};
            ops.push_back(std::move(extra));
        }
        const bool run = control.onReady(ops);
        if (!run)
            return ExecutionOutcome{.type = ExecutionOutcome::Type::PlanChanged, .ops = std::move(ops)};
        if (runError)
            return *runError;

        executing = true;
        // A cancellation request is honoured at the next step (§7.6).
        for (int i = 0; i < 200 && !control.cancelled(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (control.cancelled()) {
            ++cancels;
            executing = false;
            return Error::cancelled("cancelled");
        }
        if (control.progress) {
            control.progress(FlatpakProgress{.fraction = 0.5, .status = "installing", .ref = spec.refs.front().ref.full(),
                                             .cancellable = true});
            progressRefs.push_back(spec.refs.front().ref.full());
        }
        executing = false;
        return ExecutionOutcome{.type = ExecutionOutcome::Type::Completed, .ops = {}};
    }

private:
    static std::vector<FlatpakOp> opsFor(const TransactionSpec& spec)
    {
        OperationType type = spec.kind == TransactionKind::Install   ? OperationType::Install
                             : spec.kind == TransactionKind::Remove ? OperationType::Uninstall
                                                                    : OperationType::Update;
        std::vector<FlatpakOp> ops;
        for (const RefRequest& r : spec.refs) {
            ops.push_back(FlatpakOp{.type = type,
                                    .ref = r.ref,
                                    .remote = r.remote,
                                    .commit = "commit-1",
                                    .downloadSize = 1000,
                                    .installedSize = 2000,
                                    .permissions = {"shared=network", "socket=wayland"},
                                    .oldPermissions = {},
                                    .requested = true,
                                    .addsSource = false,
                                    .rebaseFrom = ""});
        }
        // A runtime dependency flatpak adds on its own, for installs and updates.
        if (spec.kind != TransactionKind::Remove && !spec.refs.empty()) {
            FlatpakOp runtime;
            runtime.type = type == OperationType::Install ? OperationType::Install : OperationType::Update;
            runtime.ref = FlatpakRef{RefKind::Runtime, "org.example.Platform", "x86_64", "24.08"};
            runtime.remote = spec.refs.front().remote;
            runtime.downloadSize = 500;
            runtime.requested = false;
            ops.push_back(std::move(runtime));
        }
        return ops;
    }
};

} // namespace cutefish::store::flatpak
