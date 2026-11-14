// cutefish-store-service on a private session bus: activation, the
// exported API, errors, refresh, clients and inactivity, concurrency.

#include "busfixture.h"

#include <cutefish/store/sqlitecatalog.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <thread>

using namespace busfixture;
using namespace std::chrono_literals;
namespace store = cutefish::store;

namespace {

dbus::VariantMap opts(std::initializer_list<std::pair<const std::string, dbus::Variant>> list)
{
    return dbus::VariantMap(list);
}

std::string errorName(const dbus::Reply& r)
{
    return r.isError() ? r.error().name : std::string("<no error>");
}

std::string refresh(Bus& b, bool force)
{
    dbus::ObjectPath path;
    EXPECT_TRUE(b.store().call("Refresh", opts({{"force", force}})).read(path));
    return path.path;
}

std::string transactionState(Bus& b, const std::string& path)
{
    return b.transaction(path).property<std::string>("State").value_or("<none>");
}

bool waitFinished(Bus& b, const std::string& path)
{
    return b.waitUntil([&] {
        const std::string s = transactionState(b, path);
        return s == "succeeded" || s == "failed" || s == "cancelled";
    });
}

} // namespace

// --- Activation and export ---------------------------------------------------------

TEST(ServiceBus, ActivatedByTheFirstCallAndExportsOnlyWhatWorks)
{
    Bus b;
    EXPECT_FALSE(b.serviceRunning());
    dbus::Proxy introspectable(b.client.get(), kName, kPath, "org.freedesktop.DBus.Introspectable");
    std::string xml;
    ASSERT_TRUE(introspectable.call("Introspect").read(xml)) << errorName(introspectable.call("Introspect"));
    EXPECT_TRUE(b.serviceRunning());
    for (const char* member : {"interface name=\"org.cutefish.Store1\"", "\"Search\"", "\"Browse\"",
                               "\"ListCategories\"", "\"GetSummaries\"", "\"GetDetails\"", "\"GetInstalled\"",
                               "\"ListSources\"", "\"ListTransactions\"", "\"GetTransactionHistory\"", "\"Refresh\"",
                               "\"CatalogChanged\"", "\"SourcesChanged\"", "\"State\"", "\"CatalogGeneration\""})
        EXPECT_NE(xml.find(member), std::string::npos) << member;
    // Nothing that cannot work without the backends (owner's decision).
    for (const char* member : {"\"Install\"", "\"Remove\"", "\"Update\"", "\"UpdateAll\"", "\"GetUpdates\"",
                               "\"GetPermissions\"", "\"EnableSource\"", "\"DisableSource\"",
                               "\"RemoveSource\"", "\"AddSource\"", "\"UpdateCount\""})
        EXPECT_EQ(xml.find(member), std::string::npos) << member;
    // Argument names are published for introspection.
    EXPECT_NE(xml.find("name=\"generation\""), std::string::npos);
    const dbus::Reply install = b.store().call("Install", std::string("packagekit:gimp"), dbus::VariantMap{});
    EXPECT_EQ(errorName(install), "org.freedesktop.DBus.Error.UnknownMethod");
}

TEST(ServiceBus, InitialImportThenReady)
{
    Bus b;
    ASSERT_TRUE(b.waitReady());
    EXPECT_EQ(b.property<std::uint64_t>("CatalogGeneration"), std::optional<std::uint64_t>(1));
    EXPECT_GT(b.property<std::uint64_t>("LastRefresh").value_or(0), 0u);
    EXPECT_EQ(b.property<std::string>("Version"), std::optional<std::string>("0.1.0"));
    EXPECT_EQ(b.property<bool>("Refreshing"), std::optional<bool>(false));
    const auto backends = b.property<std::vector<dbus::VariantMap>>("Backends");
    ASSERT_TRUE(backends);
    EXPECT_TRUE(backends->empty()); // no backend in v0.1
    EXPECT_TRUE(std::filesystem::exists(b.dir / "data" / "catalog.sqlite"));
    EXPECT_TRUE(std::filesystem::exists(b.dir / "data" / "state.sqlite"));
}

