#pragma once

// The Backend of PackageKit (step F, SERVICE.md §6): the core's contract
// (backend.h), the logic of store-backend-packagekit, and the D-Bus
// transport of pktransport.h. Lives in the host layer because the
// transport uses Cutefish Toolkit (P-006, D-007).
//
// Every method is called on the host's main loop and returns at once; the
// transport works on its own thread; completions and events come back to
// the main loop through its post(), in order, never from inside the call.

#include "pktransport.h"
#include "translate.h"

#include <cutefish/store/backend.h>

#include <base/event_loop.h>
#include <base/timer.h>

#include <atomic>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>

namespace cutefish::store::service {

class PackageKitBackend final : public Backend
{
public:
    struct Options
    {
        PkTransport::Bus bus = PkTransport::Bus::System;
        /// Where the distribution's DEP-11 catalogues are (Debian profile).
        std::filesystem::path swcatalog = "/var/lib/swcatalog";
        /// Grouping of the daemon's change signals (SERVICE.md §6.2).
        std::chrono::milliseconds changeDelay = std::chrono::seconds(2);
        /// Limits of the service when PackageKit does not finish (§5.3).
        std::chrono::milliseconds queryTimeout = std::chrono::minutes(2);
        std::chrono::milliseconds transactionTimeout = std::chrono::minutes(30);
    };

    PackageKitBackend(base::EventLoop& main, Options options);
    ~PackageKitBackend() override;

    BackendKind kind() const noexcept override { return BackendKind::PackageKit; }
    /// System only, no permissions, refresh, cancellation only while
    /// PackageKit's AllowCancel is true, reattach, and
    /// verifiesExecutedPlan = false: the check before an execution runs on a
    /// new simulation, a separate transaction; PackageKit offers no way to
    /// execute exactly a plan (D-017, ARCHITECTURE §7.2).
    BackendCapabilities capabilities() const noexcept override;

    void availability(Completion<void> done) override;
    void queryInstalled(Completion<std::vector<InstalledItem>> done) override;
    void sources(Completion<std::vector<Source>> done) override;
    void catalogSources(Completion<std::vector<SourceMetadata>> done) override;
    void queryUpdates(Completion<std::vector<AvailableUpdate>> done) override;
    void permissions(const OfferId& offer, Completion<std::vector<std::string>> done) override;
    void plan(const std::string& operationId, const PlanRequest& request, Completion<Plan> done) override;
    void execute(const std::string& operationId, const PlanRequest& request, const Plan& accepted,
                 OperationEvents events, Completion<ExecutionResult> done) override;
    void reattach(const std::string& operationId, const std::string& backendRef, OperationEvents events,
                  Completion<ExecutionResult> done) override;
    void refresh(const std::string& operationId, OperationEvents events, Completion<void> done) override;
    Result<void> cancel(const std::string& operationId) override;
    void setChangeListener(std::function<void(const BackendChanges&)> listener) override;

private:
    struct Operation
    {
        bool modifying = false;
        /// An execution reached the real transaction (after `started`).
        bool started = false;
        /// The last AllowCancel seen.
        bool cancellable = false;
        std::shared_ptr<std::atomic<bool>> cancelRequested = std::make_shared<std::atomic<bool>>(false);
    };

    /// Runs `then` with the profile, once the daemon was found usable;
    /// otherwise posts the error to `fail`.
    void withProfile(std::function<void(const packagekit::Profile&)> then, std::function<void(Error)> fail);
    /// Posts to the main loop, dropped if this backend is gone.
    void toMain(std::function<void()> task);
    template <typename T>
    void later(Completion<T> done, Result<T> result);
    Result<void> admit(const std::string& operationId, const PlanRequest* request, bool modifying);
    void forget(const std::string& operationId);
    /// The plan of a request: Resolve (or GetUpdates), then the simulation.
    void simulate(const std::string& operationId, PlanRequest request, const packagekit::Profile& profile,
                  bool urgent, std::function<void(Result<packagekit::SimulatedPlan>, std::vector<std::string> ids)> done);
    PkCall call(std::string method, std::function<void(base::dbus::Message&)> arguments, std::string operation,
                std::chrono::milliseconds timeout) const;

    base::EventLoop& m_main;
    Options m_options;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
    std::optional<packagekit::Profile> m_profile;
    std::map<std::string, Operation> m_operations;
    std::string m_modifying;
    std::function<void(const BackendChanges&)> m_listener;
    BackendChanges m_pendingChanges;
    base::Timer m_changeTimer;
    /// Last: destroyed first, so its thread is joined before any member its
    /// callbacks reach (m_alive through toMain(), m_options) goes away.
    std::unique_ptr<PkTransport> m_transport;
};

} // namespace cutefish::store::service
