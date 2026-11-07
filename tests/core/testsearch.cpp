#include <cutefish/store/search.h>

#include <gtest/gtest.h>

#include <string>

using namespace cutefish::store;

TEST(SearchQuery, HasTheDocumentedDefaults)
{
    auto q = SearchQuery::create("éditeur");
    ASSERT_TRUE(q);
    EXPECT_EQ(q->text(), "éditeur");
    EXPECT_EQ(q->options().limit, 60u);
    EXPECT_EQ(q->options().offset, 0u);
    EXPECT_TRUE(q->options().category.empty());
    EXPECT_FALSE(q->options().backend);
    EXPECT_FALSE(q->options().installedOnly);
}

TEST(SearchQuery, KeepsUserTextAsIs)
{
    // FTS5 syntax is text like any other: the catalogue turns it into terms.
    auto q = SearchQuery::create("fire NEAR(fox) OR \"x\"*");
    ASSERT_TRUE(q);
    EXPECT_EQ(q->text(), "fire NEAR(fox) OR \"x\"*");
}

TEST(SearchQuery, AcceptsEmptyText)
{
    EXPECT_TRUE(SearchQuery::create(""));
}

TEST(SearchQuery, RejectsInvalidText)
{
    EXPECT_FALSE(SearchQuery::create(std::string(257, 'a')));
    EXPECT_TRUE(SearchQuery::create(std::string(256, 'a')));
    EXPECT_FALSE(SearchQuery::create("\xFF"));
    EXPECT_FALSE(SearchQuery::create("tab\there"));
}

TEST(SearchQuery, ValidatesOptions)
{
    EXPECT_FALSE(SearchQuery::create("x", {.limit = 0}));
    EXPECT_FALSE(SearchQuery::create("x", {.limit = 201}));
    EXPECT_TRUE(SearchQuery::create("x", {.limit = 200}));
    EXPECT_TRUE(SearchQuery::create("x", {.category = "Graphics", .backend = BackendKind::Flatpak}));
    auto r = SearchQuery::create("x", {.category = "Bad Category"});
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code(), ErrorCode::InvalidArgument);
}

TEST(BackendSet, StoresEachBackend)
{
    BackendSet set;
    EXPECT_TRUE(set.empty());
    set.insert(BackendKind::Flatpak);
    EXPECT_TRUE(set.contains(BackendKind::Flatpak));
    EXPECT_FALSE(set.contains(BackendKind::PackageKit));
    set.insert(BackendKind::PackageKit);
    set.insert(BackendKind::PackageKit);
    EXPECT_TRUE(set.contains(BackendKind::PackageKit));
    EXPECT_FALSE(set.empty());
    static_assert(sizeof(BackendSet) == 1);
}
