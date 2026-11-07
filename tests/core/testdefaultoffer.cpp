#include "fixtures.h"

#include <cutefish/store/defaultoffer.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <numeric>
#include <vector>

using namespace cutefish::store;
using fixtures::unwrap;

namespace {

const char* kRef = "app/org.mozilla.firefox/x86_64/stable";

Offer flathubUser() { return fixtures::flatpakOffer(kRef, Scope::User); }
Offer flathubSystem() { return fixtures::flatpakOffer(kRef, Scope::System); }
Offer debianDeb() { return fixtures::debOffer("firefox-esr"); }
Offer thirdPartyFlatpak(const char* remote = "kde-nightly")
{
    return fixtures::flatpakOfferFrom(fixtures::thirdPartyFlatpakSource(remote), kRef);
}
// A deb has the id `packagekit:<package>`: a third-party repository must
// ship another package name to coexist with the Debian one in a test.
Offer vendorDeb() { return fixtures::debOfferFrom(fixtures::thirdPartyAptSource(), "firefox"); }
Offer distributionFlatpak() { return fixtures::flatpakOfferFrom(fixtures::distributionFlatpakSource(), kRef); }

std::string chosen(const std::vector<Offer>& offers, const std::vector<OfferId>& installed = {})
{
    const Offer* best = defaultOffer(offers, installed);
    return best ? best->id().str() : std::string("<none>");
}

} // namespace

TEST(OfferRank, FollowsSourceKindAndBackend)
{
    EXPECT_EQ(offerRank(flathubUser()), OfferRank::Flathub);
    EXPECT_EQ(offerRank(flathubSystem()), OfferRank::Flathub);
    EXPECT_EQ(offerRank(debianDeb()), OfferRank::Distribution);
    EXPECT_EQ(offerRank(thirdPartyFlatpak()), OfferRank::ThirdPartyFlatpak);
    EXPECT_EQ(offerRank(vendorDeb()), OfferRank::Unranked);
    EXPECT_EQ(offerRank(distributionFlatpak()), OfferRank::Unranked);
    EXPECT_EQ(toString(OfferRank::ThirdPartyFlatpak), "third-party-flatpak");
}

TEST(DefaultOffer, NothingToChooseFrom)
{
    EXPECT_EQ(defaultOffer(std::span<const Offer>{}, {}), nullptr);
}

TEST(DefaultOffer, FlathubBeforeDistributionBeforeThirdPartyFlatpak)
{
    EXPECT_EQ(chosen({debianDeb(), flathubUser(), thirdPartyFlatpak()}), flathubUser().id().str());
    EXPECT_EQ(chosen({thirdPartyFlatpak(), debianDeb()}), debianDeb().id().str());
    EXPECT_EQ(chosen({vendorDeb(), thirdPartyFlatpak()}), thirdPartyFlatpak().id().str());
}

TEST(DefaultOffer, UnrankedOffersComeLast)
{
    EXPECT_EQ(chosen({vendorDeb(), debianDeb()}), debianDeb().id().str());
    EXPECT_EQ(chosen({distributionFlatpak(), debianDeb()}), debianDeb().id().str());
    EXPECT_EQ(chosen({distributionFlatpak(), thirdPartyFlatpak()}), thirdPartyFlatpak().id().str());
    // Alone, an unranked offer is still proposed: the application must be
    // installable.
    EXPECT_EQ(chosen({vendorDeb()}), vendorDeb().id().str());
}

TEST(DefaultOffer, UserInstallationBeforeSystem)
{
    EXPECT_EQ(chosen({flathubSystem(), flathubUser()}), flathubUser().id().str());
    auto thirdSystem = fixtures::flatpakOfferFrom(fixtures::thirdPartyFlatpakSource("kde-nightly", Scope::System), kRef);
    EXPECT_EQ(chosen({thirdSystem, thirdPartyFlatpak()}), thirdPartyFlatpak().id().str());
    // The rank comes first: a system Flathub offer beats a user third-party one.
    EXPECT_EQ(chosen({thirdPartyFlatpak(), flathubSystem()}), flathubSystem().id().str());
}

