#include "planfixtures.h"

#include <cutefish/store/confirmation.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <random>

using namespace cutefish::store;
using namespace planfixtures;

namespace {

using R = ConfirmationReason;

std::vector<ConfirmationConcern> concerns(std::initializer_list<ConfirmationConcern> list) { return list; }

} // namespace

// --- Confirmation rules (P-008) -------------------------------------------

TEST(ConfirmationRules, PlainUserInstallNeedsNothing)
{
    auto a = assessPlan(flatpakPlan(TransactionKind::Install, Scope::User,
                                    {flatpakApp(PlanOperation::Install), flatpakRuntime()}));
    EXPECT_FALSE(a.required());
    EXPECT_TRUE(a.concerns.empty());
}

TEST(ConfirmationRules, PermissionsOfAnInstallAreNew)
{
    auto app = flatpakApp(PlanOperation::Install);
    app.permissions = {"shared=network"};
    auto a = assessPlan(flatpakPlan(TransactionKind::Install, Scope::User, {app}));
    EXPECT_TRUE(a.reasons.contains(R::NewPermissions));
    EXPECT_EQ(a.concerns, concerns({{R::NewPermissions, 0}}));
}

TEST(ConfirmationRules, UpdateNeedsConfirmationOnlyForAddedPermissions)
{
    auto same = flatpakApp(PlanOperation::Update);
    same.permissions = same.oldPermissions = {"shared=network", "sockets=wayland"};
    EXPECT_FALSE(assessPlan(flatpakPlan(TransactionKind::Update, Scope::User, {same})).required());

    auto fewer = same;
    fewer.permissions = {"sockets=wayland"};
    EXPECT_FALSE(assessPlan(flatpakPlan(TransactionKind::Update, Scope::User, {fewer})).required());

    auto more = same;
    more.permissions.push_back("filesystems=home");
    auto a = assessPlan(flatpakPlan(TransactionKind::Update, Scope::User, {more}));
    EXPECT_EQ(a.reasons.list(), std::vector<R>{R::NewPermissions});
}

TEST(ConfirmationRules, SourceRebaseAndTrustFlags)
{
    auto app = flatpakApp(PlanOperation::Install);
    app.addsSource = true;
    auto rebased = flatpakApp(PlanOperation::Install, "app/org.example.New/x86_64/stable", false);
    rebased.rebaseFrom = "app/org.example.Old/x86_64/stable";
    auto runtime = flatpakRuntime();
    runtime.trusted = false;
    auto a = assessPlan(flatpakPlan(TransactionKind::Install, Scope::User, {app, rebased, runtime}));
    EXPECT_EQ(a.reasons.list(), (std::vector<R>{R::NewSource, R::Rebase, R::Untrusted}));
    EXPECT_EQ(a.concerns, concerns({{R::NewSource, 0}, {R::Rebase, 1}, {R::Untrusted, 2}}));
}

TEST(ConfirmationRules, CascadedRemovalsNeedConfirmation)
{
    // PackageKit ignores allow_deps=false and removes what depends on the
    // package (prototype 4): the extra removals are shown and confirmed.
    auto plain = assessPlan(debPlan(TransactionKind::Remove, {debPackage(PlanOperation::Remove, "gimp", true)}));
    EXPECT_FALSE(plain.required());

    auto cascade = assessPlan(debPlan(TransactionKind::Remove, {debPackage(PlanOperation::Remove, "gimp", true),
                                                                debPackage(PlanOperation::Remove, "gimp-plugin", false)}));
    EXPECT_EQ(cascade.reasons.list(), std::vector<R>{R::RemovesPackages});
    EXPECT_EQ(cascade.concerns, concerns({{R::RemovesPackages, 1}}));
}

TEST(ConfirmationRules, DistributionPackagesAreNotSharedScope)
{
    auto a = assessPlan(debPlan(TransactionKind::Install, {debPackage(PlanOperation::Install, "gimp", true),
                                                           debPackage(PlanOperation::Install, "libgimp", false)}));
    EXPECT_FALSE(a.required());

    // The policy is data: another host may decide otherwise.
    ConfirmationPolicy strict;
    strict.sharedSystemScope.insert(BackendKind::PackageKit);
    auto s = assessPlan(debPlan(TransactionKind::Install, {debPackage(PlanOperation::Install, "gimp", true)}), strict);
    EXPECT_EQ(s.reasons.list(), std::vector<R>{R::SystemScope});
}

