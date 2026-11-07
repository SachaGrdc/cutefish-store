#include "importer.h"

#include "catalogfixture.h"

#include <cutefish/store/catalogfile.h>
#include <cutefish/store/sqlitecatalog.h>

#include <gtest/gtest.h>

#include <fstream>

using namespace cutefish::store;
using namespace cutefish::store::indexer;
using catalogfixture::TempDir;
using fixtures::appId;
using fixtures::unwrap;

namespace {

const std::filesystem::path kData = INDEXER_TEST_DATA;
const std::filesystem::path kGenerated = INDEXER_TEST_GENERATED;

Source debianSource()
{
    return unwrap(Source::create({.id = unwrap(SourceId::parse("packagekit:test-main")),
                                  .backend = BackendKind::PackageKit,
                                  .scope = Scope::System,
                                  .kind = SourceKind::Distribution,
                                  .origin = "test-main",
                                  .priority = 10}));
}

Source flathubSource()
{
    return unwrap(Source::create({.id = unwrap(SourceId::parse("flatpak:user:flathub")),
                                  .backend = BackendKind::Flatpak,
                                  .scope = Scope::User,
                                  .kind = SourceKind::Flathub,
                                  .origin = "flathub",
                                  .priority = 20}));
}

struct Imported
{
    std::vector<SourceReport> reports;
    CatalogBuildStats stats;
};

Imported import(const std::filesystem::path& out, std::vector<SourceConfig> sources, ImportOptions options = {})
{
    auto writer = unwrap(CatalogWriter::create(out, {.searchLocales = options.descriptionLocales}));
    Imported result;
    result.reports = importSources(writer, sources, options);
    result.stats = unwrap(writer.finish());
    return result;
}

std::vector<SourceConfig> bothSources(const char* debianFile = "debian.yml", const char* flathubFile = "flathub.xml")
{
    return {{debianSource(), kGenerated / debianFile, kData / "icons"}, {flathubSource(), kGenerated / flathubFile, {}}};
}

std::int64_t skipped(const SourceReport& r, const std::string& reason)
{
    const auto it = r.skipped.find(reason);
    return it == r.skipped.end() ? 0 : it->second;
}

} // namespace

TEST(Import, DebianDep11)
{
    TempDir dir;
    auto result = import(dir / "c.sqlite", {{debianSource(), kGenerated / "debian.yml", kData / "icons"}},
                         {.descriptionLocales = {"fr_FR.UTF-8"}});
    ASSERT_EQ(result.reports.size(), 1u);
    const SourceReport& r = result.reports[0];
    EXPECT_TRUE(r.readable) << r.error;
    EXPECT_EQ(r.components, 6);
    EXPECT_EQ(r.imported, 2);
    EXPECT_EQ(skipped(r, "not a desktop application"), 1);
    EXPECT_EQ(skipped(r, "no package name"), 1);
    EXPECT_EQ(skipped(r, "invalid-argument"), 2); // org..bad, no name

    const SqliteCatalog c = unwrap(SqliteCatalog::open(dir / "c.sqlite"));
    auto editor = c.application(appId("org.example.Editor"));
    ASSERT_TRUE(editor) << editor.error().message();
    EXPECT_EQ(editor->name(), "Example Editor");
    EXPECT_EQ(editor->summary(), "Edit text files"); // tab and spaces collapsed
    ASSERT_EQ(editor->description().size(), 3u);
    EXPECT_EQ(editor->description()[0].text, "An editor for plain text.");
    EXPECT_EQ(editor->description()[1].type, DescriptionBlockType::ListItem);
    EXPECT_EQ(editor->icon().kind, IconKind::LocalPath);
    EXPECT_EQ(editor->icon().value, (kData / "icons/64x64/example-editor_example.png").string());
    EXPECT_TRUE(editor->hasCategory("TextEditor"));
    ASSERT_EQ(editor->offers().size(), 1u);
    EXPECT_EQ(editor->offers()[0].id().str(), "packagekit:example-editor");
    EXPECT_FALSE(editor->offers()[0].version());
    // Legacy id normalised; provided id kept as an alias.
    EXPECT_TRUE(c.application(appId("example-editor")));

    const SqliteCatalog french = unwrap(SqliteCatalog::open(dir / "c.sqlite", {.locales = {"fr_FR"}}));
    auto fr = unwrap(french.application(appId("org.example.Editor")));
    EXPECT_EQ(fr.name(), "Éditeur exemple");
    ASSERT_EQ(fr.description().size(), 1u);
    EXPECT_EQ(fr.description()[0].text, "Un éditeur de texte.");
    EXPECT_EQ(unwrap(french.search(unwrap(SearchQuery::create("notepad")))).total, 1u);
}

