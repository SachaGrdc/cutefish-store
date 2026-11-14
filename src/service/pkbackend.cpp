#include "pkbackend.h"

#include <base/logging.h>

#include "debianprofile.h"
#include "pkenums.h"

#include <cutefish/store/confirmation.h>

#include <base/thread_pool.h>

#include <algorithm>

namespace cutefish::store::service {

namespace dbus = cutefish::base::dbus;
using namespace cutefish::store::packagekit;

namespace {

/// The roles the backend needs (SERVICE.md §6.1: missing roles mean the
/// backend is unavailable).
constexpr std::pair<Role, const char*> kRoles[] = {
    {Role::GetPackages, "get-packages"},   {Role::GetRepoList, "get-repo-list"},
    {Role::GetUpdates, "get-updates"},     {Role::Resolve, "resolve"},
    {Role::InstallPackages, "install-packages"}, {Role::RemovePackages, "remove-packages"},
    {Role::UpdatePackages, "update-packages"},   {Role::RefreshCache, "refresh-cache"},
};

std::vector<std::string> names(const PlanRequest& request)
{
    std::vector<std::string> out;
    for (const OfferAddress& t : request.targets())
        out.push_back(t.package);
    return out;
}

base::LogCategory cfPkBackend("cutefish.store.packagekit");

/// A request refused because PackageKit changes the system for another
/// program (H.2): temporary, nothing was started.
Error busyError(const ExternalActivity& a)
{
    return Error::backend(BackendFailure::Locked,
                          "PackageKit is busy with another program's operation (" + a.role
                                  + (a.status.empty() ? "" : ", " + a.status)
                                  + "); nothing was started: try again once it has finished");
}

} // namespace

PackageKitBackend::PackageKitBackend(base::EventLoop& main, Options options)
    : m_main(main), m_options(std::move(options)), m_changeTimer(&main),
      m_transport(std::make_unique<PkTransport>(m_options.bus))
{
    m_changeTimer.setSingleShot(true);
    m_changeTimer.onTimeout = [this] {
        const BackendChanges changes = m_pendingChanges;
        m_pendingChanges = {};
        if (m_listener)
            m_listener(changes);
    };
    // Other programs' transactions (H.2): observed from the start, so that
    // the first request already knows whether PackageKit is busy.
    m_transport->watchTransactions([this](std::vector<PkTransactionInfo> list) {
        // On the PackageKit thread.
        m_pkActivity = packagekit::externalActivity(list);
        toMain([this, activity = m_pkActivity] {
            // Told when what it is (kind) or who (role) changes, not at each
            // step of its progress.
            const bool changed = activity.kind != m_activity.kind || activity.role != m_activity.role;
            m_activity = activity;
            if (changed) {
                CF_INFO(cfPkBackend) << "PackageKit, other programs: " << toString(activity.kind)
                                     << (activity.role.empty() ? "" : " (" + activity.role + ")");
                if (m_listener)
                    m_listener(BackendChanges{.activity = true});
            }
        });
    });
}

PackageKitBackend::~PackageKitBackend()
{
    // Results still on their way to the main loop are dropped. The
    // transport, the last member, then stops its thread in its own
    // destructor, before the members its callbacks use; packagekitd keeps
    // running what it runs.
    *m_alive = false;
}

BackendCapabilities PackageKitBackend::capabilities() const noexcept
{
    return {.userScope = false,
            .systemScope = true,
            .permissions = false,
            .refresh = true,
            .cancelAnytime = false,
            .reattach = true,
            .verifiesExecutedPlan = false,
            .userSourceChanges = false}; // repositories are read-only (D-039)
}

void PackageKitBackend::toMain(std::function<void()> task)
{
    m_main.post([alive = m_alive, task = std::move(task)] {
        if (*alive)
            task();
    });
}

template <typename T>
void PackageKitBackend::later(Completion<T> done, Result<T> result)
{
    toMain([done = std::move(done), result = std::move(result)]() mutable { done(std::move(result)); });
}

PkCall PackageKitBackend::call(std::string method, std::function<void(dbus::Message&)> arguments,
                               std::string operation, std::chrono::milliseconds timeout) const
{
    PkCall c;
    c.method = std::move(method);
    c.arguments = std::move(arguments);
    c.operation = std::move(operation);
    c.timeout = timeout;
    return c;
}

void PackageKitBackend::withProfile(std::function<void(const Profile&)> then, std::function<void(Error)> fail)
{
    if (m_profile)
        return then(*m_profile);
    m_transport->daemonInfo([this, then = std::move(then), fail = std::move(fail)](PkTransport::DaemonInfo info) mutable {
        toMain([this, info = std::move(info), then = std::move(then), fail = std::move(fail)] {
            if (!info.error.empty())
                return fail(Error::backend(BackendFailure::Unavailable, "PackageKit is not reachable: " + info.error));
            if (info.backendName.empty())
                return fail(Error::backend(BackendFailure::Unavailable, "PackageKit has no backend"));
            if (!debian::applies(info.distroId))
                return fail(Error::backend(BackendFailure::Unavailable,
                                           "distribution not supported by the profiles: " + info.distroId));
            const std::string arch = debian::dpkgArchitecture(info.distroId);
            if (arch.empty())
                return fail(Error::backend(BackendFailure::Unavailable, "unknown architecture in " + info.distroId));
            std::string missing;
            for (const auto& [role, name] : kRoles)
                if (!(info.roles & bit(role)))
                    missing += (missing.empty() ? "" : ", ") + std::string(name);
            if (!missing.empty())
                return fail(Error::backend(BackendFailure::Unavailable,
                                           "PackageKit backend " + info.backendName + " lacks: " + missing));
            m_profile = Profile{.nativeArch = arch, .isProtected = [](std::string_view n) { return debian::isProtected(n); }};
            then(*m_profile);
        });
    });
}

void PackageKitBackend::availability(Completion<void> done)
{
    // Asks the daemon again each time: it may have come or gone.
    m_profile.reset();
    auto shared = std::make_shared<Completion<void>>(std::move(done));
    withProfile([this, shared](const Profile&) { later(std::move(*shared), Result<void>::success()); },
                [this, shared](Error e) { later(std::move(*shared), Result<void>(std::move(e))); });
}

void PackageKitBackend::queryInstalled(Completion<std::vector<InstalledItem>> done)
{
    using R = Result<std::vector<InstalledItem>>;
    auto shared = std::make_shared<Completion<std::vector<InstalledItem>>>(std::move(done));
    withProfile(
            [this, shared](const Profile& profile) {
                PkCall c = call("GetPackages",
                                [](dbus::Message& m) { m.append(std::uint64_t(filter::kInstalled | filter::kArch)); },
                                {}, m_options.queryTimeout);
                c.done = [this, shared, profile](PkResult r) { later(std::move(*shared), installedItems(r, profile)); };
                m_transport->submit(std::move(c));
            },
            [this, shared](Error e) { later(std::move(*shared), R(std::move(e))); });
}

void PackageKitBackend::sources(Completion<std::vector<BackendSource>> done)
{
    using R = Result<std::vector<BackendSource>>;
    auto shared = std::make_shared<Completion<std::vector<BackendSource>>>(std::move(done));
    withProfile(
            [this, shared](const Profile&) {
                PkCall c = call("GetRepoList", [](dbus::Message& m) { m.append(filter::kNone); }, {},
                                m_options.queryTimeout);
                c.done = [this, shared](PkResult r) {
                    if (auto ok = checkResult(r); !ok)
                        return later(std::move(*shared), R(std::move(ok).error()));
                    std::vector<BackendSource> out;
                    for (const PkRepo& repo : r.repos) {
                        auto s = debian::repositorySource(repo.id, repo.description, repo.enabled);
                        if (!s)
                            continue;
                        // PackageKit tells neither whether APT checks a
                        // repository's signatures nor what was installed
                        // from it: both stay unknown.
                        out.push_back({.source = std::move(*s),
                                       .catalogueSources = debian::catalogueSourceIds(repo.id),
                                       .verifiesSignatures = std::nullopt,
                                       .installedItems = std::nullopt});
                    }
                    later(std::move(*shared), R(std::move(out)));
                };
                m_transport->submit(std::move(c));
            },
            [this, shared](Error e) { later(std::move(*shared), R(std::move(e))); });
}

void PackageKitBackend::catalogSources(Completion<std::vector<SourceMetadata>> done)
{
    // A directory listing: not on the main loop, and nothing to ask the daemon.
    base::runInThread([dir = m_options.swcatalog] { return debian::catalogSources(dir); },
                      [alive = m_alive, done = std::move(done)](std::vector<SourceMetadata> sources) mutable {
                          if (*alive)
                              done(std::move(sources));
                      })
            .detach();
}

void PackageKitBackend::queryUpdates(Completion<std::vector<AvailableUpdate>> done)
{
    using R = Result<std::vector<AvailableUpdate>>;
    auto shared = std::make_shared<Completion<std::vector<AvailableUpdate>>>(std::move(done));
    withProfile(
            [this, shared](const Profile& profile) {
                // The installed versions first, to leave out the updates
                // listed at the installed version (D-010).
                PkCall installed = call("GetPackages",
                                        [](dbus::Message& m) { m.append(std::uint64_t(filter::kInstalled | filter::kArch)); },
                                        {}, m_options.queryTimeout);
                installed.done = [this, shared, profile](PkResult r) {
                    auto items = installedItems(r, profile);
                    if (!items)
                        return later(std::move(*shared), R(std::move(items).error()));
                    PkCall updates = call("GetUpdates", [](dbus::Message& m) { m.append(filter::kArch); }, {},
                                          m_options.queryTimeout);
                    updates.urgent = true;
                    updates.done = [this, shared, profile, items = std::move(items).value()](PkResult u) {
                        later(std::move(*shared), availableUpdates(u, items, profile));
                    };
                    m_transport->submit(std::move(updates));
                };
                m_transport->submit(std::move(installed));
            },
            [this, shared](Error e) { later(std::move(*shared), R(std::move(e))); });
}

void PackageKitBackend::changeSource(const std::string&, const SourceRequest&, Completion<void> done)
{
    // D-039: the store lists the repositories of the distribution and never
    // changes them. PackageKit could (RepoEnable, RepoRemove, polkit
    // system-sources-configure), but cannot add one on Debian (no RepoSetData
    // in the APT backend), and the system's repositories belong to the
    // system's tools.
    later(std::move(done), Result<void>(Error::notSupported("the store does not change the distribution's repositories")));
}

void PackageKitBackend::permissions(const OfferId&, Completion<std::vector<std::string>> done)
{
    later(std::move(done), Result<std::vector<std::string>>(Error::notSupported("PackageKit packages have no permissions")));
}

Result<void> PackageKitBackend::admit(const std::string& id, const PlanRequest* request, bool modifying)
{
    if (request && request->backend() != BackendKind::PackageKit)
        return Error::invalidArgument("not a PackageKit request");
    if (m_operations.contains(id))
        return Error::conflict("operation " + id + " is already running");
    if (modifying && !m_modifying.empty())
        return Error::conflict("PackageKit is busy with " + m_modifying);
    Operation op;
    op.modifying = modifying;
    m_operations.emplace(id, std::move(op));
    if (modifying)
        m_modifying = id;
    return {};
}

void PackageKitBackend::forget(const std::string& id)
{
    m_operations.erase(id);
    if (m_modifying == id)
        m_modifying.clear();
}

void PackageKitBackend::simulate(const std::string& id, PlanRequest request, const Profile& profile, bool urgent,
                                 std::function<void(Result<SimulatedPlan>, std::vector<std::string>)> done)
{
    // 1. The package ids: what is installable, installed, or updatable.
    std::string method = "Resolve";
    std::function<void(dbus::Message&)> args;
    const std::vector<std::string> packageNames = names(request);
    switch (request.kind()) {
    case TransactionKind::Install:
        args = [packageNames](dbus::Message& m) { m.append(std::uint64_t(filter::kNotInstalled | filter::kArch), packageNames); };
        break;
    case TransactionKind::Remove:
        args = [packageNames](dbus::Message& m) { m.append(std::uint64_t(filter::kInstalled | filter::kArch), packageNames); };
        break;
    case TransactionKind::Update:
        method = "GetUpdates";
        args = [](dbus::Message& m) { m.append(filter::kArch); };
        break;
    }
    PkCall resolve = call(method, std::move(args), id, m_options.queryTimeout);
    resolve.urgent = urgent;
    resolve.done = [this, id, request, profile, done = std::move(done)](PkResult r) mutable {
        auto ids = idsFor(request, r, profile);
        if (!ids)
            return done(std::move(ids).error(), {});
        // 2. The simulation, at once, before any other transaction.
        const std::vector<std::string> list = *ids;
        const std::uint64_t flags = flag::kOnlyTrusted | flag::kSimulate;
        std::function<void(dbus::Message&)> simArgs;
        std::string simMethod;
        switch (request.kind()) {
        case TransactionKind::Install:
            simMethod = "InstallPackages";
            simArgs = [flags, list](dbus::Message& m) { m.append(flags, list); };
            break;
        case TransactionKind::Remove:
            // allow_deps=false is ignored by the APT backend (prototype 4):
            // the cascade is in the plan and confirmed (P-008).
            simMethod = "RemovePackages";
            simArgs = [flags, list](dbus::Message& m) { m.append(flags, list, true, false); };
            break;
        case TransactionKind::Update:
            simMethod = "UpdatePackages";
            simArgs = [flags, list](dbus::Message& m) { m.append(flags, list); };
            break;
        }
        PkCall sim = call(simMethod, std::move(simArgs), id, m_options.queryTimeout);
        sim.urgent = true;
        sim.done = [request, profile, list, done = std::move(done)](PkResult s) mutable {
            done(planFromSimulation(request, s, profile), list);
        };
        m_transport->submit(std::move(sim));
    };
    m_transport->submit(std::move(resolve));
}

void PackageKitBackend::plan(const std::string& id, const PlanRequest& request, Completion<Plan> done)
{
    // A simulation would only wait behind the other program's transaction
    // in packagekitd, for as long as it lasts.
    if (m_activity.blocksChanges())
        return later(std::move(done), Result<Plan>(busyError(m_activity)));
    if (auto ok = admit(id, &request, false); !ok)
        return later(std::move(done), Result<Plan>(std::move(ok).error()));
    auto shared = std::make_shared<Completion<Plan>>(std::move(done));
    withProfile(
            [this, id, request, shared](const Profile& profile) {
                simulate(id, request, profile, false,
                         [this, id, shared](Result<SimulatedPlan> p, std::vector<std::string>) {
                             toMain([this, id, shared, p = std::move(p)]() mutable {
                                 forget(id);
                                 if (!p)
                                     return (*shared)(std::move(p).error());
                                 (*shared)(std::move(p->plan));
                             });
                         });
            },
            [this, id, shared](Error e) {
                forget(id);
                later(std::move(*shared), Result<Plan>(std::move(e)));
            });
}

void PackageKitBackend::execute(const std::string& id, const PlanRequest& request, const Plan& accepted,
                                OperationEvents events, Completion<ExecutionResult> done)
{
    using R = Result<ExecutionResult>;
    if (m_activity.blocksChanges())
        return later(std::move(done), R(busyError(m_activity)));
    if (auto ok = admit(id, &request, true); !ok)
        return later(std::move(done), R(std::move(ok).error()));
    auto shared = std::make_shared<Completion<ExecutionResult>>(std::move(done));
    auto ev = std::make_shared<OperationEvents>(std::move(events));
    auto cancelled = m_operations.at(id).cancelRequested;
    auto finish = [this, id, shared](R r) {
        toMain([this, id, shared, r = std::move(r)]() mutable {
            forget(id);
            (*shared)(std::move(r));
        });
    };
    withProfile(
            [this, id, request, accepted, shared, ev, cancelled, finish](const Profile& profile) {
                // A new simulation, compared with the accepted plan before
                // anything changes (D-017). It is a separate transaction:
                // PackageKit recomputes everything for the real one, and
                // nothing guarantees that it follows this plan (ARCHITECTURE
                // §7.2) — verifiesExecutedPlan is false.
                simulate(id, request, profile, false,
                         [this, id, request, accepted, ev, cancelled, finish](Result<SimulatedPlan> p,
                                                                              std::vector<std::string> ids) mutable {
                             if (!p)
                                 return finish(std::move(p).error());
                             if (!mayExecute(accepted, p->plan))
                                 return finish(ExecutionResult{ExecutionResult::Outcome::PlanChanged, std::move(p->plan)});
                             if (*cancelled)
                                 return finish(Error::cancelled("cancelled before the transaction"));
                             // Last check, on the thread that submits the
                             // real transaction: another program may have
                             // started changing the system meanwhile.
                             if (m_pkActivity.blocksChanges())
                                 return finish(busyError(m_pkActivity));
                             const std::uint64_t total = p->downloadSize;
                             toMain([this, id, ev, plan = p->plan] {
                                 if (auto it = m_operations.find(id); it != m_operations.end())
                                     it->second.started = true;
                                 if (ev->started)
                                     ev->started(plan);
                             });
                             // The real transaction, right after its
                             // simulation; interactive: polkit may ask.
                             std::function<void(dbus::Message&)> args;
                             std::string method;
                             const std::uint64_t flags = flag::kOnlyTrusted;
                             switch (request.kind()) {
                             case TransactionKind::Install:
                                 method = "InstallPackages";
                                 args = [flags, ids](dbus::Message& m) { m.append(flags, ids); };
                                 break;
                             case TransactionKind::Remove:
                                 method = "RemovePackages";
                                 args = [flags, ids](dbus::Message& m) { m.append(flags, ids, true, false); };
                                 break;
                             case TransactionKind::Update:
                                 method = "UpdatePackages";
                                 args = [flags, ids](dbus::Message& m) { m.append(flags, ids); };
                                 break;
                             }
                             PkCall real = call(method, std::move(args), id, m_options.transactionTimeout);
                             real.interactive = true;
                             real.urgent = true;
                             real.created = [this, ev](const std::string& path) {
                                 toMain([ev, path] {
                                     if (ev->handle)
                                         ev->handle(path);
                                 });
                             };
                             real.progress = [this, id, ev, total](const PkProgress& pr) {
                                 const Progress progress = progressOf(pr, total);
                                 toMain([this, id, ev, progress] {
                                     if (auto it = m_operations.find(id); it != m_operations.end())
                                         it->second.cancellable = progress.cancellable;
                                     if (ev->progress)
                                         ev->progress(progress);
                                 });
                             };
                             real.done = [finish](PkResult r) mutable {
                                 if (auto ok = checkResult(r); !ok)
                                     return finish(std::move(ok).error());
                                 finish(ExecutionResult{ExecutionResult::Outcome::Completed});
                             };
                             m_transport->submit(std::move(real));
                         });
            },
            [finish](Error e) mutable { finish(std::move(e)); });
}

void PackageKitBackend::reattach(const std::string& id, const std::string& backendRef, OperationEvents events,
                                 Completion<ExecutionResult> done)
{
    using R = Result<ExecutionResult>;
    if (auto ok = admit(id, nullptr, true); !ok)
        return later(std::move(done), R(std::move(ok).error()));
    if (auto it = m_operations.find(id); it != m_operations.end())
        it->second.started = true;
    auto shared = std::make_shared<Completion<ExecutionResult>>(std::move(done));
    auto ev = std::make_shared<OperationEvents>(std::move(events));
    auto finish = [this, id, shared](R r) {
        toMain([this, id, shared, r = std::move(r)]() mutable {
            forget(id);
            (*shared)(std::move(r));
        });
    };
    m_transport->transactions([this, id, backendRef, ev, finish](std::vector<std::string> paths, std::string error) mutable {
        if (!error.empty())
            return finish(Error::backend(BackendFailure::Unavailable, "PackageKit is not reachable: " + error));
        if (std::find(paths.begin(), paths.end(), backendRef) == paths.end())
            return finish(Error::notFound("PackageKit no longer runs " + backendRef));
        m_transport->attach(
                backendRef, id,
                [this, id, ev](const PkProgress& pr) {
                    const Progress progress = progressOf(pr, 0);
                    toMain([this, id, ev, progress] {
                        if (auto it = m_operations.find(id); it != m_operations.end())
                            it->second.cancellable = progress.cancellable;
                        if (ev->progress)
                            ev->progress(progress);
                    });
                },
                [finish](PkResult r) mutable {
                    if (auto ok = checkResult(r); !ok)
                        return finish(std::move(ok).error());
                    finish(ExecutionResult{ExecutionResult::Outcome::Completed});
                });
    });
}

void PackageKitBackend::refresh(const std::string& id, OperationEvents events, Completion<void> done)
{
    if (m_activity.blocksChanges())
        return later(std::move(done), Result<void>(busyError(m_activity)));
    if (auto ok = admit(id, nullptr, true); !ok)
        return later(std::move(done), Result<void>(std::move(ok).error()));
    if (auto it = m_operations.find(id); it != m_operations.end())
        it->second.started = true;
    auto shared = std::make_shared<Completion<void>>(std::move(done));
    auto ev = std::make_shared<OperationEvents>(std::move(events));
    // No `background` hint: such a transaction is cancelled by any request in
    // the foreground (SERVICE.md §6.6, Q-10).
    PkCall c = call("RefreshCache", [](dbus::Message& m) { m.append(false); }, id, m_options.transactionTimeout);
    c.interactive = true;
    c.progress = [this, id, ev](const PkProgress& pr) {
        const Progress progress = progressOf(pr, 0);
        toMain([this, id, ev, progress] {
            if (auto it = m_operations.find(id); it != m_operations.end())
                it->second.cancellable = progress.cancellable;
            if (ev->progress)
                ev->progress(progress);
        });
    };
    c.done = [this, id, shared](PkResult r) {
        toMain([this, id, shared, result = checkResult(r)]() mutable {
            forget(id);
            (*shared)(std::move(result));
        });
    };
    m_transport->submit(std::move(c));
}

Result<void> PackageKitBackend::cancel(const std::string& id)
{
    const auto it = m_operations.find(id);
    if (it == m_operations.end())
        return Error::notFound("no operation " + id);
    Operation& op = it->second;
    // Only while PackageKit says so (AllowCancel), and never promised as
    // immediate (SERVICE.md §6.5).
    if (op.modifying && op.started && !op.cancellable)
        return Error::notSupported("PackageKit does not allow cancelling now");
    *op.cancelRequested = true;
    m_transport->cancel(id);
    return {};
}

void PackageKitBackend::setChangeListener(std::function<void(const BackendChanges&)> listener)
{
    m_listener = std::move(listener);
    if (!m_listener) {
        m_transport->watchDaemon({});
        m_changeTimer.stop();
        return;
    }
    m_transport->watchDaemon([this](bool installed, bool updates, bool repositories) {
        toMain([this, installed, updates, repositories] {
            m_pendingChanges.installed = m_pendingChanges.installed || installed;
            m_pendingChanges.updates = m_pendingChanges.updates || updates;
            m_pendingChanges.sources = m_pendingChanges.sources || repositories;
            // Grouped (SERVICE.md §6.2): a burst gives one notification.
            if (!m_changeTimer.isActive())
                m_changeTimer.start(m_options.changeDelay);
        });
    });
}

} // namespace cutefish::store::service
