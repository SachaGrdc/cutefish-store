#include <cutefish/store/identifiers.h>

#include <gtest/gtest.h>

#include <string>

using namespace cutefish::store;

TEST(AppId, AcceptsRealCatalogueIds)
{
    // Forms found in the Debian DEP-11 and Flathub catalogues (prototype 3).
    for (const char* id : {"org.gnome.gitg", "org.mozilla.firefox", "io.github.mhogomchungu.sirikali",
                           "4Pane", "3dchess", "eog-pythonconsole", "tintin++", "xiterm+thai",
                           "com.github.rafostar.Clapper_Enhancers"}) {
        auto r = AppId::parse(id);
        ASSERT_TRUE(r) << id << ": " << r.error().message();
        EXPECT_EQ(r->str(), id);
    }
}

TEST(AppId, RejectsMalformedIds)
{
    for (const char* id : {"", ".org.example", "org.example.", "org..example", "org example", "org/example",
                           "org.exämple", "org.example\n"}) {
        auto r = AppId::parse(id);
        ASSERT_FALSE(r) << id;
        EXPECT_EQ(r.error().code(), ErrorCode::InvalidArgument);
    }
    EXPECT_FALSE(AppId::parse(std::string(256, 'a')));
    EXPECT_TRUE(AppId::parse(std::string(255, 'a')));
}

TEST(AppId, ParseRefusesTheLegacySuffix)
{
    EXPECT_FALSE(AppId::parse("org.gnome.Gedit.desktop"));
}

TEST(AppId, FromComponentIdRemovesTheLegacySuffix)
{
    auto r = AppId::fromComponentId("org.kde.ktimetracker.desktop");
    ASSERT_TRUE(r);
    EXPECT_EQ(r->str(), "org.kde.ktimetracker");
    EXPECT_EQ(AppId::fromComponentId("homebank.desktop")->str(), "homebank");
    EXPECT_EQ(AppId::fromComponentId("org.gnome.gitg")->str(), "org.gnome.gitg");
    EXPECT_FALSE(AppId::fromComponentId(".desktop"));
}

TEST(AppId, IsCaseSensitiveAndOrdered)
{
    const AppId a = *AppId::parse("io.github.mhogomchungu.SiriKali");
    const AppId b = *AppId::parse("io.github.mhogomchungu.sirikali");
    EXPECT_NE(a, b);
    EXPECT_LT(a, b);
}

TEST(OfferId, IsOpaqueButWellFormed)
{
    EXPECT_TRUE(OfferId::parse("packagekit:firefox-esr"));
    EXPECT_TRUE(OfferId::parse("flatpak:user:flathub:app/org.mozilla.firefox/x86_64/stable"));
    EXPECT_FALSE(OfferId::parse(""));
    EXPECT_FALSE(OfferId::parse("packagekit: firefox"));
    EXPECT_FALSE(OfferId::parse("packagekit:fire\x7f"));
    EXPECT_FALSE(OfferId::parse(std::string(256, 'x')));
}

TEST(SourceId, IsOpaqueButWellFormed)
{
    EXPECT_TRUE(SourceId::parse("flatpak:user:flathub"));
    EXPECT_FALSE(SourceId::parse("with space"));
    auto empty = SourceId::parse("");
    ASSERT_FALSE(empty);
    EXPECT_EQ(empty.error().code(), ErrorCode::InvalidArgument);
}
