#include "sourcechanges.h"

#include "marshal.h"

#include <base/logging.h>
#include <base/uuid.h>

#include <ctime>

namespace cutefish::store::service {

namespace {

base::LogCategory cfSources("cutefish.store.sources");

std::string kindOf(SourceRequest::Action action)
{
    return std::string(toString(action)) + "-source";
}

/// The plan of a source change (SERVICE.md §25): what it acts on, and the
/// reason of the confirmation while one is pending (D-042).
base::dbus::VariantMap planOf(const BackendSource& source, SourceRequest::Action action, bool pending)
{
    const Source& s = source.source;
    base::dbus::VariantMap described{{"id", s.id().str()},
                                     {"name", s.label().empty() ? s.origin() : s.label()},
                                     {"origin", s.origin()},
                                     {"url", s.url()},
                                     {"kind", std::string(toString(s.kind()))},
                                     {"scope", std::string(toString(s.scope()))},
                                     {"enabled", s.enabled()}};
    if (source.verifiesSignatures)
        described.emplace("gpg-verify", *source.verifiesSignatures);
    const bool confirm = action == SourceRequest::Action::Enable;
    std::vector<std::string> reasons;
    if (confirm && pending)
        reasons.push_back("new-source");
    return {{"kind", kindOf(action)},
            {"backend", std::string(toString(s.backend()))},
            {"scope", std::string(toString(s.scope()))},
            {"requires-confirmation", confirm},
            {"reasons", reasons},
            {"source", described}};
}

} // namespace

struct SourceChanges::Entry
{
    explicit Entry(BackendSource s) : source(std::move(s)) {}

