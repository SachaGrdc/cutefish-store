// PackageKitBackend against the system's real packagekitd (opt-in, step F).
//
// Queries and simulations ONLY: GetPackages, GetRepoList, GetUpdates,
// Resolve, and Install/Remove/UpdatePackages with the SIMULATE flag, which
// change nothing and ask polkit nothing. These tests never call execute(),
// reattach() or refresh() (no RefreshCache), never set interactive=true,
// and never change the system.
//
// Not part of ctest. Run by hand, on a Debian system with PackageKit:
//
//   CUTEFISH_STORE_REAL_PACKAGEKIT=1 build/tests/service/store-pkbackend-real-tests
//
// Optional: CUTEFISH_STORE_REAL_INSTALL (a package available but not
// installed, default `hello`), CUTEFISH_STORE_REAL_REMOVE (an installed
// package, default `nano`), CUTEFISH_STORE_REAL_PROTECTED (an installed
// protected package, default `cutefish-desktop`; skipped when absent).

#include "pkbackend.h"

#include <cutefish/store/confirmation.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <iostream>

using namespace cutefish::store;
using namespace cutefish::store::service;
using namespace std::chrono_literals;

namespace {

std::string env(const char* name, const char* fallback)
{
    const char* v = std::getenv(name);
    return v && *v ? v : fallback;
}

PlanRequest request(TransactionKind kind, const std::string& name)
{
    auto offer = OfferId::parse("packagekit:" + name);
    EXPECT_TRUE(offer);
    auto r = PlanRequest::create(kind, {std::move(offer).value()});
    EXPECT_TRUE(r);
    return std::move(r).value();
}

class RealPackageKit : public ::testing::Test
{
protected:
    void SetUp() override
    {
        if (env("CUTEFISH_STORE_REAL_PACKAGEKIT", "") != "1")
            GTEST_SKIP() << "set CUTEFISH_STORE_REAL_PACKAGEKIT=1 to run against the system's packagekitd";
        backend = std::make_unique<PackageKitBackend>(
                loop, PackageKitBackend::Options{.bus = PkTransport::Bus::System, .swcatalog = "/var/lib/swcatalog"});
        auto ok = wait<void>([&](Completion<void> d) { backend->availability(std::move(d)); });
        ASSERT_TRUE(ok && *ok) << (ok ? ok->error().message() : "no answer");
    }

    template <typename T>
    std::optional<Result<T>> wait(const std::function<void(Completion<T>)>& start, std::chrono::seconds timeout = 120s)
    {
        std::optional<Result<T>> out;
        start([&](Result<T> r) { out.emplace(std::move(r)); });
        const auto end = std::chrono::steady_clock::now() + timeout;
        while (!out && std::chrono::steady_clock::now() < end) {
            const int t = loop.startTimer(20ms, false, [this] { loop.quit(0); });
            loop.run();
            loop.stopTimer(t);
        }
        return out;
    }

    std::optional<Result<Plan>> plan(TransactionKind kind, const std::string& name)
    {
        return wait<Plan>([&](Completion<Plan> d) { backend->plan("real-" + name, request(kind, name), std::move(d)); });
    }

    static void print(const Plan& p)
    {
        std::cout << "  plan " << toString(p.kind()) << ", " << p.items().size() << " items, download "
                  << p.downloadSize() << " bytes\n";
        for (const PlanItem& i : p.items())
            std::cout << "    " << toString(i.operation()) << " " << i.package() << " "
                      << (i.fromVersion() ? i.fromVersion()->str() : "-") << " -> "
                      << (i.toVersion() ? i.toVersion()->str() : "-") << " [" << i.origin() << "]"
                      << (i.requested() ? " requested" : "") << "\n";
    }

    cutefish::base::EventLoop loop;
    std::unique_ptr<PackageKitBackend> backend;
};

} // namespace

TEST_F(RealPackageKit, InstalledPackages)
{
    auto r = wait<std::vector<InstalledItem>>([&](auto d) { backend->queryInstalled(std::move(d)); });
    ASSERT_TRUE(r && *r) << r->error().message();
    std::cout << "  " << (*r)->size() << " installed packages (native architecture and all)\n";
    EXPECT_GT((*r)->size(), 100u);
    const auto dpkg = std::find_if((*r)->begin(), (*r)->end(),
                                   [](const InstalledItem& i) { return i.offer().str() == "packagekit:dpkg"; });
    ASSERT_NE(dpkg, (*r)->end());
    EXPECT_TRUE(dpkg->version());
    EXPECT_EQ(dpkg->scope(), Scope::System);
}