TEST(ServiceBus, NotReadyBeforeTheFirstCatalogue)
{
    // The indexer is held until the test releases it: the state is
    // observable, not a fixed delay that a loaded machine can outrun.
    Bus b({.blockIndexer = true});
    ASSERT_TRUE(b.waitUntil([&] { return b.property<std::string>("State") == std::optional<std::string>("indexing"); }));
    const dbus::Reply r = b.store().call("Search", std::string("gimp"), dbus::VariantMap{});
    EXPECT_EQ(errorName(r), "org.cutefish.Store1.Error.NotReady");
    EXPECT_EQ(b.property<std::string>("State"), std::optional<std::string>("indexing"));
    EXPECT_EQ(b.property<bool>("Refreshing"), std::optional<bool>(true));
    {
        std::ofstream release(b.indexerRelease());
        release << "go\n";
    }
    ASSERT_TRUE(b.waitReady());
}

TEST(ServiceBus, DegradedWithoutAnySource)
{
    Bus b;
    std::ofstream(b.dir / "sources.conf", std::ios::trunc) << "# nothing configured\n";
    ASSERT_TRUE(b.waitUntil(
            [&] { return b.property<std::string>("State") == std::optional<std::string>("degraded"); }));
    EXPECT_EQ(errorName(b.store().call("Browse", std::string(), dbus::VariantMap{})),
              "org.cutefish.Store1.Error.NotReady");
}

// --- Reading -------------------------------------------------------------------------

TEST(ServiceBus, SearchBrowseAndCategories)
{
    Bus b;
    ASSERT_TRUE(b.waitReady());
    auto found = page(b.store().call("Search", std::string("gimp"), opts({{"limit", std::uint32_t(5)}})));
    ASSERT_TRUE(found);
    EXPECT_EQ(found->generation, 1u);
    ASSERT_FALSE(found->items.empty());
    const dbus::VariantMap& gimp = found->items.front();
    EXPECT_EQ(gimp.at("id").value<std::string>(), "org.gimp.GIMP");
    EXPECT_EQ(gimp.at("installed").value<bool>(true), false);
    EXPECT_EQ(gimp.at("backends").value<std::vector<std::string>>().size(), 2u);

    auto all = page(b.store().call("Browse", std::string(), opts({{"limit", std::uint32_t(200)}})));
    ASSERT_TRUE(all);
    ASSERT_GT(all->total, 2u);
    EXPECT_EQ(all->items.size(), all->total);
    // Pages cover the list.
    std::vector<std::string> paged;
    for (std::uint32_t offset = 0; offset < all->total; offset += 2) {
        auto p = page(b.store().call("Browse", std::string(),
                                     opts({{"limit", std::uint32_t(2)}, {"offset", offset}})));
        ASSERT_TRUE(p);
        for (const auto& id : p->ids())
            paged.push_back(id);
    }
    EXPECT_EQ(paged, all->ids());

    std::uint64_t generation = 0;
    std::vector<dbus::VariantMap> categories;
    ASSERT_TRUE(b.store().call("ListCategories").read(generation, categories));
    ASSERT_FALSE(categories.empty());
    for (const auto& c : categories) {
        const std::string id = c.at("id").value<std::string>();
        auto inCategory = page(b.store().call("Browse", id, dbus::VariantMap{}));
        ASSERT_TRUE(inCategory);
        EXPECT_EQ(inCategory->total, c.at("count").value<std::uint32_t>()) << id;
    }
    auto flatpakOnly = page(b.store().call("Browse", std::string(), opts({{"backend", std::string("flatpak")}})));
    ASSERT_TRUE(flatpakOnly);
    EXPECT_LT(flatpakOnly->total, all->total);
    // installed-only works: state.sqlite is attached, and nothing is installed.
    auto installed = page(b.store().call("Browse", std::string(), opts({{"installed-only", true}})));
    ASSERT_TRUE(installed);
    EXPECT_EQ(installed->total, 0u);
}

