#include "catalogfixture.h"

#include <cutefish/store/catalogfile.h>
#include <cutefish/store/sqlitecatalog.h>

#include <gtest/gtest.h>

#include <algorithm>

using namespace cutefish::store;
using catalogfixture::TempDir;
using fixtures::appId;
using fixtures::unwrap;

namespace {

std::vector<std::string> ids(const SearchPage& page)
{
    std::vector<std::string> out;
    for (const AppSummary& s : page.items)
        out.push_back(s.id.str());
    return out;
}

SearchPage find(const Catalog& c, const char* text, SearchOptions options = {})
{
    return unwrap(c.search(unwrap(SearchQuery::create(text, std::move(options)))));
}

class CatalogTest : public ::testing::Test
{
protected:
    void SetUp() override { built = catalogfixture::build(dir / "catalog.sqlite", 7); }
    SqliteCatalog open(std::vector<std::string> locales = {})
    {
        return unwrap(SqliteCatalog::open(dir / "catalog.sqlite", {.locales = std::move(locales)}));
    }

    TempDir dir;
    catalogfixture::Built built;
};

} // namespace

TEST_F(CatalogTest, BuildKeepsWhatIsValidAndCountsTheRest)
{
    EXPECT_EQ(built.stats.sources, 2);
    // gimp, gitg, qgis, kwalletmanager5, kwalletmanager, inkscape, editeur,
    // viewer, badtext.
    EXPECT_EQ(built.stats.applications, 9);
    EXPECT_EQ(built.stats.offers, 12);
    EXPECT_EQ(built.stats.aliases, 1);          // inkscape
    EXPECT_EQ(built.stats.ambiguousAliases, 1); // org.example.Shared
    EXPECT_EQ(built.stats.droppedTexts, 2);     // control character, invalid locale

    ASSERT_EQ(built.rejected.size(), 4u);
    EXPECT_EQ(built.rejected[0].code(), ErrorCode::InvalidArgument); // org..broken
    EXPECT_EQ(built.rejected[1].code(), ErrorCode::InvalidArgument); // no C name
    EXPECT_EQ(built.rejected[2].code(), ErrorCode::InvalidArgument); // Bad_Package
    EXPECT_EQ(built.rejected[3].code(), ErrorCode::Conflict);        // second gimp offer
}

TEST_F(CatalogTest, FileIsAValidCatalogue)
{
    auto info = inspectCatalog(dir / "catalog.sqlite");
    ASSERT_TRUE(info) << info.error().message();
    EXPECT_EQ(info->generation, 7u);
    EXPECT_EQ(info->applications, 9);
    EXPECT_EQ(info->offers, 12);
    EXPECT_EQ(info->offersPerSource.at("packagekit:debian-stable-main"), 6);
    EXPECT_EQ(info->offersPerSource.at("flatpak:user:flathub"), 6);
}

TEST_F(CatalogTest, ApplicationWithOffersFromBothSources)
{
    const SqliteCatalog c = open();
    EXPECT_EQ(c.generation(), 7u);
    auto gimp = c.application(appId("org.gimp.GIMP"));
    ASSERT_TRUE(gimp) << gimp.error().message();
    ASSERT_EQ(gimp->offers().size(), 2u);
    // Offers in source priority order: Flathub (20) before Debian (10).
    EXPECT_EQ(gimp->offers()[0].id().str(), "flatpak:user:flathub:app/org.gimp.GIMP/x86_64/stable");
    EXPECT_EQ(gimp->offers()[0].version()->str(), "3.0.4");
    EXPECT_EQ(gimp->offers()[1].id().str(), "packagekit:gimp");
    EXPECT_FALSE(gimp->offers()[1].version()); // DEP-11 has no package version
    // Displayed texts: highest-priority source that has the field.
    EXPECT_EQ(gimp->name(), "GIMP");
    // Only Debian has a description and an icon: they complete the rest.
    ASSERT_EQ(gimp->description().size(), 2u);
    EXPECT_EQ(gimp->description()[0].text, "GIMP is an image editor.");
    EXPECT_EQ(gimp->description()[1].type, DescriptionBlockType::ListItem);
    EXPECT_EQ(gimp->icon().kind, IconKind::LocalPath);
    EXPECT_TRUE(gimp->hasCategory("2DGraphics"));
    EXPECT_TRUE(gimp->hasCategory("Graphics"));
}

