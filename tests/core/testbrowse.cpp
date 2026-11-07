#include "catalogfixture.h"

#include <cutefish/store/sqlitecatalog.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <strings.h>

using namespace cutefish::store;
using catalogfixture::TempDir;
using fixtures::unwrap;

namespace {

class BrowseTest : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        dir = new TempDir;
        catalogfixture::build(path(), 7);
    }
    static void TearDownTestSuite()
    {
        delete dir;
        dir = nullptr;
    }
    static std::filesystem::path path() { return *dir / "catalog.sqlite"; }

    static SearchPage browse(const SqliteCatalog& c, SearchOptions o = {})
    {
        return unwrap(c.browse(unwrap(BrowseQuery::create(std::move(o)))));
    }
    static std::vector<std::string> ids(const SearchPage& p)
    {
        std::vector<std::string> out;
        for (const AppSummary& s : p.items)
            out.push_back(s.id.str());
        return out;
    }

    static inline TempDir* dir = nullptr;
};

// Every application of the fixture that was accepted (catalogfixture.h).
const std::vector<std::string> kAllIds{"org.example.BadText",   "org.example.Editeur",    "org.example.Viewer",
                                       "org.gimp.GIMP",         "org.gnome.gitg",         "org.inkscape.Inkscape",
                                       "org.kde.kwalletmanager", "org.kde.kwalletmanager5", "org.qgis.qgis"};

} // namespace

TEST_F(BrowseTest, ListsEveryApplicationOnceSortedByDisplayedName)
{
    auto c = unwrap(SqliteCatalog::open(path()));
    const SearchPage all = browse(c, {.limit = 200});
    EXPECT_EQ(all.generation, 7u);
    EXPECT_EQ(all.total, kAllIds.size());
    auto got = ids(all);
    auto sortedIds = got;
    std::sort(sortedIds.begin(), sortedIds.end());
    EXPECT_EQ(sortedIds, kAllIds); // each application once, offers never multiply rows

    // Order: displayed name (ASCII case ignored), then id.
    for (std::size_t i = 1; i < all.items.size(); ++i) {
        const int cmp = ::strcasecmp(all.items[i - 1].name.c_str(), all.items[i].name.c_str());
        EXPECT_TRUE(cmp < 0 || (cmp == 0 && all.items[i - 1].id < all.items[i].id))
                << all.items[i - 1].name << " / " << all.items[i].name;
    }
    // Two applications with the same name stay two rows, ordered by id
    // (never merged on the name, ARCHITECTURE §6.4).
    const auto k1 = std::find(got.begin(), got.end(), "org.kde.kwalletmanager");
    const auto k2 = std::find(got.begin(), got.end(), "org.kde.kwalletmanager5");
    ASSERT_NE(k1, got.end());
    EXPECT_EQ(k1 + 1, k2);
}

TEST_F(BrowseTest, SortFollowsTheDisplayedNameOfTheReadersLanguage)
{
    // In French, GIMP is displayed with the Debian translation: the
    // translation wins over the source priority (CATALOG §4, D-013).
    auto fr = unwrap(SqliteCatalog::open(path(), {.locales = {"fr_FR.UTF-8"}}));
    const SearchPage all = browse(fr, {.limit = 200});
    for (std::size_t i = 1; i < all.items.size(); ++i)
        EXPECT_LE(::strcasecmp(all.items[i - 1].name.c_str(), all.items[i].name.c_str()), 0)
                << all.items[i - 1].name << " / " << all.items[i].name;
    const auto gimp = std::find_if(all.items.begin(), all.items.end(),
                                   [](const AppSummary& s) { return s.id.str() == "org.gimp.GIMP"; });
    ASSERT_NE(gimp, all.items.end());
    EXPECT_EQ(gimp->name, "Programme de manipulation d'images GNU");

    auto c = unwrap(SqliteCatalog::open(path()));
    const SearchPage untranslated = browse(c, {.limit = 200});
    const auto gimpC = std::find_if(untranslated.items.begin(), untranslated.items.end(),
                                    [](const AppSummary& s) { return s.id.str() == "org.gimp.GIMP"; });
    EXPECT_EQ(gimpC->name, "GIMP"); // Flathub's text, higher priority than Debian's
}

