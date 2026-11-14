// Coexistence with the system updates of Cutefish Settings (H.2, SERVICE.md
// §22), through the service's D-Bus API: the service in-process with its
// real PackageKit backend against a simulated PackageKit (fakepackagekit.h)
// and a scripted Flatpak backend, on a private bus. "Another program" (the
// SoftwareUpdate service of cutefish-core, which drives packagekitd itself)
// is played by the fake's startExternal(). No real PackageKit, APT or
// Cutefish service is touched.

#include "fakebackend.h"
#include "fakepackagekit.h"
#include "pkbackend.h"
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
#include <vector>

using namespace cutefish::store;
using namespace cutefish::store::service;
using namespace cutefish::store::service::testutil;
namespace base = cutefish::base;
namespace dbus = cutefish::base::dbus;
namespace pk = cutefish::store::packagekit;
using namespace std::chrono_literals;

namespace {

constexpr const char* kGimpFlathub = "flatpak:user:flathub:app/org.gimp.GIMP/x86_64/stable";

class Coexistence : public ::testing::Test
{
protected:
    void SetUp() override
    {
        pk::PkPackage gimp{static_cast<std::uint32_t>(pk::Info::Available),
                           "gimp;3.0.4-3+deb13u11;amd64;debian-stable-main", "GIMP"};
        fakepk::Answer resolve;
        resolve.packages = {gimp};
        m_fake.answer("Resolve", resolve);
        catalogfixture::build(m_dir / "catalog.sqlite");
        m_conn = dbus::Connection::open(dbus::Connection::Bus::Session, &m_loop);
        ASSERT_TRUE(m_conn);
        ServiceOptions options;
        options.dataDir = m_dir.path();
        options.sourcesFile = m_dir / "sources.conf";
        options.indexer = "cutefish-store-indexer";
        options.readLocales = {"C"};
        options.busName = "org.cutefish.Store";
        std::vector<std::unique_ptr<Backend>> backends;
        backends.push_back(std::make_unique<PackageKitBackend>(
                m_loop, PackageKitBackend::Options{.bus = PkTransport::Bus::Session,
                                                   .swcatalog = m_dir / "swcatalog",
                                                   .changeDelay = 100ms}));
        auto flatpak = std::make_unique<FakeBackend>(m_loop, BackendKind::Flatpak);
        m_flatpak = flatpak.get();
        backends.push_back(std::move(flatpak));
        m_service = std::make_unique<StoreService>(m_loop, *m_conn, std::move(options), std::move(backends));
        ASSERT_TRUE(m_service->start());
        ASSERT_TRUE(spin([&] {
            const auto b = backends_();
            return b.size() == 2 && b[0].at("available").value<bool>(false) && b[1].at("available").value<bool>(false)
                    && state() == "ready";
        })) << "the service did not become ready";
        m_matches.push_back(m_conn->subscribe(rule("org.freedesktop.DBus.Properties", "PropertiesChanged"),
                                              [this](const dbus::Message& m) {
                                                  std::string iface;
                                                  dbus::VariantMap changed;
                                                  std::vector<std::string> invalidated;
                                                  if (m.path() == "/org/cutefish/Store"
                                                      && m.read(iface, changed, invalidated)
                                                      && changed.contains("Backends"))
                                                      ++m_backendsChanges;
                                              }));
        m_matches.push_back(m_conn->subscribe(rule("org.cutefish.Store1", "TransactionAdded"),
                                              [this](const dbus::Message&) { ++m_added; }));
    }

    static dbus::MatchRule rule(const char* interface, const char* member)
    {
        dbus::MatchRule r;
        r.interface = interface;
        r.member = member;
        return r;
    }

    bool spin(const std::function<bool()>& pred, std::chrono::milliseconds timeout = 10s)
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

    std::optional<dbus::VariantMap> getAll(const std::string& path, const char* interface)
    {
        auto out = std::make_shared<std::optional<dbus::VariantMap>>();
        auto done = std::make_shared<bool>(false);
        std::vector<dbus::PendingCall> calls;
        calls.push_back(dbus::Proxy(m_conn.get(), "org.cutefish.Store", path, interface)
                                .getAllAsync([out, done](const dbus::Reply& r) {
                                    *out = r.value<dbus::VariantMap>();
                                    *done = true;
                                }));
        spin([&] { return *done; });
        return *out;
    }

    std::vector<dbus::VariantMap> backends_()
    {
        auto p = getAll("/org/cutefish/Store", "org.cutefish.Store1");
        if (!p || !p->contains("Backends"))
            return {};
        return p->at("Backends").value<std::vector<dbus::VariantMap>>();
    }

    std::string state()
    {
        auto p = getAll("/org/cutefish/Store", "org.cutefish.Store1");
        return p && p->contains("State") ? p->at("State").value<std::string>() : std::string();
    }

    dbus::VariantMap backend(const std::string& id)
    {
        for (auto& b : backends_())
            if (b.at("id").value<std::string>() == id)
                return b;
        return {};
    }

    std::string activity(const std::string& id)
    {
        const auto b = backend(id);
        return b.contains("activity") ? b.at("activity").value<std::string>() : std::string();
    }

