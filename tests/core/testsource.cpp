#include "fixtures.h"

#include <gtest/gtest.h>

using namespace cutefish::store;
using fixtures::unwrap;

namespace {

SourceSpec spec(BackendKind backend, Scope scope, SourceKind kind, std::string origin = "origin")
{
    return {.id = unwrap(SourceId::parse("test:source")),
            .backend = backend,
            .scope = scope,
            .kind = kind,
            .origin = std::move(origin)};
}

} // namespace

TEST(Source, KeepsItsProvenance)
{
    const Source s = fixtures::debianSource();
    EXPECT_EQ(s.id().str(), "packagekit:debian-stable-main");
    EXPECT_EQ(s.backend(), BackendKind::PackageKit);
    EXPECT_EQ(s.scope(), Scope::System);
    EXPECT_EQ(s.kind(), SourceKind::Distribution);
    EXPECT_EQ(s.origin(), "debian-stable-main");
    EXPECT_EQ(s.label(), "Debian 13");
    EXPECT_TRUE(s.enabled());
    EXPECT_EQ(s.priority(), 10);
}

TEST(Source, DistinguishesTheTwoFlatpakInstallations)
{
    const Source user = fixtures::flathubSource(Scope::User);
    const Source system = fixtures::flathubSource(Scope::System);
    EXPECT_NE(user.id(), system.id());
    EXPECT_EQ(user.origin(), system.origin());
    EXPECT_EQ(user.scope(), Scope::User);
    EXPECT_EQ(system.scope(), Scope::System);
}

TEST(Source, PackageKitSourcesAreSystemWide)
{
    auto r = Source::create(spec(BackendKind::PackageKit, Scope::User, SourceKind::Distribution));
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code(), ErrorCode::InvalidArgument);
}

TEST(Source, OnlyAFlatpakSourceCanBeFlathub)
{
    EXPECT_FALSE(Source::create(spec(BackendKind::PackageKit, Scope::System, SourceKind::Flathub)));
    EXPECT_TRUE(Source::create(spec(BackendKind::Flatpak, Scope::System, SourceKind::Flathub)));
    EXPECT_TRUE(Source::create(spec(BackendKind::Flatpak, Scope::User, SourceKind::ThirdParty)));
}

TEST(Source, ValidatesItsFields)
{
    EXPECT_FALSE(Source::create(spec(BackendKind::Flatpak, Scope::User, SourceKind::ThirdParty, "")));
    EXPECT_FALSE(Source::create(spec(BackendKind::Flatpak, Scope::User, SourceKind::ThirdParty, "two words")));

    auto badLabel = spec(BackendKind::Flatpak, Scope::User, SourceKind::ThirdParty);
    badLabel.label = "line\nbreak";
    EXPECT_FALSE(Source::create(std::move(badLabel)));

    auto badUrl = spec(BackendKind::Flatpak, Scope::User, SourceKind::ThirdParty);
    badUrl.url = "https://example.org/a b";
    EXPECT_FALSE(Source::create(std::move(badUrl)));

    auto translated = spec(BackendKind::Flatpak, Scope::User, SourceKind::ThirdParty);
    translated.label = "Dépôt de test";
    EXPECT_TRUE(Source::create(std::move(translated)));
}

TEST(Source, KindNamesRoundTrip)
{
    for (const BackendKind k : {BackendKind::PackageKit, BackendKind::Flatpak})
        EXPECT_EQ(parseBackendKind(toString(k)), k);
    for (const Scope s : {Scope::System, Scope::User})
        EXPECT_EQ(parseScope(toString(s)), s);
    for (const SourceKind k : {SourceKind::Distribution, SourceKind::Flathub, SourceKind::ThirdParty})
        EXPECT_EQ(parseSourceKind(toString(k)), k);
    EXPECT_EQ(toString(SourceKind::ThirdParty), "third-party");
    EXPECT_FALSE(parseBackendKind("apt"));
    EXPECT_FALSE(parseScope(""));
}