TEST(ConfirmationRules, SystemFlatpakInstallAndRemove)
{
    auto install = assessPlan(flatpakPlan(TransactionKind::Install, Scope::System,
                                          {flatpakApp(PlanOperation::Install), flatpakRuntime()}));
    EXPECT_EQ(install.concerns, concerns({{R::SystemScope, 0}, {R::SystemScope, 1}}));

    auto remove = assessPlan(flatpakPlan(TransactionKind::Remove, Scope::System, {flatpakApp(PlanOperation::Remove)}));
    EXPECT_EQ(remove.reasons.list(), std::vector<R>{R::SystemScope});
}

TEST(ConfirmationRules, D014SystemUpdateWithoutNewRuntimeNeedsNothing)
{
    auto a = assessPlan(flatpakPlan(TransactionKind::Update, Scope::System,
                                    {flatpakApp(PlanOperation::Update), flatpakRuntime(PlanOperation::Update)}));
    EXPECT_FALSE(a.required());
}

TEST(ConfirmationRules, D014SystemUpdateInstallingARuntimeIsConfirmed)
{
    // An update that installs a new system runtime: never silent (D-014).
    // The concern points at the runtime, which the interface shows.
    auto a = assessPlan(flatpakPlan(TransactionKind::Update, Scope::System,
                                    {flatpakApp(PlanOperation::Update), flatpakRuntime(PlanOperation::Install)}));
    EXPECT_EQ(a.reasons.list(), std::vector<R>{R::SystemScope});
    EXPECT_EQ(a.concerns, concerns({{R::SystemScope, 1}}));
}

TEST(ConfirmationRules, D014UserUpdateInstallingARuntimeNeedsNothing)
{
    auto a = assessPlan(flatpakPlan(TransactionKind::Update, Scope::User,
                                    {flatpakApp(PlanOperation::Update), flatpakRuntime(PlanOperation::Install)}));
    EXPECT_FALSE(a.required());
}

TEST(ConfirmationRules, StableNames)
{
    EXPECT_EQ(toString(R::RemovesPackages), "removes-packages");
    EXPECT_EQ(toString(R::SystemScope), "system-scope");
    EXPECT_EQ(toString(R::PlanChanged), "plan-changed");
    EXPECT_EQ(toString(PlanChange::ItemSatisfied), "item-satisfied");
    ConfirmationReasons all;
    for (R r : {R::PlanChanged, R::RemovesPackages, R::Untrusted})
        all.insert(r);
    EXPECT_EQ(all.list(), (std::vector<R>{R::RemovesPackages, R::Untrusted, R::PlanChanged}));
}

// --- Plan comparison (P-008, second pass) ---------------------------------

namespace {

Plan systemUpdate(std::vector<PlanItemSpec> items)
{
    return flatpakPlan(TransactionKind::Update, Scope::System, std::move(items));
}

std::vector<PlanChange> changes(const PlanComparison& c)
{
    std::vector<PlanChange> out;
    for (const auto& d : c.differences)
        out.push_back(d.change);
    return out;
}

} // namespace

TEST(PlanComparison, IdenticalPlans)
{
    auto a = systemUpdate({flatpakApp(PlanOperation::Update), flatpakRuntime(PlanOperation::Install)});
    auto c = comparePlans(a, a);
    EXPECT_EQ(c.verdict, PlanVerdict::Identical);
    EXPECT_TRUE(c.differences.empty());
}

TEST(PlanComparison, RuntimeAppearingInSecondPassNeedsConfirmation)
{
    auto accepted = systemUpdate({flatpakApp(PlanOperation::Update)});
    auto fresh = systemUpdate({flatpakApp(PlanOperation::Update), flatpakRuntime(PlanOperation::Install)});
    auto c = comparePlans(accepted, fresh);
    EXPECT_EQ(c.verdict, PlanVerdict::NeedsConfirmation);
    ASSERT_EQ(c.differences.size(), 1u);
    EXPECT_EQ(c.differences[0], (PlanDifference{PlanChange::ItemAdded, kRuntime, true}));
}

