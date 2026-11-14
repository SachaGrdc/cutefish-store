#pragma once

// In-memory implementations of the core interfaces, for tests. They show
// what a real implementation must do; they are not part of store-core.

#include <cutefish/store/backend.h>
#include <cutefish/store/catalog.h>
#include <cutefish/store/confirmation.h>
#include <cutefish/store/installedstate.h>

#include <algorithm>
#include <cctype>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <utility>
#include <vector>

namespace fakes {

using namespace cutefish::store;

inline std::string lower(std::string_view s)
{
    std::string out(s);
    for (char& c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

class FakeInstalledState final : public InstalledState
{
public:
    explicit FakeInstalledState(std::vector<InstalledItem> items = {}) : m_items(std::move(items)) {}

    void setFailure(std::optional<Error> e) { m_failure = std::move(e); }

    Result<std::vector<InstalledItem>> items(std::optional<BackendKind> backend) const override
    {
        if (m_failure)
            return *m_failure;
        std::vector<InstalledItem> out;
        for (const InstalledItem& i : m_items)
            if (!backend || i.backend() == *backend)
                out.push_back(i);
        return out;
    }

    Result<std::optional<InstalledItem>> find(const OfferId& offer) const override
    {
        if (m_failure)
            return *m_failure;
        for (const InstalledItem& i : m_items)
            if (i.offer() == offer)
                return std::optional<InstalledItem>(i);
        return std::optional<InstalledItem>();
    }

    Result<std::vector<InstalledItem>> forApplication(const AppId& app) const override
    {
        if (m_failure)
            return *m_failure;
        std::vector<InstalledItem> out;
        for (const InstalledItem& i : m_items)
            if (i.app() && *i.app() == app)
                out.push_back(i);
        return out;
    }

private:
    std::vector<InstalledItem> m_items;
    std::optional<Error> m_failure;
};

/// Case-insensitive ASCII substring search on names and summaries; the
/// real catalogue uses FTS5.
class FakeCatalog final : public Catalog
{
public:
    FakeCatalog(std::uint64_t generation, std::vector<Application> apps, const InstalledState* installed = nullptr)
        : m_generation(generation), m_apps(std::move(apps)), m_installed(installed)
    {
    }

    std::uint64_t generation() const noexcept override { return m_generation; }

    Result<Application> application(const AppId& id) const override
    {
        for (const Application& a : m_apps)
            if (a.isKnownAs(id))
                return a;
        return Error::notFound("no application " + id.str());
    }

    Result<SearchPage> search(const SearchQuery& query) const override
    {
        SearchPage page{.generation = m_generation, .total = 0, .items = {}};
        if (query.text().empty())
            return page;
        const std::string needle = lower(query.text());
        const SearchOptions& o = query.options();
        for (const Application& a : m_apps) {
            if (lower(a.name()).find(needle) == std::string::npos &&
                lower(a.summary()).find(needle) == std::string::npos)
                continue;
            if (!o.category.empty() && !a.hasCategory(o.category))
                continue;
            if (o.backend && !a.hasBackend(*o.backend))
                continue;
            AppSummary s = summarize(a);
            if (o.installedOnly && !s.installed)
                continue;
            if (page.total >= o.offset && page.items.size() < o.limit)
                page.items.push_back(std::move(s));
            ++page.total;
        }
        return page;
    }

    Result<SearchPage> browse(const BrowseQuery& query) const override
    {
        const SearchOptions& o = query.options();
        std::vector<const Application*> sorted;
        for (const Application& a : m_apps)
            sorted.push_back(&a);
        std::sort(sorted.begin(), sorted.end(), [](const Application* x, const Application* y) {
            const std::string a = lower(x->name()), b = lower(y->name());
            return a != b ? a < b : x->id() < y->id();
        });
        SearchPage page{.generation = m_generation, .total = 0, .items = {}};
        for (const Application* a : sorted) {
            if (!o.category.empty() && !a->hasCategory(o.category))
                continue;
            if (o.backend && !a->hasBackend(*o.backend))
                continue;
            AppSummary s = summarize(*a);
            if (o.installedOnly && !s.installed)
                continue;
            if (page.total >= o.offset && page.items.size() < o.limit)
                page.items.push_back(std::move(s));
            ++page.total;
        }
        return page;
    }

    Result<std::vector<CategoryCount>> listCategories() const override
    {
        std::map<std::string, std::uint32_t> counts;
        for (const Application& a : m_apps)
            for (const std::string& c : a.categories())
                ++counts[c];
        std::vector<CategoryCount> out;
        for (const auto& [c, n] : counts)
            out.push_back({c, n});
        return out;
    }

    Result<std::vector<std::pair<OfferId, AppId>>> offerApplications(std::span<const OfferId> offers) const override
    {
        std::vector<std::pair<OfferId, AppId>> out;
        for (const OfferId& id : offers)
            for (const Application& a : m_apps)
                for (const Offer& o : a.offers())
                    if (o.id() == id)
                        out.emplace_back(id, a.id());
        return out;
    }

    /// What sources() returns; set by the test.
    std::vector<RecordedSource> recordedSources;

    Result<std::vector<RecordedSource>> sources() const override { return recordedSources; }

    Result<std::vector<AppSummary>> summaries(std::span<const AppId> ids) const override
    {
        std::vector<AppSummary> out;
        out.reserve(ids.size());
        for (const AppId& id : ids)
            if (auto app = application(id))
                out.push_back(summarize(*app));
        return out;
    }

private:
    AppSummary summarize(const Application& a) const
    {
        AppSummary s{.id = a.id(), .name = a.name(), .summary = a.summary(), .icon = a.icon(), .backends = {}};
        for (const Offer& o : a.offers())
            s.backends.insert(o.backend());
        if (m_installed)
            if (auto items = m_installed->forApplication(a.id()))
                s.installed = !items->empty();
        return s;
    }

    std::uint64_t m_generation;
    std::vector<Application> m_apps;
    const InstalledState* m_installed;
};

/// Stands for the host's `post`: work is queued and run by runAll(), as the
/// host's main loop would do.
class PostQueue
{
public:
    void post(std::function<void()> f) { m_queue.push_back(std::move(f)); }
    std::size_t pending() const { return m_queue.size(); }
    /// Runs the oldest queued task, if any.
    void runOne()
    {
        if (m_queue.empty())
            return;
        auto f = std::move(m_queue.front());
        m_queue.pop_front();
        f();
    }
    void runAll()
    {
        while (!m_queue.empty()) {
            auto f = std::move(m_queue.front());
            m_queue.pop_front();
            f();
        }
    }

private:
    std::deque<std::function<void()>> m_queue;
};

/// A scriptable backend. Answers are set by the test; every completion and
/// event goes through the PostQueue, never from inside the call. Long
/// operations advance one step per posted task, so that a test can cancel
/// or start another operation between two steps.
class FakeBackend final : public Backend
{
public:
    static BackendCapabilities defaultCapabilities(BackendKind kind)
    {
        if (kind == BackendKind::PackageKit)
            return {.userScope = false, .systemScope = true, .permissions = false, .refresh = true,
                    .cancelAnytime = false, .reattach = true, .verifiesExecutedPlan = false};
        return {.userScope = true, .systemScope = true, .permissions = true, .refresh = true,
                .cancelAnytime = true, .reattach = false, .verifiesExecutedPlan = true};
    }

    FakeBackend(BackendKind kind, PostQueue& queue)
        : m_kind(kind), m_queue(queue), m_capabilities(defaultCapabilities(kind))
    {
    }

    // --- Script -----------------------------------------------------------
    Result<void> availabilityAnswer = Result<void>::success();
    Result<std::vector<InstalledItem>> installedAnswer = std::vector<InstalledItem>{};
    Result<std::vector<BackendSource>> sourcesAnswer = std::vector<BackendSource>{};
    Result<void> changeSourceAnswer = Result<void>::success();
    Result<std::vector<SourceMetadata>> catalogSourcesAnswer = std::vector<SourceMetadata>{};
    Result<std::vector<AvailableUpdate>> updatesAnswer = std::vector<AvailableUpdate>{};
    Result<std::vector<std::string>> permissionsAnswer = std::vector<std::string>{};
    /// First-pass plans (plan()).
    std::function<Result<Plan>(const PlanRequest&)> planner;
    /// Plan found at execution time; planner when empty.
    std::function<Result<Plan>(const PlanRequest&)> executionPlanner;
    /// Progress reported during an execution, refresh or reattach, one step each.
    std::vector<Progress> progressSteps;
    /// Error reported after the progress steps instead of success.
    std::optional<Error> executionError;
    /// Handle reported by execute() (`events.handle`), if not empty.
    std::string handleToReport;
    /// Handles reattach() still knows.
    std::vector<std::string> knownHandles;

    // --- Observation --------------------------------------------------------
    int calls = 0;
    /// Plans the fake actually executed (after `started`).
    std::vector<Plan> executed;
    /// Source changes the fake accepted.
    std::vector<SourceRequest> sourceChanges;

    void setCapabilities(BackendCapabilities c) { m_capabilities = c; }
    /// A change made outside the store, delivered through the queue.
    void emitChanges(BackendChanges changes)
    {
        m_queue.post([this, changes] {
            if (m_listener)
                m_listener(changes);
        });
    }
    bool busy() const { return !m_modifying.empty(); }

    // --- Backend ------------------------------------------------------------
    BackendKind kind() const noexcept override { return m_kind; }
    BackendCapabilities capabilities() const noexcept override { return m_capabilities; }

    void availability(Completion<void> done) override { deliver(std::move(done), availabilityAnswer); }
    void queryInstalled(Completion<std::vector<InstalledItem>> done) override { deliver(std::move(done), installedAnswer); }
    void sources(Completion<std::vector<BackendSource>> done) override { deliver(std::move(done), sourcesAnswer); }
    void catalogSources(Completion<std::vector<SourceMetadata>> done) override
    {
        deliver(std::move(done), catalogSourcesAnswer);
    }
    void queryUpdates(Completion<std::vector<AvailableUpdate>> done) override { deliver(std::move(done), updatesAnswer); }
    void permissions(const OfferId&, Completion<std::vector<std::string>> done) override
    {
        if (!m_capabilities.permissions)
            return deliver(std::move(done), Result<std::vector<std::string>>(Error::notSupported("no permissions")));
        deliver(std::move(done), permissionsAnswer);
    }

    void plan(const std::string& id, const PlanRequest& request, Completion<Plan> done) override
    {
        ++calls;
        auto op = start(id, false);
        m_queue.post([this, op, request, done = std::move(done)]() mutable {
            Result<Plan> r = op->cancelRequested ? Result<Plan>(Error::cancelled("plan cancelled"))
                    : planner                    ? planner(request)
                                                 : Result<Plan>(Error::internal("no planner"));
            finish(op);
            done(std::move(r));
        });
    }

    void execute(const std::string& id, const PlanRequest& request, const Plan& accepted, OperationEvents events,
                 Completion<ExecutionResult> done) override
    {
        ++calls;
        if (busy())
            return deliver(std::move(done), Result<ExecutionResult>(Error::conflict("busy: " + m_modifying)));
        auto op = start(id, true);
        m_queue.post([this, op, request, accepted, events = std::move(events), done = std::move(done)]() mutable {
            if (op->cancelRequested) {
                finish(op);
                return done(Error::cancelled("cancelled before execution"));
            }
            auto& plans = executionPlanner ? executionPlanner : planner;
            Result<Plan> fresh = plans ? plans(request) : Result<Plan>(Error::internal("no planner"));
            if (!fresh) {
                finish(op);
                return done(std::move(fresh).error());
            }
            // D-017: nothing changes unless the fresh plan may run.
            if (!mayExecute(accepted, *fresh)) {
                finish(op);
                return done(ExecutionResult{ExecutionResult::Outcome::PlanChanged, std::move(fresh).value()});
            }
            if (events.started)
                events.started(*fresh);
            executed.push_back(*fresh);
            if (!handleToReport.empty() && events.handle)
                events.handle(handleToReport);
            runSteps(op, std::move(events), [done = std::move(done)](Result<void> r) mutable {
                if (!r)
                    return done(std::move(r).error());
                done(ExecutionResult{ExecutionResult::Outcome::Completed});
            });
        });
    }

    void reattach(const std::string& id, const std::string& backendRef, OperationEvents events,
                  Completion<ExecutionResult> done) override
    {
        ++calls;
        if (!m_capabilities.reattach)
            return deliver(std::move(done), Result<ExecutionResult>(Error::notSupported("no reattach")));
        if (std::find(knownHandles.begin(), knownHandles.end(), backendRef) == knownHandles.end())
            return deliver(std::move(done), Result<ExecutionResult>(Error::notFound("unknown " + backendRef)));
        if (busy())
            return deliver(std::move(done), Result<ExecutionResult>(Error::conflict("busy: " + m_modifying)));
        auto op = start(id, true);
        m_queue.post([this, op, events = std::move(events), done = std::move(done)]() mutable {
            runSteps(op, std::move(events), [done = std::move(done)](Result<void> r) mutable {
                if (!r)
                    return done(std::move(r).error());
                done(ExecutionResult{ExecutionResult::Outcome::Completed});
            });
        });
    }

    void refresh(const std::string& id, OperationEvents events, Completion<void> done) override
    {
        ++calls;
        if (!m_capabilities.refresh)
            return deliver(std::move(done), Result<void>(Error::notSupported("no refresh")));
        if (busy())
            return deliver(std::move(done), Result<void>(Error::conflict("busy: " + m_modifying)));
        auto op = start(id, true);
        m_queue.post([this, op, events = std::move(events), done = std::move(done)]() mutable {
            runSteps(op, std::move(events), std::move(done));
        });
    }

    void changeSource(const std::string&, const SourceRequest& change, Completion<void> done) override
    {
        ++calls;
        if (!m_capabilities.userSourceChanges)
            return deliver(std::move(done), Result<void>(Error::notSupported("no source changes")));
        if (busy())
            return deliver(std::move(done), Result<void>(Error::conflict("busy: " + m_modifying)));
        sourceChanges.push_back(change);
        deliver(std::move(done), changeSourceAnswer);
    }

    Result<void> cancel(const std::string& id) override
    {
        const auto it = m_operations.find(id);
        if (it == m_operations.end())
            return Error::notFound("no operation " + id);
        Operation& op = *it->second;
        if (op.modifying && op.started && !m_capabilities.cancelAnytime && !op.cancellableNow)
            return Error::notSupported("not cancellable now");
        op.cancelRequested = true;
        return {};
    }

    void setChangeListener(std::function<void(const BackendChanges&)> listener) override
    {
        m_listener = std::move(listener);
    }

private:
    struct Operation
    {
        std::string id;
        bool modifying = false;
        bool started = false;
        bool cancellableNow = false;
        bool cancelRequested = false;
    };

    std::shared_ptr<Operation> start(const std::string& id, bool modifying)
    {
        auto op = std::make_shared<Operation>(Operation{.id = id, .modifying = modifying});
        m_operations[id] = op;
        if (modifying)
            m_modifying = id;
        return op;
    }

    void finish(const std::shared_ptr<Operation>& op)
    {
        m_operations.erase(op->id);
        if (op->modifying && m_modifying == op->id)
            m_modifying.clear();
    }

    // One progress step per posted task; checks for a cancellation between
    // steps; then the error or success.
    void runSteps(std::shared_ptr<Operation> op, OperationEvents events, std::function<void(Result<void>)> done,
                  std::size_t step = 0)
    {
        op->started = true;
        if (op->cancelRequested) {
            finish(op);
            return done(Error::cancelled("cancelled"));
        }
        if (step < progressSteps.size()) {
            op->cancellableNow = progressSteps[step].cancellable;
            if (events.progress)
                events.progress(progressSteps[step]);
            m_queue.post([this, op, events = std::move(events), done = std::move(done), step]() mutable {
                runSteps(op, std::move(events), std::move(done), step + 1);
            });
            return;
        }
        finish(op);
        if (executionError)
            return done(*executionError);
        done(Result<void>::success());
    }

    template <typename T>
    void deliver(Completion<T> done, Result<T> answer)
    {
        ++calls;
        m_queue.post([done = std::move(done), answer = std::move(answer)]() mutable { done(std::move(answer)); });
    }

    BackendKind m_kind;
    PostQueue& m_queue;
    BackendCapabilities m_capabilities;
    std::map<std::string, std::shared_ptr<Operation>> m_operations;
    std::string m_modifying;
    std::function<void(const BackendChanges&)> m_listener;
};

} // namespace fakes
