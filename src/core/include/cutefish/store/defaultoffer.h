#pragma once

#include <cutefish/store/application.h>
#include <cutefish/store/identifiers.h>
#include <cutefish/store/offer.h>

#include <cstdint>
#include <span>
#include <string_view>

namespace cutefish::store {

/// Installation priority of an offer (D-009, ARCHITECTURE §6.4), from the
/// nature of its source and its backend. It never uses Source::priority,
/// which only decides the displayed texts (editorial priority).
enum class OfferRank : std::uint8_t {
    Flathub,           ///< A Flatpak offer from Flathub.
    Distribution,      ///< A deb from the official repository of the distribution.
    ThirdPartyFlatpak, ///< A Flatpak offer from another remote.
    /// Anything D-009 does not rank: a deb from a third-party APT repository
    /// (SERVICE.md Q-13), a Flatpak remote marked as the distribution's.
    /// Never chosen before a ranked offer.
    Unranked,
};

/// "flathub", "distribution", "third-party-flatpak", "unranked".
std::string_view toString(OfferRank rank) noexcept;

OfferRank offerRank(const Offer& offer) noexcept;

/// The offer proposed by default among `offers` (D-009):
/// 1. an installed offer (its id is in `installed`), before any other;
/// 2. then by rank: Flathub, distribution, third-party Flatpak, unranked;
/// 3. then the user installation before the system one (D-005);
/// 4. then the smallest offer id, so that the choice never depends on the
///    order of `offers` or `installed`.
/// Returns nullptr when `offers` is empty. Computed when reading, never
/// stored (it depends on the installed state). The user may always install
/// another offer.
const Offer* defaultOffer(std::span<const Offer> offers, std::span<const OfferId> installed) noexcept;

/// Same rule for an application, which always has at least one offer.
const Offer& defaultOffer(const Application& app, std::span<const OfferId> installed) noexcept;

} // namespace cutefish::store
