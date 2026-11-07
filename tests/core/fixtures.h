#pragma once

// Builders of valid test objects. They abort the test binary through
// gtest's assertion if a fixture itself is invalid.

#include <cutefish/store/application.h>
#include <cutefish/store/offer.h>
#include <cutefish/store/source.h>
#include <cutefish/store/version.h>

#include <gtest/gtest.h>

#include <string>
#include <utility>

namespace fixtures {

using namespace cutefish::store;

template <typename T>
T unwrap(Result<T> r)
{
    if (!r) {
        ADD_FAILURE() << "fixture failed: " << r.error().message();
        std::abort();
    }
    return std::move(r).value();
}

inline AppId appId(const char* id)
{
    return unwrap(AppId::parse(id));
}

inline Source debianSource()
{
    return unwrap(Source::create({.id = unwrap(SourceId::parse("packagekit:debian-stable-main")),
                                  .backend = BackendKind::PackageKit,
                                  .scope = Scope::System,
                                  .kind = SourceKind::Distribution,
                                  .origin = "debian-stable-main",
                                  .label = "Debian 13",
                                  .url = "http://deb.debian.org/debian/",
                                  .priority = 10}));
}

inline Source flathubSource(Scope scope = Scope::User)
{
    std::string id = std::string("flatpak:") + std::string(toString(scope)) + ":flathub";
    return unwrap(Source::create({.id = unwrap(SourceId::parse(id)),
                                  .backend = BackendKind::Flatpak,
                                  .scope = scope,
                                  .kind = SourceKind::Flathub,
                                  .origin = "flathub",
                                  .label = "Flathub",
                                  .url = "https://dl.flathub.org/repo/",
                                  .priority = 20}));
}

inline Offer debOffer(const char* package, const char* version = "128.4.0esr-1")
{
    return unwrap(Offer::create({.format = PackageFormat::Deb,
                                 .package = package,
                                 .version = unwrap(Version::debian(version)),
                                 .architecture = {}},
                                debianSource()));
}

inline Offer flatpakOffer(const char* ref, Scope scope = Scope::User, const char* version = "128.4.0")
{
    return unwrap(Offer::create({.format = PackageFormat::Flatpak,
                                 .package = ref,
                                 .version = unwrap(Version::upstream(version)),
                                 .architecture = {}},
                                flathubSource(scope)));
}

inline ApplicationSpec firefoxSpec()
{
    ApplicationSpec spec{.id = appId("org.mozilla.firefox"),
                         .name = "Firefox",
                         .summary = "Navigateur web",
                         .description = {{DescriptionBlockType::Paragraph, "A web browser."}},
                         .icon = {IconKind::LocalPath, "/var/lib/swcatalog/icons/debian/64x64/firefox.png"},
                         .categories = {"Network", "WebBrowser"},
                         .aliases = {appId("firefox-esr")},
                         .offers = {}};
    spec.offers.push_back(debOffer("firefox-esr"));
    spec.offers.push_back(flatpakOffer("app/org.mozilla.firefox/x86_64/stable"));
    return spec;
}

} // namespace fixtures