TEST(ServiceBus, SummariesAndDetails)
{
    Bus b;
    ASSERT_TRUE(b.waitReady());
    std::uint64_t generation = 0;
    std::vector<dbus::VariantMap> summaries;
    ASSERT_TRUE(b.store()
                        .call("GetSummaries", std::vector<std::string>{"org.gimp.GIMP", "org.example.Unknown"})
                        .read(generation, summaries));
    ASSERT_EQ(summaries.size(), 1u); // unknown ids are left out
    EXPECT_EQ(summaries[0].at("name").value<std::string>(), "GIMP");

    dbus::VariantMap details;
    ASSERT_TRUE(b.store().call("GetDetails", std::string("org.gimp.GIMP")).read(generation, details));
    const auto offers = details.at("offers").value<std::vector<dbus::VariantMap>>();
    ASSERT_EQ(offers.size(), 2u);
    int defaults = 0;
    for (const auto& o : offers) {
        defaults += o.at("default").value<bool>();
        EXPECT_FALSE(o.at("installed").value<bool>(true));
        EXPECT_FALSE(o.at("origin").value<std::string>().empty() && o.at("backend").value<std::string>() == "flatpak");
    }
    EXPECT_EQ(defaults, 1); // D-009: exactly one
    for (const auto& o : offers) {
        if (o.at("default").value<bool>()) {
            EXPECT_EQ(o.at("backend").value<std::string>(), "flatpak"); // Flathub first
        }
    }
    using Blocks = std::vector<std::tuple<std::string, std::string>>;
    EXPECT_FALSE(details.at("description").value<Blocks>().empty());
}

TEST(ServiceBus, SourcesInstalledAndHistory)
{
    Bus b;
    ASSERT_TRUE(b.waitReady());
    std::vector<dbus::VariantMap> sources;
    ASSERT_TRUE(b.store().call("ListSources").read(sources));
    ASSERT_EQ(sources.size(), 2u);
    for (const auto& s : sources) {
        EXPECT_GT(s.at("offers").value<std::uint32_t>(), 0u);
        EXPECT_TRUE(s.contains("last-refresh"));
        EXPECT_FALSE(s.at("can-toggle").value<bool>(true));
    }
    std::vector<dbus::VariantMap> installed;
    ASSERT_TRUE(b.store().call("GetInstalled", dbus::VariantMap{}).read(installed));
    EXPECT_TRUE(installed.empty()); // nothing fills the inventory before the backends
    std::vector<dbus::VariantMap> history;
    ASSERT_TRUE(b.store().call("GetTransactionHistory", std::uint32_t(10)).read(history));
    EXPECT_TRUE(history.empty());
}

TEST(ServiceBus, InvalidArgumentsAreRefused)
{
    Bus b;
    ASSERT_TRUE(b.waitReady());
    auto s = b.store();
    const std::string invalid = "org.freedesktop.DBus.Error.InvalidArgs";
    EXPECT_EQ(errorName(s.call("Search", std::string(300, 'a'), dbus::VariantMap{})), invalid);
    EXPECT_EQ(errorName(s.call("Search", std::string("gimp"), opts({{"bogus", true}}))), invalid);
    EXPECT_EQ(errorName(s.call("Search", std::string("gimp"), opts({{"limit", std::string("ten")}}))), invalid);
    EXPECT_EQ(errorName(s.call("Search", std::string("gimp"), opts({{"limit", std::uint32_t(0)}}))), invalid);
    EXPECT_EQ(errorName(s.call("Search", std::string("gimp"), opts({{"limit", std::uint32_t(201)}}))), invalid);
    EXPECT_EQ(errorName(s.call("Search", std::string("gimp"), opts({{"backend", std::string("snap")}}))), invalid);
    EXPECT_EQ(errorName(s.call("Search", std::int32_t(3))), invalid); // wrong signature
    EXPECT_EQ(errorName(s.call("Browse", std::string("Graphics"), opts({{"category", std::string("Office")}}))),
              invalid);
    EXPECT_EQ(errorName(s.call("GetSummaries", std::vector<std::string>(201, "org.gimp.GIMP"))), invalid);
    EXPECT_EQ(errorName(s.call("GetSummaries", std::vector<std::string>{"not..valid"})), invalid);
    EXPECT_EQ(errorName(s.call("GetDetails", std::string("org.example.Unknown"))), "org.cutefish.Store1.Error.NotFound");
    EXPECT_EQ(errorName(s.call("GetDetails", std::string(""))), invalid);
    EXPECT_EQ(errorName(s.call("GetInstalled", opts({{"scope", std::string("user")}}))), invalid);
    EXPECT_EQ(errorName(s.call("GetTransactionHistory", std::uint32_t(0))), invalid);
    EXPECT_EQ(errorName(s.call("Refresh", opts({{"force", std::string("yes")}}))), invalid);
    // The service is still there and answers.
    EXPECT_TRUE(page(s.call("Search", std::string("gimp"), dbus::VariantMap{})));
}

