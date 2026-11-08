#include "storeservice.h"

#include "marshal.h"

#include <cutefish/store/sqlitecatalog.h>

#include <base/logging.h>
#include <base/uuid.h>

#include <algorithm>
#include <ctime>
#include <system_error>

namespace cutefish::store::service {

namespace {

base::LogCategory cfStore("cutefish.store");

constexpr const char* kPath = "/org/cutefish/Store";
constexpr const char* kInterface = "org.cutefish.Store1";
constexpr std::size_t kMaxIds = 200;

template <typename T>
void replyError(base::dbus::MethodCall& call, const Result<T>& r)
{
    const DbusError e = toDbusError(r.error());
    call.replyError(e.name, e.message);
}

} // namespace

StoreService::StoreService(base::EventLoop& loop, base::dbus::Connection& connection, ServiceOptions options,
                           std::unique_ptr<Backend> backend)
    : m_loop(loop), m_connection(connection), m_options(std::move(options)),
      m_reads(m_options.dataDir / "catalog.sqlite", m_options.dataDir / "state.sqlite", m_options.readLocales),
      m_refresher(loop, m_options), m_lifecycle(loop, connection, m_options.idleTimeout, [this] {
          if (onIdle)
              onIdle();
      }),
      m_backend(std::move(backend))
{
    if (!m_backend)
        return;
    m_inventory = std::make_unique<Inventory>(*m_backend, m_options.dataDir / "catalog.sqlite",
                                              m_options.dataDir / "state.sqlite");
    m_inventory->onAvailabilityChanged = [this] {
        m_object->emitPropertiesChanged({"Backends"});
        if (m_generation > 0)
            setState("ready"); // or degraded, see setState()
    };
    m_inventory->onInstalledChanged = [this](const std::vector<std::string>& apps) {
        m_object->emitSignal("InstalledChanged", apps);
    };
    m_inventory->onUpdatesChanged = [this] {
        m_object->emitPropertiesChanged({"UpdateCount"});
        m_object->emitSignal("UpdatesChanged", static_cast<std::uint32_t>(m_inventory->updates().size()));
    };
}

StoreService::~StoreService() = default;

UnixSeconds StoreService::now() const
{
    return static_cast<UnixSeconds>(std::time(nullptr));
}

Result<void> StoreService::start()
{
    std::error_code ec;
    std::filesystem::create_directories(m_options.dataDir, ec);
    if (ec)
        return Error::internal("cannot create " + m_options.dataDir.string() + ": " + ec.message());
    // Startup, before the name is taken: the only blocking disk work of the
    // main thread, as SERVICE.md §3.1 plans (small, WAL).
    auto store = StateStore::open(m_options.dataDir / "state.sqlite", now());
    if (!store)
        return std::move(store).error();
    m_state.emplace(std::move(store).value());
    if (auto report = m_state->recoverAfterRestart(now()); !report) {
        CF_WARNING(cfStore) << "journal recovery failed: " << report.error().message();
    } else {
        // Running transactions kept with a backend handle (SERVICE.md §5.6).
        m_toReattach = std::move(report->toReattach);
    }
    if (m_backend) {
        m_manager = std::make_unique<TransactionManager>(
                m_loop, m_connection, *m_state,
                TransactionManager::Options{.confirmationTimeout = m_options.confirmationTimeout,
                                            .retention = m_options.transactionRetention});
        m_manager->addBackend(*m_backend);
        m_manager->newPath = [this] { return newTransaction(); };
        m_manager->onAdded = [this](const std::string& path) {
            m_object->emitSignal("TransactionAdded", base::dbus::ObjectPath{path});
        };
        m_manager->onRemoved = [this](const std::string& path) {
            m_object->emitSignal("TransactionRemoved", base::dbus::ObjectPath{path});
        };
        // Never an optimistic update: the backend is asked (§8.3).
        m_manager->onSystemMayHaveChanged = [this](BackendKind) { m_inventory->check(); };
        m_manager->onActivityBegin = [this] { m_lifecycle.beginActivity(); };
        m_manager->onActivityEnd = [this] { m_lifecycle.endActivity(); };
    }
    if (auto last = m_state->lastRefresh())
        m_lastRefresh = *last;

    exportObject();
    if (!m_connection.requestName(m_options.busName))
        return Error::conflict("the name " + m_options.busName + " is owned by another instance");
    loadGeneration();
    // The backend is asked once the name is ours: its answers become
    // signals and properties.
    if (m_inventory)
        m_inventory->check();
    if (m_manager)
        m_manager->recover(std::move(m_toReattach));
    m_lifecycle.evaluate();
    return {};
}

void StoreService::setState(std::string state)
{
    // A configured backend that cannot be used degrades a ready service
    // (ARCHITECTURE §5.2): the catalogue reads, nothing can be installed.
    if (state == "ready" && m_inventory && m_inventory->available() == std::optional<bool>(false))
        state = "degraded";
    if (state == m_stateName)
        return;
    m_stateName = std::move(state);
    m_object->emitPropertiesChanged({"State"});
}

void StoreService::loadGeneration()
{
    // Reading the catalogue's metadata is a SQLite read: on the pool.
    const std::filesystem::path path = m_options.dataDir / "catalog.sqlite";
    m_reads.read<std::uint64_t>(
            [path](Readers&) -> std::uint64_t {
                std::error_code ec;
                if (!std::filesystem::exists(path, ec))
                    return 0;
                auto c = SqliteCatalog::open(path);
                return c ? c->generation() : 0;
            },
            [this](std::uint64_t generation) {
                setGeneration(generation);
                if (generation > 0) {
                    setState("ready");
                    m_object->emitPropertiesChanged({"CatalogGeneration"});
                    return;
                }
                // No catalogue: initial import (D-015), no transaction object.
                setState("indexing");
                m_initialImport = true;
                startRefresh(true, false, nullptr);
            });
}

void StoreService::setGeneration(std::uint64_t generation)
{
    m_generation = generation;
    m_reads.setGeneration(generation);
    if (m_inventory)
        m_inventory->setGeneration(generation);
}

std::string StoreService::newTransaction()
{
    return std::string(kPath) + "/Transactions/" + std::to_string(m_nextTransaction++);
}

void StoreService::startRefresh(bool force, bool repositories, RefreshTransaction* transaction)
{
    m_refreshing = true;
    m_refreshCancelled = false;
    m_repositoryError.reset();
    m_lifecycle.beginActivity();
    m_object->emitPropertiesChanged({"Refreshing"});
    // Two operations (D-028): the repositories (PackageKit: RefreshCache,
    // network), asked for by a full Refresh only, then the local metadata
    // already on disk, always. The initial import is local.
    if (transaction && repositories && m_inventory && m_inventory->available() == std::optional<bool>(true)
        && m_backend->capabilities().refresh) {
        // A modifying operation of the backend: it takes its turn in the
        // backend's lane, behind a transaction in progress (P-009).
        m_refreshPhase = RefreshPhase::Lane;
        m_refreshBackendCancellable = false;
        auto run = [this, force, transaction](std::function<void()> laneDone) {
            if (m_refreshCancelled) {
                laneDone();
                return refreshDone(Error::cancelled("refresh cancelled"), transaction);
            }
            m_refreshPhase = RefreshPhase::Backend;
            m_refreshOperation = base::Uuid::generate().toString();
            transaction->updateCancellable();
            OperationEvents events;
            events.progress = [this, transaction](const Progress& p) {
                if (p.cancellable == m_refreshBackendCancellable)
                    return;
                m_refreshBackendCancellable = p.cancellable;
                transaction->updateCancellable();
        };
        m_backend->refresh(m_refreshOperation, std::move(events), [this, force, transaction, laneDone](Result<void> r) {
            laneDone();
            m_refreshOperation.clear();
            m_refreshBackendCancellable = false;
            // Cancelled by the user: nothing more is done.
            if (m_refreshCancelled || (!r && r.error().code() == ErrorCode::Cancelled))
                return refreshDone(Error::cancelled("refresh cancelled"), transaction);
            // Offline, refused, PackageKit failing: the local metadata is
            // still reindexed; the refresh will report the failure.
            if (!r) {
                CF_WARNING(cfStore) << "repositories not refreshed, reindexing the local metadata: "
                                    << r.error().message();
                m_repositoryError = std::move(r).error();
            }
            indexSources(force, transaction);
        });
        };
        transaction->updateCancellable();
        if (m_manager)
            m_manager->exclusive(m_backend->kind(), std::move(run));
        else
            run([] {});
        return;
    }
    indexSources(force, transaction);
}

void StoreService::indexSources(bool force, RefreshTransaction* transaction)
{
    auto index = [this, force, transaction](std::vector<ConfiguredSource> provided) {
        m_refreshPhase = RefreshPhase::Index;
        m_refresher.start(force, m_generation, std::move(provided),
                          [this, transaction](Result<RefreshOutcome> r) { refreshDone(std::move(r), transaction); });
        if (transaction)
            transaction->updateCancellable();
    };
    if (!m_backend)
        return index({});
    m_refreshPhase = RefreshPhase::Sources;
    if (transaction)
        transaction->updateCancellable();
    m_backend->catalogSources([this, transaction, index](Result<std::vector<SourceMetadata>> list) {
        if (m_refreshCancelled)
            return refreshDone(Error::cancelled("refresh cancelled"), transaction);
        std::vector<ConfiguredSource> provided;
        if (list) {
            for (SourceMetadata& m : *list)
                provided.push_back({std::move(m.source), std::move(m.files)});
        } else {
            CF_WARNING(cfStore) << "no catalogue sources from " << toString(m_backend->kind()) << ": "
                                << list.error().message();
        }
        index(std::move(provided));
    });
}

bool StoreService::cancelRefresh()
{
    switch (m_refreshPhase) {
    case RefreshPhase::Lane:
        m_refreshCancelled = true; // nothing started: the lane is skipped
        return true;
    case RefreshPhase::Backend:
        // Only while the backend allows it; its completion then tells.
        if (!m_refreshBackendCancellable || !m_backend->cancel(m_refreshOperation))
            return false;
        m_refreshCancelled = true;
        return true;
    case RefreshPhase::Sources:
        m_refreshCancelled = true;
        return true;
    case RefreshPhase::Index:
        return m_refresher.cancel();
    case RefreshPhase::None:
        break;
    }
    return false;
}

bool StoreService::refreshCancellable() const
{
    switch (m_refreshPhase) {
    case RefreshPhase::Lane: return !m_refreshCancelled;
    case RefreshPhase::Backend: return m_refreshBackendCancellable;
    case RefreshPhase::Sources: return !m_refreshCancelled;
    case RefreshPhase::Index: return m_refresher.cancellable();
    case RefreshPhase::None: break;
    }
    return false;
}

void StoreService::refreshDone(Result<RefreshOutcome> result, RefreshTransaction* transaction)
{
    m_refreshing = false;
    m_refreshPhase = RefreshPhase::None;
    std::vector<std::string> changed{"Refreshing"};
    // LastRefresh means everything asked for was refreshed: not after a
    // repository failure, even when the local catalogue was replaced.
    if (result && !m_repositoryError) {
        if (auto ok = m_state->recordRefresh(result->refreshedSources, now()); ok) {
            m_lastRefresh = now();
            changed.push_back("LastRefresh");
        } else {
            CF_WARNING(cfStore) << "cannot record the refresh: " << ok.error().message();
        }
    }
    if (result) {
        if (result->replaced) {
            setGeneration(result->generation);
            changed.push_back("CatalogGeneration");
        }
        if (m_generation > 0)
            setState("ready");
    } else {
        CF_WARNING(cfStore) << "refresh failed: " << result.error().message();
        if (m_generation == 0)
            setState("degraded");
    }
    m_initialImport = false;
    m_object->emitPropertiesChanged(changed);
    if (result && result->replaced)
        m_object->emitSignal("CatalogChanged", static_cast<std::uint64_t>(m_generation));
    if (result && result->sourcesChanged)
        m_object->emitSignal("SourcesChanged");
    if (transaction) {
        if (result && m_repositoryError) {
            // The catalogue is current with the disk; the repositories are not.
            const Error& e = *m_repositoryError;
            const std::string message = "repositories not refreshed (" + e.message()
                    + "); the local metadata was reindexed";
            const Error failure = e.code() == ErrorCode::Backend ? Error::backend(e.backendFailure(), message)
                                                                 : Error(e.code(), message);
            transaction->finish(TransactionState::Failed, failure, now());
        } else if (result)
            transaction->finish(TransactionState::Succeeded, std::nullopt, now());
        else if (result.error().code() == ErrorCode::Cancelled)
            transaction->finish(TransactionState::Cancelled, std::nullopt, now());
        else
            transaction->finish(TransactionState::Failed, result.error(), now());
        retire(transaction->path());
    }
    m_currentRefresh.clear();
    m_repositoryError.reset();
    // The backend is asked again: a daemon may have come back, and a
    // refreshed cache gives other updates.
    if (m_inventory)
        m_inventory->check();
    m_lifecycle.endActivity();
}

void StoreService::retire(const std::string& path)
{
    // A finished transaction stays on the bus for a while (A §5.3).
    auto timer = std::make_unique<base::Timer>(&m_loop);
    timer->setSingleShot(true);
    timer->onTimeout = [this, path] {
        m_loop.post([this, path] {
            m_transactions.erase(path);
            m_retirements.erase(path);
            m_object->emitSignal("TransactionRemoved", base::dbus::ObjectPath{path});
        });
    };
    timer->start(m_options.transactionRetention);
    m_retirements[path] = std::move(timer);
}

bool StoreService::admit(base::dbus::MethodCall& call, bool needsCatalog)
{
    m_lifecycle.noteCaller(call.message().sender());
    if (needsCatalog && m_generation == 0) {
        call.replyError(errors::kNotReady, "no catalogue in service yet (state " + m_stateName + ")");
        return false;
    }
    return true;
}

void StoreService::exportObject()
{
    using namespace base::dbus;
    m_object = std::make_unique<Object>(&m_connection, kPath, kInterface);
    Object& o = *m_object;

    // --- Properties (SERVICE.md §4.2) ---------------------------------------
    o.addProperty("Version", [] { return std::string(CUTEFISH_STORE_VERSION); });
    o.addProperty("State", [this] { return m_stateName; });
    o.addProperty("CatalogGeneration", [this] { return m_generation; });
    o.addProperty("Backends", [this] {
        std::vector<VariantMap> out;
        if (m_inventory)
            out.push_back(backendMap(m_backend->kind(), m_backend->capabilities(), m_inventory->available(),
                                     m_inventory->reason()));
        return out;
    });
    o.addProperty("Refreshing", [this] { return m_refreshing; });
    o.addProperty("LastRefresh", [this] { return static_cast<std::uint64_t>(m_lastRefresh.value_or(0)); });
    if (m_inventory)
        o.addProperty("UpdateCount", [this] { return static_cast<std::uint32_t>(m_inventory->updates().size()); });

    // --- Signals (§4.7) ------------------------------------------------------
    o.addSignal("CatalogChanged", "t", {"generation"});
    o.addSignal("InstalledChanged", "as", {"app_ids"});
    o.addSignal("TransactionAdded", "o", {"transaction"});
    o.addSignal("TransactionRemoved", "o", {"transaction"});
    o.addSignal("SourcesChanged", "", {});
    if (m_inventory)
        o.addSignal("UpdatesChanged", "u", {"count"});

    // --- Reading (§4.3) ------------------------------------------------------
    o.addMethod(
            "Search", "sa{sv}", "tuaa{sv}",
            [this](MethodCall& call) {
                if (!admit(call, true))
                    return;
                std::string text;
                VariantMap options;
                call.read(text, options);
                auto opts = searchOptions(options);
                if (!opts)
                    return replyError(call, opts);
                auto query = SearchQuery::create(std::move(text), std::move(opts).value());
                if (!query)
                    return replyError(call, query);
                m_reads.read<Result<SearchPage>>(
                        [q = std::move(query).value()](Readers& r) -> Result<SearchPage> {
                            auto c = r.catalog();
                            if (!c)
                                return std::move(c).error();
                            return (*c)->search(q);
                        },
                        [call](Result<SearchPage> page) mutable {
                            if (!page)
                                return replyError(call, page);
                            std::vector<VariantMap> items;
                            for (const AppSummary& s : page->items)
                                items.push_back(toMap(s));
                            call.reply(page->generation, page->total, items);
                        });
            },
            {{"text", "options"}, {"generation", "total", "results"}});

    o.addMethod(
            "Browse", "sa{sv}", "tuaa{sv}",
            [this](MethodCall& call) {
                if (!admit(call, true))
                    return;
                std::string category;
                VariantMap options;
                call.read(category, options);
                auto opts = searchOptions(options);
                if (!opts)
                    return replyError(call, opts);
                if (!opts->category.empty() && opts->category != category)
                    return call.replyError(errors::kInvalidArgs, "two different categories");
                opts->category = category;
                auto query = BrowseQuery::create(std::move(opts).value());
                if (!query)
                    return replyError(call, query);
                m_reads.read<Result<SearchPage>>(
                        [q = std::move(query).value()](Readers& r) -> Result<SearchPage> {
                            auto c = r.catalog();
                            if (!c)
                                return std::move(c).error();
                            return (*c)->browse(q);
                        },
                        [call](Result<SearchPage> page) mutable {
                            if (!page)
                                return replyError(call, page);
                            std::vector<VariantMap> items;
                            for (const AppSummary& s : page->items)
                                items.push_back(toMap(s));
                            call.reply(page->generation, page->total, items);
                        });
            },
            {{"category", "options"}, {"generation", "total", "results"}});

    o.addMethod(
            "ListCategories", "", "taa{sv}",
            [this](MethodCall& call) {
                if (!admit(call, true))
                    return;
                using Answer = Result<std::pair<std::uint64_t, std::vector<CategoryCount>>>;
                m_reads.read<Answer>(
                        [](Readers& r) -> Answer {
                            auto c = r.catalog();
                            if (!c)
                                return std::move(c).error();
                            auto list = (*c)->listCategories();
                            if (!list)
                                return std::move(list).error();
                            return std::pair{(*c)->generation(), std::move(list).value()};
                        },
                        [call](Answer a) mutable {
                            if (!a)
                                return replyError(call, a);
                            std::vector<VariantMap> items;
                            for (const CategoryCount& c : a->second)
                                items.push_back(toMap(c));
                            call.reply(a->first, items);
                        });
            },
            {{}, {"generation", "categories"}});

    o.addMethod(
            "GetSummaries", "as", "taa{sv}",
            [this](MethodCall& call) {
                if (!admit(call, true))
                    return;
                std::vector<std::string> raw;
                call.read(raw);
                if (raw.size() > kMaxIds)
                    return call.replyError(errors::kInvalidArgs, "at most 200 ids");
                std::vector<AppId> ids;
                for (const std::string& s : raw) {
                    auto id = AppId::parse(s);
                    if (!id)
                        return replyError(call, id);
                    ids.push_back(std::move(id).value());
                }
                using Answer = Result<std::pair<std::uint64_t, std::vector<AppSummary>>>;
                m_reads.read<Answer>(
                        [ids = std::move(ids)](Readers& r) -> Answer {
                            auto c = r.catalog();
                            if (!c)
                                return std::move(c).error();
                            auto list = (*c)->summaries(ids);
                            if (!list)
                                return std::move(list).error();
                            return std::pair{(*c)->generation(), std::move(list).value()};
                        },
                        [call](Answer a) mutable {
                            if (!a)
                                return replyError(call, a);
                            std::vector<VariantMap> items;
                            for (const AppSummary& s : a->second)
                                items.push_back(toMap(s));
                            call.reply(a->first, items);
                        });
            },
            {{"app_ids"}, {"generation", "summaries"}});

    o.addMethod(
            "GetDetails", "s", "ta{sv}",
            [this](MethodCall& call) {
                if (!admit(call, true))
                    return;
                std::string raw;
                call.read(raw);
                auto id = AppId::parse(raw);
                if (!id)
                    return replyError(call, id);
                using Answer = Result<std::pair<std::uint64_t, VariantMap>>;
                m_reads.read<Answer>(
                        [id = std::move(id).value()](Readers& r) -> Answer {
                            auto c = r.catalog();
                            if (!c)
                                return std::move(c).error();
                            auto app = (*c)->application(id);
                            if (!app)
                                return std::move(app).error();
                            const std::vector<AppId> one{app->id()};
                            auto summaries = (*c)->summaries(one);
                            if (!summaries)
                                return std::move(summaries).error();
                            if (summaries->empty())
                                return Error::notFound("no application " + id.str());
                            // Installed offers of this application: by offer
                            // (the catalogue's link) or by the inventory's link.
                            std::vector<InstalledItem> installed;
                            auto state = r.installed();
                            if (!state)
                                return std::move(state).error();
                            for (const Offer& o : app->offers())
                                if (auto item = (*state)->find(o.id()); item && *item)
                                    installed.push_back(std::move(**item));
                            if (auto linked = (*state)->forApplication(app->id()))
                                for (InstalledItem& i : *linked)
                                    if (std::none_of(installed.begin(), installed.end(),
                                                     [&](const InstalledItem& x) { return x.offer() == i.offer(); }))
                                        installed.push_back(std::move(i));
                            return std::pair{(*c)->generation(), details(summaries->front(), *app, installed)};
                        },
                        [call](Answer a) mutable {
                            if (!a)
                                return replyError(call, a);
                            call.reply(a->first, a->second);
                        });
            },
            {{"app_id"}, {"generation", "details"}});

    o.addMethod(
            "GetInstalled", "a{sv}", "aa{sv}",
            [this](MethodCall& call) {
                if (!admit(call, false))
                    return;
                VariantMap options;
                call.read(options);
                auto backend = installedOptions(options);
                if (!backend)
                    return replyError(call, backend);
                m_reads.read<Result<std::vector<InstalledItem>>>(
                        [b = *backend](Readers& r) -> Result<std::vector<InstalledItem>> {
                            auto s = r.installed();
                            if (!s)
                                return std::move(s).error();
                            return (*s)->items(b);
                        },
                        [call](Result<std::vector<InstalledItem>> items) mutable {
                            if (!items)
                                return replyError(call, items);
                            std::vector<VariantMap> out;
                            for (const InstalledItem& i : *items)
                                out.push_back(toMap(i));
                            call.reply(out);
                        });
            },
            {{"options"}, {"items"}});

    if (m_inventory) {
        // What the backend reports, filtered by the catalogue (D-010); kept
        // in memory, refreshed on the backend's signals and each refresh.
        o.addMethod(
                "GetUpdates", "", "aa{sv}",
                [this](MethodCall& call) {
                    admit(call, false);
                    std::vector<VariantMap> out;
                    for (const AvailableUpdate& u : m_inventory->updates())
                        out.push_back(toMap(u, m_backend->kind()));
                    call.reply(out);
                },
                {{}, {"updates"}});
    }

    o.addMethod(
            "ListSources", "", "aa{sv}",
            [this](MethodCall& call) {
                if (!admit(call, true))
                    return;
                m_reads.read<Result<std::vector<VariantMap>>>(
                        [](Readers& r) -> Result<std::vector<VariantMap>> {
                            auto c = r.catalog();
                            if (!c)
                                return std::move(c).error();
                            auto sources = (*c)->sources();
                            if (!sources)
                                return std::move(sources).error();
                            auto journal = r.journal();
                            std::vector<VariantMap> out;
                            for (const RecordedSource& s : *sources) {
                                std::optional<UnixSeconds> last;
                                if (journal)
                                    if (auto l = (*journal)->lastRefresh(s.source.id()))
                                        last = *l;
                                out.push_back(toMap(s, last));
                            }
                            return out;
                        },
                        [call](Result<std::vector<VariantMap>> out) mutable {
                            if (!out)
                                return replyError(call, out);
                            call.reply(*out);
                        });
            },
            {{}, {"sources"}});

    o.addMethod(
            "ListTransactions", "", "ao",
            [this](MethodCall& call) {
                admit(call, false);
                std::vector<ObjectPath> paths;
                for (const auto& [path, t] : m_transactions)
                    paths.push_back({path});
                if (m_manager)
                    for (const std::string& path : m_manager->paths())
                        paths.push_back({path});
                call.reply(paths);
            },
            {{}, {"paths"}});

    o.addMethod(
            "GetTransactionHistory", "u", "aa{sv}",
            [this](MethodCall& call) {
                if (!admit(call, false))
                    return;
                std::uint32_t limit = 0;
                call.read(limit);
                if (limit == 0 || limit > kMaxFinishedTransactions)
                    return call.replyError(errors::kInvalidArgs, "limit must be between 1 and 200");
                m_reads.read<Result<std::vector<TransactionRecord>>>(
                        [limit](Readers& r) -> Result<std::vector<TransactionRecord>> {
                            auto j = r.journal();
                            if (!j)
                                return std::move(j).error();
                            return (*j)->history(limit);
                        },
                        [call](Result<std::vector<TransactionRecord>> records) mutable {
                            if (!records)
                                return replyError(call, records);
                            std::vector<VariantMap> out;
                            for (const TransactionRecord& t : *records)
                                out.push_back(toMap(t));
                            call.reply(out);
                        });
            },
            {{"limit"}, {"entries"}});

    // --- Refresh (§3.5, D-015): rereads the local metadata ---------------------
    o.addMethod(
            "Refresh", "a{sv}", "o",
            [this](MethodCall& call) {
                admit(call, false);
                VariantMap options;
                call.read(options);
                // `force` (b): reimport even when nothing changed.
                // `repositories` (b, default true): refresh the backend's
                // repositories first; false rereads the local metadata only,
                // without network (D-028).
                bool force = false;
                bool repositories = true;
                for (const auto& [key, value] : options) {
                    if (key != "force" && key != "repositories")
                        return call.replyError(errors::kInvalidArgs, "unknown option " + key);
                    const auto f = value.get<bool>();
                    if (!f)
                        return call.replyError(errors::kInvalidArgs, "option " + key + " must be of type b");
                    (key == "force" ? force : repositories) = *f;
                }
                // One refresh at a time: a request during one joins it (D-015).
                if (!m_currentRefresh.empty())
                    return call.reply(ObjectPath{m_currentRefresh});
                if (m_refreshing) // the initial import, without an object
                    return call.replyError(errors::kBusy, "the initial import is running");
                const std::string path = newTransaction();
                auto t = std::make_unique<RefreshTransaction>(
                        &m_connection, path, base::Uuid::generate().toString(), now(),
                        [this] { return cancelRefresh(); }, [this] { return refreshCancellable(); });
                RefreshTransaction* raw = t.get();
                m_transactions[path] = std::move(t);
                m_currentRefresh = path;
                call.reply(ObjectPath{path});
                m_object->emitSignal("TransactionAdded", ObjectPath{path});
                startRefresh(force, repositories, raw);
            },
            {{"options"}, {"transaction"}});

    if (m_manager)
        exportActions();

    o.publish();
}

Result<void> StoreService::actionBackend(const OfferId& offer) const
{
    auto address = decodeOfferId(offer);
    if (!address)
        return std::move(address).error();
    if (!m_backend || m_backend->kind() != address->backend)
        return Error::backend(BackendFailure::Unavailable,
                              "no " + std::string(toString(address->backend)) + " backend in this service");
    if (m_inventory->available() != std::optional<bool>(true))
        return Error::backend(BackendFailure::Unavailable, toString(m_backend->kind()).data()
                                      + std::string(" is not usable: ") + m_inventory->reason());
    return {};
}

void StoreService::exportActions()
{
    using namespace base::dbus;
    Object& o = *m_object;
    // No option is defined yet (SERVICE.md §4.4): any key is refused, so that
    // a client never believes one was applied.
    auto noOptions = [](MethodCall& call, const VariantMap& options) {
        if (options.empty())
            return true;
        call.replyError(errors::kInvalidArgs, "unknown option " + options.begin()->first);
        return false;
    };
    auto answer = [](MethodCall& call, const Result<std::string>& path) {
        if (!path)
            return replyError(call, path);
        call.reply(ObjectPath{*path});
    };

    // Install: the offer must be in the catalogue (§4.1), not installed yet.
    o.addMethod(
            "Install", "sa{sv}", "o",
            [this, noOptions, answer](MethodCall& call) {
                if (!admit(call, true))
                    return;
                std::string raw;
                VariantMap options;
                call.read(raw, options);
                if (!noOptions(call, options))
                    return;
                auto offer = OfferId::parse(raw);
                if (!offer)
                    return replyError(call, offer);
                if (auto ok = actionBackend(*offer); !ok)
                    return replyError(call, ok);
                using Answer = Result<std::pair<std::optional<AppId>, bool>>;
                m_reads.read<Answer>(
                        [offer = *offer](Readers& r) -> Answer {
                            auto c = r.catalog();
                            if (!c)
                                return std::move(c).error();
                            const std::vector<OfferId> one{offer};
                            auto links = (*c)->offerApplications(one);
                            if (!links)
                                return std::move(links).error();
                            if (links->empty())
                                return Error::notFound("no offer " + offer.str() + " in the catalogue");
                            auto state = r.installed();
                            if (!state)
                                return std::move(state).error();
                            auto item = (*state)->find(offer);
                            if (!item)
                                return std::move(item).error();
                            return std::pair{std::optional<AppId>(links->front().second), item->has_value()};
                        },
                        [this, call, answer, offer = *offer](Answer a) mutable {
                            if (!a)
                                return replyError(call, a);
                            if (a->second)
                                return call.replyError(errors::kNotAllowed, offer.str() + " is already installed");
                            answer(call, m_manager->start(TransactionKind::Install, {{offer, a->first}}));
                        });
            },
            {{"offer_id", "options"}, {"transaction"}});

    // Remove: the offer must be installed (state.sqlite). A protected package
    // is refused while planning, with not-allowed (D-032).
    o.addMethod(
            "Remove", "sa{sv}", "o",
            [this, noOptions, answer](MethodCall& call) {
                if (!admit(call, false))
                    return;
                std::string raw;
                VariantMap options;
                call.read(raw, options);
                if (!noOptions(call, options))
                    return;
                auto offer = OfferId::parse(raw);
                if (!offer)
                    return replyError(call, offer);
                if (auto ok = actionBackend(*offer); !ok)
                    return replyError(call, ok);
                using Answer = Result<std::optional<AppId>>;
                m_reads.read<Answer>(
                        [offer = *offer](Readers& r) -> Answer {
                            auto state = r.installed();
                            if (!state)
                                return std::move(state).error();
                            auto item = (*state)->find(offer);
                            if (!item)
                                return std::move(item).error();
                            if (!item->has_value())
                                return Error::notFound(offer.str() + " is not installed");
                            return (*item)->app();
                        },
                        [this, call, answer, offer = *offer](Answer a) mutable {
                            if (!a)
                                return replyError(call, a);
                            answer(call, m_manager->start(TransactionKind::Remove, {{offer, *a}}));
                        });
            },
            {{"offer_id", "options"}, {"transaction"}});

    // Update: every offer must have an update listed by GetUpdates (D-010).
    o.addMethod(
            "Update", "asa{sv}", "o",
            [this, noOptions, answer](MethodCall& call) {
                if (!admit(call, false))
                    return;
                std::vector<std::string> raw;
                VariantMap options;
                call.read(raw, options);
                if (!noOptions(call, options))
                    return;
                if (raw.empty() || raw.size() > kMaxIds)
                    return call.replyError(errors::kInvalidArgs, "between 1 and 200 offers");
                std::vector<TransactionManager::Target> targets;
                for (const std::string& r : raw) {
                    auto offer = OfferId::parse(r);
                    if (!offer)
                        return replyError(call, offer);
                    if (auto ok = actionBackend(*offer); !ok)
                        return replyError(call, ok);
                    const auto& updates = m_inventory->updates();
                    const auto it = std::find_if(updates.begin(), updates.end(),
                                                 [&](const AvailableUpdate& u) { return u.offer == *offer; });
                    if (it == updates.end())
                        return call.replyError(errors::kNotFound, "no update for " + offer->str());
                    targets.push_back({*offer, it->app});
                }
                answer(call, m_manager->start(TransactionKind::Update, std::move(targets)));
            },
            {{"offer_ids", "options"}, {"transaction"}});

    // UpdateAll: a parent update-all transaction, one child per backend and
    // installation (D-031).
    o.addMethod(
            "UpdateAll", "a{sv}", "o",
            [this, noOptions, answer](MethodCall& call) {
                if (!admit(call, false))
                    return;
                VariantMap options;
                call.read(options);
                if (!noOptions(call, options))
                    return;
                if (m_inventory->available() != std::optional<bool>(true))
                    return call.replyError(errors::kBackendUnavailable, "no usable backend");
                std::vector<TransactionManager::Target> targets;
                for (const AvailableUpdate& u : m_inventory->updates())
                    targets.push_back({u.offer, u.app});
                answer(call, m_manager->startUpdateAll(std::move(targets)));
            },
            {{"options"}, {"transaction"}});
}

} // namespace cutefish::store::service
