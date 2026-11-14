// The service's pure parts: options, sources file, D-Bus conversions.

#include "marshal.h"
#include "options.h"
#include "sourcelist.h"
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

TEST(ServiceOptions, PackageKitBus)
{
    auto d = parseArguments({});
    ASSERT_TRUE(d);
    EXPECT_EQ(d->packagekit, PackageKitBus::System); // production
    EXPECT_EQ(d->swcatalog, "/var/lib/swcatalog");
    auto o = parseArguments({"--packagekit", "session", "--swcatalog", "/tmp/sw"});
    ASSERT_TRUE(o);
    EXPECT_EQ(o->packagekit, PackageKitBus::Session);
    EXPECT_EQ(o->swcatalog, "/tmp/sw");
    auto off = parseArguments({"--packagekit", "off"});
    ASSERT_TRUE(off);
    EXPECT_EQ(off->packagekit, PackageKitBus::Off);
    EXPECT_FALSE(parseArguments({"--packagekit", "maybe"}));
    EXPECT_EQ(d->confirmationTimeout, std::chrono::minutes(10)); // D-019
    auto c = parseArguments({"--confirmation-timeout-ms", "500"});
    ASSERT_TRUE(c);
    EXPECT_EQ(c->confirmationTimeout, std::chrono::milliseconds(500));
}