    std::string id;
    std::string path;
    Backend* backend = nullptr;
    BackendSource source;
    SourceRequest::Action action = SourceRequest::Action::Disable;
    bool finished = false;
    std::unique_ptr<ActionTransaction> object;
    std::unique_ptr<base::Timer> deadline;
};

SourceChanges::SourceChanges(base::EventLoop& loop, base::dbus::Connection& connection, Options options)
    : m_loop(loop), m_connection(connection), m_options(options)
{
}

SourceChanges::~SourceChanges()
{
    *m_alive = false;
}

UnixSeconds SourceChanges::now() const
{
    return static_cast<UnixSeconds>(std::time(nullptr));
}

SourceChanges::Entry* SourceChanges::find(const std::string& id)
{
    const auto it = m_entries.find(id);
    return it == m_entries.end() ? nullptr : it->second.get();
}

std::vector<std::string> SourceChanges::paths() const
{
    std::vector<std::string> out;
    for (const auto& [id, e] : m_entries)
        out.push_back(e->path);
    return out;
}

bool SourceChanges::engaged(const SourceId& source) const
{
    for (const auto& [id, e] : m_entries)
        if (!e->finished && e->source.source.id() == source)
            return true;
    return false;
}

Result<std::string> SourceChanges::start(Backend& backend, const BackendSource& source, SourceRequest::Action action,
                                         std::optional<Error> refusal)
{
    if (!newPath)
        return Error::internal("no object path for a source transaction");
    auto e = std::make_unique<Entry>(source);
    e->id = base::Uuid::generate().toString();
    e->path = newPath();
    e->backend = &backend;
    e->action = action;
    const std::string id = e->id;
    ActionTransaction::Identity identity{.path = e->path,
                                         .id = e->id,
                                         .kind = kindOf(action),
                                         .backend = std::string(toString(backend.kind())),
                                         .appIds = {},
                                         .offerIds = {},
                                         .parent = {},
                                         .created = now()};
    e->object = std::make_unique<ActionTransaction>(
            m_loop, &m_connection, std::move(identity), [this, id] { return confirm(id); },
            [this, id] { return cancel(id); });
    const bool confirmFirst = action == SourceRequest::Action::Enable && !refusal;
    e->object->setPlan(planOf(source, action, confirmFirst));
    Entry& entry = *e;
    m_entries.emplace(id, std::move(e));
    if (onActivityBegin)
        onActivityBegin();
    if (onAdded)
        onAdded(entry.path);
    CF_INFO(cfSources) << id << ": " << kindOf(action) << " " << source.source.id().str();

    if (refusal) {
        // After the caller has the path: the object, then its Finished.
        m_loop.post([this, alive = m_alive, id, error = *refusal] {
            if (!*alive)
                return;
            if (Entry* e = find(id); e && !e->finished)
                finish(*e, TransactionState::Failed, error);
        });
        return entry.path;
    }
    if (!confirmFirst) {
        enqueue(entry);
        return entry.path;
    }
    // D-042: nothing is changed before an explicit Confirm; D-019: not after
    // the delay either.
    entry.object->setState(TransactionState::AwaitingConfirmation);
    entry.object->setCancellable(true);
    entry.deadline = std::make_unique<base::Timer>(&m_loop);
    entry.deadline->setSingleShot(true);
    entry.deadline->onTimeout = [this, id] {
        m_loop.post([this, alive = m_alive, id] {
            if (!*alive)
                return;
            Entry* e = find(id);
            if (!e || e->finished || e->object->state() != TransactionState::AwaitingConfirmation)
                return;
            CF_INFO(cfSources) << id << ": no confirmation within the delay, cancelled";
            finish(*e, TransactionState::Cancelled, std::nullopt);
        });
    };
    entry.deadline->start(m_options.confirmationTimeout);
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(m_options.confirmationTimeout).count();
    entry.object->setDeadline(now() + static_cast<UnixSeconds>(seconds));
    return entry.path;
}

void SourceChanges::enqueue(Entry& e)
{
    e.object->setState(TransactionState::Queued);
    e.object->setCancellable(true);
    const std::string id = e.id;
    const BackendKind kind = e.backend->kind();
    auto work = [this, alive = m_alive, id](std::function<void()> laneDone) {
        if (!*alive)
            return laneDone();
        run(id, std::move(laneDone));
    };
    if (exclusive)
        exclusive(kind, std::move(work));
    else
        work([] {});
}

void SourceChanges::run(const std::string& id, std::function<void()> laneDone)
{
    Entry* e = find(id);
    if (!e || e->finished) // cancelled while it waited
        return laneDone();
    e->object->setCancellable(false);
    e->object->setState(TransactionState::Running);
    const BackendKind kind = e->backend->kind();
    const SourceRequest request{.source = e->source.source.id(), .action = e->action};
    e->backend->changeSource(id, request, [this, alive = m_alive, id, kind, laneDone](Result<void> r) {
        if (!*alive)
            return;
        laneDone();
        if (Entry* e = find(id); e && !e->finished) {
            if (r)
                finish(*e, TransactionState::Succeeded, std::nullopt);
            else
                finish(*e, r.error().code() == ErrorCode::Cancelled ? TransactionState::Cancelled
                                                                    : TransactionState::Failed,
                       r.error());
        }
        // Never an optimistic update: the backend is asked (§8.3).
        if (onSourcesMayHaveChanged)
            onSourcesMayHaveChanged(kind);
    });
}

void SourceChanges::finish(Entry& e, TransactionState state, std::optional<Error> error)
{
    if (e.finished)
        return;
    e.finished = true;
    e.deadline.reset();
    if (error) {
        CF_INFO(cfSources) << e.id << ": " << toString(state) << ": " << error->message();
    }
    e.object->setCancellable(false);
    e.object->setDeadline(0);
    e.object->finish(state, std::move(error), now());
    const std::string id = e.id;
    const std::string path = e.path;
    // A finished transaction stays on the bus for a while (A §5.3).
    auto timer = std::make_unique<base::Timer>(&m_loop);
    timer->setSingleShot(true);
    timer->onTimeout = [this, id, path] {
        m_loop.post([this, alive = m_alive, id, path] {
            if (!*alive)
                return;
            m_entries.erase(id);
            m_retirements.erase(id);
            if (onRemoved)
                onRemoved(path);
        });
    };
    timer->start(m_options.retention);
    m_retirements[id] = std::move(timer);
    if (onActivityEnd)
        onActivityEnd();
}

std::string SourceChanges::confirm(const std::string& id)
{
    Entry* e = find(id);
    if (!e || e->finished || e->object->state() != TransactionState::AwaitingConfirmation)
        return errors::kNotAllowed;
    e->deadline.reset();
    e->object->setDeadline(0);
    e->object->setPlan(planOf(e->source, e->action, false));
    enqueue(*e);
    return {};
}

std::string SourceChanges::cancel(const std::string& id)
{
    Entry* e = find(id);
    if (!e || e->finished)
        return errors::kNotAllowed;
    const TransactionState s = e->object->state();
    if (s != TransactionState::AwaitingConfirmation && s != TransactionState::Queued)
        return errors::kNotAllowed; // a running change is not cancellable
    finish(*e, TransactionState::Cancelled, std::nullopt);
    return {};
}

} // namespace cutefish::store::service