TEST_F(CatalogTest, ProvenanceIsKept)
{
    const SqliteCatalog c = open();
    auto gitg = unwrap(c.application(appId("org.gnome.gitg")));
    ASSERT_EQ(gitg.offers().size(), 2u);
    const Offer& flathub = gitg.offers()[0];
    const Offer& debian = gitg.offers()[1];
    EXPECT_EQ(flathub.source().str(), "flatpak:user:flathub");
    EXPECT_EQ(flathub.sourceKind(), SourceKind::Flathub);
    EXPECT_EQ(flathub.origin(), "flathub");
    EXPECT_EQ(flathub.scope(), Scope::User);
    EXPECT_EQ(debian.source().str(), "packagekit:debian-stable-main");
    EXPECT_EQ(debian.sourceKind(), SourceKind::Distribution);
    EXPECT_EQ(debian.origin(), "debian-stable-main");
}

TEST_F(CatalogTest, LegacyDesktopIdsAreNormalised)
{
    const SqliteCatalog c = open();
    // Stored under its normalised id; looked up after normalisation.
    EXPECT_TRUE(c.application(appId("org.gnome.gitg")));
    EXPECT_TRUE(c.application(unwrap(AppId::fromComponentId("org.gnome.gitg.desktop"))));
}

TEST_F(CatalogTest, FlatpakBranchesAreSeparateOffers)
{
    const SqliteCatalog c = open();
    auto qgis = unwrap(c.application(appId("org.qgis.qgis")));
    ASSERT_EQ(qgis.offers().size(), 2u);
    EXPECT_EQ(qgis.offers()[0].package(), "app/org.qgis.qgis/x86_64/lts");
    EXPECT_EQ(qgis.offers()[1].package(), "app/org.qgis.qgis/x86_64/stable");
}

TEST_F(CatalogTest, SameLauncherDifferentIdsStayApart)
{
    const SqliteCatalog c = open();
    auto a = unwrap(c.application(appId("org.kde.kwalletmanager5")));
    auto b = unwrap(c.application(appId("org.kde.kwalletmanager")));
    EXPECT_EQ(a.offers().size(), 1u);
    EXPECT_EQ(b.offers().size(), 1u);
    // Same name, two results: never merged on the name.
    const SearchPage page = find(c, "KWalletManager");
    EXPECT_EQ(page.total, 2u);
}

TEST_F(CatalogTest, AliasesAreKeptOnlyWhenUnambiguous)
{
    const SqliteCatalog c = open();
    auto byAlias = c.application(appId("inkscape"));
    ASSERT_TRUE(byAlias);
    EXPECT_EQ(byAlias->id().str(), "org.inkscape.Inkscape");
    EXPECT_TRUE(byAlias->isKnownAs(appId("inkscape")));
    // Claimed by two applications: neither gets it.
    EXPECT_EQ(c.application(appId("org.example.Shared")).error().code(), ErrorCode::NotFound);
}

TEST_F(CatalogTest, UnknownApplication)
{
    const SqliteCatalog c = open();
    auto r = c.application(appId("org.example.Missing"));
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code(), ErrorCode::NotFound);
}

TEST_F(CatalogTest, ChoosesTheLanguageAtReadTime)
{
    const SqliteCatalog french = open({"fr_FR.UTF-8"});
    auto gimp = unwrap(french.application(appId("org.gimp.GIMP")));
    // fr_FR falls back to fr; the Debian offer is the only one with French.
    EXPECT_EQ(gimp.name(), "Programme de manipulation d'images GNU");
    EXPECT_EQ(gimp.summary(), "Créer des images et retoucher des photos");
    ASSERT_EQ(gimp.description().size(), 1u);
    EXPECT_EQ(gimp.description()[0].text, "GIMP est un éditeur d'images.");

    const SqliteCatalog german = open({"de_DE"});
    EXPECT_EQ(unwrap(german.application(appId("org.gimp.GIMP"))).name(), "GIMP"); // falls back to C

    const SqliteCatalog none = open();
    EXPECT_EQ(unwrap(none.application(appId("org.gimp.GIMP"))).name(), "GIMP");
}