// --- Refresh --------------------------------------------------------------------------

TEST(ServiceBus, RefreshIsATransaction)
{
    Bus b({.retention = 60s});
    ASSERT_TRUE(b.waitReady());
    std::vector<std::uint64_t> catalogChanged;
    auto match = b.client->subscribe(rule(kPath, kInterface, "CatalogChanged"),
                                     [&](const dbus::Message& m) {
                                         if (auto g = m.argument<std::uint64_t>())
                                             catalogChanged.push_back(*g);
                                     });

    // Nothing changed: success, same generation.
    const std::string first = refresh(b, false);
    ASSERT_TRUE(waitFinished(b, first));
    EXPECT_EQ(transactionState(b, first), "succeeded");
    auto t = b.transaction(first);
    EXPECT_EQ(t.property<std::string>("Kind"), std::optional<std::string>("refresh"));
    EXPECT_EQ(t.property<std::string>("Backend"), std::optional<std::string>("multiple"));
    EXPECT_GT(t.property<std::uint64_t>("Finished").value_or(0), 0u);
    EXPECT_EQ(errorName(t.call("Confirm")), "org.cutefish.Store1.Error.NotAllowed");
    EXPECT_EQ(b.property<std::uint64_t>("CatalogGeneration"), std::optional<std::uint64_t>(1));

    // A changed source: imported, new generation, CatalogChanged.
    {
        std::ofstream(b.dir / "flathub.xml", std::ios::app) << "<!-- changed -->\n";
    }
    std::vector<std::string> finished;
    const std::string second = refresh(b, false);
    auto finishedMatch = b.client->subscribe(rule(second, kTransaction, "Finished"),
                                             [&](const dbus::Message& m) {
                                                 if (auto s = m.argument<std::string>())
                                                     finished.push_back(*s);
                                             });
    ASSERT_TRUE(waitFinished(b, second));
    EXPECT_NE(second, first);
    EXPECT_EQ(transactionState(b, second), "succeeded");
    ASSERT_TRUE(b.waitUntil([&] { return !catalogChanged.empty(); }));
    EXPECT_EQ(catalogChanged.back(), 2u);
    EXPECT_EQ(b.property<std::uint64_t>("CatalogGeneration"), std::optional<std::uint64_t>(2));

    // Forced: imported even when nothing changed.
    const std::string third = refresh(b, true);
    ASSERT_TRUE(waitFinished(b, third));
    EXPECT_EQ(b.property<std::uint64_t>("CatalogGeneration"), std::optional<std::uint64_t>(3));

    std::vector<dbus::ObjectPath> listed;
    ASSERT_TRUE(b.store().call("ListTransactions").read(listed));
    EXPECT_EQ(listed.size(), 3u); // kept on the bus for the retention time
    // Searches see the new generation.
    auto p = page(b.store().call("Search", std::string("gimp"), dbus::VariantMap{}));
    ASSERT_TRUE(p);
    EXPECT_EQ(p->generation, 3u);
}

