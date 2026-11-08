// The service with two scripted backends (H.1): the Backends property lists
// both, one backend's unavailability does not degrade the other, the service
// degrades only when all are unavailable, and an action selects the backend
// its offer names. In-process (no service binary), on a private bus, with a
// prebuilt catalogue; no package manager is touched.

#include "fakebackend.h"
#include "storeservice.h"

#include "catalogfixture.h"
#include "privatebus.h"

#include <base/dbus.h>
#include <base/event_loop.h>

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace cutefish::store;
using namespace cutefish::store::service;
using namespace cutefish::store::service::testutil;
namespace base = cutefish::base;
namespace dbus = cutefish::base::dbus;
using namespace std::chrono_literals;

namespace {

struct Fetch
{
    bool done = false;
    std::optional<dbus::VariantMap> map;
    std::optional<dbus::Reply> reply;
};

class ServiceFixture : public ::testing::Test
{
protected:
    void SetUp() override
    {
        catalogfixture::build(m_dir / "catalog.sqlite");
        m_conn = dbus::Connection::open(dbus::Connection::Bus::Session, &m_loop);
        ASSERT_TRUE(m_conn);
        ServiceOptions options;
        options.dataDir = m_dir.path();
        options.sourcesFile = m_dir / "sources.conf";
        options.indexer = "cutefish-store-indexer";
        options.readLocales = {"C"};
        options.busName = "org.cutefish.Store";
        m_service = std::make_unique<StoreService>(m_loop, *m_conn, std::move(options), makeBackends());
        ASSERT_TRUE(m_service->start()) << "service did not start";
    }

    /// The backends the fixture registers; overridden per test.
    virtual std::vector<std::unique_ptr<Backend>> makeBackends() = 0;

    bool spin(const std::function<bool()>& pred, std::chrono::milliseconds timeout = 5s)
    {
        const auto end = std::chrono::steady_clock::now() + timeout;
        while (!pred()) {
            if (std::chrono::steady_clock::now() > end)
                return false;
            const int t = m_loop.startTimer(5ms, false, [this] { m_loop.quit(0); });
            m_loop.run();
            m_loop.stopTimer(t);
        }
        return true;
    }

    std::optional<dbus::VariantMap> properties()
    {
        auto fetch = std::make_shared<Fetch>();
        std::vector<dbus::PendingCall> calls;
        calls.push_back(store().getAllAsync([fetch](const dbus::Reply& reply) {
            fetch->map = reply.value<dbus::VariantMap>();
            fetch->done = true;
        }));
        spin([&] { return fetch->done; });
        return fetch->map;
    }

    std::optional<dbus::Reply> install(const std::string& offer)
    {
        auto fetch = std::make_shared<Fetch>();
        std::vector<dbus::PendingCall> calls;
        calls.push_back(store().callAsync("Install", [fetch](const dbus::Reply& reply) {
            fetch->reply = reply;
            fetch->done = true;
        }, offer, dbus::VariantMap{}));
        spin([&] { return fetch->done; });
        return fetch->reply;
    }

    std::optional<dbus::VariantMap> transaction(const std::string& path)
    {
        auto fetch = std::make_shared<Fetch>();
        std::vector<dbus::PendingCall> calls;
        calls.push_back(dbus::Proxy(m_conn.get(), "org.cutefish.Store", path, "org.cutefish.Store1.Transaction")
                                .getAllAsync([fetch](const dbus::Reply& reply) {
                                    fetch->map = reply.value<dbus::VariantMap>();
                                    fetch->done = true;
                                }));
        spin([&] { return fetch->done; });
        return fetch->map;
    }

    dbus::Proxy store() const
    {
        return dbus::Proxy(m_conn.get(), "org.cutefish.Store", "/org/cutefish/Store", "org.cutefish.Store1");
    }

    base::EventLoop& loop() { return m_loop; }