TEST(Import, FlathubXml)
{
    TempDir dir;
    auto result = import(dir / "c.sqlite", {{flathubSource(), kGenerated / "flathub.xml", {}}});
    const SourceReport& r = result.reports[0];
    EXPECT_TRUE(r.readable) << r.error;
    EXPECT_EQ(r.components, 7);
    EXPECT_EQ(r.imported, 5);
    EXPECT_EQ(skipped(r, "not a desktop application"), 1);
    EXPECT_EQ(skipped(r, "no Flatpak bundle"), 1);

    const SqliteCatalog c = unwrap(SqliteCatalog::open(dir / "c.sqlite"));
    auto gimp = unwrap(c.application(appId("org.gimp.GIMP")));
    ASSERT_EQ(gimp.offers().size(), 1u);
    EXPECT_EQ(gimp.offers()[0].package(), "app/org.gimp.GIMP/x86_64/stable");
    EXPECT_EQ(gimp.offers()[0].version()->str(), "3.0.4");
    EXPECT_EQ(gimp.offers()[0].version()->scheme(), VersionScheme::Upstream);
    EXPECT_EQ(gimp.icon(), (IconRef{IconKind::RemoteUrl, "https://dl.flathub.org/media/gimp.png"}));
    ASSERT_EQ(gimp.description().size(), 3u);
    EXPECT_EQ(gimp.description()[0].text, "GIMP is free software.");
    EXPECT_EQ(gimp.description()[2].type, DescriptionBlockType::OrderedListItem);
    const auto summary = unwrap(c.summaries(std::vector<AppId>{appId("org.gimp.GIMP")}));
    EXPECT_TRUE(summary[0].verified);
    EXPECT_EQ(summary[0].developer, "The GIMP team");

    // Two branches: two offers of one application.
    EXPECT_EQ(unwrap(c.application(appId("org.qgis.qgis"))).offers().size(), 2u);
    // A ref whose ID ends in .desktop: application id normalised, ref intact.
    auto delta = unwrap(c.application(appId("chat.delta")));
    EXPECT_EQ(delta.offers()[0].package(), "app/chat.delta.desktop/x86_64/stable");
    // No icon, no summary, no version: still imported.
    auto bare = unwrap(c.application(appId("org.example.Bare")));
    EXPECT_EQ(bare.icon().kind, IconKind::None);
    EXPECT_EQ(bare.summary(), "");
    EXPECT_FALSE(bare.offers()[0].version());
}

TEST(Import, BothSourcesShareApplications)
{
    TempDir dir;
    auto result = import(dir / "c.sqlite", bothSources("debian.yml.gz", "flathub.xml.gz"));
    for (const SourceReport& r : result.reports)
        EXPECT_TRUE(r.readable) << r.sourceId << ": " << r.error;
    const SqliteCatalog c = unwrap(SqliteCatalog::open(dir / "c.sqlite"));
    auto gimp = unwrap(c.application(appId("org.gimp.GIMP")));
    ASSERT_EQ(gimp.offers().size(), 2u);
    EXPECT_EQ(gimp.offers()[0].format(), PackageFormat::Flatpak); // priority 20
    EXPECT_EQ(gimp.offers()[1].format(), PackageFormat::Deb);
    EXPECT_EQ(gimp.name(), "GIMP"); // from the higher-priority source
    EXPECT_TRUE(inspectCatalog(dir / "c.sqlite"));
}

