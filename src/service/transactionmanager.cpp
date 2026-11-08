#include "transactionmanager.h"

#include "marshal.h"

#include <base/logging.h>
#include <base/uuid.h>

#include <algorithm>
#include <ctime>
#include <set>

namespace cutefish::store::service {

namespace {

base::LogCategory cfTx("cutefish.store.transactions");

/// Owner of a lane while exclusive work runs (never an entry id: those are
/// UUIDs).
constexpr const char* kExclusive = "<exclusive>";

std::string kindName(TransactionKind kind)
{
    return std::string(toString(kind));
}

} // namespace

struct TransactionManager::Entry
{
    Entry(TransactionKind kind, PlanRequest r) : tx(kind), request(std::move(r)) {}

    std::string id;
    std::string path;
    std::string parent; ///< parent id, empty for none
    Transaction tx;
    PlanRequest request;
    std::vector<Target> targets;
    UnixSeconds created = 0;
    std::string backendRef;
    /// The backend operation in progress for this transaction, if any.
    std::string operation;
    int operations = 0;
    /// An execution was asked of the backend and has not completed.
    bool executing = false;
    /// It reached running: the system may have changed.
    bool ran = false;
    bool finished = false;
    std::unique_ptr<ActionTransaction> object;
    std::unique_ptr<base::Timer> deadline;
};

struct TransactionManager::Parent
{
    std::string id;
    std::string path;
    std::vector<std::string> children; // entry ids
    std::unique_ptr<ActionTransaction> object;
    bool finished = false;
};

TransactionManager::TransactionManager(base::EventLoop& loop, base::dbus::Connection& connection,
                                       StateStore& journal, Options options)
    : m_loop(loop), m_connection(connection), m_journal(journal), m_options(options)
{
}

TransactionManager::~TransactionManager()
{
    *m_alive = false;
}

UnixSeconds TransactionManager::now() const
{
    return static_cast<UnixSeconds>(std::time(nullptr));
}

void TransactionManager::addBackend(Backend& backend)
{
    m_lanes[backend.kind()].backend = &backend;
}

TransactionManager::Entry* TransactionManager::find(const std::string& id)
{
    const auto it = m_entries.find(id);
    return it == m_entries.end() ? nullptr : it->second.get();
}

bool TransactionManager::engaged(const OfferId& offer) const
{
    for (const auto& [id, e] : m_entries)
        if (!e->finished)
            for (const Target& t : e->targets)
                if (t.offer == offer)
                    return true;
    return false;
}

std::vector<std::string> TransactionManager::paths() const
{
    std::vector<std::string> out;
    for (const auto& [id, p] : m_parents)
        out.push_back(p->path);
    for (const auto& [id, e] : m_entries)
        out.push_back(e->path);
    return out;
}

std::string TransactionManager::operation(Entry& e, const char* what)
{
    // Unique per backend operation: the backend refuses an id in use.
    e.operation = e.id + ":" + what + ":" + std::to_string(++e.operations);
    return e.operation;
}

void TransactionManager::save(const Entry& e)
{
    TransactionRecord r{.id = e.id,
                        .backend = e.request.backend(),
                        .scope = e.request.scope(),
                        .offers = e.targets,
                        .snapshot = e.tx.snapshot(),
                        .backendRef = e.backendRef,
                        .created = e.created,
                        .updated = now()};
    if (isTerminal(e.tx.state()))
        r.finished = now();
    if (auto ok = m_journal.saveTransaction(r); !ok) {
        CF_WARNING(cfTx) << "journal not written for " << e.id << ": " << ok.error().message();
    }
}

Result<std::unique_ptr<TransactionManager::Entry>> TransactionManager::make(TransactionKind kind,
                                                                            std::vector<Target> targets,
                                                                            std::string parent)
{
    std::vector<OfferId> offers;
    for (const Target& t : targets)
        offers.push_back(t.offer);
    auto request = PlanRequest::create(kind, std::move(offers));
    if (!request)
        return std::move(request).error();
    const auto lane = m_lanes.find(request->backend());
    if (lane == m_lanes.end() || !lane->second.backend)
        return Error::backend(BackendFailure::Unavailable,
                              "no " + std::string(toString(request->backend())) + " backend");
    for (const Target& t : targets)
        if (engaged(t.offer))
            return Error::conflict("offer " + t.offer.str() + " is engaged by another transaction");
    auto e = std::make_unique<Entry>(kind, std::move(request).value());
    e->id = base::Uuid::generate().toString();
    e->parent = std::move(parent);
    e->targets = std::move(targets);
    e->created = now();
    return e;
}

void TransactionManager::publish(Entry& e)
{
    e.path = newPath ? newPath() : "/org/cutefish/Store/Transactions/" + e.id;
    ActionTransaction::Identity identity{.path = e.path,
                                         .id = e.id,
                                         .kind = kindName(e.tx.kind()),
                                         .backend = std::string(toString(e.request.backend())),
                                         .appIds = {},
                                         .offerIds = {},
                                         .parent = {},
                                         .created = e.created};
    std::set<std::string> apps;
    for (const Target& t : e.targets) {
        identity.offerIds.push_back(t.offer.str());
        if (t.app)
            apps.insert(t.app->str());
    }
    identity.appIds.assign(apps.begin(), apps.end());
    if (!e.parent.empty())
        if (const auto it = m_parents.find(e.parent); it != m_parents.end())
            identity.parent = it->second->path;
    const std::string id = e.id;
    e.object = std::make_unique<ActionTransaction>(
            m_loop, &m_connection, std::move(identity), [this, id] { return confirm(id); },
            [this, id] { return cancel(id); });
    e.object->setState(e.tx.state());
    if (onActivityBegin)
        onActivityBegin();
}

Result<std::string> TransactionManager::start(TransactionKind kind, std::vector<Target> targets)
{
    auto made = make(kind, std::move(targets), {});
    if (!made)
        return std::move(made).error();
    std::unique_ptr<Entry> owned = std::move(made).value();
    Entry& e = *owned;
    // In the journal before anything can happen (D-017).
    TransactionRecord first{.id = e.id,
                            .backend = e.request.backend(),
                            .scope = e.request.scope(),
                            .offers = e.targets,
                            .snapshot = e.tx.snapshot(),
                            .created = e.created,
                            .updated = e.created};
    if (auto ok = m_journal.saveTransaction(first); !ok)
        return Error::internal("the journal cannot be written: " + ok.error().message());
    m_entries.emplace(e.id, std::move(owned));
    publish(e);
    if (onAdded)
        onAdded(e.path);
    const BackendKind backend = e.request.backend();
    m_lanes[backend].waiting.push_back(Job{e.id});
    const std::string path = e.path;
    // The answer goes out first; the work starts on the next turn.
    m_loop.post([this, alive = m_alive, backend] {
        if (*alive)
            pump(backend);
    });
    return path;
}

Result<std::string> TransactionManager::startUpdateAll(std::vector<Target> updates)
{
    // One child per backend and installation (D-031).
    std::map<std::pair<BackendKind, Scope>, std::vector<Target>> groups;
    for (Target& t : updates) {
        auto address = decodeOfferId(t.offer);
        if (!address)
            return std::move(address).error();
        groups[{address->backend, address->scope}].push_back(std::move(t));
    }
    if (groups.empty())
        return Error::notFound("no update available");
    auto parent = std::make_unique<Parent>();
    parent->id = base::Uuid::generate().toString();
    std::vector<std::unique_ptr<Entry>> children;
    for (auto& [key, targets] : groups) {
        auto child = make(TransactionKind::Update, std::move(targets), parent->id);
        if (!child)
            return std::move(child).error(); // nothing created yet
        children.push_back(std::move(child).value());
    }
    for (const auto& c : children) {
        TransactionRecord first{.id = c->id,
                                .backend = c->request.backend(),
                                .scope = c->request.scope(),
                                .offers = c->targets,
                                .snapshot = c->tx.snapshot(),
                                .created = c->created,
                                .updated = c->created};
        if (auto ok = m_journal.saveTransaction(first); !ok)
            return Error::internal("the journal cannot be written: " + ok.error().message());
    }
    // The parent is not journaled (D-018 keeps install, remove, update);
    // its children are.
    parent->path = newPath ? newPath() : "/org/cutefish/Store/Transactions/" + parent->id;
    ActionTransaction::Identity identity{.path = parent->path,
                                         .id = parent->id,
                                         .kind = "update-all",
                                         .backend = "multiple",
                                         .appIds = {},
                                         .offerIds = {},
                                         .parent = {},
                                         .created = now()};
    std::set<std::string> apps;
    for (const auto& c : children)
        for (const Target& t : c->targets) {
            identity.offerIds.push_back(t.offer.str());
            if (t.app)
                apps.insert(t.app->str());
        }
    identity.appIds.assign(apps.begin(), apps.end());
    const std::string pid = parent->id;
    // Confirm and Cancel on the parent apply to every child that allows it.
    parent->object = std::make_unique<ActionTransaction>(
            m_loop, &m_connection, std::move(identity),
            [this, pid] {
                std::string result = errors::kNotAllowed;
                for (const std::string& c : m_parents.at(pid)->children)
                    if (confirm(c).empty())
                        result.clear();
                return result;
            },
            [this, pid] {
                std::string result = errors::kNotAllowed;
                for (const std::string& c : m_parents.at(pid)->children)
                    if (cancel(c).empty())
                        result.clear();
                return result;
            });
    Parent& p = *parent;
    m_parents.emplace(p.id, std::move(parent));
    if (onAdded)
        onAdded(p.path);
    std::set<BackendKind> lanes;
    for (auto& c : children) {
        Entry& e = *c;
        p.children.push_back(e.id);
        m_entries.emplace(e.id, std::move(c));
        publish(e);
        if (onAdded)
            onAdded(e.path);
        m_lanes[e.request.backend()].waiting.push_back(Job{e.id});
        lanes.insert(e.request.backend());
    }
    const std::string path = p.path;
    m_loop.post([this, alive = m_alive, lanes] {
        if (!*alive)
            return;
        for (BackendKind b : lanes)
            pump(b);
    });
    return path;
}

void TransactionManager::exclusive(BackendKind backend, std::function<void(std::function<void()>)> work)
{
    m_lanes[backend].waiting.push_back(Job{{}, std::move(work)});
    pump(backend);
}

void TransactionManager::pump(BackendKind backend)
{
    Lane& lane = m_lanes[backend];
    while (lane.owner.empty() && !lane.waiting.empty()) {
        Job job = std::move(lane.waiting.front());
        lane.waiting.pop_front();
        if (job.entry.empty()) {
            lane.owner = kExclusive;
            auto once = std::make_shared<bool>(false);
            job.work([this, alive = m_alive, backend, once] {
                if (!*alive || std::exchange(*once, true))
                    return;
                m_lanes[backend].owner.clear();
                pump(backend);
            });
            return;
        }
        Entry* e = find(job.entry);
        if (!e || e->finished)
            continue;
        lane.owner = e->id;
        if (e->tx.state() == TransactionState::Queued)
            plan(*e);
        else if (e->tx.state() == TransactionState::Planning && e->tx.acceptedPlan())
            execute(*e); // confirmed: its turn has come (D-030)
        else
            release(*e);
        return;
    }
}

void TransactionManager::release(Entry& e)
{
    const BackendKind backend = e.request.backend();
    Lane& lane = m_lanes[backend];
    if (lane.owner != e.id)
        return;
    lane.owner.clear();
    m_loop.post([this, alive = m_alive, backend] {
        if (*alive)
            pump(backend);
    });
}

void TransactionManager::stateChanged(Entry& e)
{
    e.object->setState(e.tx.state());
    if (e.tx.plan())
        e.object->setPlan(planMap(*e.tx.plan(), e.tx.reasons(), e.tx.concerns()));
    save(e);
    if (!e.parent.empty())
        updateParent(e.parent);
}

void TransactionManager::plan(Entry& e)
{
    if (auto ok = e.tx.startPlanning(); !ok)
        return endWith(e, ok.error());
    stateChanged(e);
    Backend& backend = *m_lanes[e.request.backend()].backend;
    const std::string id = e.id;
    backend.plan(operation(e, "plan"), e.request, [this, alive = m_alive, id](Result<Plan> plan) {
        if (!*alive)
            return;
        Entry* e = find(id);
        if (!e)
            return;
        e->operation.clear();
        if (e->finished) // cancelled while planning
            return;
        if (!plan)
            return endWith(*e, plan.error());
        if (auto ok = e->tx.planReady(std::move(plan).value()); !ok)
            return endWith(*e, ok.error());
        stateChanged(*e);
        if (e->tx.state() == TransactionState::AwaitingConfirmation)
            return awaitConfirmation(*e);
        execute(*e); // accepted as is: still its turn
    });
}

void TransactionManager::awaitConfirmation(Entry& e)
{
    // The lane is given up while the user decides (D-030).
    release(e);
    e.deadline = std::make_unique<base::Timer>(&m_loop);
    e.deadline->setSingleShot(true);
    const std::string id = e.id;
    e.deadline->onTimeout = [this, id] {
        // D-019: an old plan is never executed.
        m_loop.post([this, alive = m_alive, id] {
            if (!*alive)
                return;
            Entry* e = find(id);
            if (!e || e->finished || e->tx.state() != TransactionState::AwaitingConfirmation)
                return;
            CF_INFO(cfTx) << id << ": no confirmation within the delay, cancelled";
            if (e->tx.cancel())
                finish(*e);
        });
    };
    e.deadline->start(m_options.confirmationTimeout);
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(m_options.confirmationTimeout).count();
    e.object->setDeadline(now() + static_cast<UnixSeconds>(seconds));
}

void TransactionManager::execute(Entry& e)
{
    Backend& backend = *m_lanes[e.request.backend()].backend;
    const std::string id = e.id;
    OperationEvents events;
    events.started = [this, alive = m_alive, id](const Plan& plan) {
        if (!*alive)
            return;
        Entry* e = find(id);
        if (!e || e->finished)
            return;
        // The backend checked mayExecute(); the transaction applies the
        // same rule, and only then is running (D-017).
        auto verdict = e->tx.verifyExecutionPlan(plan);
        if (!verdict || *verdict == PlanVerdict::NeedsConfirmation) {
            CF_WARNING(cfTx) << id << ": execution plan refused by the transaction; asking the backend to stop";
            if (!e->operation.empty())
                (void)m_lanes[e->request.backend()].backend->cancel(e->operation);
        } else {
            e->ran = true;
        }
        stateChanged(*e);
    };
    events.progress = [this, alive = m_alive, id](const Progress& p) {
        if (!*alive)
            return;
        if (Entry* e = find(id); e && !e->finished)
            e->object->setProgress(p);
    };
    events.handle = [this, alive = m_alive, id](const std::string& ref) {
        if (!*alive)
            return;
        if (Entry* e = find(id); e && !e->finished) {
            e->backendRef = ref; // for a re-attach after a restart
            save(*e);
        }
    };
    e.executing = true;
    e.object->setDeadline(0);
    backend.execute(operation(e, "execute"), e.request, *e.tx.acceptedPlan(), std::move(events),
                    [this, alive = m_alive, id](Result<ExecutionResult> r) {
                        if (!*alive)
                            return;
                        Entry* e = find(id);
                        if (!e)
                            return;
                        e->operation.clear();
                        e->executing = false;
                        if (e->finished)
                            return;
                        if (!r)
                            return endWith(*e, r.error());
                        if (r->outcome == ExecutionResult::Outcome::PlanChanged) {
                            // Nothing executed: a new confirmation (D-017).
                            if (!r->freshPlan)
                                return endWith(*e, Error::internal("plan changed without a plan"));
                            auto verdict = e->tx.verifyExecutionPlan(*r->freshPlan);
                            if (!verdict)
                                return endWith(*e, verdict.error());
                            stateChanged(*e);
                            if (e->tx.state() == TransactionState::AwaitingConfirmation)
                                return awaitConfirmation(*e);
                            return endWith(*e, Error::internal("plan changed but found compatible"));
                        }
                        if (auto ok = e->tx.succeed(); !ok)
                            return endWith(*e, ok.error());
                        finish(*e);
                    });
}

void TransactionManager::endWith(Entry& e, const Error& error)
{
    const TransactionState state = e.tx.state();
    Result<void> ok = Result<void>::success();
    if (error.code() == ErrorCode::Cancelled)
        ok = e.tx.cancel();
    else if (error.code() == ErrorCode::Backend && error.backendFailure() == BackendFailure::Interrupted
             && state == TransactionState::Running)
        ok = e.tx.interrupt();
    else
        ok = e.tx.fail(error);
    if (!ok) {
        // A state that allows nothing of the above (queued): fail is not
        // allowed there either; cancel is.
        CF_WARNING(cfTx) << e.id << ": " << error.message() << " (" << ok.error().message() << ")";
        if (!e.tx.cancel())
            (void)e.tx.interrupt();
    }
    finish(e);
}

void TransactionManager::finish(Entry& e)
{
    if (e.finished)
        return;
    e.finished = true;
    e.deadline.reset();
    e.object->finish(e.tx.state(), e.tx.error(), now());
    if (e.tx.plan())
        e.object->setPlan(planMap(*e.tx.plan(), {}, {}));
    save(e);
    release(e);
    if (e.ran && onSystemMayHaveChanged)
        onSystemMayHaveChanged(e.request.backend());
    if (!e.parent.empty())
        updateParent(e.parent);
    const std::string id = e.id;
    retire(e.path, [this, id] { m_entries.erase(id); });
    if (onActivityEnd)
        onActivityEnd();
}

void TransactionManager::retire(const std::string& path, std::function<void()> erase)
{
    auto timer = std::make_unique<base::Timer>(&m_loop);
    timer->setSingleShot(true);
    timer->onTimeout = [this, path, erase] {
        m_loop.post([this, alive = m_alive, path, erase] {
            if (!*alive)
                return;
            erase();
            m_retirements.erase(path);
            if (onRemoved)
                onRemoved(path);
        });
    };
    timer->start(m_options.retention);
    m_retirements[path] = std::move(timer);
}

std::string TransactionManager::confirm(const std::string& id)
{
    Entry* e = find(id);
    if (!e || e->finished || e->tx.state() != TransactionState::AwaitingConfirmation)
        return errors::kNotAllowed;
    if (!e->tx.confirm())
        return errors::kNotAllowed;
    e->deadline.reset();
    e->object->setDeadline(0);
    stateChanged(*e);
    // Back to planning; it executes when its turn comes (D-030).
    m_lanes[e->request.backend()].waiting.push_back(Job{e->id});
    const BackendKind backend = e->request.backend();
    m_loop.post([this, alive = m_alive, backend] {
        if (*alive)
            pump(backend);
    });
    return {};
}

std::string TransactionManager::cancel(const std::string& id)
{
    Entry* e = find(id);
    if (!e || e->finished)
        return errors::kNotAllowed;
    Backend& backend = *m_lanes[e->request.backend()].backend;
    switch (e->tx.state()) {
    case TransactionState::Queued:
    case TransactionState::AwaitingConfirmation:
        if (!e->tx.cancel())
            return errors::kNotAllowed;
        finish(*e);
        return {};
    case TransactionState::Planning:
        if (e->executing) {
            // The backend is simulating again before executing: it decides,
            // and its completion tells (it may already be too late).
            if (!backend.cancel(e->operation))
                return errors::kNotAllowed;
            return {};
        }
        // Computing a plan, or waiting for its turn: nothing changes the
        // system yet, so the cancellation is immediate.
        if (!e->operation.empty())
            (void)backend.cancel(e->operation);
        if (!e->tx.cancel())
            return errors::kNotAllowed;
        finish(*e);
        return {};
    case TransactionState::Running:
        // Only a request: PackageKit decides (ARCHITECTURE §7.2).
        if (e->operation.empty() || !backend.cancel(e->operation))
            return errors::kNotAllowed;
        return {};
    default:
        return errors::kNotAllowed;
    }
}

void TransactionManager::updateParent(const std::string& parentId)
{
    const auto it = m_parents.find(parentId);
    if (it == m_parents.end() || it->second->finished)
        return;
    Parent& p = *it->second;
    bool open = false;
    bool awaiting = false;
    bool running = false;
    bool planning = false;
    bool anyFailed = false;
    bool anyInterrupted = false;
    bool allSucceeded = true;
    std::optional<Error> firstError;
    for (const std::string& c : p.children) {
        const Entry* e = find(c);
        if (!e)
            continue;
        const TransactionState s = e->tx.state();
        if (!isTerminal(s))
            open = true;
        awaiting = awaiting || s == TransactionState::AwaitingConfirmation;
        running = running || s == TransactionState::Running;
        planning = planning || s == TransactionState::Planning;
        anyFailed = anyFailed || s == TransactionState::Failed;
        anyInterrupted = anyInterrupted || s == TransactionState::Interrupted;
        allSucceeded = allSucceeded && s == TransactionState::Succeeded;
        if (!firstError && e->tx.error())
            firstError = e->tx.error();
    }
    if (open) {
        // What the user has to act on first.
        p.object->setState(awaiting  ? TransactionState::AwaitingConfirmation
                           : running ? TransactionState::Running
                           : planning ? TransactionState::Planning
                                      : TransactionState::Queued);
        return;
    }
    p.finished = true;
    const TransactionState end = allSucceeded     ? TransactionState::Succeeded
                                 : anyFailed      ? TransactionState::Failed
                                 : anyInterrupted ? TransactionState::Interrupted
                                                  : TransactionState::Cancelled;
    p.object->finish(end, end == TransactionState::Failed ? firstError : std::nullopt, now());
    const std::string id = p.id;
    retire(p.path, [this, id] { m_parents.erase(id); });
}

void TransactionManager::recover(std::vector<TransactionRecord> toReattach)
{
    for (TransactionRecord& r : toReattach) {
        const TransactionKind kind = r.snapshot.kind;
        auto restored = Transaction::restore(r.snapshot);
        std::vector<OfferId> offers;
        for (const Target& t : r.offers)
            offers.push_back(t.offer);
        auto request = PlanRequest::create(kind, std::move(offers));
        if (!restored || !request) {
            CF_WARNING(cfTx) << r.id << ": cannot be restored, left to the journal: "
                             << (!restored ? restored.error().message() : request.error().message());
            continue;
        }
        auto owned = std::make_unique<Entry>(kind, std::move(request).value());
        Entry& e = *owned;
        e.tx = std::move(restored).value();
        e.id = r.id;
        e.targets = r.offers;
        e.created = r.created;
        e.backendRef = r.backendRef;
        e.ran = true;
        m_entries.emplace(e.id, std::move(owned));
        publish(e);
        if (onAdded)
            onAdded(e.path);
        Lane& lane = m_lanes[e.request.backend()];
        if (!lane.backend || !lane.owner.empty()) {
            // No backend to ask, or another one is already being followed:
            // its outcome cannot be known.
            endWith(e, Error::backend(BackendFailure::Interrupted, "the transaction could not be re-attached"));
            continue;
        }
        lane.owner = e.id;
        const std::string id = e.id;
        OperationEvents events;
        events.progress = [this, alive = m_alive, id](const Progress& p) {
            if (!*alive)
                return;
            if (Entry* e = find(id); e && !e->finished)
                e->object->setProgress(p);
        };
        lane.backend->reattach(operation(e, "reattach"), e.backendRef, std::move(events),
                               [this, alive = m_alive, id](Result<ExecutionResult> result) {
                                   if (!*alive)
                                       return;
                                   Entry* e = find(id);
                                   if (!e || e->finished)
                                       return;
                                   e->operation.clear();
                                   if (!result) {
                                       // NotFound: packagekitd no longer has it (§5.6).
                                       const Error& err = result.error();
                                       if (err.code() == ErrorCode::NotFound)
                                           return endWith(*e, Error::backend(BackendFailure::Interrupted,
                                                                             err.message()));
                                       return endWith(*e, err);
                                   }
                                   if (auto ok = e->tx.succeed(); !ok)
                                       return endWith(*e, ok.error());
                                   finish(*e);
                               });
    }
}

} // namespace cutefish::store::service