    privatebus::PrivateBus m_bus;
    base::EventLoop m_loop;
    std::unique_ptr<dbus::Connection> m_conn;
    catalogfixture::TempDir m_dir;
    std::unique_ptr<StoreService> m_service;
};

class TwoAvailableBackends : public ServiceFixture
{
    std::vector<std::unique_ptr<Backend>> makeBackends() override
    {
        std::vector<std::unique_ptr<Backend>> backends;
        backends.push_back(std::make_unique<FakeBackend>(loop(), BackendKind::PackageKit));
        backends.push_back(std::make_unique<FakeBackend>(loop(), BackendKind::Flatpak));
        return backends;
    }
};

TEST_F(TwoAvailableBackends, ListsBothBackends)
{
    std::optional<dbus::VariantMap> props;
    ASSERT_TRUE(spin([&] {
        props = properties();
        if (!props)
            return false;
        const auto it = props->find("State");
        return it != props->end() && it->second.get<std::string>().value_or("") == "ready";
    }));

    const auto backends = props->at("Backends").value<std::vector<dbus::VariantMap>>();
    ASSERT_EQ(backends.size(), 2u);
    const dbus::VariantMap* packagekit = nullptr;
    const dbus::VariantMap* flatpak = nullptr;
    for (const auto& entry : backends)
        (entry.at("id").value<std::string>() == "packagekit" ? packagekit : flatpak) = &entry;
    ASSERT_TRUE(packagekit);
    ASSERT_TRUE(flatpak);
    EXPECT_EQ(packagekit->at("available").value<bool>(false), true);
    EXPECT_EQ(packagekit->at("formats").value<std::vector<std::string>>(), std::vector<std::string>{"deb"});
    EXPECT_EQ(flatpak->at("available").value<bool>(false), true);
    EXPECT_EQ(flatpak->at("formats").value<std::vector<std::string>>(), std::vector<std::string>{"flatpak"});
    EXPECT_EQ(flatpak->at("scopes").value<std::vector<std::string>>(),
              (std::vector<std::string>{"system", "user"}));
}

TEST_F(TwoAvailableBackends, SelectsTheBackendEachOfferNames)
{
    ASSERT_TRUE(spin([&] { return properties().has_value(); }));

    // Two offers of the same application, from two sources: each names its
    // own backend, and the selection must not fall back.
    auto deb = install("packagekit:gimp");
    ASSERT_TRUE(deb) << "Install(packagekit:gimp) was not answered";
    dbus::ObjectPath debPath;
    ASSERT_TRUE(deb->read(debPath));
    auto debTx = transaction(debPath.path);
    ASSERT_TRUE(debTx);
    EXPECT_EQ(debTx->at("Backend").value<std::string>(), "packagekit");

    auto flatpak = install("flatpak:user:flathub:app/org.gimp.GIMP/x86_64/stable");
    ASSERT_TRUE(flatpak);
    dbus::ObjectPath flatpakPath;
    ASSERT_TRUE(flatpak->read(flatpakPath));
    auto flatpakTx = transaction(flatpakPath.path);
    ASSERT_TRUE(flatpakTx);
    EXPECT_EQ(flatpakTx->at("Backend").value<std::string>(), "flatpak");
}

class OneAvailableOneUnavailable : public ServiceFixture
{
    std::vector<std::unique_ptr<Backend>> makeBackends() override
    {
        std::vector<std::unique_ptr<Backend>> backends;
        backends.push_back(std::make_unique<FakeBackend>(loop(), BackendKind::PackageKit));
        backends.push_back(std::make_unique<FakeBackend>(loop(), BackendKind::Flatpak,
                                                         Error::backend(BackendFailure::Unavailable, "no flatpak")));
        return backends;
    }
};

TEST_F(OneAvailableOneUnavailable, OneUnavailableBackendDoesNotDegradeTheService)
{
    std::optional<dbus::VariantMap> props;
    ASSERT_TRUE(spin([&] {
        props = properties();
        if (!props)
            return false;
        const auto it = props->find("State");
        return it != props->end() && it->second.get<std::string>().value_or("") == "ready";
    }));
    EXPECT_EQ(props->at("State").value<std::string>(), "ready");

    const auto backends = props->at("Backends").value<std::vector<dbus::VariantMap>>();
    ASSERT_EQ(backends.size(), 2u);
    for (const auto& entry : backends) {
        if (entry.at("id").value<std::string>() == "flatpak") {
            EXPECT_EQ(entry.at("available").value<bool>(true), false);
            EXPECT_EQ(entry.at("reason").value<std::string>(), "no flatpak");
        } else {
            EXPECT_EQ(entry.at("available").value<bool>(false), true);
        }
    }
}

TEST_F(OneAvailableOneUnavailable, AFlatpakOfferIsRefusedWhenFlatpakIsUnavailable)
{
    ASSERT_TRUE(spin([&] { return properties().has_value(); }));
    const auto reply = install("flatpak:user:flathub:app/org.gimp.GIMP/x86_64/stable");
    ASSERT_TRUE(reply);
    EXPECT_EQ(reply->isError() ? reply->error().name : std::string("<no error>"),
              "org.cutefish.Store1.Error.BackendUnavailable");
}

class AllUnavailable : public ServiceFixture
{
    std::vector<std::unique_ptr<Backend>> makeBackends() override
    {
        std::vector<std::unique_ptr<Backend>> backends;
        backends.push_back(std::make_unique<FakeBackend>(loop(), BackendKind::PackageKit,
                                                         Error::backend(BackendFailure::Unavailable, "no packagekit")));
        backends.push_back(std::make_unique<FakeBackend>(loop(), BackendKind::Flatpak,
                                                         Error::backend(BackendFailure::Unavailable, "no flatpak")));
        return backends;
    }
};

TEST_F(AllUnavailable, DegradesWhenEveryBackendIsUnavailable)
{
    std::optional<dbus::VariantMap> props;
    ASSERT_TRUE(spin([&] {
        props = properties();
        if (!props)
            return false;
        const auto it = props->find("State");
        return it != props->end() && it->second.get<std::string>().value_or("") == "degraded";
    }));
    EXPECT_EQ(props->at("State").value<std::string>(), "degraded");
}

} // namespace