    /// Install(offer): the transaction's path, or empty.
    std::string install(const std::string& offer)
    {
        auto path = std::make_shared<std::string>();
        auto done = std::make_shared<bool>(false);
        std::vector<dbus::PendingCall> calls;
        calls.push_back(dbus::Proxy(m_conn.get(), "org.cutefish.Store", "/org/cutefish/Store", "org.cutefish.Store1")
                                .callAsync("Install", [path, done](const dbus::Reply& r) {
                                    dbus::ObjectPath p;
                                    if (!r.isError() && r.read(p))
                                        *path = p.path;
                                    *done = true;
                                }, offer, dbus::VariantMap{}));
        spin([&] { return *done; });
        return *path;
    }

    /// The transaction once finished: its State and Error code.
    std::pair<std::string, std::string> outcome(const std::string& path)
    {
        std::pair<std::string, std::string> out;
        spin([&] {
            auto p = getAll(path, "org.cutefish.Store1.Transaction");
            if (!p)
                return false;
            out.first = p->at("State").value<std::string>();
            const auto error = p->at("Error").value<dbus::VariantMap>();
            out.second = error.contains("code") ? error.at("code").value<std::string>() : std::string();
            return out.first == "failed" || out.first == "succeeded" || out.first == "cancelled"
                    || out.first == "awaiting-confirmation";
        });
        return out;
    }

    int resolves()
    {
        int n = 0;
        for (const auto& c : m_fake.calls())
            n += c.method == "Resolve";
        return n;
    }

    privatebus::PrivateBus m_bus;
    fakepk::FakePackageKit m_fake;
    base::EventLoop m_loop;
    std::unique_ptr<dbus::Connection> m_conn;
    catalogfixture::TempDir m_dir;
    FakeBackend* m_flatpak = nullptr;
    std::unique_ptr<StoreService> m_service;
    std::vector<dbus::Match> m_matches;
    int m_backendsChanges = 0;
    int m_added = 0;
};

fakepk::Answer held()
{
    fakepk::Answer a;
    a.hold = true;
    a.steps = {{pk::Status::Download, 20, true}, {pk::Status::Install, 60, false}};
    return a;
}

TEST_F(Coexistence, IdlePackageKitIsReported)
{
    EXPECT_EQ(activity("packagekit"), "none");
    EXPECT_EQ(activity("flatpak"), "none");
    EXPECT_FALSE(backend("packagekit").contains("activity-role"));
}

TEST_F(Coexistence, ASystemUpdateFromSettingsHoldsThePackageKitActionsOnly)
{
    // Settings → SoftwareUpdate.update() → UpdatePackages in packagekitd.
    const std::string update = m_fake.startExternal("UpdatePackages", held());
    ASSERT_TRUE(spin([&] { return activity("packagekit") == "changing"; }));
    EXPECT_EQ(backend("packagekit").at("activity-role").value<std::string>(), "update-packages");
    EXPECT_TRUE(backend("packagekit").at("available").value<bool>(false)); // busy, not unavailable
    EXPECT_EQ(activity("flatpak"), "none");
    EXPECT_EQ(state(), "ready");
    EXPECT_EQ(m_backendsChanges, 1);

    // A deb install: refused with a temporary code, nothing sent to PackageKit.
    const int before = resolves();
    const std::string deb = install("packagekit:gimp");
    ASSERT_FALSE(deb.empty());
    EXPECT_EQ(outcome(deb), (std::pair<std::string, std::string>{"failed", "locked"}));
    EXPECT_EQ(resolves(), before);

    // A Flatpak install goes on to its own backend (whose fake plan then
    // refuses with not-supported: what matters is that it was asked).
    const std::string flatpak = install(kGimpFlathub);
    ASSERT_FALSE(flatpak.empty());
    const auto fp = outcome(flatpak);
    EXPECT_NE(fp.second, "locked");
    EXPECT_EQ(m_flatpak->planCalls(), 1);
    EXPECT_EQ(m_added, 2); // one transaction each, none launched twice

    // The update ends: PackageKit is usable again; the user retries.
    ASSERT_TRUE(m_fake.finish(update));
    ASSERT_TRUE(spin([&] { return activity("packagekit") == "none"; }));
    EXPECT_EQ(m_backendsChanges, 2);
    const std::string retry = install("packagekit:gimp");
    ASSERT_FALSE(retry.empty());
    ASSERT_TRUE(spin([&] { return resolves() > before; })); // this time it reached PackageKit
    EXPECT_NE(outcome(retry).second, "locked");
    // The store never cancelled the system update.
    EXPECT_EQ(m_fake.cancels(), 0);
    EXPECT_EQ(state(), "ready");
}

TEST_F(Coexistence, ACheckForUpdatesFromSettingsHoldsNothing)
{
    // SoftwareUpdate.check(): RefreshCache, then GetUpdates.
    const std::string refresh = m_fake.startExternal("RefreshCache", held(), 0);
    ASSERT_TRUE(spin([&] { return activity("packagekit") == "refreshing"; }));
    const int before = resolves();
    const std::string deb = install("packagekit:gimp");
    ASSERT_FALSE(deb.empty());
    ASSERT_TRUE(spin([&] { return resolves() > before; })); // sent to PackageKit, which queues it
    EXPECT_NE(outcome(deb).second, "locked");
    ASSERT_TRUE(m_fake.finish(refresh));
    const std::string check = m_fake.startExternal("GetUpdates", held(), pk::filter::kNone);
    ASSERT_TRUE(spin([&] { return activity("packagekit") == "checking"; }));
    ASSERT_TRUE(m_fake.finish(check));
    ASSERT_TRUE(spin([&] { return activity("packagekit") == "none"; }));
}

} // namespace
