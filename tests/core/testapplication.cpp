#include "fixtures.h"

#include <gtest/gtest.h>

using namespace cutefish::store;
using fixtures::appId;
using fixtures::unwrap;

TEST(Application, IsCreatedWithSeveralOffers)
{
    auto r = Application::create(fixtures::firefoxSpec());
    ASSERT_TRUE(r) << r.error().message();
    const Application& app = *r;
    EXPECT_EQ(app.id().str(), "org.mozilla.firefox");
    EXPECT_EQ(app.name(), "Firefox");
    EXPECT_EQ(app.summary(), "Navigateur web");
    ASSERT_EQ(app.description().size(), 1u);
    EXPECT_EQ(app.description()[0].type, DescriptionBlockType::Paragraph);
    EXPECT_EQ(app.icon().kind, IconKind::LocalPath);
    ASSERT_EQ(app.offers().size(), 2u);
    EXPECT_EQ(app.offers()[0].format(), PackageFormat::Deb);
    EXPECT_EQ(app.offers()[1].format(), PackageFormat::Flatpak);
    EXPECT_TRUE(app.hasBackend(BackendKind::PackageKit));
    EXPECT_TRUE(app.hasBackend(BackendKind::Flatpak));
    EXPECT_TRUE(app.hasCategory("WebBrowser"));
    EXPECT_FALSE(app.hasCategory("Game"));
}

TEST(Application, FindsItsOffers)
{
    const Application app = unwrap(Application::create(fixtures::firefoxSpec()));
    const Offer* deb = app.findOffer(unwrap(OfferId::parse("packagekit:firefox-esr")));
    ASSERT_NE(deb, nullptr);
    EXPECT_EQ(deb->package(), "firefox-esr");
    EXPECT_EQ(app.findOffer(unwrap(OfferId::parse("packagekit:chromium"))), nullptr);
}

TEST(Application, KeepsUserAndSystemFlatpakOffersApart)
{
    ApplicationSpec spec = fixtures::firefoxSpec();
    spec.offers.push_back(fixtures::flatpakOffer("app/org.mozilla.firefox/x86_64/stable", Scope::System));
    const Application app = unwrap(Application::create(std::move(spec)));
    ASSERT_EQ(app.offers().size(), 3u);
    EXPECT_NE(app.offers()[1].id(), app.offers()[2].id());
}

TEST(Application, IsKnownByItsAliases)
{
    ApplicationSpec spec = fixtures::firefoxSpec();
    spec.aliases.push_back(unwrap(AppId::parse("firefox")));
    const Application app = unwrap(Application::create(std::move(spec)));
    EXPECT_TRUE(app.isKnownAs(appId("org.mozilla.firefox")));
    EXPECT_TRUE(app.isKnownAs(appId("firefox-esr")));
    EXPECT_TRUE(app.isKnownAs(appId("firefox")));
    EXPECT_FALSE(app.isKnownAs(appId("org.chromium.Chromium")));
    EXPECT_EQ(app.aliases().size(), 2u);
}

TEST(Application, LegacyDesktopIdBecomesTheIdAndAnAlias)
{
    // The indexer normalises the id and keeps the legacy form as an alias.
    // An alias is itself an AppId, so the legacy form is stored normalised;
    // here the Debian component was "org.gnome.gitg.desktop".
    const AppId id = unwrap(AppId::fromComponentId("org.gnome.gitg.desktop"));
    EXPECT_EQ(id.str(), "org.gnome.gitg");
    ApplicationSpec spec{.id = id, .name = "gitg", .summary = {}, .description = {}, .icon = {},
                         .categories = {}, .aliases = {appId("gitg")}, .offers = {}};
    spec.offers.push_back(fixtures::debOffer("gitg", "44-2"));
    EXPECT_TRUE(Application::create(std::move(spec)));
}

TEST(Application, RequiresANameAndAnOffer)
{
    ApplicationSpec noName = fixtures::firefoxSpec();
    noName.name.clear();
    EXPECT_FALSE(Application::create(std::move(noName)));

    ApplicationSpec noOffer = fixtures::firefoxSpec();
    noOffer.offers.clear();
    auto r = Application::create(std::move(noOffer));
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code(), ErrorCode::InvalidArgument);
}

TEST(Application, RejectsDuplicates)
{
    ApplicationSpec offers = fixtures::firefoxSpec();
    offers.offers.push_back(fixtures::debOffer("firefox-esr", "115.0esr-1"));
    EXPECT_FALSE(Application::create(std::move(offers)));

    ApplicationSpec aliases = fixtures::firefoxSpec();
    aliases.aliases.push_back(appId("firefox-esr"));
    EXPECT_FALSE(Application::create(std::move(aliases)));

    ApplicationSpec selfAlias = fixtures::firefoxSpec();
    selfAlias.aliases.push_back(appId("org.mozilla.firefox"));
    EXPECT_FALSE(Application::create(std::move(selfAlias)));

    ApplicationSpec categories = fixtures::firefoxSpec();
    categories.categories.push_back("Network");
    EXPECT_FALSE(Application::create(std::move(categories)));
}

TEST(Application, ValidatesText)
{
    ApplicationSpec badName = fixtures::firefoxSpec();
    badName.name = "Fire\x01" "fox";
    EXPECT_FALSE(Application::create(std::move(badName)));

    ApplicationSpec badUtf8 = fixtures::firefoxSpec();
    badUtf8.summary = "\xC3\x28";
    EXPECT_FALSE(Application::create(std::move(badUtf8)));

    ApplicationSpec emptyBlock = fixtures::firefoxSpec();
    emptyBlock.description.push_back({DescriptionBlockType::ListItem, ""});
    EXPECT_FALSE(Application::create(std::move(emptyBlock)));

    ApplicationSpec multiline = fixtures::firefoxSpec();
    multiline.description.push_back({DescriptionBlockType::OrderedListItem, "first line\nsecond line"});
    EXPECT_TRUE(Application::create(std::move(multiline)));
}

TEST(Application, ValidatesIcons)
{
    auto withIcon = [](IconRef icon) {
        ApplicationSpec spec = fixtures::firefoxSpec();
        spec.icon = std::move(icon);
        return Application::create(std::move(spec));
    };
    EXPECT_TRUE(withIcon({IconKind::None, ""}));
    EXPECT_TRUE(withIcon({IconKind::RemoteUrl, "https://dl.flathub.org/media/icon.png"}));
    EXPECT_TRUE(withIcon({IconKind::StockName, "firefox"}));
    EXPECT_FALSE(withIcon({IconKind::None, "firefox"}));
    EXPECT_FALSE(withIcon({IconKind::RemoteUrl, "http://example.org/icon.png"}));
    EXPECT_FALSE(withIcon({IconKind::LocalPath, "icons/firefox.png"}));
    EXPECT_FALSE(withIcon({IconKind::LocalPath, "/var/lib/../../etc/passwd"}));
    EXPECT_FALSE(withIcon({IconKind::StockName, ""}));
}
