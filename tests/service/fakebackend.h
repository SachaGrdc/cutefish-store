#pragma once

// A minimal scripted Backend for the service tests: it answers availability,
// the installed items and updates it is given (none by default), and refuses
// everything else, so the registry, selection, inventory aggregation (H.2)
// and Backends property can be tested without a package manager. Answers
// may be held and released together, to make two backends answer in the
// same loop iteration; changes "made outside the store" are emitted by
// emitChange(). Answers capture copies, never the fake itself.
// Test-only; not part of store-service.

#include <cutefish/store/backend.h>
#include <cutefish/store/plan.h>

#include <base/event_loop.h>

#include <cstddef>
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

    // --- Scripting --------------------------------------------------------

    void setAvailability(Result<void> availability) { m_availability = std::move(availability); }
    void setInstalled(std::vector<InstalledItem> items) { m_installed = std::move(items); }
    void setUpdates(std::vector<AvailableUpdate> updates) { m_updates = std::move(updates); }
    /// While held, answers wait for release(), which posts them in order.
    void hold() { m_holding = true; }
    void release()
    {
        m_holding = false;
        for (auto& task : std::exchange(m_held, {}))
            m_loop.post(std::move(task));
    }
    std::size_t heldAnswers() const noexcept { return m_held.size(); }
    /// A change made outside the store, as the package manager reports it.
    void emitChange(BackendChanges changes)
    {
        if (m_listener)
            m_listener(changes);
    }
    bool listening() const noexcept { return static_cast<bool>(m_listener); }
    int installedQueries() const noexcept { return m_installedQueries; }
    int updatesQueries() const noexcept { return m_updatesQueries; }

    // --- Backend ----------------------------------------------------------

    void availability(Completion<void> done) override
    {
        post([done, a = m_availability] { done(a); });
    }

    void queryInstalled(Completion<std::vector<InstalledItem>> done) override
    {
        ++m_installedQueries;
        post([done, items = m_installed] { done(items); });
    }

    void sources(Completion<std::vector<BackendSource>> done) override
    {
        post([done] { done(std::vector<BackendSource>{}); });
    }

    void changeSource(const std::string&, const SourceRequest&, Completion<void> done) override
    {
        post([done] { done(Error::notSupported("no source changes in this fake")); });
    }

    void catalogSources(Completion<std::vector<SourceMetadata>> done) override
    {
        post([done] { done(std::vector<SourceMetadata>{}); });
    }

    void queryUpdates(Completion<std::vector<AvailableUpdate>> done) override
    {
        ++m_updatesQueries;
        post([done, updates = m_updates] { done(updates); });
    }

    void permissions(const OfferId&, Completion<std::vector<std::string>> done) override
    {
        post([done] { done(Error::notSupported("no permissions in this fake")); });
    }

    void plan(const std::string&, const PlanRequest&, Completion<Plan> done) override
    {
        ++m_planCalls;
        post([done] { done(Error::notSupported("no planning in this fake")); });
    }
    int planCalls() const noexcept { return m_planCalls; }

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

    void setChangeListener(std::function<void(const BackendChanges&)> listener) override
    {
        m_listener = std::move(listener);
    }

private:
    void post(std::function<void()> task)
    {
        if (m_holding)
            m_held.push_back(std::move(task));
        else
            m_loop.post(std::move(task));
    }

    base::EventLoop& m_loop;
    BackendKind m_kind;
    Result<void> m_availability;
    std::vector<InstalledItem> m_installed;
    std::vector<AvailableUpdate> m_updates;
    std::function<void(const BackendChanges&)> m_listener;
    bool m_holding = false;
    std::vector<std::function<void()>> m_held;
    int m_installedQueries = 0;
    int m_updatesQueries = 0;
    int m_planCalls = 0;
};

} // namespace cutefish::store::service::testutil
