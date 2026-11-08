#include "storeservice.h"

#include "marshal.h"

#include <cutefish/store/sqlitecatalog.h>

#include <base/logging.h>
#include <base/uuid.h>

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

StoreService::StoreService(base::EventLoop& loop, base::dbus::Connection& connection, ServiceOptions options)
    : m_loop(loop), m_connection(connection), m_options(std::move(options)),
      m_reads(m_options.dataDir / "catalog.sqlite", m_options.dataDir / "state.sqlite", m_options.readLocales),
      m_refresher(loop, m_options), m_lifecycle(loop, connection, m_options.idleTimeout, [this] {
          if (onIdle)
              onIdle();
      })
{
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
    }
    if (auto last = m_state->lastRefresh())
        m_lastRefresh = *last;

    exportObject();
    if (!m_connection.requestName(m_options.busName))
        return Error::conflict("the name " + m_options.busName + " is owned by another instance");
    loadGeneration();
    m_lifecycle.evaluate();
    return {};
}

void StoreService::setState(std::string state)
{
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
                m_generation = generation;
                m_reads.setGeneration(generation);
                if (generation > 0) {
                    setState("ready");
                    m_object->emitPropertiesChanged({"CatalogGeneration"});
                    return;
                }
                // No catalogue: initial import (D-015), no transaction object.
                setState("indexing");
                m_initialImport = true;
                startRefresh(true, nullptr);
            });
}

std::string StoreService::newTransaction()
{
    return std::string(kPath) + "/Transactions/" + std::to_string(m_nextTransaction++);
}

void StoreService::startRefresh(bool force, RefreshTransaction* transaction)
{
    m_refreshing = true;
    m_lifecycle.beginActivity();
    m_object->emitPropertiesChanged({"Refreshing"});
    m_refresher.start(force, m_generation,
                      [this, transaction](Result<RefreshOutcome> r) { refreshDone(std::move(r), transaction); });
    if (transaction)
        transaction->updateCancellable();
}

void StoreService::refreshDone(Result<RefreshOutcome> result, RefreshTransaction* transaction)
{
    m_refreshing = false;
    std::vector<std::string> changed{"Refreshing"};
    if (result) {
        if (auto ok = m_state->recordRefresh(result->refreshedSources, now()); ok) {
            m_lastRefresh = now();
            changed.push_back("LastRefresh");
        } else {
            CF_WARNING(cfStore) << "cannot record the refresh: " << ok.error().message();
        }
        if (result->replaced) {
            m_generation = result->generation;
            m_reads.setGeneration(m_generation);
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
        if (result)
            transaction->finish(TransactionState::Succeeded, std::nullopt, now());
        else if (result.error().code() == ErrorCode::Cancelled)
            transaction->finish(TransactionState::Cancelled, std::nullopt, now());
        else
            transaction->finish(TransactionState::Failed, result.error(), now());
        retire(transaction->path());
    }
    m_currentRefresh.clear();
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
    // No backend in service v0.1 (steps F and G).
    o.addProperty("Backends", [] { return std::vector<VariantMap>{}; });
    o.addProperty("Refreshing", [this] { return m_refreshing; });
    o.addProperty("LastRefresh", [this] { return static_cast<std::uint64_t>(m_lastRefresh.value_or(0)); });

    // --- Signals (§4.7) ------------------------------------------------------
    o.addSignal("CatalogChanged", "t", {"generation"});
    o.addSignal("InstalledChanged", "as", {"app_ids"});
    o.addSignal("TransactionAdded", "o", {"transaction"});
    o.addSignal("TransactionRemoved", "o", {"transaction"});
    o.addSignal("SourcesChanged", "", {});

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
                bool force = false;
                for (const auto& [key, value] : options) {
                    if (key != "force")
                        return call.replyError(errors::kInvalidArgs, "unknown option " + key);
                    const auto f = value.get<bool>();
                    if (!f)
                        return call.replyError(errors::kInvalidArgs, "option force must be of type b");
                    force = *f;
                }
                // One refresh at a time: a request during one joins it (D-015).
                if (!m_currentRefresh.empty())
                    return call.reply(ObjectPath{m_currentRefresh});
                if (m_refreshing) // the initial import, without an object
                    return call.replyError(errors::kBusy, "the initial import is running");
                const std::string path = newTransaction();
                auto t = std::make_unique<RefreshTransaction>(
                        &m_connection, path, base::Uuid::generate().toString(), now(),
                        [this] { return m_refresher.cancel(); }, [this] { return m_refresher.cancellable(); });
                RefreshTransaction* raw = t.get();
                m_transactions[path] = std::move(t);
                m_currentRefresh = path;
                call.reply(ObjectPath{path});
                m_object->emitSignal("TransactionAdded", ObjectPath{path});
                startRefresh(force, raw);
            },
            {{"options"}, {"transaction"}});

    o.publish();
}

} // namespace cutefish::store::service
