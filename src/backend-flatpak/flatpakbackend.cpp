#include "flatpakbackend.h"

#include "translate.h"

#include <cutefish/store/confirmation.h>

#include <algorithm>
#include <optional>
#include <string>
#include <utility>

namespace cutefish::store::flatpak {

namespace {

/// The transaction a PlanRequest describes, for the store: one kind, one
/// installation, one or more refs. The offer id already names the remote
/// (OfferAddress), so the store does not have to look it up.
Result<TransactionSpec> specFor(const PlanRequest& request)
{
    if (request.backend() != BackendKind::Flatpak)
        return Error::invalidArgument("not a Flatpak request");
    TransactionSpec spec{.kind = request.kind(), .scope = request.scope(), .refs = {}};
    for (const OfferAddress& target : request.targets()) {
        auto ref = parseRef(target.package);
        if (!ref)
            return std::move(ref).error();
        spec.refs.push_back({std::move(ref).value(), target.origin});
    }
    return spec;
}

/// The plan of a transaction, checked against its request (the backend's own
/// check, before the manager applies the confirmation rules).
Result<Plan> planFor(const PlanRequest& request, const std::vector<FlatpakOp>& ops)
{
    std::vector<std::string> requested;
    requested.reserve(request.targets().size());
    for (const OfferAddress& target : request.targets())
        requested.push_back(target.package);
    auto plan = buildPlan(request.kind(), request.scope(), ops, requested);
    if (!plan)
        return std::move(plan).error();
    if (auto ok = checkPlanForRequest(request, *plan); !ok)
        return std::move(ok).error();
    return plan;
}

std::optional<OperationPhase> phaseOf(std::string_view status)
{
    if (status == "waiting-for-auth" || status == "waiting-for-lock")
        return status == "waiting-for-auth" ? OperationPhase::WaitingForAuth : OperationPhase::WaitingForLock;
    if (status == "downloading" || status == "pulling" || status == "fetching")
        return OperationPhase::Downloading;
    if (status == "installing" || status == "deploying" || status == "updating")
        return OperationPhase::Installing;
    if (status == "removing" || status == "uninstalling")
        return OperationPhase::Removing;
    if (status == "finalizing" || status == "finishing")
        return OperationPhase::Finishing;
    return std::nullopt;
}

Progress progressOf(const FlatpakProgress& p)
{
    Progress out;
    out.phase = phaseOf(p.status);
    out.fraction = p.fraction;
    out.cancellable = true; // Flatpak is cancellable at the next step (§7.6)
    out.item = p.ref;
    return out;
}

/// The offer id of an installed or updatable ref, matching Offer::create:
/// `flatpak:<scope>:<remote>:<ref>`.
Result<OfferId> offerOf(Scope scope, const std::string& origin, const FlatpakRef& ref)
{
    return OfferId::parse("flatpak:" + std::string(toString(scope)) + ":" + origin + ":" + ref.full());
}

} // namespace

FlatpakBackend::FlatpakBackend(Options options, std::unique_ptr<FlatpakStore> store)
    : m_options(std::move(options)), m_store(std::move(store))
{
}

FlatpakBackend::~FlatpakBackend()
{
    *m_alive = false;
    // m_worker is destroyed after this body: its thread is joined before
    // m_store and m_options go away.
}

BackendCapabilities FlatpakBackend::capabilities() const noexcept
{
    return {.userScope = true,
            .systemScope = true,
            .permissions = true,
            .refresh = false, // appstream update comes with step I
            .cancelAnytime = true,
            .reattach = false, // Flatpak runs in-service: nothing to reattach
            .verifiesExecutedPlan = true};
}

void FlatpakBackend::availability(Completion<void> done)
{
    run<void>([this]() -> Result<void> {
        if (m_store->scopes().empty())
            return Error::backend(BackendFailure::Unavailable, "no Flatpak installation is available");
        return Result<void>::success();
    }, std::move(done));
}

void FlatpakBackend::queryInstalled(Completion<std::vector<InstalledItem>> done)
{
    run<std::vector<InstalledItem>>([this]() -> Result<std::vector<InstalledItem>> {
        std::vector<InstalledItem> out;
        for (const Scope scope : m_store->scopes()) {
            auto installed = m_store->installed(scope);
            if (!installed)
                return std::move(installed).error();
            for (const InstalledRef& r : *installed) {
                if (r.origin.empty())
                    continue; // no remote to name it by: nothing links to the catalogue
                auto offer = offerOf(scope, r.origin, r.ref);
                if (!offer)
                    continue;
                auto item = InstalledItem::create({.offer = std::move(offer).value(),
                                                    .app = std::nullopt,
                                                    .backend = BackendKind::Flatpak,
                                                    .scope = scope,
                                                    .version = std::nullopt,
                                                    .origin = r.origin});
                if (item)
                    out.push_back(std::move(item).value());
            }
        }
        return out;
    }, std::move(done));
}

void FlatpakBackend::sources(Completion<std::vector<Source>> done)
{
    run<std::vector<Source>>([this]() -> Result<std::vector<Source>> {
        std::vector<Source> out;
        for (const Scope scope : m_store->scopes()) {
            auto remotes = m_store->remotes(scope);
            if (!remotes)
                return std::move(remotes).error();
            for (const RemoteInfo& remote : *remotes) {
                auto source = sourceForRemote(scope, remote);
                if (source)
                    out.push_back(std::move(source).value());
            }
        }
        return out;
    }, std::move(done));
}

void FlatpakBackend::catalogSources(Completion<std::vector<SourceMetadata>> done)
{
    run<std::vector<SourceMetadata>>([this]() -> Result<std::vector<SourceMetadata>> {
        std::vector<SourceMetadata> out;
        for (const Scope scope : m_store->scopes()) {
            auto remotes = m_store->remotes(scope);
            if (!remotes)
                return std::move(remotes).error();
            for (const RemoteInfo& remote : *remotes) {
                auto source = sourceForRemote(scope, remote);
                if (!source)
                    continue;
                if (remote.metadataPath.empty())
                    continue; // no appstream yet: nothing for the indexer
                out.push_back({.source = std::move(source).value(),
                               .files = {.metadata = remote.metadataPath, .icons = remote.iconsPath}});
            }
        }
        return out;
    }, std::move(done));
}

void FlatpakBackend::queryUpdates(Completion<std::vector<AvailableUpdate>> done)
{
    run<std::vector<AvailableUpdate>>([this]() -> Result<std::vector<AvailableUpdate>> {
        std::vector<AvailableUpdate> out;
        for (const Scope scope : m_store->scopes()) {
            auto updates = m_store->updates(scope);
            if (!updates)
                return std::move(updates).error();
            for (const UpdateRef& u : *updates) {
                auto offer = offerOf(scope, u.origin, u.ref);
                if (!offer)
                    continue;
                out.push_back({.offer = std::move(offer).value(),
                               .app = std::nullopt,
                               .kind = u.ref.kind == RefKind::App ? PlanItemKind::Application : PlanItemKind::Runtime,
                               .fromVersion = std::nullopt,
                               .toVersion = std::nullopt,
                               .downloadSize = 0});
            }
        }
        return out;
    }, std::move(done));
}

void FlatpakBackend::permissions(const OfferId& offer, Completion<std::vector<std::string>> done)
{
    auto address = decodeOfferId(offer);
    if (!address)
        return run<std::vector<std::string>>([e = std::move(address).error()]() -> Result<std::vector<std::string>> {
            return e;
        }, std::move(done));
    if (address->backend != BackendKind::Flatpak)
        return run<std::vector<std::string>>([]() -> Result<std::vector<std::string>> {
            return Error::notSupported("not a Flatpak offer");
        }, std::move(done));
    auto ref = parseRef(address->package);
    if (!ref)
        return run<std::vector<std::string>>([e = std::move(ref).error()]() -> Result<std::vector<std::string>> {
            return e;
        }, std::move(done));
    const Scope scope = address->scope;
    const std::string remote = address->origin;
    const FlatpakRef parsed = std::move(ref).value();
    run<std::vector<std::string>>([this, scope, remote, parsed]() -> Result<std::vector<std::string>> {
        auto metadata = m_store->metadata(scope, remote, parsed);
        if (!metadata)
            return std::move(metadata).error();
        return metadata->permissions;
    }, std::move(done));
}

void FlatpakBackend::plan(const std::string&, const PlanRequest& request, Completion<Plan> done)
{
    auto spec = specFor(request);
    if (!spec)
        return run<Plan>([e = std::move(spec).error()]() -> Result<Plan> { return e; }, std::move(done));
    const TransactionSpec value = std::move(spec).value();
    run<Plan>([this, request, value]() -> Result<Plan> {
        auto ops = m_store->resolve(value);
        if (!ops)
            return std::move(ops).error();
        return planFor(request, *ops);
    }, std::move(done));
}

void FlatpakBackend::execute(const std::string& operationId, const PlanRequest& request, const Plan& accepted,
                             OperationEvents events, Completion<ExecutionResult> done)
{
    auto spec = specFor(request);
    if (!spec)
        return run<ExecutionResult>([e = std::move(spec).error()]() -> Result<ExecutionResult> { return e; }, std::move(done));
    const TransactionSpec value = std::move(spec).value();
    const auto cancelled = std::make_shared<std::atomic<bool>>(false);
    {
        std::lock_guard lock(m_mutex);
        m_cancelled[operationId] = cancelled;
    }
    const auto post = m_options.post;
    const auto alive = m_alive;

    m_worker.run([this, operationId, request, accepted, value, events = std::move(events), done = std::move(done),
                  cancelled, post, alive]() mutable {
        // The second pass: the plan that is about to run is compared with the
        // accepted one in the transaction's `ready` (D-017). Everything the
        // host posts is in order: started before progress before completion.
        struct State
        {
            std::optional<Plan> fresh;
            std::optional<Error> error;
        };
        auto state = std::make_shared<State>();

        ExecuteControl control;
        control.cancelled = [cancelled] { return cancelled->load(); };
        control.progress = [post, alive, events](const FlatpakProgress& p) {
            const Progress progress = progressOf(p);
            post([alive, events, progress] {
                if (*alive && events.progress)
                    events.progress(progress);
            });
        };
        control.onReady = [request, accepted, events, post, alive, state](const std::vector<FlatpakOp>& ops) -> bool {
            auto plan = planFor(request, ops);
            if (!plan) {
                state->error = std::move(plan).error();
                return false;
            }
            if (!mayExecute(accepted, *plan)) {
                state->fresh = std::move(plan).value();
                return false;
            }
            const Plan fresh = *plan;
            post([alive, events, fresh] {
                if (*alive && events.started)
                    events.started(fresh);
            });
            return true;
        };

        auto result = m_store->execute(value, std::move(control));
        {
            std::lock_guard lock(m_mutex);
            m_cancelled.erase(operationId);
        }
        if (!result) {
            post([alive, done, e = std::move(result).error()]() mutable {
                if (*alive)
                    done(std::move(e));
            });
            return;
        }
        const ExecutionOutcome outcome = std::move(result).value();
        if (outcome.type == ExecutionOutcome::Type::PlanChanged && state->fresh) {
            const Plan fresh = *state->fresh;
            post([alive, done, fresh]() mutable {
                if (*alive)
                    done(ExecutionResult{ExecutionResult::Outcome::PlanChanged, std::move(fresh)});
            });
            return;
        }
        if (outcome.type == ExecutionOutcome::Type::PlanChanged && state->error) {
            const Error e = *state->error;
            post([alive, done, e]() mutable {
                if (*alive)
                    done(e);
            });
            return;
        }
        if (outcome.type == ExecutionOutcome::Type::Completed) {
            post([alive, done]() mutable {
                if (*alive)
                    done(ExecutionResult{ExecutionResult::Outcome::Completed});
            });
            return;
        }
        post([alive, done]() mutable {
            if (*alive)
                done(Error::internal("Flatpak execution ended without an outcome"));
        });
    });
}

void FlatpakBackend::reattach(const std::string&, const std::string&, OperationEvents,
                              Completion<ExecutionResult> done)
{
    run<ExecutionResult>([]() -> Result<ExecutionResult> {
        return Error::notSupported("a Flatpak transaction does not outlive the service");
    }, std::move(done));
}

void FlatpakBackend::refresh(const std::string&, OperationEvents, Completion<void> done)
{
    run<void>([]() -> Result<void> {
        return Error::notSupported("Flatpak repository refresh comes with step I");
    }, std::move(done));
}

Result<void> FlatpakBackend::cancel(const std::string& operationId)
{
    std::lock_guard lock(m_mutex);
    const auto it = m_cancelled.find(operationId);
    if (it == m_cancelled.end())
        return Error::notFound("no operation " + operationId);
    it->second->store(true);
    return Result<void>::success();
}

void FlatpakBackend::setChangeListener(std::function<void(const BackendChanges&)> listener)
{
    // The installation monitor needs a GMainContext; not wired in this v0.
    // The service re-reads the state at each refresh and after a transaction.
    m_listener = std::move(listener);
}

} // namespace cutefish::store::flatpak
