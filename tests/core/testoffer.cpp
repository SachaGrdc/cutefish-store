#include "fixtures.h"

#include <gtest/gtest.h>

using namespace cutefish::store;
using fixtures::unwrap;

TEST(Offer, DebOfferFromPackageKit)
{
    const Offer o = fixtures::debOffer("firefox-esr");
    EXPECT_EQ(o.id().str(), "packagekit:firefox-esr");
    EXPECT_EQ(o.format(), PackageFormat::Deb);
    EXPECT_EQ(o.backend(), BackendKind::PackageKit);
    EXPECT_EQ(o.scope(), Scope::System);
    EXPECT_EQ(o.source().str(), "packagekit:debian-stable-main");
    EXPECT_EQ(o.sourceKind(), SourceKind::Distribution);
    EXPECT_EQ(o.origin(), "debian-stable-main");
    EXPECT_EQ(o.package(), "firefox-esr");
    ASSERT_TRUE(o.version());
    EXPECT_EQ(o.version()->scheme(), VersionScheme::Debian);
    EXPECT_EQ(o.architecture(), ""); // resolved by PackageKit when planning
}

TEST(Offer, FlatpakOfferKeepsScopeRemoteAndRef)
{
    const Offer user = fixtures::flatpakOffer("app/org.mozilla.firefox/x86_64/stable", Scope::User);
    EXPECT_EQ(user.id().str(), "flatpak:user:flathub:app/org.mozilla.firefox/x86_64/stable");
    EXPECT_EQ(user.format(), PackageFormat::Flatpak);
    EXPECT_EQ(user.backend(), BackendKind::Flatpak);
    EXPECT_EQ(user.architecture(), "x86_64");
    EXPECT_EQ(user.sourceKind(), SourceKind::Flathub);

    const Offer system = fixtures::flatpakOffer("app/org.mozilla.firefox/x86_64/stable", Scope::System);
    EXPECT_EQ(system.id().str(), "flatpak:system:flathub:app/org.mozilla.firefox/x86_64/stable");
    EXPECT_NE(user.id(), system.id());
}

TEST(Offer, BranchesAreDistinctOffers)
{
    // Prototype 3: org.qgis.qgis is published on two branches.
    const Offer lts = fixtures::flatpakOffer("app/org.qgis.qgis/x86_64/lts");
    const Offer stable = fixtures::flatpakOffer("app/org.qgis.qgis/x86_64/stable");
    EXPECT_NE(lts.id(), stable.id());
}

TEST(Offer, AcceptsRuntimes)
{
    EXPECT_TRUE(Offer::create({PackageFormat::Flatpak, "runtime/org.gnome.Platform/x86_64/47", std::nullopt, {}},
                              fixtures::flathubSource()));
}

TEST(Offer, FormatMustMatchTheSourceBackend)
{
    auto debFromFlathub = Offer::create({PackageFormat::Deb, "firefox-esr", std::nullopt, {}}, fixtures::flathubSource());
    ASSERT_FALSE(debFromFlathub);
    EXPECT_EQ(debFromFlathub.error().code(), ErrorCode::InvalidArgument);
    EXPECT_FALSE(Offer::create({PackageFormat::Flatpak, "app/org.mozilla.firefox/x86_64/stable", std::nullopt, {}},
                               fixtures::debianSource()));
}

TEST(Offer, VersionSchemeMustMatchTheFormat)
{
    EXPECT_FALSE(Offer::create({PackageFormat::Deb, "firefox-esr", unwrap(Version::upstream("128.4")), {}},
                               fixtures::debianSource()));
    EXPECT_FALSE(Offer::create({PackageFormat::Flatpak, "app/org.mozilla.firefox/x86_64/stable",
                                unwrap(Version::debian("128.4-1")), {}},
                               fixtures::flathubSource()));
}