TEST(ServiceBus, AFailedRefreshKeepsTheCatalogue)
{
    Bus b;
    ASSERT_TRUE(b.waitReady());
    std::filesystem::remove(b.dir / "flathub.xml");
    const std::string path = refresh(b, true);
    ASSERT_TRUE(waitFinished(b, path));
    EXPECT_EQ(transactionState(b, path), "failed");
    const auto error = b.transaction(path).property<dbus::VariantMap>("Error");
    ASSERT_TRUE(error);
    EXPECT_EQ(error->at("code").value<std::string>(), "not-found");
    EXPECT_NE(error->at("details").value<std::string>().find("flatpak:user:flathub"), std::string::npos);
    EXPECT_EQ(b.property<std::uint64_t>("CatalogGeneration"), std::optional<std::uint64_t>(1));
    EXPECT_EQ(b.property<std::string>("State"), std::optional<std::string>("ready"));
    EXPECT_TRUE(page(b.store().call("Search", std::string("inkscape"), dbus::VariantMap{})));
}

TEST(ServiceBus, ARemovedSourceIsDroppedOnPurpose)
{
    Bus b;
    ASSERT_TRUE(b.waitReady());
    bool sourcesChanged = false;
    auto match = b.client->subscribe(rule(kPath, kInterface, "SourcesChanged"),
                                     [&](const dbus::Message&) { sourcesChanged = true; });
    b.writeSources(false);
    const std::string path = refresh(b, false);
    ASSERT_TRUE(waitFinished(b, path));
    EXPECT_EQ(transactionState(b, path), "succeeded");
    std::vector<dbus::VariantMap> sources;
    ASSERT_TRUE(b.store().call("ListSources").read(sources));
    EXPECT_EQ(sources.size(), 1u);
    ASSERT_TRUE(b.waitUntil([&] { return sourcesChanged; }));
}

TEST(ServiceBus, OneRefreshAtATimeAndCancellation)
{
    Bus b({.indexerDelay = 3});
    ASSERT_TRUE(b.waitReady());
    const std::string path = refresh(b, true);
    // A request during a refresh joins it (D-015).
    EXPECT_EQ(refresh(b, true), path);
    EXPECT_EQ(b.property<bool>("Refreshing"), std::optional<bool>(true));
    ASSERT_TRUE(b.waitUntil([&] { return b.transaction(path).property<bool>("Cancellable") == std::optional<bool>(true); }));
    const auto cancelledAt = std::chrono::steady_clock::now();
    ASSERT_FALSE(b.transaction(path).call("Cancel").isError());
    ASSERT_TRUE(waitFinished(b, path));
    EXPECT_LT(std::chrono::steady_clock::now() - cancelledAt, 2s); // the indexer is stopped, not awaited
    EXPECT_EQ(transactionState(b, path), "cancelled");
    EXPECT_EQ(b.property<std::uint64_t>("CatalogGeneration"), std::optional<std::uint64_t>(1));
    EXPECT_EQ(errorName(b.transaction(path).call("Cancel")), "org.cutefish.Store1.Error.NotAllowed");
}

TEST(ServiceBus, FinishedTransactionsLeaveTheBus)
{
    Bus b({.retention = 300ms});
    ASSERT_TRUE(b.waitReady());
    std::string removed;
    auto match = b.client->subscribe(rule(kPath, kInterface, "TransactionRemoved"),
                                     [&](const dbus::Message& m) {
                                         if (auto p = m.argument<dbus::ObjectPath>())
                                             removed = p->path;
                                     });
    const std::string path = refresh(b, false);
    ASSERT_TRUE(waitFinished(b, path));
    ASSERT_TRUE(b.waitUntil([&] { return removed == path; }, 5s));
    std::vector<dbus::ObjectPath> listed;
    ASSERT_TRUE(b.store().call("ListTransactions").read(listed));
    EXPECT_TRUE(listed.empty());
    EXPECT_FALSE(b.transaction(path).property<std::string>("State"));
}

// --- Clients and inactivity (P-010) -------------------------------------------------------

TEST(ServiceBus, StopsWhenTheLastClientLeaves)
{
    Bus b({.idleTimeout = 800ms});
    ASSERT_TRUE(b.waitReady());
    ASSERT_TRUE(page(b.store().call("Search", std::string("gimp"), dbus::VariantMap{})));
    // A known client keeps it up, well past the timeout.
    std::this_thread::sleep_for(2s);
    EXPECT_TRUE(b.serviceRunning());
    // The client goes; an observer that never calls the service does not count.
    b.client = dbus::Connection::open(dbus::Connection::Bus::Session, &b.loop);
    ASSERT_TRUE(b.waitUntil([&] { return !b.serviceRunning(); }, 10s));
    // Activated again by the next call, with the same catalogue.
    ASSERT_TRUE(b.waitReady());
    EXPECT_EQ(b.property<std::uint64_t>("CatalogGeneration"), std::optional<std::uint64_t>(1));
}