TEST_F(CatalogTest, InvalidTranslationsAreDroppedNotTheComponent)
{
    const SqliteCatalog c = open({"de"});
    auto app = c.application(appId("org.example.BadText"));
    ASSERT_TRUE(app);
    EXPECT_EQ(app->summary(), "");
}

TEST_F(CatalogTest, SearchExactNameComesFirst)
{
    const SqliteCatalog c = open();
    const SearchPage page = find(c, "gimp");
    ASSERT_FALSE(page.items.empty());
    EXPECT_EQ(page.items[0].id.str(), "org.gimp.GIMP");
    EXPECT_EQ(page.generation, 7u);
    // One result per application, not per offer.
    const auto found = ids(page);
    EXPECT_EQ(std::count(found.begin(), found.end(), "org.gimp.GIMP"), 1);
    EXPECT_TRUE(page.items[0].backends.contains(BackendKind::PackageKit));
    EXPECT_TRUE(page.items[0].backends.contains(BackendKind::Flatpak));
    EXPECT_TRUE(page.items[0].verified);
    EXPECT_EQ(page.items[0].developer, "The GIMP team");
}

TEST_F(CatalogTest, SearchPrefixWhileTyping)
{
    const SqliteCatalog c = open();
    const SearchPage page = find(c, "gi");
    const auto found = ids(page);
    ASSERT_GE(found.size(), 2u);
    // "GIMP" and "gitg" start with "gi" and have the same length: the
    // application id breaks the tie.
    EXPECT_EQ(found[0], "org.gimp.GIMP");
    EXPECT_EQ(found[1], "org.gnome.gitg");
}

TEST_F(CatalogTest, SearchWordsInSummaryAndKeywords)
{
    const SqliteCatalog c = open();
    EXPECT_EQ(ids(find(c, "photographs")), std::vector<std::string>{"org.gimp.GIMP"});
    EXPECT_EQ(ids(find(c, "photoshop")), std::vector<std::string>{"org.gimp.GIMP"});
    EXPECT_EQ(ids(find(c, "svg")), std::vector<std::string>{"org.inkscape.Inkscape"});
    // Several words: all must match.
    EXPECT_EQ(ids(find(c, "vector editor")), std::vector<std::string>{"org.inkscape.Inkscape"});
}

TEST_F(CatalogTest, SearchIgnoresCaseAndAccents)
{
    const SqliteCatalog c = open();
    for (const char* q : {"editeur", "ÉDITEUR", "éditeur", "Edition"}) {
        const auto found = ids(find(c, q));
        EXPECT_NE(std::find(found.begin(), found.end(), "org.example.Editeur"), found.end()) << q;
    }
}

TEST_F(CatalogTest, SearchIndexesTheRequestedLanguages)
{
    const SqliteCatalog c = open();
    // French summary, indexed because the build asked for fr_FR.
    EXPECT_EQ(ids(find(c, "retoucher")), std::vector<std::string>{"org.gimp.GIMP"});
}

TEST_F(CatalogTest, SearchSubstringOfAName)
{
    const SqliteCatalog c = open();
    EXPECT_EQ(ids(find(c, "kscap")), std::vector<std::string>{"org.inkscape.Inkscape"});
}

TEST_F(CatalogTest, SearchToleratesTyposOnlyWhenNothingElseMatches)
{
    const SqliteCatalog c = open();
    EXPECT_EQ(ids(find(c, "inkscpae")), std::vector<std::string>{"org.inkscape.Inkscape"});
    EXPECT_EQ(ids(find(c, "Inksacpe")), std::vector<std::string>{"org.inkscape.Inkscape"});
    // Too far.
    EXPECT_EQ(find(c, "inxxxpae").total, 0u);
}

TEST_F(CatalogTest, SearchWithoutResults)
{
    const SqliteCatalog c = open();
    const SearchPage page = find(c, "zzzqqqxxx");
    EXPECT_EQ(page.total, 0u);
    EXPECT_TRUE(page.items.empty());
    EXPECT_EQ(find(c, "").total, 0u);
}

