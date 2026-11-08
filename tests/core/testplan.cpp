#include "planfixtures.h"

#include <cutefish/store/plan.h>

#include <gtest/gtest.h>

using namespace cutefish::store;
using namespace planfixtures;

namespace {

Result<Plan> tryPlan(TransactionKind kind, BackendKind backend, Scope scope, std::vector<PlanItemSpec> items)
{
    return Plan::create({.kind = kind, .backend = backend, .scope = scope, .items = std::move(items)});
}

void expectInvalid(const Result<Plan>& r)
{
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code(), ErrorCode::InvalidArgument);
}

} // namespace

TEST(Plan, KeepsItemsInBackendOrder)
{
    auto plan = flatpakPlan(TransactionKind::Install, Scope::User,
                            {flatpakApp(PlanOperation::Install), flatpakRuntime()});
    EXPECT_EQ(plan.kind(), TransactionKind::Install);
    EXPECT_EQ(plan.backend(), BackendKind::Flatpak);
    EXPECT_EQ(plan.scope(), Scope::User);
    ASSERT_EQ(plan.items().size(), 2u);
    EXPECT_EQ(plan.items()[0].package(), kApp);
    EXPECT_EQ(plan.items()[1].kind(), PlanItemKind::Runtime);
    EXPECT_FALSE(plan.items()[1].requested());
    EXPECT_EQ(plan.downloadSize(), 201000u);
    // A backend's total, when it has only that, replaces the sum.
    auto total = unwrap(Plan::create({.kind = TransactionKind::Install,
                                      .backend = BackendKind::Flatpak,
                                      .scope = Scope::User,
                                      .items = {flatpakApp(PlanOperation::Install), flatpakRuntime()},
                                      .downloadSize = 7}));
    EXPECT_EQ(total.downloadSize(), 7u);
    ASSERT_NE(plan.find(kRuntime), nullptr);
    EXPECT_EQ(plan.find(kRuntime)->revision(), "beef0001");
    EXPECT_EQ(plan.find("runtime/org.example.Missing/x86_64/1"), nullptr);
}

TEST(Plan, NormalisesPermissions)
{
    auto item = flatpakApp(PlanOperation::Update);
    item.permissions = {"sockets=wayland", "shared=network", "sockets=wayland", "filesystems=xdg-download"};
    item.oldPermissions = {"sockets=wayland", "filesystems=xdg-download"};
    auto plan = flatpakPlan(TransactionKind::Update, Scope::User, {item});
    const PlanItem& p = plan.items()[0];
    ASSERT_EQ(p.permissions().size(), 3u);
    EXPECT_EQ(p.permissions()[0], "filesystems=xdg-download");
    EXPECT_EQ(p.permissions()[1], "shared=network");
    EXPECT_EQ(p.permissions()[2], "sockets=wayland");
    EXPECT_EQ(p.addedPermissions(), std::vector<std::string>{"shared=network"});
}

TEST(Plan, RejectsEmptyOrUnrequestedPlans)
{
    expectInvalid(tryPlan(TransactionKind::Install, BackendKind::Flatpak, Scope::User, {}));
    expectInvalid(tryPlan(TransactionKind::Install, BackendKind::Flatpak, Scope::User, {flatpakRuntime()}));
}

TEST(Plan, RequestedItemsDoWhatTheTransactionAsks)
{
    expectInvalid(tryPlan(TransactionKind::Update, BackendKind::Flatpak, Scope::User,
                          {flatpakApp(PlanOperation::Install)}));
    expectInvalid(tryPlan(TransactionKind::Remove, BackendKind::PackageKit, Scope::System,
                          {debPackage(PlanOperation::Update, "gimp", true)}));
    // Added items may do anything: an update can install a runtime (D-014)
    // or downgrade a dependency.
    EXPECT_TRUE(tryPlan(TransactionKind::Update, BackendKind::PackageKit, Scope::System,
                        {debPackage(PlanOperation::Update, "gimp", true),
                         debPackage(PlanOperation::Downgrade, "libgimp", false),
                         debPackage(PlanOperation::Install, "libnew", false)}));
}

TEST(Plan, RejectsDuplicatesAndMalformedNames)
{
    expectInvalid(tryPlan(TransactionKind::Install, BackendKind::Flatpak, Scope::User,
                          {flatpakApp(PlanOperation::Install), flatpakApp(PlanOperation::Install, kApp, false)}));
    expectInvalid(tryPlan(TransactionKind::Install, BackendKind::PackageKit, Scope::System,
                          {debPackage(PlanOperation::Install, "has space", true)}));
    auto badOrigin = flatpakApp(PlanOperation::Install);
    badOrigin.origin = "two words";
    expectInvalid(tryPlan(TransactionKind::Install, BackendKind::Flatpak, Scope::User, {badOrigin}));
    auto badPermission = flatpakApp(PlanOperation::Install);
    badPermission.permissions = {"shared=network\n"};
    expectInvalid(tryPlan(TransactionKind::Install, BackendKind::Flatpak, Scope::User, {badPermission}));
}

TEST(Plan, ChecksVersionsAgainstOperationAndBackend)
{
    auto removal = debPackage(PlanOperation::Remove, "gimp", true);
    removal.toVersion = unwrap(Version::debian("1.0-2"));
    expectInvalid(tryPlan(TransactionKind::Remove, BackendKind::PackageKit, Scope::System, {removal}));

    auto install = debPackage(PlanOperation::Install, "gimp", true);
    install.fromVersion = unwrap(Version::debian("1.0-1"));
    expectInvalid(tryPlan(TransactionKind::Install, BackendKind::PackageKit, Scope::System, {install}));

    // A deb plan with an upstream version, a Flatpak plan with a Debian one.
    auto upstreamDeb = debPackage(PlanOperation::Install, "gimp", true);
    upstreamDeb.toVersion = unwrap(Version::upstream("3.0"));
    expectInvalid(tryPlan(TransactionKind::Install, BackendKind::PackageKit, Scope::System, {upstreamDeb}));
    auto debianFlatpak = flatpakApp(PlanOperation::Install);
    debianFlatpak.toVersion = unwrap(Version::debian("1.0-1"));
    expectInvalid(tryPlan(TransactionKind::Install, BackendKind::Flatpak, Scope::User, {debianFlatpak}));
}

TEST(Plan, PackageKitIsSystemWide)
{
    expectInvalid(tryPlan(TransactionKind::Install, BackendKind::PackageKit, Scope::User,
                          {debPackage(PlanOperation::Install, "gimp", true)}));
}

TEST(Plan, StableNames)
{
    EXPECT_EQ(toString(TransactionKind::Update), "update");
    EXPECT_EQ(toString(PlanOperation::Downgrade), "downgrade");
    EXPECT_EQ(toString(PlanItemKind::Runtime), "runtime");
}
