// The service's pure parts: options, sources file, D-Bus conversions.

#include "marshal.h"
#include "options.h"
#include "sourcesfile.h"

#include <gtest/gtest.h>

using namespace cutefish::store;
using namespace cutefish::store::service;

TEST(ServiceOptions, DefaultsAndOverrides)
{
    auto d = parseArguments({});
    ASSERT_TRUE(d);
    EXPECT_EQ(d->idleTimeout, std::chrono::minutes(5)); // P-010
    EXPECT_EQ(d->transactionRetention, std::chrono::minutes(5));
    EXPECT_EQ(d->indexLocales, std::vector<std::string>{"fr"}); // D-013
    EXPECT_EQ(d->dataDir.filename(), "cutefish-store");

    auto o = parseArguments({"--data-dir", "/tmp/x", "--idle-timeout-ms", "250", "--locale", "de_DE.UTF-8"});
    ASSERT_TRUE(o);
    EXPECT_EQ(o->dataDir, "/tmp/x");
    EXPECT_EQ(o->idleTimeout, std::chrono::milliseconds(250));
    EXPECT_EQ(o->readLocales, std::vector<std::string>{"de_DE.UTF-8"});

    EXPECT_FALSE(parseArguments({"--idle-timeout-ms", "0"}));
    EXPECT_FALSE(parseArguments({"--idle-timeout-ms", "10x"}));
    EXPECT_FALSE(parseArguments({"--unknown", "1"}));
    EXPECT_FALSE(parseArguments({"--data-dir"}));
}

TEST(SourcesFile, ParsesSourcesAndComments)
{
    auto s = parseSources("# comment\n\n"
                          "  id=packagekit:debian-main,backend=packagekit,scope=system,kind=distribution,"
                          "origin=debian-main,priority=10,file=/var/lib/swcatalog/yaml/main.yml.gz,"
                          "icons=/var/lib/swcatalog/icons/debian,label=Debian 13\r\n"
                          "id=flatpak:user:flathub,backend=flatpak,scope=user,kind=flathub,origin=flathub,"
                          "priority=20,file=/home/u/.local/share/flatpak/appstream/flathub/x86_64/active/appstream.xml.gz\n");
    ASSERT_TRUE(s) << s.error().message();
    ASSERT_EQ(s->size(), 2u);
    EXPECT_EQ((*s)[0].source.label(), "Debian 13");
    EXPECT_EQ((*s)[0].files.icons, "/var/lib/swcatalog/icons/debian");
    EXPECT_EQ((*s)[1].source.kind(), SourceKind::Flathub);
    EXPECT_TRUE((*s)[1].files.icons.empty());

    // The indexer's argument reads back to the same source.
    auto again = parseSources(indexerArgument((*s)[0]));
    ASSERT_TRUE(again);
    EXPECT_EQ((*again)[0].source.id(), (*s)[0].source.id());
    EXPECT_EQ((*again)[0].files.metadata, (*s)[0].files.metadata);
    EXPECT_EQ((*again)[0].source.priority(), 10);
}

TEST(SourcesFile, RefusesMalformedLines)
{
    const std::string ok = "id=packagekit:a,backend=packagekit,scope=system,kind=distribution,origin=a,priority=1,file=/a\n";
    for (const char* bad : {"id=packagekit:a,backend=packagekit\n",
                            "id=packagekit:a,backend=packagekit,scope=system,kind=distribution,origin=a,priority=x,file=/a\n",
                            "id=packagekit:a,backend=packagekit,scope=system,kind=distribution,origin=a,priority=1,file=relative\n",
                            "id=packagekit:a,backend=apt,scope=system,kind=distribution,origin=a,priority=1,file=/a\n",
                            "id=packagekit:a,backend=packagekit,scope=user,kind=distribution,origin=a,priority=1,file=/a\n",
                            "id=packagekit:a,backend=packagekit,scope=system,kind=distribution,origin=a,priority=1,file=/a,colour=red\n",
                            "just text\n"}) {
        auto r = parseSources(bad);
        ASSERT_FALSE(r) << bad;
        EXPECT_NE(r.error().message().find("line 1"), std::string::npos) << r.error().message();
    }
    auto twice = parseSources(ok + ok);
    ASSERT_FALSE(twice);
    EXPECT_NE(twice.error().message().find("duplicate"), std::string::npos);
    EXPECT_TRUE(readSources("/nonexistent/sources.conf")->empty()); // no file: no source
}

TEST(Marshal, SearchOptions)
{
    namespace dbus = cutefish::base::dbus;
    auto o = searchOptions({{"limit", std::uint32_t(20)}, {"offset", std::uint32_t(40)},
                            {"category", std::string("Graphics")}, {"backend", std::string("flatpak")},
                            {"installed-only", true}});
    ASSERT_TRUE(o);
    EXPECT_EQ(o->limit, 20u);
    EXPECT_EQ(o->offset, 40u);
    EXPECT_EQ(o->category, "Graphics");
    EXPECT_EQ(o->backend, std::optional<BackendKind>(BackendKind::Flatpak));
    EXPECT_TRUE(o->installedOnly);
    EXPECT_FALSE(searchOptions({{"limit", std::int32_t(20)}}));
    EXPECT_FALSE(searchOptions({{"unknown", true}}));
    EXPECT_FALSE(searchOptions({{"backend", std::string("snap")}}));
    EXPECT_TRUE(installedOptions({}));
    EXPECT_FALSE(installedOptions({{"scope", std::string("user")}}));
}

TEST(Marshal, ErrorNames)
{
    EXPECT_EQ(toDbusError(Error::invalidArgument("x")).name, errors::kInvalidArgs);
    EXPECT_EQ(toDbusError(Error::notFound("x")).name, errors::kNotFound);
    EXPECT_EQ(toDbusError(Error::notSupported("x")).name, errors::kNotSupported);
    EXPECT_EQ(toDbusError(Error::conflict("x")).name, errors::kBusy);
    EXPECT_EQ(toDbusError(Error::backend(BackendFailure::Unavailable, "x")).name, errors::kBackendUnavailable);
    EXPECT_EQ(toDbusError(Error::internal("x")).name, errors::kFailed);
    const auto m = errorMap(Error::backend(BackendFailure::NoSpace, "disk full"));
    EXPECT_EQ(m.at("code").value<std::string>(), "no-space");
    EXPECT_EQ(m.at("details").value<std::string>(), "disk full");
}
