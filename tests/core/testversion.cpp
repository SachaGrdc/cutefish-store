#include <cutefish/store/version.h>

#include <gtest/gtest.h>

#include <string>

using namespace cutefish::store;

namespace {

struct Case
{
    const char* a;
    const char* b;
    const char* expected; // "less", "equivalent" or "greater"
};

// Expected results produced by `dpkg --compare-versions` (dpkg 1.22.22,
// Debian 13) on 7 November 2026, then written here so that the test needs
// no dpkg. The last lines are real versions seen on the audit machine.
constexpr Case kDpkgCases[] = {
    {"1.0", "1.0", "equivalent"},
    {"1.0", "0:1.0", "equivalent"},
    {"1.0", "1.0-0", "equivalent"},
    {"1.0~rc1", "1.0", "less"},
    {"1.0~~", "1.0~", "less"},
    {"1.0~", "1.0", "less"},
    {"1.0", "1.0a", "less"},
    {"1.0a", "1.0+", "less"},
    {"1.0+", "1.0.", "less"},
    {"1.0", "1.0.0", "less"},
    {"1.2", "1.10", "less"},
    {"1.010", "1.10", "equivalent"},
    {"2:0.1", "1:9.9", "greater"},
    {"1.0-1", "1.0-2", "less"},
    {"1.0-1", "1.0-1.1", "less"},
    {"1.0-1~bpo1", "1.0-1", "less"},
    {"1.0-1+deb13u1", "1.0-1", "greater"},
    {"0.9", "1.0~alpha", "less"},
    {"1.0-a", "1.0-1", "greater"},
    {"7.0", "7", "greater"},
    {"1.0-1", "1.0-01", "equivalent"},
    {"1:1.3.dfsg+really1.3.1-1+b1", "1:1.3.dfsg+really1.3.1-1+deb13u1", "less"},
    {"3.0.4-3+deb13u11", "3.0.4-3+deb13u2", "greater"},
    {"1.0~git20261106141003.538e010-1", "1.0~git20261104215447.9aa396e-1", "greater"},
};

std::string name(std::weak_ordering o)
{
    if (o < 0)
        return "less";
    if (o > 0)
        return "greater";
    return "equivalent";
}

std::string reversed(const std::string& s)
{
    return s == "less" ? "greater" : s == "greater" ? "less" : s;
}

} // namespace

TEST(DebianVersion, OrdersLikeDpkg)
{
    for (const Case& c : kDpkgCases) {
        auto a = Version::debian(c.a);
        auto b = Version::debian(c.b);
        ASSERT_TRUE(a) << c.a << ": " << a.error().message();
        ASSERT_TRUE(b) << c.b << ": " << b.error().message();
        auto order = compare(*a, *b);
        ASSERT_TRUE(order) << c.a << " vs " << c.b;
        EXPECT_EQ(name(*order), c.expected) << c.a << " vs " << c.b;
        auto back = compare(*b, *a);
        ASSERT_TRUE(back);
        EXPECT_EQ(name(*back), reversed(c.expected)) << c.b << " vs " << c.a;
    }
}

TEST(DebianVersion, SplitsEpochUpstreamAndRevision)
{
    auto v = Version::debian("1:1.3.dfsg+really1.3.1-1+deb13u1");
    ASSERT_TRUE(v);
    EXPECT_EQ(v->scheme(), VersionScheme::Debian);
    EXPECT_EQ(v->epoch(), 1u);
    EXPECT_EQ(v->upstreamPart(), "1.3.dfsg+really1.3.1");
    EXPECT_EQ(v->revision(), "1+deb13u1");

    auto plain = Version::debian("2.42.4");
    ASSERT_TRUE(plain);
    EXPECT_EQ(plain->epoch(), 0u);
    EXPECT_EQ(plain->upstreamPart(), "2.42.4");
    EXPECT_EQ(plain->revision(), "");

    // As in dpkg: the revision starts after the last hyphen, and a colon may
    // appear in the upstream part once an epoch is present.
    auto hyphens = Version::debian("1.0-1-2");
    ASSERT_TRUE(hyphens);
    EXPECT_EQ(hyphens->upstreamPart(), "1.0-1");
    EXPECT_EQ(hyphens->revision(), "2");
    auto colons = Version::debian("1:2:3");
    ASSERT_TRUE(colons);
    EXPECT_EQ(colons->upstreamPart(), "2:3");
}

TEST(DebianVersion, RejectsWhatDpkgRejectsOrWarnsAbout)
{
    // dpkg errors: "1:", ":1.0", "1.0-", "x:1.0". dpkg warnings, rejected
    // here too: "a1.0", "1_0", "1.0-1_2". Whitespace is never accepted.
    for (const char* text : {"", "1:", ":1.0", "1.0-", "x:1.0", "a1.0", "1_0", "1.0-1_2", "1.0 ", " 1.0",
                             "99999999999:1.0"}) {
        auto v = Version::debian(text);
        ASSERT_FALSE(v) << '"' << text << '"';
        EXPECT_EQ(v.error().code(), ErrorCode::InvalidArgument);
    }
}

TEST(DebianVersion, IdentityIsNotOrdering)
{
    const Version a = *Version::debian("1.0");
    const Version b = *Version::debian("0:1.0");
    EXPECT_NE(a, b);
    EXPECT_EQ(name(*compare(a, b)), "equivalent");
}

TEST(UpstreamVersion, IsAcceptedButNeverOrdered)
{
    auto a = Version::upstream("124.0.1");
    auto b = Version::upstream("125.0");
    ASSERT_TRUE(a);
    ASSERT_TRUE(b);
    EXPECT_EQ(a->scheme(), VersionScheme::Upstream);
    EXPECT_EQ(a->upstreamPart(), "124.0.1");

    auto order = compare(*a, *b);
    ASSERT_FALSE(order);
    EXPECT_EQ(order.error().code(), ErrorCode::NotSupported);

    // Even identical texts are not declared equivalent: equality of two
    // upstream strings says nothing about the builds behind them.
    EXPECT_FALSE(compare(*a, *a));
    EXPECT_EQ(*a, *Version::upstream("124.0.1"));
}

TEST(UpstreamVersion, RejectsEmptyOrControlText)
{
    EXPECT_FALSE(Version::upstream(""));
    EXPECT_FALSE(Version::upstream("1.0\n"));
    EXPECT_FALSE(Version::upstream(std::string(256, '1')));
    EXPECT_TRUE(Version::upstream("3.0 beta 2"));
}

TEST(VersionComparison, RefusesDifferentSchemes)
{
    // The same text in two schemes is still not comparable: the Debian
    // package version and the upstream version of a Flatpak offer come from
    // different systems (D-009).
    const Version deb = *Version::debian("124.0.1-1");
    const Version flatpak = *Version::upstream("124.0.1-1");
    auto order = compare(deb, flatpak);
    ASSERT_FALSE(order);
    EXPECT_EQ(order.error().code(), ErrorCode::NotSupported);
    EXPECT_NE(deb, flatpak);
}

TEST(VersionScheme, NamesAreStable)
{
    EXPECT_EQ(toString(VersionScheme::Debian), "debian");
    EXPECT_EQ(toString(VersionScheme::Upstream), "upstream");
}
