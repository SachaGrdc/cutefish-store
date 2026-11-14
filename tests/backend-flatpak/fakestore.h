#pragma once

// A deterministic double of FlatpakStore, for the tests of the Flatpak
// backend (like fakepackagekit.h is for PackageKit) and of the service with
// that backend (H.3). It holds two installations, a few remotes, installed
// refs and updates, and resolves transactions into operations; its execution
// can be told to report a changed plan, fail, wait for a cancellation, stop
// at a gate, and apply a completed transaction to its installed state.
//
// Threads: the script fields are set by the test while the backend's worker
// is idle (before the call that reaches it); the counters and gates may be
// read from any thread.

#include "flatpakstore.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace cutefish::store::flatpak {

/// A point of the execution where the fake can be held, from any thread.
/// Closed, the worker waits there until it is opened (or, when given a
/// cancellation check, until a cancellation is asked).
class FakeGate
{
public:
    void close()
    {
        std::lock_guard lock(m_mutex);
        m_closed = true;
    }
    void open()
    {
        {
            std::lock_guard lock(m_mutex);
            m_closed = false;
        }
        m_changed.notify_all();
    }
    /// How many times the worker reached the gate.
    int reached() const
    {
        std::lock_guard lock(m_mutex);
        return m_reached;
    }
    /// Called by the worker.
    void pass(const std::function<bool()>& cancelled = {})
    {
        std::unique_lock lock(m_mutex);
        ++m_reached;
        m_changed.notify_all();
        while (m_closed && !(cancelled && cancelled()))
            m_changed.wait_for(lock, std::chrono::milliseconds(1));
    }

private:
    mutable std::mutex m_mutex;
    std::condition_variable m_changed;
    bool m_closed = false;
    int m_reached = 0;
};

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
    /// When set, these are the operations the first pass resolves to
    /// (overrides the default).
    std::optional<std::vector<FlatpakOp>> resolveOps;
    /// When set, these are the operations the second pass resolves to
    /// (overrides the default), to script a specific plan change.
    std::optional<std::vector<FlatpakOp>> executeOps;

    /// When set, a completed transaction changes the installed refs and the
    /// updates as Flatpak would (for the reconciliation after it).
    bool applyCompleted = false;
    /// Held before the second pass resolves (`ready`), and once running.
    FakeGate beforeReady;
    FakeGate whileRunning;

    std::atomic<int> resolves{0};
    std::atomic<int> executes{0};
    std::atomic<int> cancels{0};
    /// Executions that reached running (`ready` returned true).
    std::atomic<int> runs{0};
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
        return resolveOps ? *resolveOps : opsFor(spec);
    }

    Result<ExecutionOutcome> execute(const TransactionSpec& spec, ExecuteControl control) override
    {
        ++executes;
        if (remoteAddAttempted)
            return Error::notAllowed("the transaction would add the remote 'flathub', which the store only adds "
                                     "with explicit consent");
        if (executeError)
            return *executeError;
        beforeReady.pass();
        std::vector<FlatpakOp> ops = executeOps ? *executeOps : opsFor(spec);
        if (planChanged) {
            FlatpakOp extra;
            extra.type = OperationType::Uninstall;
            extra.ref = FlatpakRef{RefKind::Runtime, "org.example.Extra", "x86_64", "24.08"};
            ops.push_back(std::move(extra));
        }
        // Like LibFlatpakStore: a cancellation asked before `ready` stops the
        // transaction there, without running anything.
        if (control.cancelled && control.cancelled()) {
            ++cancels;
            return Error::cancelled("cancelled");
        }
        const bool run = control.onReady(ops);
        if (!run)
            return ExecutionOutcome{.type = ExecutionOutcome::Type::PlanChanged, .ops = std::move(ops)};
        ++runs;
        if (runError)
            return *runError;

        executing = true;
        whileRunning.pass(control.cancelled);
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
        if (applyCompleted)
            apply(spec);
        return ExecutionOutcome{.type = ExecutionOutcome::Type::Completed, .ops = {}};
    }

private:
    void apply(const TransactionSpec& spec)
    {
        auto& installed = installed_[spec.scope];
        auto& updates = updates_[spec.scope];
        for (const RefRequest& r : spec.refs) {
            const std::string full = r.ref.full();
            auto same = [&](const auto& x) { return x.ref.full() == full; };
            if (spec.kind == TransactionKind::Install)
                installed.push_back({r.ref, r.remote});
            else if (spec.kind == TransactionKind::Remove)
                installed.erase(std::remove_if(installed.begin(), installed.end(), same), installed.end());
            updates.erase(std::remove_if(updates.begin(), updates.end(), same), updates.end());
        }
    }

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