TEST(ServiceOptions, FlatpakMode)
{
    auto d = parseArguments({});
    ASSERT_TRUE(d);
    EXPECT_EQ(d->flatpak, FlatpakMode::Auto); // production
    auto off = parseArguments({"--flatpak", "off"});
    ASSERT_TRUE(off);
    EXPECT_EQ(off->flatpak, FlatpakMode::Off);
    auto auto_ = parseArguments({"--flatpak", "auto"});
    ASSERT_TRUE(auto_);
    EXPECT_EQ(auto_->flatpak, FlatpakMode::Auto);
    EXPECT_FALSE(parseArguments({"--flatpak", "maybe"}));
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

TEST(Marshal, ANewInstallInASharedSystemInstallationIsRefused)
{
    auto offer = [](const char* raw) { return OfferId::parse(raw).value(); };
    // D-005, D-011 (O-013 open): a new Flatpak goes to the user installation.
    const std::string refused = newInstallRefusal(offer("flatpak:system:flathub:app/org.gimp.GIMP/x86_64/stable"));
    EXPECT_NE(refused.find("user installation"), std::string::npos) << refused;
    EXPECT_NE(refused.find("flatpak:system:flathub:app/org.gimp.GIMP/x86_64/stable"), std::string::npos);
    EXPECT_EQ(newInstallRefusal(offer("flatpak:user:flathub:app/org.gimp.GIMP/x86_64/stable")), "");
    // Distribution packages are system-wide by nature: not concerned.
    EXPECT_EQ(newInstallRefusal(offer("packagekit:gimp")), "");
}

// --- Sources (H.5) ----------------------------------------------------------------

namespace {

Source makeSource(const char* id, BackendKind backend, Scope scope, SourceKind kind, const char* origin, bool enabled)
{
    return Source::create({.id = SourceId::parse(id).value(),
                           .backend = backend,
                           .scope = scope,
                           .kind = kind,
                           .origin = origin,
                           .label = {},
                           .url = "https://example.org/",
                           .enabled = enabled,
                           .priority = 10})
            .value();
}

BackendSource remote(const char* name, Scope scope, bool enabled, std::optional<bool> gpg, std::optional<std::uint32_t> installed)
{
    const std::string id = std::string("flatpak:") + (scope == Scope::User ? "user:" : "system:") + name;
    return {.source = makeSource(id.c_str(), BackendKind::Flatpak, scope, SourceKind::ThirdParty, name, enabled),
            .catalogueSources = {},
            .verifiesSignatures = gpg,
            .installedItems = installed};
}

CatalogueSource recorded(const char* id, BackendKind backend, Scope scope, std::int64_t offers,
                         std::optional<UnixSeconds> last)
{
    return {.recorded = {.source = makeSource(id, backend, scope, SourceKind::Distribution, "origin", true),
                         .fingerprint = std::nullopt,
                         .offers = offers},
            .lastRefresh = last};
}

const dbus::VariantMap& entry(const std::vector<dbus::VariantMap>& list, const std::string& id)
{
    for (const auto& m : list)
        if (m.at("id").value<std::string>() == id)
            return m;
    static const dbus::VariantMap none;
    ADD_FAILURE() << "no entry " << id;
    return none;
}

} // namespace

TEST(SourceList, MergesWhatBackendsHaveWithTheCatalogue)
{
    // A Debian repository feeds two DEP-11 sources; a disabled user remote
    // is not in the catalogue; a source of the sources file is reported by
    // no backend.
    BackendSource debian{.source = makeSource("packagekit:/etc/apt/sources.list:deb:http://deb.debian.org/debian/:trixie:main,contrib",
                                              BackendKind::PackageKit, Scope::System, SourceKind::Distribution,
                                              "/etc/apt/sources.list:deb:http://deb.debian.org/debian/:trixie:main,contrib",
                                              true),
                         .catalogueSources = {SourceId::parse("packagekit:debian-trixie-main").value(),
                                              SourceId::parse("packagekit:debian-trixie-contrib").value()},
                         .verifiesSignatures = std::nullopt,
                         .installedItems = std::nullopt};
    const std::vector<BackendSources> backends{
            {.kind = BackendKind::PackageKit, .userSourceChanges = false, .sources = {debian}},
            {.kind = BackendKind::Flatpak,
             .userSourceChanges = true,
             .sources = {remote("flathub", Scope::User, true, true, 2), remote("spare", Scope::User, false, true, 0),
                         remote("unsigned", Scope::User, false, false, 0), remote("flathub", Scope::System, true, true, 0)}}};
    const std::vector<CatalogueSource> catalogue{
            recorded("packagekit:debian-trixie-main", BackendKind::PackageKit, Scope::System, 100, 2000),
            recorded("packagekit:debian-trixie-contrib", BackendKind::PackageKit, Scope::System, 5, 1000),
            recorded("flatpak:user:flathub", BackendKind::Flatpak, Scope::User, 40, 3000),
            recorded("packagekit:local-file", BackendKind::PackageKit, Scope::System, 3, std::nullopt)};
    const auto list = sourceList(backends, catalogue);
    // The two DEP-11 sources are accounted for by their repository.
    ASSERT_EQ(list.size(), 6u);

    const auto& d = entry(list, debian.source.id().str());
    EXPECT_TRUE(d.at("configured").value<bool>());
    EXPECT_EQ(d.at("offers").value<std::uint32_t>(), 105u);
    EXPECT_EQ(d.at("last-refresh").value<std::uint64_t>(), 1000u); // the oldest
    EXPECT_FALSE(d.at("can-toggle").value<bool>(true));             // D-039
    EXPECT_FALSE(d.at("can-remove").value<bool>(true));
    EXPECT_FALSE(d.contains("gpg-verify")); // PackageKit cannot tell
    EXPECT_FALSE(d.contains("installed-count"));

    const auto& f = entry(list, "flatpak:user:flathub");
    EXPECT_EQ(f.at("offers").value<std::uint32_t>(), 40u);
    EXPECT_TRUE(f.at("can-toggle").value<bool>());
    EXPECT_FALSE(f.at("can-remove").value<bool>(true)); // two items came from it (D-041)
    EXPECT_TRUE(f.at("gpg-verify").value<bool>());
    EXPECT_EQ(f.at("installed-count").value<std::uint32_t>(), 2u);

    const auto& spare = entry(list, "flatpak:user:spare");
    EXPECT_FALSE(spare.at("enabled").value<bool>(true)); // listed although disabled
    EXPECT_EQ(spare.at("offers").value<std::uint32_t>(), 0u);
    EXPECT_FALSE(spare.contains("last-refresh"));
    EXPECT_TRUE(spare.at("can-toggle").value<bool>());
    EXPECT_TRUE(spare.at("can-remove").value<bool>());

    // Without signature checks it cannot be enabled (D-044), only removed.
    const auto& u = entry(list, "flatpak:user:unsigned");
    EXPECT_FALSE(u.at("can-toggle").value<bool>(true));
    EXPECT_TRUE(u.at("can-remove").value<bool>());
    EXPECT_FALSE(u.at("gpg-verify").value<bool>(true));

    // The system installation is read-only (D-011).
    const auto& sys = entry(list, "flatpak:system:flathub");
    EXPECT_FALSE(sys.at("can-toggle").value<bool>(true));
    EXPECT_FALSE(sys.at("can-remove").value<bool>(true));

    const auto& file = entry(list, "packagekit:local-file");
    EXPECT_FALSE(file.at("configured").value<bool>(true));
    EXPECT_FALSE(file.at("can-toggle").value<bool>(true));
    EXPECT_FALSE(file.at("can-remove").value<bool>(true));
    for (const auto& m : list)
        EXPECT_FALSE(m.at("suggested").value<bool>(true)); // nothing before P-011

    // Sorted by id.
    for (std::size_t i = 1; i < list.size(); ++i)
        EXPECT_LT(list[i - 1].at("id").value<std::string>(), list[i].at("id").value<std::string>());
}

TEST(SourceList, AnUnusableBackendLeavesTheCatalogueEntries)
{
    // Its inventory lists nothing: the catalogue still shows the source,
    // as not configured (nothing can be changed through it).
    const std::vector<BackendSources> backends{{.kind = BackendKind::Flatpak, .userSourceChanges = true, .sources = {}}};
    const auto list = sourceList(backends, {recorded("flatpak:user:flathub", BackendKind::Flatpak, Scope::User, 40, 3000)});
    ASSERT_EQ(list.size(), 1u);
    EXPECT_FALSE(list[0].at("configured").value<bool>(true));
    EXPECT_FALSE(list[0].at("can-toggle").value<bool>(true));
    EXPECT_EQ(list[0].at("last-refresh").value<std::uint64_t>(), 3000u);
}

TEST(SourceList, RequestsTheServiceRefusesBeforeAnyTransaction)
{
    using A = SourceRequest::Action;
    auto code = [](const std::optional<Error>& e) { return e ? std::optional<ErrorCode>(e->code()) : std::nullopt; };
    const auto enabled = remote("on", Scope::User, true, true, 0);
    const auto disabled = remote("off", Scope::User, false, true, 0);
    const auto used = remote("used", Scope::User, true, true, 1);
    const auto unknown = remote("unknown", Scope::User, true, true, std::nullopt);
    const auto system = remote("sys", Scope::System, false, true, 0);

    EXPECT_EQ(code(sourceRequestRefusal(enabled, false, A::Disable)), ErrorCode::NotSupported); // D-039
    for (const A a : {A::Enable, A::Disable, A::Remove})
        EXPECT_EQ(code(sourceRequestRefusal(system, true, a)), ErrorCode::NotAllowed); // D-011
    EXPECT_EQ(code(sourceRequestRefusal(enabled, true, A::Enable)), ErrorCode::NotAllowed);
    EXPECT_FALSE(sourceRequestRefusal(enabled, true, A::Disable));
    EXPECT_FALSE(sourceRequestRefusal(disabled, true, A::Enable));
    EXPECT_EQ(code(sourceRequestRefusal(disabled, true, A::Disable)), ErrorCode::NotAllowed);
    EXPECT_FALSE(sourceRequestRefusal(enabled, true, A::Remove));
    EXPECT_EQ(code(sourceRequestRefusal(used, true, A::Remove)), ErrorCode::NotAllowed);    // D-041
    EXPECT_EQ(code(sourceRequestRefusal(unknown, true, A::Remove)), ErrorCode::NotAllowed); // cannot tell
    // Disabling a used source is allowed: nothing is removed.
    EXPECT_FALSE(sourceRequestRefusal(used, true, A::Disable));

    EXPECT_FALSE(untrustedEnable(disabled));
    EXPECT_TRUE(untrustedEnable(remote("u", Scope::User, false, false, 0)));
    EXPECT_TRUE(untrustedEnable(remote("u", Scope::User, false, std::nullopt, 0))); // unknown is not trusted
}