TEST(DefaultOffer, InstalledOfferComesFirst)
{
    const std::vector<Offer> all{flathubUser(), flathubSystem(), debianDeb(), thirdPartyFlatpak(), vendorDeb()};
    EXPECT_EQ(chosen(all, {debianDeb().id()}), debianDeb().id().str());
    EXPECT_EQ(chosen(all, {flathubSystem().id()}), flathubSystem().id().str());
    EXPECT_EQ(chosen(all, {vendorDeb().id()}), vendorDeb().id().str());
    // Several installed offers: the same order applies among them.
    EXPECT_EQ(chosen(all, {thirdPartyFlatpak().id(), debianDeb().id()}), debianDeb().id().str());
    EXPECT_EQ(chosen(all, {flathubSystem().id(), debianDeb().id()}), flathubSystem().id().str());
    // An installed id that is not among the offers changes nothing.
    EXPECT_EQ(chosen(all, {unwrap(OfferId::parse("packagekit:unrelated"))}), flathubUser().id().str());
}

TEST(DefaultOffer, SameRankAndScopeIsDecidedByOfferId)
{
    auto a = thirdPartyFlatpak("aaa-remote");
    auto z = thirdPartyFlatpak("zzz-remote");
    EXPECT_EQ(chosen({z, a}), a.id().str());
    EXPECT_EQ(chosen({a, z}), a.id().str());
}

TEST(DefaultOffer, IgnoresEditorialPriority)
{
    // Source::priority decides the displayed texts, not the offer. Here the
    // third-party remote has the highest editorial priority (30), Debian
    // the lowest (10), Flathub in between (20).
    auto third = thirdPartyFlatpak();
    auto flathub = flathubUser();
    auto deb = debianDeb();
    EXPECT_EQ(chosen({third, deb, flathub}), flathub.id().str());

    auto lowFlathub = unwrap(Source::create({.id = unwrap(SourceId::parse("flatpak:user:flathub")),
                                             .backend = BackendKind::Flatpak,
                                             .scope = Scope::User,
                                             .kind = SourceKind::Flathub,
                                             .origin = "flathub",
                                             .priority = -100}));
    EXPECT_EQ(chosen({third, deb, fixtures::flatpakOfferFrom(lowFlathub, kRef)}), flathub.id().str());
}

TEST(DefaultOffer, DoesNotDependOnOrder)
{
    const std::vector<Offer> base{flathubUser(), flathubSystem(), debianDeb(),         thirdPartyFlatpak("aaa"),
                                  thirdPartyFlatpak("zzz"), vendorDeb(), distributionFlatpak()};
    const std::vector<std::vector<OfferId>> installedSets{
            {},
            {debianDeb().id()},
            {flathubSystem().id(), vendorDeb().id()},
            {thirdPartyFlatpak("zzz").id(), thirdPartyFlatpak("aaa").id()},
            {distributionFlatpak().id(), vendorDeb().id()},
    };
    for (const auto& installedIn : installedSets) {
        const std::string expected = chosen(base, installedIn);
        std::vector<std::size_t> order(base.size());
        std::iota(order.begin(), order.end(), 0);
        int permutations = 0;
        do {
            std::vector<Offer> offers;
            for (std::size_t i : order)
                offers.push_back(base[i]);
            auto installed = installedIn;
            std::reverse(installed.begin(), installed.end());
            ASSERT_EQ(chosen(offers, installed), expected);
            ++permutations;
        } while (std::next_permutation(order.begin(), order.end()));
        EXPECT_EQ(permutations, 5040);
    }
}

TEST(DefaultOffer, ForAnApplication)
{
    auto app = unwrap(Application::create(fixtures::firefoxSpec())); // deb, then Flathub user
    EXPECT_EQ(defaultOffer(app, {}).id().str(), flathubUser().id().str());
    // The catalogue lists the deb first (editorial priority): the default
    // offer is still Flathub.
    EXPECT_EQ(app.offers().front().id().str(), "packagekit:firefox-esr");
    const std::vector<OfferId> installed{unwrap(OfferId::parse("packagekit:firefox-esr"))};
    EXPECT_EQ(defaultOffer(app, installed).id().str(), "packagekit:firefox-esr");
}