TEST_F(RealPackageKit, RepositoriesAndCatalogueSources)
{
    auto r = wait<std::vector<Source>>([&](auto d) { backend->sources(std::move(d)); });
    ASSERT_TRUE(r && *r) << r->error().message();
    EXPECT_FALSE((*r)->empty());
    for (const Source& s : **r)
        std::cout << "  repository " << s.id().str() << " " << toString(s.kind()) << (s.enabled() ? "" : " disabled")
                  << "\n";
    EXPECT_TRUE(std::any_of((*r)->begin(), (*r)->end(), [](const Source& s) { return s.kind() == SourceKind::Distribution; }));

    auto c = wait<std::vector<SourceMetadata>>([&](auto d) { backend->catalogSources(std::move(d)); });
    ASSERT_TRUE(c && *c);
    for (const SourceMetadata& m : **c)
        std::cout << "  catalogue " << m.source.id().str() << " " << m.files.metadata.string() << " icons "
                  << (m.files.icons.empty() ? "<none found>" : m.files.icons.string()) << "\n";
    EXPECT_TRUE(std::any_of((*c)->begin(), (*c)->end(),
                            [](const SourceMetadata& m) { return m.source.kind() == SourceKind::Distribution; }));
}

TEST_F(RealPackageKit, UpdatesAreNotAtTheInstalledVersion)
{
    auto r = wait<std::vector<AvailableUpdate>>([&](auto d) { backend->queryUpdates(std::move(d)); });
    ASSERT_TRUE(r && *r) << r->error().message();
    std::cout << "  " << (*r)->size() << " updates seen by PackageKit (before the catalogue's filter)\n";
    for (const AvailableUpdate& u : **r) {
        ASSERT_TRUE(u.toVersion);
        if (u.fromVersion) {
            EXPECT_NE(u.fromVersion->str(), u.toVersion->str()) << u.offer.str();
        }
    }
}

TEST_F(RealPackageKit, SimulatedInstall)
{
    const std::string name = env("CUTEFISH_STORE_REAL_INSTALL", "hello");
    auto r = plan(TransactionKind::Install, name);
    ASSERT_TRUE(r && *r) << r->error().message();
    print(**r);
    const PlanItem* item = (*r)->find(name);
    ASSERT_TRUE(item);
    EXPECT_TRUE(item->requested());
    EXPECT_EQ(item->operation(), PlanOperation::Install);
    EXPECT_TRUE(item->toVersion());
    EXPECT_FALSE(item->origin().empty());
    EXPECT_TRUE(checkPlanForRequest(request(TransactionKind::Install, name), **r));
    // A second simulation, as execute() runs before any change, gives a plan
    // that may be executed (nothing changed in between).
    auto again = plan(TransactionKind::Install, name);
    ASSERT_TRUE(again && *again);
    EXPECT_TRUE(mayExecute(**r, **again));
}

TEST_F(RealPackageKit, SimulatedRemoval)
{
    const std::string name = env("CUTEFISH_STORE_REAL_REMOVE", "nano");
    auto r = plan(TransactionKind::Remove, name);
    ASSERT_TRUE(r) << "no answer";
    if (!*r) {
        // Refused by the profile (a protected package in the cascade) is
        // also a valid outcome; anything else is a failure.
        std::cout << "  refused: " << r->error().message() << "\n";
        EXPECT_NE(r->error().message().find("protected"), std::string::npos) << r->error().message();
        return;
    }
    print(**r);
    const PlanItem* item = (*r)->find(name);
    ASSERT_TRUE(item);
    EXPECT_EQ(item->operation(), PlanOperation::Remove);
    EXPECT_TRUE(item->requested());
}

TEST_F(RealPackageKit, AProtectedPackageIsNeverPlannedForRemoval)
{
    const std::string name = env("CUTEFISH_STORE_REAL_PROTECTED", "cutefish-desktop");
    auto installed = wait<std::vector<InstalledItem>>([&](auto d) { backend->queryInstalled(std::move(d)); });
    ASSERT_TRUE(installed && *installed);
    if (std::none_of((*installed)->begin(), (*installed)->end(),
                     [&](const InstalledItem& i) { return i.offer().str() == "packagekit:" + name; }))
        GTEST_SKIP() << name << " is not installed";
    auto r = plan(TransactionKind::Remove, name);
    ASSERT_TRUE(r);
    ASSERT_FALSE(*r);
    std::cout << "  refused: " << r->error().message() << "\n";
    EXPECT_NE(r->error().message().find(name), std::string::npos);
}

TEST_F(RealPackageKit, SimulatedUpdateOfOnePackage)
{
    auto updates = wait<std::vector<AvailableUpdate>>([&](auto d) { backend->queryUpdates(std::move(d)); });
    ASSERT_TRUE(updates && *updates);
    if ((*updates)->empty())
        GTEST_SKIP() << "no update available on this system";
    const std::string name = (*updates)->front().offer.str().substr(std::string("packagekit:").size());
    auto r = plan(TransactionKind::Update, name);
    ASSERT_TRUE(r) << "no answer";
    ASSERT_TRUE(*r) << r->error().message();
    print(**r);
    const PlanItem* item = (*r)->find(name);
    ASSERT_TRUE(item);
    EXPECT_TRUE(item->requested());
}

TEST_F(RealPackageKit, AnUnknownPackageIsNotFound)
{
    auto r = plan(TransactionKind::Install, "cutefish-store-no-such-package");
    ASSERT_TRUE(r);
    ASSERT_FALSE(*r);
    EXPECT_EQ(r->error().code(), ErrorCode::NotFound) << r->error().message();
}
