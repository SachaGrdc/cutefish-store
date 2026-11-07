// The catalogue read together with state.sqlite (Catalogue v0.3, part C):
// installed applications in search, browse and summaries.

#include "catalogfixture.h"

#include <cutefish/store/catalogfile.h>
#include <cutefish/store/sqlitecatalog.h>
#include <cutefish/store/statestore.h>

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <algorithm>
#include <atomic>
#include <set>
#include <thread>

using namespace cutefish::store;
using catalogfixture::TempDir;
using fixtures::appId;
using fixtures::unwrap;

namespace {

constexpr UnixSeconds kNow = 1'800'000'000;

InstalledItem deb(const char* package, const char* app = nullptr)
{
    InstalledItemSpec spec{.offer = unwrap(OfferId::parse(std::string("packagekit:") + package)),
                           .backend = BackendKind::PackageKit,
                           .scope = Scope::System,
                           .version = unwrap(Version::debian("1.0-1"))};
    if (app)
        spec.app = appId(app);
    return unwrap(InstalledItem::create(std::move(spec)));
}

InstalledItem flatpak(const std::string& offer, const char* app = nullptr, Scope scope = Scope::User)
{
    InstalledItemSpec spec{.offer = unwrap(OfferId::parse(offer)), .backend = BackendKind::Flatpak, .scope = scope};
    if (app)
        spec.app = appId(app);
    return unwrap(InstalledItem::create(std::move(spec)));
}

class CatalogStateTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        catalogfixture::build(catalog, 1);
        store.emplace(unwrap(StateStore::open(state, kNow)));
    }

    SqliteCatalog open(std::vector<std::string> locales = {})
    {
        return unwrap(SqliteCatalog::open(catalog, {.locales = std::move(locales), .statePath = state}));
    }
    void installDebs(std::vector<InstalledItem> items)
    {
        ASSERT_TRUE(store->replaceInstalled(BackendKind::PackageKit, Scope::System, items, kNow));
    }
    void installUserFlatpaks(std::vector<InstalledItem> items)
    {
        ASSERT_TRUE(store->replaceInstalled(BackendKind::Flatpak, Scope::User, items, kNow));
    }
    static std::set<std::string> installedIds(const SearchPage& page)
    {
        std::set<std::string> out;
        for (const AppSummary& s : page.items)
            if (s.installed)
                out.insert(s.id.str());
        return out;
    }
    static SearchPage browse(const SqliteCatalog& c, SearchOptions o = {})
    {
        o.limit = 200;
        return unwrap(c.browse(unwrap(BrowseQuery::create(std::move(o)))));
    }

    TempDir dir;
    const std::filesystem::path catalog = dir / "catalog.sqlite";
    const std::filesystem::path state = dir / "state.sqlite";
    std::optional<StateStore> store;
};

} // namespace

TEST_F(CatalogStateTest, NothingInstalled)
{
    auto c = open();
    EXPECT_TRUE(installedIds(browse(c)).empty());
    EXPECT_EQ(browse(c, {.installedOnly = true}).total, 0u);
    EXPECT_EQ(unwrap(c.search(unwrap(SearchQuery::create("gimp", {.installedOnly = true})))).total, 0u);
}

TEST_F(CatalogStateTest, InstalledOffersMarkTheirApplication)
{
    installDebs({deb("gimp", "org.gimp.GIMP"), deb("gitg")}); // gitg: the inventory did not link it
    installUserFlatpaks({flatpak("flatpak:user:flathub:app/org.qgis.qgis/x86_64/lts")});
    auto c = open();

    // The catalogue's own offer is enough: no application link needed.
    const std::set<std::string> expected{"org.gimp.GIMP", "org.gnome.gitg", "org.qgis.qgis"};
    EXPECT_EQ(installedIds(browse(c)), expected);

    const SearchPage only = browse(c, {.installedOnly = true});
    EXPECT_EQ(only.total, 3u);
    EXPECT_EQ(installedIds(only), expected);

    const SearchPage found = unwrap(c.search(unwrap(SearchQuery::create("gimp", {.installedOnly = true}))));
    ASSERT_EQ(found.items.size(), 1u);
    EXPECT_TRUE(found.items[0].installed);
    EXPECT_FALSE(found.items[0].hasUpdate); // no update table yet

    const std::vector<AppId> ids{appId("org.qgis.qgis"), appId("org.inkscape.Inkscape")};
    const auto summaries = unwrap(c.summaries(ids));
    ASSERT_EQ(summaries.size(), 2u);
    EXPECT_TRUE(summaries[0].installed);
    EXPECT_FALSE(summaries[1].installed);

    // Filters combine.
    EXPECT_EQ(installedIds(browse(c, {.backend = BackendKind::Flatpak, .installedOnly = true})),
              (std::set<std::string>{"org.gimp.GIMP", "org.gnome.gitg", "org.qgis.qgis"}));
    EXPECT_EQ(browse(c, {.category = "VectorGraphics", .installedOnly = true}).total, 0u);
}