TEST(ServiceBus, NeverStopsDuringARefresh)
{
    Bus b({.idleTimeout = 500ms, .indexerDelay = 3});
    ASSERT_TRUE(b.waitReady());
    // A client asks for a refresh and leaves at once.
    refresh(b, true);
    b.client = dbus::Connection::open(dbus::Connection::Bus::Session, &b.loop);
    std::this_thread::sleep_for(1500ms); // three idle timeouts
    EXPECT_TRUE(b.serviceRunning());
    // Once the refresh is done, the idle rule applies again.
    ASSERT_TRUE(b.waitUntil([&] { return !b.serviceRunning(); }, 15s));
    auto catalog = store::SqliteCatalog::open(b.dir / "data" / "catalog.sqlite");
    ASSERT_TRUE(catalog);
    EXPECT_EQ(catalog->generation(), 2u); // the refresh went to its end
}

TEST(ServiceBus, OnlyOneInstance)
{
    Bus b;
    ASSERT_TRUE(b.waitReady());
    const pid_t pid = ::fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        const std::string data = (b.dir / "data").string();
        const std::string sources = (b.dir / "sources.conf").string();
        ::execl(SERVICE_BINARY, SERVICE_BINARY, "--data-dir", data.c_str(), "--sources", sources.c_str(), nullptr);
        ::_exit(127);
    }
    int status = 0;
    ASSERT_EQ(::waitpid(pid, &status, 0), pid);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 1); // the name is taken
    EXPECT_TRUE(page(b.store().call("Search", std::string("gimp"), dbus::VariantMap{})));
}

// --- Concurrency ---------------------------------------------------------------------------

TEST(ServiceBus, ConcurrentClients)
{
    Bus b;
    ASSERT_TRUE(b.waitReady());
    std::atomic<int> ok{0}, failed{0};
    std::vector<std::thread> clients;
    for (int c = 0; c < 4; ++c)
        clients.emplace_back([&, c] {
            base::EventLoop loop;
            auto connection = dbus::Connection::open(dbus::Connection::Bus::Session, &loop);
            if (!connection) {
                ++failed;
                return;
            }
            dbus::Proxy store(connection.get(), kName, kPath, kInterface);
            for (int i = 0; i < 25; ++i) {
                const bool search = (i + c) % 2 == 0;
                auto p = search ? page(store.call("Search", std::string("gimp"), dbus::VariantMap{}))
                                : page(store.call("Browse", std::string(), dbus::VariantMap{}));
                if (p && !p->items.empty() && (!search || p->items.front().at("id").value<std::string>() == "org.gimp.GIMP"))
                    ++ok;
                else
                    ++failed;
            }
        });
    for (auto& t : clients)
        t.join();
    EXPECT_EQ(failed.load(), 0);
    EXPECT_EQ(ok.load(), 100);
}

TEST(ServiceBus, AClientLeavingDuringRequestsHarmsNobody)
{
    Bus b;
    ASSERT_TRUE(b.waitReady());
    {
        auto leaving = dbus::Connection::open(dbus::Connection::Bus::Session, &b.loop);
        ASSERT_TRUE(leaving);
        dbus::Proxy store(leaving.get(), kName, kPath, kInterface);
        std::vector<dbus::PendingCall> pending;
        for (int i = 0; i < 20; ++i)
            pending.push_back(store.callAsync("Browse", [](const dbus::Reply&) {}, std::string(), dbus::VariantMap{}));
        // Gone before any reply arrives.
    }
    std::this_thread::sleep_for(200ms);
    EXPECT_TRUE(b.serviceRunning());
    auto p = page(b.store().call("Search", std::string("gimp"), dbus::VariantMap{}));
    ASSERT_TRUE(p);
    EXPECT_FALSE(p->items.empty());
}
