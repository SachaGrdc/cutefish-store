#include <cutefish/store/defaultoffer.h>

#include <algorithm>
#include <tuple>

namespace cutefish::store {

std::string_view toString(OfferRank rank) noexcept
{
    switch (rank) {
    case OfferRank::Flathub: return "flathub";
    case OfferRank::Distribution: return "distribution";
    case OfferRank::ThirdPartyFlatpak: return "third-party-flatpak";
    case OfferRank::Unranked: return "unranked";
    }
    return "unranked";
}

OfferRank offerRank(const Offer& offer) noexcept
{
    switch (offer.sourceKind()) {
    case SourceKind::Flathub:
        return OfferRank::Flathub; // Source::create allows Flathub only for Flatpak.
    case SourceKind::Distribution:
        return offer.backend() == BackendKind::PackageKit ? OfferRank::Distribution : OfferRank::Unranked;
    case SourceKind::ThirdParty:
        return offer.backend() == BackendKind::Flatpak ? OfferRank::ThirdPartyFlatpak : OfferRank::Unranked;
    }
    return OfferRank::Unranked;
}

namespace {

// Smaller is better. The offer id makes the key total for distinct offers.
auto sortKey(const Offer& offer, std::span<const OfferId> installed)
{
    const bool isInstalled = std::find(installed.begin(), installed.end(), offer.id()) != installed.end();
    return std::make_tuple(!isInstalled, offerRank(offer), offer.scope() == Scope::User ? 0 : 1,
                           std::string_view(offer.id().str()));
}

} // namespace

const Offer* defaultOffer(std::span<const Offer> offers, std::span<const OfferId> installed) noexcept
{
    const Offer* best = nullptr;
    for (const Offer& offer : offers)
        if (!best || sortKey(offer, installed) < sortKey(*best, installed))
            best = &offer;
    return best;
}

const Offer& defaultOffer(const Application& app, std::span<const OfferId> installed) noexcept
{
    return *defaultOffer(app.offers(), installed);
}

} // namespace cutefish::store