TEST_F(CatalogTest, SearchSyntaxIsJustText)
{
    const SqliteCatalog c = open();
    for (const char* q : {"\"", "gimp\"", "NEAR(gimp)", "gimp OR inkscape", "*", "name:gimp", "50%", "a_b", "-"})
        EXPECT_TRUE(c.search(unwrap(SearchQuery::create(q)))) << q;
    EXPECT_EQ(find(c, "%").total, 0u);
}

TEST_F(CatalogTest, SearchFiltersAndPages)
{
    const SqliteCatalog c = open();
    const SearchPage graphics = find(c, "e", {.category = "Graphics"});
    for (const AppSummary& s : graphics.items)
        EXPECT_TRUE(s.id.str() == "org.gimp.GIMP" || s.id.str() == "org.inkscape.Inkscape") << s.id.str();

    const SearchPage debOnly = find(c, "wallet", {.backend = BackendKind::PackageKit});
    EXPECT_EQ(ids(debOnly), std::vector<std::string>{"org.kde.kwalletmanager5"});

    const SearchPage all = find(c, "e");
    ASSERT_GE(all.total, 3u);
    const SearchPage second = find(c, "e", {.limit = 1, .offset = 1});
    EXPECT_EQ(second.total, all.total);
    ASSERT_EQ(second.items.size(), 1u);
    EXPECT_EQ(second.items[0].id, all.items[1].id);

    auto installed = c.search(unwrap(SearchQuery::create("gimp", {.installedOnly = true})));
    ASSERT_FALSE(installed);
    EXPECT_EQ(installed.error().code(), ErrorCode::NotSupported);
}

TEST_F(CatalogTest, SummariesInOrder)
{
    const SqliteCatalog c = open({"fr"});
    const std::vector<AppId> wanted{appId("org.inkscape.Inkscape"), appId("org.example.Missing"), appId("org.gimp.GIMP")};
    auto r = c.summaries(wanted);
    ASSERT_TRUE(r);
    ASSERT_EQ(r->size(), 2u);
    EXPECT_EQ((*r)[0].name, "Inkscape");
    EXPECT_EQ((*r)[1].name, "Programme de manipulation d'images GNU");
    EXPECT_EQ((*r)[1].icon.kind, IconKind::LocalPath);
}

TEST(CatalogWriter, RefusesUnknownSourcesAndDoubleFinish)
{
    TempDir dir;
    auto writer = unwrap(CatalogWriter::create(dir / "c.sqlite"));
    auto r = writer.addComponent(unwrap(SourceId::parse("nowhere")), catalogfixture::deb("org.a.b", "ab", "AB"));
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code(), ErrorCode::NotFound);
    EXPECT_TRUE(writer.addSource(fixtures::debianSource()));
    EXPECT_EQ(writer.addSource(fixtures::debianSource()).error().code(), ErrorCode::Conflict);
    ASSERT_TRUE(writer.finish());
    EXPECT_FALSE(writer.finish());
}

TEST(CatalogWriter, EmptyCatalogueIsValid)
{
    TempDir dir;
    auto writer = unwrap(CatalogWriter::create(dir / "c.sqlite"));
    ASSERT_TRUE(writer.finish());
    auto c = unwrap(SqliteCatalog::open(dir / "c.sqlite"));
    EXPECT_EQ(unwrap(c.search(unwrap(SearchQuery::create("gimp")))).total, 0u);
}

TEST(SqliteCatalog, RefusesFilesThatAreNotCompleteCatalogues)
{
    TempDir dir;
    EXPECT_FALSE(SqliteCatalog::open(dir / "missing.sqlite"));
    {
        auto writer = unwrap(CatalogWriter::create(dir / "unfinished.sqlite"));
        EXPECT_TRUE(writer.addSource(fixtures::debianSource()));
        // Destroyed without finish(): user_version stays 0.
    }
    auto r = SqliteCatalog::open(dir / "unfinished.sqlite");
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code(), ErrorCode::InvalidArgument);
}