TEST_F(BrowseTest, PagesCoverTheListWithoutGapsOrRepeats)
{
    auto c = unwrap(SqliteCatalog::open(path()));
    const auto full = ids(browse(c, {.limit = 200}));
    std::vector<std::string> paged;
    for (std::uint32_t offset = 0; offset < full.size(); offset += 2) {
        const SearchPage p = browse(c, {.limit = 2, .offset = offset});
        EXPECT_EQ(p.total, full.size());
        EXPECT_LE(p.items.size(), 2u);
        for (const auto& id : ids(p))
            paged.push_back(id);
    }
    EXPECT_EQ(paged, full);
    const SearchPage past = browse(c, {.limit = 5, .offset = 1000});
    EXPECT_EQ(past.total, full.size());
    EXPECT_TRUE(past.items.empty());
}

TEST_F(BrowseTest, FiltersByCategoryAndBackend)
{
    auto c = unwrap(SqliteCatalog::open(path()));
    auto sorted = [](std::vector<std::string> v) {
        std::sort(v.begin(), v.end());
        return v;
    };
    EXPECT_EQ(sorted(ids(browse(c, {.category = "Graphics"}))),
              (std::vector<std::string>{"org.gimp.GIMP", "org.inkscape.Inkscape"}));
    EXPECT_EQ(sorted(ids(browse(c, {.category = "VectorGraphics"}))), std::vector<std::string>{"org.inkscape.Inkscape"});
    EXPECT_EQ(browse(c, {.category = "Unknown"}).total, 0u);

    EXPECT_EQ(sorted(ids(browse(c, {.limit = 200, .backend = BackendKind::Flatpak}))),
              (std::vector<std::string>{"org.gimp.GIMP", "org.gnome.gitg", "org.inkscape.Inkscape",
                                        "org.kde.kwalletmanager", "org.qgis.qgis"}));
    EXPECT_EQ(sorted(ids(browse(c, {.limit = 200, .backend = BackendKind::PackageKit}))),
              (std::vector<std::string>{"org.example.BadText", "org.example.Editeur", "org.example.Viewer",
                                        "org.gimp.GIMP", "org.gnome.gitg", "org.kde.kwalletmanager5"}));
    // Both filters apply to the application, as in search(): categories are
    // the union of its offers' categories, so GIMP (2DGraphics from Debian)
    // also matches with the Flatpak backend.
    EXPECT_EQ(ids(browse(c, {.category = "2DGraphics", .backend = BackendKind::Flatpak})),
              std::vector<std::string>{"org.gimp.GIMP"});
    EXPECT_EQ(ids(browse(c, {.category = "VectorGraphics", .backend = BackendKind::PackageKit})),
              std::vector<std::string>{});

    // Changing the filters and coming back gives the same list again.
    const auto all = ids(browse(c, {.limit = 200}));
    (void) browse(c, {.category = "Graphics"});
    EXPECT_EQ(ids(browse(c, {.limit = 200})), all);
}

TEST_F(BrowseTest, AnApplicationWithSeveralOffersIsOneRowWithEveryBackend)
{
    auto c = unwrap(SqliteCatalog::open(path()));
    const SearchPage p = browse(c, {.category = "2DGraphics"});
    ASSERT_EQ(p.items.size(), 1u);
    EXPECT_TRUE(p.items[0].backends.contains(BackendKind::PackageKit));
    EXPECT_TRUE(p.items[0].backends.contains(BackendKind::Flatpak));
    EXPECT_TRUE(p.items[0].verified);
}

TEST_F(BrowseTest, InvalidQueriesAreRefused)
{
    EXPECT_FALSE(BrowseQuery::create({.limit = 0}));
    EXPECT_FALSE(BrowseQuery::create({.limit = 201}));
    EXPECT_FALSE(BrowseQuery::create({.category = "two words"}));
    EXPECT_TRUE(BrowseQuery::create());
}

TEST_F(BrowseTest, ListsCategoriesWithCounts)
{
    auto c = unwrap(SqliteCatalog::open(path()));
    const auto categories = unwrap(c.listCategories());
    EXPECT_EQ(categories, (std::vector<CategoryCount>{{"2DGraphics", 1}, {"Graphics", 2}, {"VectorGraphics", 1}}));
    // Counts agree with browsing each category.
    for (const CategoryCount& cc : categories)
        EXPECT_EQ(browse(c, {.category = cc.category}).total, cc.applications) << cc.category;
}
