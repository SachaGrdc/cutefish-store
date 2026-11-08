#include "catalogfixture.h"

#include <cutefish/store/applicationlinks.h>
#include <cutefish/store/sqlitecatalog.h>

#include <gtest/gtest.h>

using namespace cutefish::store;
using catalogfixture::TempDir;
using fixtures::appId;
using fixtures::unwrap;

namespace {

OfferId offer(const char* id) { return unwrap(OfferId::parse(id)); }

InstalledItem deb(const char* package, const char* app = nullptr)
{
    InstalledItemSpec spec{.offer = offer((std::string("packagekit:") + package).c_str()),
                           .backend = BackendKind::PackageKit,
                           .scope = Scope::System,
                           .version = unwrap(Version::debian("1.0-1")),
                           .origin = "debian-stable-main"};
    if (app)
        spec.app = appId(app);
    return unwrap(InstalledItem::create(std::move(spec)));
}

AvailableUpdate update(const char* offerId, PlanItemKind kind = PlanItemKind::Package)
{
    return {.offer = offer(offerId), .app = std::nullopt, .kind = kind, .fromVersion = {}, .toVersion = {},
            .downloadSize = 10};
}

} // namespace

TEST(ApplicationLinks, CatalogueMapsOffersToApplications)
{
    TempDir dir;
    catalogfixture::build(dir / "catalog.sqlite", 1);
    auto c = unwrap(SqliteCatalog::open(dir / "catalog.sqlite"));
    const std::vector<OfferId> asked{offer("packagekit:gimp"), offer("packagekit:libfoo1"),
                                     offer("flatpak:user:flathub:app/org.qgis.qgis/x86_64/lts")};
    const auto links = unwrap(c.offerApplications(asked));
    ASSERT_EQ(links.size(), 2u); // libfoo1 is no application's offer
    EXPECT_EQ(links[0], (OfferApplication{offer("packagekit:gimp"), appId("org.gimp.GIMP")}));
    EXPECT_EQ(links[1].second, appId("org.qgis.qgis"));
    EXPECT_TRUE(unwrap(c.offerApplications({})).empty());
}

TEST(ApplicationLinks, LinksInstalledItems)
{
    const std::vector<OfferApplication> links{{offer("packagekit:gimp"), appId("org.gimp.GIMP")},
                                              {offer("packagekit:viewer"), appId("org.example.Zeta")},
                                              {offer("packagekit:viewer"), appId("org.example.Alpha")}};
    const auto items = linkInstalled({deb("gimp"), deb("libfoo1"), deb("viewer"), deb("gitg", "org.gnome.gitg")}, links);
    ASSERT_EQ(items.size(), 4u);
    EXPECT_EQ(items[0].app(), std::optional<AppId>(appId("org.gimp.GIMP")));
    EXPECT_FALSE(items[1].app()); // not in the catalogue
    EXPECT_EQ(items[2].app(), std::optional<AppId>(appId("org.example.Alpha"))); // first by id
    EXPECT_EQ(items[3].app(), std::optional<AppId>(appId("org.gnome.gitg"))); // the backend's link stays
    EXPECT_EQ(items[0].version()->str(), "1.0-1");
    EXPECT_EQ(items[0].origin(), "debian-stable-main");
}

TEST(ApplicationLinks, D010KeepsApplicationUpdatesOnly)
{
    const std::vector<OfferApplication> links{{offer("packagekit:gimp"), appId("org.gimp.GIMP")}};
    const auto kept = applicationUpdates(
            {update("packagekit:gimp"), update("packagekit:linux-image-amd64"), update("packagekit:zlib1g"),
             update("flatpak:user:flathub:runtime/org.gnome.Platform/x86_64/48", PlanItemKind::Runtime)},
            links);
    ASSERT_EQ(kept.size(), 2u);
    EXPECT_EQ(kept[0].offer, offer("packagekit:gimp"));
    EXPECT_EQ(kept[0].app, std::optional<AppId>(appId("org.gimp.GIMP")));
    EXPECT_EQ(kept[0].kind, PlanItemKind::Application);
    EXPECT_EQ(kept[1].kind, PlanItemKind::Runtime); // runtimes of installed Flatpak applications
    EXPECT_TRUE(applicationUpdates({update("packagekit:zlib1g")}, {}).empty());
}
