#pragma once

// A minimal scripted Backend for the service tests: it answers availability,
// empty installed/updates, and refuses everything else, so the registry,
// selection and Backends property can be tested without a package manager.
// Test-only; not part of store-service.

#include <cutefish/store/backend.h>
#include <cutefish/store/plan.h>

#include <base/event_loop.h>

#include <functional>
#include <utility>
#include <vector>

namespace cutefish::store::service::testutil {

class FakeBackend final : public Backend
{
public:
    FakeBackend(base::EventLoop& loop, BackendKind kind, Result<void> availability = Result<void>::success())
        : m_loop(loop), m_kind(kind), m_availability(std::move(availability))
    {
    }

    BackendKind kind() const noexcept override { return m_kind; }

    BackendCapabilities capabilities() const noexcept override
    {
        return {.userScope = m_kind == BackendKind::Flatpak,
                .systemScope = true,
                .permissions = m_kind == BackendKind::Flatpak,
                .refresh = m_kind == BackendKind::PackageKit,
                .cancelAnytime = false,
                .reattach = false,
                .verifiesExecutedPlan = false};
    }

    void availability(Completion<void> done) override { post([this, done] { done(m_availability); }); }

    void queryInstalled(Completion<std::vector<InstalledItem>> done) override
    {
        post([done] { done(std::vector<InstalledItem>{}); });
    }

    void sources(Completion<std::vector<Source>> done) override { post([done] { done(std::vector<Source>{}); }); }

    void catalogSources(Completion<std::vector<SourceMetadata>> done) override
    {
        post([done] { done(std::vector<SourceMetadata>{}); });
    }

    void queryUpdates(Completion<std::vector<AvailableUpdate>> done) override
    {
        post([done] { done(std::vector<AvailableUpdate>{}); });
    }

    void permissions(const OfferId&, Completion<std::vector<std::string>> done) override
    {
        post([done] { done(Error::notSupported("no permissions in this fake")); });
    }

    void plan(const std::string&, const PlanRequest&, Completion<Plan> done) override
    {
        post([done] { done(Error::notSupported("no planning in this fake")); });
    }

    void execute(const std::string&, const PlanRequest&, const Plan&, OperationEvents,
                 Completion<ExecutionResult> done) override
    {
        post([done] { done(Error::notSupported("no execution in this fake")); });
    }

    void reattach(const std::string&, const std::string&, OperationEvents,
                  Completion<ExecutionResult> done) override
    {
        post([done] { done(Error::notSupported("no re-attach in this fake")); });
    }

    void refresh(const std::string&, OperationEvents, Completion<void> done) override
    {
        post([done] { done(Result<void>::success()); });
    }

    Result<void> cancel(const std::string&) override { return Result<void>::success(); }

    void setChangeListener(std::function<void(const BackendChanges&)>) override {}

private:
    void post(std::function<void()> task) { m_loop.post(std::move(task)); }

    base::EventLoop& m_loop;
    BackendKind m_kind;
    Result<void> m_availability;
};

} // namespace cutefish::store::service::testutil
