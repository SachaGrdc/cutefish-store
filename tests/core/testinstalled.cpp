#include "fixtures.h"

#include <cutefish/store/installed.h>

#include <gtest/gtest.h>

using namespace cutefish::store;
using fixtures::appId;
using fixtures::unwrap;

TEST(InstalledItem, DebPackage)
{
    auto r = InstalledItem::create({.offer = unwrap(OfferId::parse("packagekit:gimp")),
                                    .app = appId("org.gimp.GIMP"),
                                    .backend = BackendKind::PackageKit,
                                    .scope = Scope::System,
                                    .version = unwrap(Version::debian("3.0.4-3+deb13u11")),
                                    .origin = "debian-stable-security-main"});
    ASSERT_TRUE(r) << r.error().message();
    EXPECT_EQ(r->offer().str(), "packagekit:gimp");
    ASSERT_TRUE(r->app());
    EXPECT_EQ(r->app()->str(), "org.gimp.GIMP");
    EXPECT_EQ(r->version()->str(), "3.0.4-3+deb13u11");
}

TEST(InstalledItem, MayHaveNoApplication)
{
    // A runtime, or a package installed by hand that the catalogue does not
    // describe; prototype 4 saw packages from no repository (`local`).
    auto r = InstalledItem::create({.offer = unwrap(OfferId::parse("packagekit:cutefish-calculator")),
                                    .app = std::nullopt,
                                    .backend = BackendKind::PackageKit,
                                    .scope = Scope::System,
                                    .version = std::nullopt,
                                    .origin = "local"});
    ASSERT_TRUE(r);
    EXPECT_FALSE(r->app());
    EXPECT_FALSE(r->version());
}

TEST(InstalledItem, FlatpakInBothScopes)
{
    for (const Scope scope : {Scope::User, Scope::System}) {
        auto r = InstalledItem::create(
                {.offer = unwrap(OfferId::parse("flatpak:user:flathub:app/org.mozilla.firefox/x86_64/stable")),
                 .app = appId("org.mozilla.firefox"),
                 .backend = BackendKind::Flatpak,
                 .scope = scope,
                 .version = unwrap(Version::upstream("128.4.0")),
                 .origin = "flathub"});
        EXPECT_TRUE(r);
    }
}

TEST(InstalledItem, RejectsInconsistentItems)
{
    EXPECT_FALSE(InstalledItem::create({.offer = unwrap(OfferId::parse("packagekit:gimp")),
                                        .app = std::nullopt,
                                        .backend = BackendKind::PackageKit,
                                        .scope = Scope::User,
                                        .version = std::nullopt,
                                        .origin = {}}));
    EXPECT_FALSE(InstalledItem::create({.offer = unwrap(OfferId::parse("packagekit:gimp")),
                                        .app = std::nullopt,
                                        .backend = BackendKind::PackageKit,
                                        .scope = Scope::System,
                                        .version = unwrap(Version::upstream("3.0.4")),
                                        .origin = {}}));
    EXPECT_FALSE(InstalledItem::create({.offer = unwrap(OfferId::parse("flatpak:user:flathub:app/a.b/x86_64/stable")),
                                        .app = std::nullopt,
                                        .backend = BackendKind::Flatpak,
                                        .scope = Scope::User,
                                        .version = unwrap(Version::debian("1.0-1")),
                                        .origin = {}}));
    EXPECT_FALSE(InstalledItem::create({.offer = unwrap(OfferId::parse("packagekit:gimp")),
                                        .app = std::nullopt,
                                        .backend = BackendKind::PackageKit,
                                        .scope = Scope::System,
                                        .version = std::nullopt,
                                        .origin = "with space"}));
}