TEST(PlanComparison, SatisfiedDependencyIsHarmless)
{
    // Prototype 1: a runtime installed by another process between the
    // passes must not stop the transaction.
    auto accepted = systemUpdate({flatpakApp(PlanOperation::Update), flatpakRuntime(PlanOperation::Install)});
    auto fresh = systemUpdate({flatpakApp(PlanOperation::Update)});
    auto c = comparePlans(accepted, fresh);
    EXPECT_EQ(c.verdict, PlanVerdict::Compatible);
    EXPECT_EQ(changes(c), std::vector<PlanChange>{PlanChange::ItemSatisfied});
}

TEST(PlanComparison, DisappearedRequestedItemOrRemovalIsNotHarmless)
{
    auto accepted = systemUpdate({flatpakApp(PlanOperation::Update),
                                  flatpakApp(PlanOperation::Update, "app/org.example.Other/x86_64/stable")});
    auto fresh = systemUpdate({flatpakApp(PlanOperation::Update)});
    EXPECT_EQ(comparePlans(accepted, fresh).verdict, PlanVerdict::NeedsConfirmation);

    auto cascade = debPlan(TransactionKind::Remove, {debPackage(PlanOperation::Remove, "gimp", true),
                                                     debPackage(PlanOperation::Remove, "gimp-plugin", false)});
    auto smaller = debPlan(TransactionKind::Remove, {debPackage(PlanOperation::Remove, "gimp", true)});
    auto c = comparePlans(cascade, smaller);
    EXPECT_EQ(c.verdict, PlanVerdict::NeedsConfirmation);
    EXPECT_EQ(changes(c), std::vector<PlanChange>{PlanChange::ItemDropped});
}

TEST(PlanComparison, SubstantialItemChanges)
{
    auto base = flatpakApp(PlanOperation::Update);
    auto check = [&](PlanItemSpec changed, PlanChange expected) {
        auto c = comparePlans(systemUpdate({base}), systemUpdate({std::move(changed)}));
        EXPECT_EQ(c.verdict, PlanVerdict::NeedsConfirmation) << toString(expected);
        EXPECT_EQ(changes(c), std::vector<PlanChange>{expected});
    };
    auto commit = base;
    commit.revision = "c0ffee02";
    check(commit, PlanChange::RevisionChanged);
    auto remote = base;
    remote.origin = "flathub-beta";
    check(remote, PlanChange::OriginChanged);
    auto version = base;
    version.toVersion = unwrap(Version::upstream("1.2"));
    check(version, PlanChange::VersionChanged);
    auto permission = base;
    permission.permissions = {"devices=all"};
    check(permission, PlanChange::PermissionsAdded);
    auto source = base;
    source.addsSource = true;
    check(source, PlanChange::SourceAdded);
    auto rebase = base;
    rebase.rebaseFrom = "app/org.example.Old/x86_64/stable";
    check(rebase, PlanChange::RebaseChanged);
    auto trust = base;
    trust.trusted = false;
    check(trust, PlanChange::TrustLost);
    auto kind = base;
    kind.kind = PlanItemKind::Runtime;
    check(kind, PlanChange::ItemKindChanged);

    auto accepted = debPlan(TransactionKind::Update, {debPackage(PlanOperation::Update, "gimp", true),
                                                      debPackage(PlanOperation::Update, "libgimp", false)});
    auto downgraded = debPlan(TransactionKind::Update, {debPackage(PlanOperation::Update, "gimp", true),
                                                        debPackage(PlanOperation::Downgrade, "libgimp", false)});
    EXPECT_EQ(changes(comparePlans(accepted, downgraded)), std::vector<PlanChange>{PlanChange::OperationChanged});
    auto nowRequested = debPlan(TransactionKind::Update, {debPackage(PlanOperation::Update, "gimp", true),
                                                          debPackage(PlanOperation::Update, "libgimp", true)});
    EXPECT_EQ(changes(comparePlans(accepted, nowRequested)), std::vector<PlanChange>{PlanChange::RequestChanged});
}

