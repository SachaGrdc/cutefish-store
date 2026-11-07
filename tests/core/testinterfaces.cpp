#include "fakes.h"
#include "fixtures.h"

#include <gtest/gtest.h>

#include <memory>

using namespace cutefish::store;
using fixtures::appId;
using fixtures::unwrap;

namespace {

Application gimp()
{
    ApplicationSpec spec{.id = appId("org.gimp.GIMP"),
                         .name = "GIMP",
                         .summary = "Image editor",
                         .categories = {"Graphics"}};
    spec.offers.push_back(fixtures::debOffer("gimp", "3.0.4-3+deb13u11"));
    return unwrap(Application::create(std::move(spec)));
}

Application inkscape()
{
    ApplicationSpec spec{.id = appId("org.inkscape.Inkscape"),
                         .name = "Inkscape",
                         .summary = "Vector graphics editor",
                         .categories = {"Graphics"}};
    spec.offers.push_back(fixtures::flatpakOffer("app/org.inkscape.Inkscape/x86_64/stable"));
    return unwrap(Application::create(std::move(spec)));
}

InstalledItem installedGimp()
{
    return unwrap(InstalledItem::create({.offer = unwrap(OfferId::parse("packagekit:gimp")),
                                         .app = appId("org.gimp.GIMP"),
                                         .backend = BackendKind::PackageKit,
                                         .scope = Scope::System,
                                         .version = unwrap(Version::debian("3.0.4-3+deb13u11")),
                                         .origin = "debian-stable-main"}));
}

} // namespace

TEST(CatalogInterface, FindsApplicationsByIdAndAlias)
{
    std::vector<Application> apps{unwrap(Application::create(fixtures::firefoxSpec())), gimp()};
    fakes::FakeCatalog catalog(42, std::move(apps));
    const Catalog& c = catalog; // used only through the interface

    EXPECT_EQ(c.generation(), 42u);
    ASSERT_TRUE(c.application(appId("org.gimp.GIMP")));
    auto byAlias = c.application(appId("firefox-esr"));
    ASSERT_TRUE(byAlias);
    EXPECT_EQ(byAlias->id().str(), "org.mozilla.firefox");
    auto missing = c.application(appId("org.example.Missing"));
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code(), ErrorCode::NotFound);
}

TEST(CatalogInterface, SearchesWithFiltersAndPages)
{
    fakes::FakeInstalledState installed({installedGimp()});
    fakes::FakeCatalog catalog(7, {gimp(), inkscape(), unwrap(Application::create(fixtures::firefoxSpec()))},
                               &installed);
    const Catalog& c = catalog;

    auto all = c.search(unwrap(SearchQuery::create("editor")));
    ASSERT_TRUE(all);
    EXPECT_EQ(all->generation, 7u);
    EXPECT_EQ(all->total, 2u);
    ASSERT_EQ(all->items.size(), 2u);
    EXPECT_TRUE(all->items[0].installed);
    EXPECT_TRUE(all->items[0].backends.contains(BackendKind::PackageKit));
    EXPECT_FALSE(all->items[1].installed);
    EXPECT_TRUE(all->items[1].backends.contains(BackendKind::Flatpak));

    auto flatpakOnly = c.search(unwrap(SearchQuery::create("editor", {.backend = BackendKind::Flatpak})));
    ASSERT_TRUE(flatpakOnly);
    ASSERT_EQ(flatpakOnly->items.size(), 1u);
    EXPECT_EQ(flatpakOnly->items[0].id.str(), "org.inkscape.Inkscape");

    auto installedOnly = c.search(unwrap(SearchQuery::create("editor", {.installedOnly = true})));
    ASSERT_TRUE(installedOnly);
    ASSERT_EQ(installedOnly->items.size(), 1u);
    EXPECT_EQ(installedOnly->items[0].id.str(), "org.gimp.GIMP");

    auto secondPage = c.search(unwrap(SearchQuery::create("editor", {.limit = 1, .offset = 1})));
    ASSERT_TRUE(secondPage);
    EXPECT_EQ(secondPage->total, 2u);
    ASSERT_EQ(secondPage->items.size(), 1u);
    EXPECT_EQ(secondPage->items[0].id.str(), "org.inkscape.Inkscape");

    auto none = c.search(unwrap(SearchQuery::create("")));
    ASSERT_TRUE(none);
    EXPECT_EQ(none->total, 0u);
}