TEST_F(CatalogStateTest, UnknownOffersUseTheInventoryLink)
{
    // Inkscape installed from a remote the catalogue does not index: only
    // the inventory knows the application, by id or by alias.
    installUserFlatpaks({flatpak("flatpak:user:kde-nightly:app/org.inkscape.Inkscape/x86_64/master",
                                 "org.inkscape.Inkscape")});
    auto c = open();
    EXPECT_EQ(installedIds(browse(c)), std::set<std::string>{"org.inkscape.Inkscape"});

    installUserFlatpaks({flatpak("flatpak:user:other:app/org.inkscape.Inkscape/x86_64/master", "inkscape")});
    EXPECT_EQ(installedIds(browse(c)), std::set<std::string>{"org.inkscape.Inkscape"}); // via the alias
}

TEST_F(CatalogStateTest, MissingOrDanglingLinksMarkNothing)
{
    installDebs({deb("not-in-catalogue"),                        // unknown offer, no application
                 deb("another-unknown", "org.example.NotInCatalogue")}); // unknown application
    auto c = open();
    EXPECT_TRUE(installedIds(browse(c)).empty());
    EXPECT_EQ(browse(c, {.installedOnly = true}).total, 0u);
}

TEST_F(CatalogStateTest, TheCataloguesOfferWinsOverAContradictoryLink)
{
    // The inventory says `viewer` belongs to Editeur; the catalogue says it
    // is Viewer's offer. The catalogue decides: Viewer is installed, Editeur
    // is not (state.sqlite is a cache, not a source of truth).
    installDebs({deb("viewer", "org.example.Editeur")});
    auto c = open();
    EXPECT_EQ(installedIds(browse(c)), std::set<std::string>{"org.example.Viewer"});
}

TEST_F(CatalogStateTest, WithoutStateInstalledOnlyIsRefused)
{
    installDebs({deb("gimp", "org.gimp.GIMP")});
    auto c = unwrap(SqliteCatalog::open(catalog));
    EXPECT_TRUE(installedIds(browse(c)).empty());
    auto b = c.browse(unwrap(BrowseQuery::create({.installedOnly = true})));
    ASSERT_FALSE(b);
    EXPECT_EQ(b.error().code(), ErrorCode::NotSupported);
    auto s = c.search(unwrap(SearchQuery::create("gimp", {.installedOnly = true})));
    ASSERT_FALSE(s);
    EXPECT_EQ(s.error().code(), ErrorCode::NotSupported);
}

TEST_F(CatalogStateTest, AMissingOrForeignStateFileIsAnError)
{
    auto missing = SqliteCatalog::open(catalog, {.statePath = dir / "absent.sqlite"});
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code(), ErrorCode::NotFound);
    EXPECT_FALSE(std::filesystem::exists(dir / "absent.sqlite"));

    store.reset();
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(state.c_str(), &db), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(db, "PRAGMA user_version = 99", nullptr, nullptr, nullptr), SQLITE_OK);
    sqlite3_close(db);
    auto newer = SqliteCatalog::open(catalog, {.statePath = state});
    ASSERT_FALSE(newer);
    EXPECT_EQ(newer.error().code(), ErrorCode::NotSupported);

    auto notState = SqliteCatalog::open(catalog, {.statePath = catalog});
    EXPECT_FALSE(notState);
}

TEST_F(CatalogStateTest, ReadsWorkWhenTheWriterIsClosed)
{
    installDebs({deb("gimp", "org.gimp.GIMP")});
    store.reset(); // last writer gone: no -wal or -shm file left
    auto c = open();
    EXPECT_EQ(installedIds(browse(c)), std::set<std::string>{"org.gimp.GIMP"});
}

TEST_F(CatalogStateTest, InstalledOnlyListsFollowNewInventories)
{
    auto c = open();
    EXPECT_EQ(browse(c, {.installedOnly = true}).total, 0u);
    installDebs({deb("gimp", "org.gimp.GIMP")});
    EXPECT_EQ(browse(c, {.installedOnly = true}).total, 1u); // the cached list is not reused
    installDebs({deb("gimp", "org.gimp.GIMP"), deb("viewer")});
    EXPECT_EQ(browse(c, {.installedOnly = true}).total, 2u);
    EXPECT_EQ(unwrap(c.search(unwrap(SearchQuery::create("viewer", {.installedOnly = true})))).total, 1u);
    installDebs({});
    EXPECT_EQ(unwrap(c.search(unwrap(SearchQuery::create("viewer", {.installedOnly = true})))).total, 0u);
    EXPECT_EQ(browse(c, {.installedOnly = true}).total, 0u);
}

