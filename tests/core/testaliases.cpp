// Rule D-012: a <provides><id> becomes an alias only when exactly one
// application claims it and no application has that id. It never merges
// applications and never moves offers.

#include "catalogfixture.h"

#include <cutefish/store/sqlitecatalog.h>

#include <gtest/gtest.h>

using namespace cutefish::store;
using catalogfixture::TempDir;
using fixtures::appId;
using fixtures::unwrap;

namespace {

ComponentRecord deb(const char* id, const char* package, std::vector<std::string> provides = {})
{
    ComponentRecord r = catalogfixture::deb(id, package, package);
    r.providedIds = std::move(provides);
    return r;
}

ComponentRecord flatpak(const char* id, const char* ref, std::vector<std::string> provides = {})
{
    ComponentRecord r = catalogfixture::flatpak(id, ref, id);
    r.providedIds = std::move(provides);
    return r;
}

struct Catalogue
{
    TempDir dir;
    CatalogBuildStats stats;

    explicit Catalogue(std::vector<std::pair<bool, ComponentRecord>> components)
    {
        auto writer = unwrap(CatalogWriter::create(dir / "c.sqlite"));
        EXPECT_TRUE(writer.addSource(fixtures::debianSource()));
        EXPECT_TRUE(writer.addSource(fixtures::flathubSource()));
        for (auto& [isDeb, record] : components) {
            const SourceId source = isDeb ? fixtures::debianSource().id() : fixtures::flathubSource().id();
            EXPECT_TRUE(writer.addComponent(source, std::move(record)));
        }
        stats = unwrap(writer.finish());
    }

    SqliteCatalog open() const { return unwrap(SqliteCatalog::open(dir / "c.sqlite")); }
};

} // namespace

TEST(AliasRule, UniqueClaimBecomesAnAlias)
{
    Catalogue c({{true, deb("org.kde.konsole", "konsole", {"konsole"})}});
    EXPECT_EQ(c.stats.aliases, 1);
    EXPECT_EQ(c.stats.ambiguousAliases, 0);
    const SqliteCatalog catalog = c.open();
    auto app = unwrap(catalog.application(appId("konsole")));
    EXPECT_EQ(app.id().str(), "org.kde.konsole");
    EXPECT_TRUE(app.isKnownAs(appId("konsole")));
    ASSERT_EQ(app.aliases().size(), 1u);
    EXPECT_EQ(app.aliases()[0].str(), "konsole");
}

TEST(AliasRule, LegacyFormIsNormalised)
{
    Catalogue c({{true, deb("org.kde.konsole", "konsole", {"konsole.desktop"})}});
    EXPECT_EQ(c.stats.aliases, 1);
    EXPECT_TRUE(c.open().application(appId("konsole")));
}

TEST(AliasRule, ClaimedByTwoApplicationsIsDroppedAndNothingMerges)
{
    Catalogue c({{true, deb("org.example.A", "pkga", {"org.example.Shared"})},
                 {false, flatpak("org.example.B", "app/org.example.B/x86_64/stable", {"org.example.Shared"})}});
    EXPECT_EQ(c.stats.aliases, 0);
    EXPECT_EQ(c.stats.ambiguousAliases, 1);
    EXPECT_EQ(c.stats.applications, 2);
    const SqliteCatalog catalog = c.open();
    EXPECT_EQ(catalog.application(appId("org.example.Shared")).error().code(), ErrorCode::NotFound);
    auto a = unwrap(catalog.application(appId("org.example.A")));
    auto b = unwrap(catalog.application(appId("org.example.B")));
    EXPECT_EQ(a.offers().size(), 1u);
    EXPECT_EQ(b.offers().size(), 1u);
    EXPECT_TRUE(a.aliases().empty());
    EXPECT_TRUE(b.aliases().empty());
}

TEST(AliasRule, IdOfAnExistingApplicationIsNeverAnAlias)
{
    // The §6.4 rule before D-012 would have merged these two applications.
    for (const bool claimFirst : {true, false}) {
        std::vector<std::pair<bool, ComponentRecord>> components;
        auto claim = deb("org.kde.kwalletmanager5", "kwalletmanager", {"org.kde.kwalletmanager"});
        auto target = flatpak("org.kde.kwalletmanager", "app/org.kde.kwalletmanager/x86_64/stable");
        if (claimFirst) {
            components.emplace_back(true, std::move(claim));
            components.emplace_back(false, std::move(target));
        } else {
            components.emplace_back(false, std::move(target));
            components.emplace_back(true, std::move(claim));
        }
        Catalogue c(std::move(components));
        EXPECT_EQ(c.stats.aliases, 0) << claimFirst;
        EXPECT_EQ(c.stats.ambiguousAliases, 1) << claimFirst;
        EXPECT_EQ(c.stats.applications, 2) << claimFirst;
        const SqliteCatalog catalog = c.open();
        auto found = unwrap(catalog.application(appId("org.kde.kwalletmanager")));
        ASSERT_EQ(found.offers().size(), 1u);
        EXPECT_EQ(found.offers()[0].format(), PackageFormat::Flatpak);
        auto claimer = unwrap(catalog.application(appId("org.kde.kwalletmanager5")));
        ASSERT_EQ(claimer.offers().size(), 1u);
        EXPECT_EQ(claimer.offers()[0].format(), PackageFormat::Deb);
    }
}

TEST(AliasRule, SameApplicationClaimingTwiceKeepsOneAlias)
{
    // The Debian and Flathub components of one application both provide it.
    Catalogue c({{true, deb("org.gnome.gitg", "gitg", {"gitg"})},
                 {false, flatpak("org.gnome.gitg", "app/org.gnome.gitg/x86_64/stable", {"gitg.desktop"})}});
    EXPECT_EQ(c.stats.aliases, 1);
    EXPECT_EQ(c.stats.ambiguousAliases, 0);
    auto app = unwrap(c.open().application(appId("gitg")));
    EXPECT_EQ(app.offers().size(), 2u);
    EXPECT_EQ(app.aliases().size(), 1u);
}

TEST(AliasRule, OwnIdIsIgnored)
{
    Catalogue c({{true, deb("org.gnome.gitg.desktop", "gitg", {"org.gnome.gitg", "org.gnome.gitg.desktop"})}});
    EXPECT_EQ(c.stats.aliases, 0);
    EXPECT_EQ(c.stats.ambiguousAliases, 0);
    EXPECT_EQ(c.stats.invalidAliases, 0);
}

TEST(AliasRule, InvalidProvidedIdsAreCounted)
{
    Catalogue c({{true, deb("org.example.A", "pkga", {"not valid", "", "org..x", "fine.id"})}});
    EXPECT_EQ(c.stats.invalidAliases, 3);
    EXPECT_EQ(c.stats.aliases, 1);
    EXPECT_TRUE(c.open().application(appId("fine.id")));
}

TEST(AliasRule, AnApplicationIdAlwaysWinsOverAnAlias)
{
    // Lookup tries ids first: even if an alias table entry existed for an
    // id, the application with that id would be returned. The rule above
    // keeps the table free of such entries; this checks the lookup order.
    Catalogue c({{true, deb("org.example.A", "pkga", {"org.example.Alias"})},
                 {true, deb("org.example.B", "pkgb")}});
    const SqliteCatalog catalog = c.open();
    EXPECT_EQ(unwrap(catalog.application(appId("org.example.B"))).id().str(), "org.example.B");
    EXPECT_EQ(unwrap(catalog.application(appId("org.example.Alias"))).id().str(), "org.example.A");
}