TEST(CatalogInterface, SummariesKeepOrderAndSkipUnknownIds)
{
    fakes::FakeCatalog catalog(1, {gimp(), inkscape()});
    const std::vector<AppId> ids{appId("org.inkscape.Inkscape"), appId("org.example.Missing"), appId("org.gimp.GIMP")};
    auto r = static_cast<const Catalog&>(catalog).summaries(ids);
    ASSERT_TRUE(r);
    ASSERT_EQ(r->size(), 2u);
    EXPECT_EQ((*r)[0].name, "Inkscape");
    EXPECT_EQ((*r)[1].name, "GIMP");
}

TEST(InstalledStateInterface, ListsFindsAndGroups)
{
    auto flatpakFirefox = unwrap(InstalledItem::create(
            {.offer = unwrap(OfferId::parse("flatpak:user:flathub:app/org.mozilla.firefox/x86_64/stable")),
             .app = appId("org.mozilla.firefox"),
             .backend = BackendKind::Flatpak,
             .scope = Scope::User}));
    auto debFirefox = unwrap(InstalledItem::create({.offer = unwrap(OfferId::parse("packagekit:firefox-esr")),
                                                    .app = appId("org.mozilla.firefox"),
                                                    .backend = BackendKind::PackageKit,
                                                    .scope = Scope::System}));
    fakes::FakeInstalledState state({installedGimp(), flatpakFirefox, debFirefox});
    const InstalledState& s = state;

    EXPECT_EQ(s.items(std::nullopt)->size(), 3u);
    EXPECT_EQ(s.items(BackendKind::Flatpak)->size(), 1u);
    EXPECT_EQ(s.forApplication(appId("org.mozilla.firefox"))->size(), 2u);

    auto found = s.find(unwrap(OfferId::parse("packagekit:gimp")));
    ASSERT_TRUE(found);
    ASSERT_TRUE(found->has_value());
    EXPECT_EQ((*found)->version()->str(), "3.0.4-3+deb13u11");

    auto absent = s.find(unwrap(OfferId::parse("packagekit:inkscape")));
    ASSERT_TRUE(absent);
    EXPECT_FALSE(absent->has_value());
}

TEST(InstalledStateInterface, ReportsReadFailures)
{
    fakes::FakeInstalledState state;
    state.setFailure(Error::internal("database is locked"));
    auto r = static_cast<const InstalledState&>(state).items(std::nullopt);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code(), ErrorCode::Internal);
}

TEST(BackendInterface, CompletesLaterThroughPost)
{
    fakes::PostQueue queue;
    auto fake = std::make_unique<fakes::FakeBackend>(BackendKind::PackageKit, queue);
    fake->installedAnswer = std::vector<InstalledItem>{installedGimp()};
    std::unique_ptr<Backend> backend = std::move(fake);

    int called = 0;
    std::size_t count = 0;
    backend->queryInstalled([&](Result<std::vector<InstalledItem>> r) {
        ++called;
        ASSERT_TRUE(r);
        count = r->size();
    });
    EXPECT_EQ(called, 0) << "a completion must never run inside the call";
    EXPECT_EQ(queue.pending(), 1u);
    queue.runAll();
    EXPECT_EQ(called, 1);
    EXPECT_EQ(count, 1u);
    EXPECT_EQ(backend->kind(), BackendKind::PackageKit);
}

TEST(BackendInterface, ReportsUnavailabilityAsABackendError)
{
    fakes::PostQueue queue;
    fakes::FakeBackend fake(BackendKind::Flatpak, queue);
    fake.availabilityAnswer = Error::backend(BackendFailure::Unavailable, "no user installation");
    Backend& backend = fake;

    std::optional<Error> error;
    backend.availability([&](Result<void> r) {
        if (!r)
            error = r.error();
    });
    queue.runAll();
    ASSERT_TRUE(error);
    EXPECT_EQ(error->code(), ErrorCode::Backend);
    EXPECT_EQ(error->backendFailure(), BackendFailure::Unavailable);
}

TEST(BackendInterface, ReturnsSources)
{
    fakes::PostQueue queue;
    fakes::FakeBackend fake(BackendKind::Flatpak, queue);
    fake.sourcesAnswer = std::vector<Source>{fixtures::flathubSource(Scope::User), fixtures::flathubSource(Scope::System)};
    Backend& backend = fake;

    std::vector<Source> sources;
    backend.sources([&](Result<std::vector<Source>> r) {
        ASSERT_TRUE(r);
        sources = std::move(r).value();
    });
    queue.runAll();
    ASSERT_EQ(sources.size(), 2u);
    EXPECT_EQ(sources[0].kind(), SourceKind::Flathub);
    EXPECT_NE(sources[0].id(), sources[1].id());
    EXPECT_EQ(fake.calls, 1);
}