TEST_F(CatalogStateTest, AgreesWithTheStateStore)
{
    installDebs({deb("gimp", "org.gimp.GIMP"), deb("viewer", "org.example.Editeur"), deb("unknown")});
    installUserFlatpaks({flatpak("flatpak:user:flathub:app/org.qgis.qgis/x86_64/stable"),
                         flatpak("flatpak:user:remote:app/org.inkscape.Inkscape/x86_64/master", "inkscape")});
    auto c = open();
    // For every application: installed in the catalogue exactly when the
    // state store holds one of its offers, or an offer unknown to the
    // catalogue linked to it.
    std::set<std::string> catalogueOffers;
    const SearchPage all = browse(c);
    for (const AppSummary& s : all.items) {
        const Application app = unwrap(c.application(s.id));
        for (const Offer& o : app.offers())
            catalogueOffers.insert(o.id().str());
    }
    const auto inventory = unwrap(store->items(std::nullopt));
    for (const AppSummary& s : all.items) {
        const Application app = unwrap(c.application(s.id));
        bool expected = false;
        for (const Offer& o : app.offers())
            expected = expected || store->find(o.id())->has_value();
        for (const InstalledItem& i : inventory)
            if (i.app() && !catalogueOffers.contains(i.offer().str()) && app.isKnownAs(*i.app()))
                expected = true;
        EXPECT_EQ(s.installed, expected) << s.id.str();
    }
}

// --- Concurrency ----------------------------------------------------------------

TEST_F(CatalogStateTest, APageSeesOneInventory)
{
    // The writer switches between "gimp and gitg installed" and "nothing";
    // a page must never show one without the other.
    std::atomic<bool> done{false};
    std::atomic<int> pages{0};
    std::atomic<int> torn{0};
    std::vector<std::thread> readers;
    for (int r = 0; r < 3; ++r)
        readers.emplace_back([&] {
            auto c = SqliteCatalog::open(catalog, {.statePath = state});
            if (!c) {
                ++torn;
                return;
            }
            while (!done.load()) {
                auto page = c->browse(*BrowseQuery::create({.limit = 200}));
                if (!page) {
                    ++torn;
                    continue;
                }
                const auto ids = installedIds(*page);
                if (!(ids.empty() || ids == std::set<std::string>{"org.gimp.GIMP", "org.gnome.gitg"}))
                    ++torn;
                ++pages;
            }
        });
    for (int i = 0; i < 300; ++i) {
        if (i % 2)
            installDebs({});
        else
            installDebs({deb("gimp"), deb("gitg")});
    }
    while (pages.load() < 30)
        std::this_thread::yield();
    done = true;
    for (auto& t : readers)
        t.join();
    EXPECT_EQ(torn.load(), 0);
}

TEST_F(CatalogStateTest, ReadersAcrossCatalogueReplacements)
{
    // Generation g has 9 applications when odd (with Flathub), 6 when even.
    installDebs({deb("gimp")});
    const auto candidate = dir / "catalog.sqlite.tmp";
    std::atomic<bool> done{false};
    std::atomic<int> pages{0};
    std::atomic<int> wrong{0};
    std::vector<std::thread> readers;
    for (int r = 0; r < 3; ++r)
        readers.emplace_back([&] {
            while (!done.load()) {
                // A reader opened before a replacement keeps its snapshot; a
                // new one sees the new file. Both stay consistent.
                auto c = SqliteCatalog::open(catalog, {.statePath = state});
                if (!c) {
                    ++wrong;
                    continue;
                }
                for (int k = 0; k < 3; ++k) {
                    auto page = c->browse(*BrowseQuery::create({.limit = 200}));
                    if (!page || page->generation != c->generation()
                        || page->total != (c->generation() % 2 ? 9u : 6u)
                        || installedIds(*page) != std::set<std::string>{"org.gimp.GIMP"})
                        ++wrong;
                    ++pages;
                }
            }
        });
    const SourceId flathub = fixtures::flathubSource(Scope::User).id();
    for (std::uint64_t g = 2; g <= 12; ++g) {
        catalogfixture::build(candidate, g, g % 2 == 1);
        CatalogReplaceRules rules;
        if (g % 2 == 0)
            rules.removedSources = {flathub.str()};
        ASSERT_TRUE(replaceCatalog(candidate, catalog, rules));
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    while (pages.load() < 30)
        std::this_thread::yield();
    done = true;
    for (auto& t : readers)
        t.join();
    EXPECT_EQ(wrong.load(), 0);
    EXPECT_EQ(unwrap(SqliteCatalog::open(catalog)).generation(), 12u);
}