TEST(PlanComparison, HarmlessItemChanges)
{
    auto base = flatpakApp(PlanOperation::Update);
    base.permissions = {"shared=network", "sockets=wayland"};
    base.addsSource = true;
    base.trusted = false;

    auto fewer = base;
    fewer.permissions = {"sockets=wayland"};
    auto c = comparePlans(systemUpdate({base}), systemUpdate({fewer}));
    EXPECT_EQ(c.verdict, PlanVerdict::Compatible);
    EXPECT_EQ(changes(c), std::vector<PlanChange>{PlanChange::PermissionsReduced});

    auto safer = base;
    safer.addsSource = false;
    safer.trusted = true;
    EXPECT_EQ(changes(comparePlans(systemUpdate({base}), systemUpdate({safer}))),
              std::vector<PlanChange>{PlanChange::RiskReduced});

    auto bigger = base;
    bigger.downloadSize = 5000;
    c = comparePlans(systemUpdate({base}), systemUpdate({bigger}));
    EXPECT_EQ(c.verdict, PlanVerdict::Compatible);
    EXPECT_EQ(changes(c), std::vector<PlanChange>{PlanChange::SizeChanged});
}

TEST(PlanComparison, ATotalOnlyDownloadSizeIsCompared)
{
    // PackageKit gives the total only (PlanSpec::downloadSize).
    auto total = [](std::uint64_t bytes) {
        return unwrap(Plan::create({.kind = TransactionKind::Install,
                                    .backend = BackendKind::PackageKit,
                                    .scope = Scope::System,
                                    .items = {debPackage(PlanOperation::Install, "gimp", true)},
                                    .downloadSize = bytes}));
    };
    EXPECT_EQ(comparePlans(total(52458144), total(52458144)).verdict, PlanVerdict::Identical);
    const auto c = comparePlans(total(52458144), total(6500000)); // dependencies cached meanwhile
    EXPECT_EQ(c.verdict, PlanVerdict::Compatible);
    EXPECT_EQ(changes(c), std::vector<PlanChange>{PlanChange::SizeChanged});
    EXPECT_TRUE(mayExecute(total(52458144), total(6500000)));
}

TEST(PlanComparison, DifferentContextIsNeverCompared)
{
    auto user = flatpakPlan(TransactionKind::Update, Scope::User, {flatpakApp(PlanOperation::Update)});
    auto system = systemUpdate({flatpakApp(PlanOperation::Update)});
    auto c = comparePlans(user, system);
    EXPECT_EQ(c.verdict, PlanVerdict::NeedsConfirmation);
    EXPECT_EQ(changes(c), std::vector<PlanChange>{PlanChange::ContextChanged});
}

TEST(PlanComparison, DoesNotDependOnItemOrder)
{
    std::vector<PlanItemSpec> acceptedItems{flatpakApp(PlanOperation::Update),
                                            flatpakApp(PlanOperation::Update, "app/org.example.B/x86_64/stable"),
                                            flatpakRuntime(PlanOperation::Install),
                                            flatpakRuntime(PlanOperation::Update, "runtime/org.example.R/x86_64/1")};
    auto freshItems = acceptedItems;
    freshItems[1].revision = "changed";
    freshItems.erase(freshItems.begin() + 2); // satisfied runtime
    freshItems.push_back(flatpakRuntime(PlanOperation::Install, "runtime/org.example.New/x86_64/1"));

    const auto reference = comparePlans(systemUpdate(acceptedItems), systemUpdate(freshItems));
    EXPECT_EQ(reference.verdict, PlanVerdict::NeedsConfirmation);
    ASSERT_EQ(reference.differences.size(), 3u);

    std::mt19937 rng(14);
    for (int i = 0; i < 50; ++i) {
        auto a = acceptedItems;
        auto f = freshItems;
        std::shuffle(a.begin(), a.end(), rng);
        std::shuffle(f.begin(), f.end(), rng);
        auto c = comparePlans(systemUpdate(a), systemUpdate(f));
        ASSERT_EQ(c.verdict, reference.verdict);
        ASSERT_EQ(c.differences, reference.differences);
    }
}