TEST(Offer, ValidatesDebPackageNames)
{
    for (const char* name : {"gimp", "libstdc++6", "g++", "0ad", "python3.13", "r-base-core"})
        EXPECT_TRUE(Offer::create({PackageFormat::Deb, name, std::nullopt, {}}, fixtures::debianSource())) << name;
    for (const char* name : {"", "a", "Firefox", "-dash", "fire fox", "firefox_esr", "+plus"})
        EXPECT_FALSE(Offer::create({PackageFormat::Deb, name, std::nullopt, {}}, fixtures::debianSource())) << name;
}

TEST(Offer, ValidatesDebArchitecture)
{
    EXPECT_TRUE(Offer::create({PackageFormat::Deb, "libc6", std::nullopt, "i386"}, fixtures::debianSource()));
    EXPECT_TRUE(Offer::create({PackageFormat::Deb, "fonts-noto", std::nullopt, "all"}, fixtures::debianSource()));
    EXPECT_FALSE(Offer::create({PackageFormat::Deb, "libc6", std::nullopt, "AMD64"}, fixtures::debianSource()));
}

TEST(Offer, ValidatesFlatpakRefs)
{
    for (const char* ref : {"org.mozilla.firefox", "app/org.mozilla.firefox", "app/org.mozilla.firefox/x86_64",
                            "app/org.mozilla.firefox/x86_64/", "extension/org.x/x86_64/stable",
                            "app//x86_64/stable", "app/org.mozilla.firefox/x86_64/stable/extra",
                            "app/org.mozilla.firefox/X86/stable", "app/org.mozilla.firefox/x86_64/st able"}) {
        EXPECT_FALSE(Offer::create({PackageFormat::Flatpak, ref, std::nullopt, {}}, fixtures::flathubSource())) << ref;
    }
}

TEST(Offer, FlatpakArchitectureMustMatchTheRef)
{
    EXPECT_TRUE(Offer::create({PackageFormat::Flatpak, "app/org.x.y/aarch64/stable", std::nullopt, "aarch64"},
                              fixtures::flathubSource()));
    EXPECT_FALSE(Offer::create({PackageFormat::Flatpak, "app/org.x.y/aarch64/stable", std::nullopt, "x86_64"},
                               fixtures::flathubSource()));
}

TEST(Offer, VersionIsOptional)
{
    auto o = Offer::create({PackageFormat::Deb, "gimp", std::nullopt, {}}, fixtures::debianSource());
    ASSERT_TRUE(o);
    EXPECT_FALSE(o->version());
}

TEST(Offer, VersionsOfTwoFormatsAreNotCompared)
{
    const Offer deb = fixtures::debOffer("firefox-esr", "128.4.0esr-1");
    const Offer flatpak = fixtures::flatpakOffer("app/org.mozilla.firefox/x86_64/stable", Scope::User, "128.4.0");
    auto order = compare(*deb.version(), *flatpak.version());
    ASSERT_FALSE(order);
    EXPECT_EQ(order.error().code(), ErrorCode::NotSupported);
}

TEST(Offer, RejectsAnIdLongerThan255Bytes)
{
    const std::string branch(240, 'b');
    const std::string ref = "app/org.example.App/x86_64/" + branch;
    auto r = Offer::create({PackageFormat::Flatpak, ref, std::nullopt, {}}, fixtures::flathubSource());
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code(), ErrorCode::InvalidArgument);
}

TEST(PackageFormat, NamesRoundTrip)
{
    EXPECT_EQ(toString(PackageFormat::Deb), "deb");
    EXPECT_EQ(toString(PackageFormat::Flatpak), "flatpak");
    EXPECT_EQ(parsePackageFormat("deb"), PackageFormat::Deb);
    EXPECT_EQ(parsePackageFormat("flatpak"), PackageFormat::Flatpak);
    EXPECT_FALSE(parsePackageFormat("snap"));
    EXPECT_FALSE(parsePackageFormat("rpm"));
}