TEST(Import, BatchSizeDoesNotChangeTheResult)
{
    TempDir dir;
    std::vector<std::string> dumps;
    for (std::size_t batch : {0u, 1u, 2u, 20u}) {
        const auto path = dir.path() / ("c" + std::to_string(batch) + ".sqlite");
        auto result = import(path, bothSources("debian.yml.gz", "flathub.xml.gz"), {.batchSize = batch});
        const SqliteCatalog c = unwrap(SqliteCatalog::open(path));
        std::string dump = std::to_string(result.stats.applications) + "/" + std::to_string(result.stats.offers) +
                           "/" + std::to_string(result.stats.texts);
        for (const char* id : {"org.gimp.GIMP", "org.qgis.qgis", "org.example.Editor", "chat.delta", "org.example.Bare"}) {
            auto app = unwrap(c.application(appId(id)));
            dump += "|" + app.name() + ":" + app.summary() + ":" + std::to_string(app.offers().size()) + ":" +
                    std::to_string(app.description().size());
        }
        dumps.push_back(dump);
    }
    for (const std::string& d : dumps)
        EXPECT_EQ(d, dumps[0]);
}

TEST(Import, UnavailableSourceDoesNotStopTheOthers)
{
    TempDir dir;
    std::vector<SourceConfig> sources = bothSources();
    sources[0].file = kGenerated / "missing.yml";
    auto result = import(dir / "c.sqlite", sources);
    EXPECT_FALSE(result.reports[0].readable);
    EXPECT_NE(result.reports[0].error.find("no such file"), std::string::npos);
    EXPECT_TRUE(result.reports[1].readable);
    EXPECT_EQ(result.reports[1].imported, 5);
    // The candidate is valid but has no Debian offer: against an active
    // catalogue that had some, the replacement is refused.
    auto info = unwrap(inspectCatalog(dir / "c.sqlite"));
    EXPECT_EQ(info.offersPerSource.at("packagekit:test-main"), 0);
}

TEST(Import, TruncatedFilesAreReported)
{
    TempDir dir;
    for (const char* name : {"flathub.xml", "flathub.xml.gz", "debian.yml.gz"}) {
        const std::filesystem::path truncated = dir.path() / name;
        std::filesystem::copy_file(kGenerated / name, truncated);
        std::filesystem::resize_file(truncated, std::filesystem::file_size(truncated) * 2 / 3);
        const bool yaml = std::string_view(name).starts_with("debian");
        for (std::size_t batch : {0u, 20u}) {
            const auto out = dir.path() / ("out-" + std::to_string(batch) + ".sqlite");
            SourceConfig config{yaml ? debianSource() : flathubSource(), truncated, {}};
            auto result = import(out, {config}, {.batchSize = batch});
            EXPECT_FALSE(result.reports[0].readable) << name << " batch " << batch;
            EXPECT_FALSE(result.reports[0].error.empty()) << name;
            // Whatever was read stays consistent: the file is a valid catalogue.
            EXPECT_TRUE(inspectCatalog(out)) << name;
        }
    }
}

TEST(Import, GarbageIsReportedNotFatal)
{
    TempDir dir;
    const auto garbage = dir.path() / "garbage.xml";
    std::ofstream(garbage) << "this is not XML";
    for (std::size_t batch : {0u, 20u}) {
        auto result = import(dir.path() / ("g" + std::to_string(batch) + ".sqlite"),
                             {{flathubSource(), garbage, {}}}, {.batchSize = batch});
        EXPECT_FALSE(result.reports[0].readable);
        EXPECT_EQ(result.reports[0].imported, 0);
    }
}
