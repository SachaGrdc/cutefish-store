#include "catalogfixture.h"

#include <cutefish/store/sourcefingerprint.h>
#include <cutefish/store/sqlitecatalog.h>

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <fstream>
#include <thread>

#include <sys/stat.h>
#include <unistd.h>

using namespace cutefish::store;
using catalogfixture::TempDir;
using fixtures::unwrap;

namespace {

void write(const std::filesystem::path& path, const std::string& content)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
}

std::string shaOf(const std::filesystem::path& path)
{
    const SourceProbe p = probeSource({path});
    EXPECT_EQ(p.state, SourceFileState::Present) << p.detail;
    return p.fingerprint ? p.fingerprint->sha256 : std::string();
}

SourceFingerprint fp(const char* sha, const char* path = "/var/lib/swcatalog/yaml/main.yml.gz",
                     std::uint64_t size = 100, std::int64_t mtime = 1)
{
    std::string hash(64, '0');
    hash.replace(0, std::string_view(sha).size(), sha);
    return {.path = path, .size = size, .mtimeNs = mtime, .sha256 = hash, .iconsPath = ""};
}

SourceProbe present(SourceFingerprint f) { return {SourceFileState::Present, std::move(f), {}}; }

Source withPriority(const Source& s, std::int32_t priority)
{
    return unwrap(Source::create({.id = s.id(),
                                  .backend = s.backend(),
                                  .scope = s.scope(),
                                  .kind = s.kind(),
                                  .origin = s.origin(),
                                  .label = s.label(),
                                  .url = s.url(),
                                  .enabled = s.enabled(),
                                  .priority = priority}));
}

const SourceStatus& statusOf(const SourceAssessment& a, const Source& s)
{
    for (const SourceStatus& st : a.sources)
        if (st.id == s.id())
            return st;
    ADD_FAILURE() << "no status for " << s.id().str();
    std::abort();
}

} // namespace

// --- Hashing and probing -------------------------------------------------------

TEST(SourceProbe, HashesWithSha256)
{
    TempDir dir;
    const auto file = dir / "f";
    // FIPS 180-4 test vectors.
    write(file, "");
    EXPECT_EQ(shaOf(file), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    write(file, "abc");
    EXPECT_EQ(shaOf(file), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    write(file, "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq");
    EXPECT_EQ(shaOf(file), "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    // A million bytes: several reads, and blocks split across them.
    write(file, std::string(1'000'000, 'a'));
    EXPECT_EQ(shaOf(file), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST(SourceProbe, RecordsPathSizeTimeAndIcons)
{
    TempDir dir;
    const auto file = dir / "appstream.xml";
    write(file, "abc");
    const SourceProbe p = probeSource({file, dir / "icons"});
    ASSERT_EQ(p.state, SourceFileState::Present);
    EXPECT_EQ(p.fingerprint->path, file.string());
    EXPECT_EQ(p.fingerprint->size, 3u);
    EXPECT_GT(p.fingerprint->mtimeNs, 0);
    EXPECT_EQ(p.fingerprint->iconsPath, (dir / "icons").string());
}

TEST(SourceProbe, MissingAndUnreadableFiles)
{
    TempDir dir;
    EXPECT_EQ(probeSource({dir / "absent.xml"}).state, SourceFileState::Missing);
    EXPECT_EQ(probeSource({dir / "absent" / "below.xml"}).state, SourceFileState::Missing);
    const SourceProbe directory = probeSource({dir.path()});
    EXPECT_EQ(directory.state, SourceFileState::Unreadable);
    EXPECT_FALSE(directory.fingerprint);
    EXPECT_FALSE(directory.detail.empty());

    if (::geteuid() == 0)
        GTEST_SKIP() << "root reads files whatever their permissions";
    const auto locked = dir / "locked.xml";
    write(locked, "secret");
    ASSERT_EQ(::chmod(locked.c_str(), 0), 0);
    EXPECT_EQ(probeSource({locked}).state, SourceFileState::Unreadable);
}

TEST(SourceProbe, ReusesThePreviousHashOnlyForTheSameSizeAndTime)
{
    TempDir dir;
    const auto file = dir / "appstream.xml";
    write(file, "abc");
    const SourceFingerprint real = *probeSource({file}).fingerprint;

    // Same path, size and time: the file is not read again.
    SourceFingerprint previous = real;
    previous.sha256 = std::string(64, 'f');
    EXPECT_EQ(probeSource({file}, &previous).fingerprint->sha256, std::string(64, 'f'));

    // Another time: hashed again, whatever the previous value says.
    previous.mtimeNs -= 1;
    EXPECT_EQ(probeSource({file}, &previous).fingerprint->sha256, real.sha256);
    // Another size: likewise.
    previous = real;
    previous.size += 1;
    previous.sha256 = std::string(64, 'f');
    EXPECT_EQ(probeSource({file}, &previous).fingerprint->sha256, real.sha256);
}

// --- Assessment ------------------------------------------------------------------

TEST(SourceAssessment, NoCatalogueYetIsAnInitialImport)
{
    const Source debian = fixtures::debianSource();
    const std::vector<ObservedSource> observed{{debian, present(fp("aa"))}};
    const SourceAssessment a = assessSources({}, observed);
    EXPECT_TRUE(a.reimport);
    EXPECT_EQ(statusOf(a, debian).change, SourceChange::Added);
    EXPECT_FALSE(statusOf(a, debian).inCatalog);
    EXPECT_EQ(a.importable, std::vector<SourceId>{debian.id()});
}

TEST(SourceAssessment, SameContentIsUnchangedEvenIfTouched)
{
    const Source debian = fixtures::debianSource();
    const std::vector<RecordedSource> recorded{{debian, fp("aa"), 12}};
    const std::vector<ObservedSource> same{{debian, present(fp("aa"))}};
    SourceAssessment a = assessSources(recorded, same);
    EXPECT_FALSE(a.reimport);
    EXPECT_EQ(statusOf(a, debian).change, SourceChange::Unchanged);

    // Rewritten with identical bytes: new size field and time, same hash.
    const std::vector<ObservedSource> touched{{debian, present(fp("aa", "/var/lib/swcatalog/yaml/main.yml.gz", 100, 99))}};
    a = assessSources(recorded, touched);
    EXPECT_FALSE(a.reimport);
    EXPECT_EQ(statusOf(a, debian).change, SourceChange::Unchanged);
}

TEST(SourceAssessment, WhatCountsAsAModification)
{
    const Source debian = fixtures::debianSource();
    const std::vector<RecordedSource> recorded{{debian, fp("aa"), 12}};
    auto changeFor = [&](ObservedSource o) {
        const std::vector<ObservedSource> observed{std::move(o)};
        const SourceAssessment a = assessSources(recorded, observed);
        EXPECT_TRUE(a.reimport);
        return statusOf(a, debian).change;
    };
    EXPECT_EQ(changeFor({debian, present(fp("bb"))}), SourceChange::Modified); // content
    EXPECT_EQ(changeFor({debian, present(fp("aa", "/elsewhere/main.yml.gz"))}), SourceChange::Modified); // file
    SourceFingerprint icons = fp("aa");
    icons.iconsPath = "/var/lib/swcatalog/icons/debian";
    EXPECT_EQ(changeFor({debian, present(icons)}), SourceChange::Modified); // icon directory
    EXPECT_EQ(changeFor({withPriority(debian, 99), present(fp("aa"))}), SourceChange::Modified); // descriptor

    // Nothing recorded (unreadable at the last import, or an older
    // catalogue): imported again.
    const std::vector<RecordedSource> unknown{{debian, std::nullopt, 0}};
    const std::vector<ObservedSource> observed{{debian, present(fp("aa"))}};
    EXPECT_EQ(statusOf(assessSources(unknown, observed), debian).change, SourceChange::Modified);
}

TEST(SourceAssessment, RemovedSourcesAreDeclaredForTheReplacement)
{
    const Source debian = fixtures::debianSource();
    const Source flathub = fixtures::flathubSource();
    const std::vector<RecordedSource> recorded{{debian, fp("aa"), 12}, {flathub, fp("cc"), 30}};
    const std::vector<ObservedSource> observed{{debian, present(fp("aa"))}};
    const SourceAssessment a = assessSources(recorded, observed);
    EXPECT_TRUE(a.reimport);
    EXPECT_EQ(statusOf(a, flathub).change, SourceChange::Removed);
    EXPECT_EQ(a.removed, std::vector<SourceId>{flathub.id()});
    EXPECT_EQ(a.importable, std::vector<SourceId>{debian.id()});
}

TEST(SourceAssessment, AMissingImportedSourceBlocksTheImport)
{
    const Source debian = fixtures::debianSource();
    const Source flathub = fixtures::flathubSource();
    const std::vector<RecordedSource> recorded{{debian, fp("aa"), 12}, {flathub, fp("cc"), 30}};
    // Debian changed, but Flathub's file is gone: importing now would lose
    // Flathub and be refused; the catalogue in service stays.
    const std::vector<ObservedSource> observed{
            {debian, present(fp("bb"))},
            {flathub, {SourceFileState::Missing, std::nullopt, "no such file"}}};
    const SourceAssessment a = assessSources(recorded, observed);
    EXPECT_FALSE(a.reimport);
    EXPECT_EQ(a.blocking, std::vector<SourceId>{flathub.id()});
    EXPECT_EQ(statusOf(a, flathub).change, SourceChange::Missing);
    EXPECT_EQ(statusOf(a, flathub).detail, "no such file");
    EXPECT_EQ(statusOf(a, debian).change, SourceChange::Modified);

    const std::vector<ObservedSource> unreadable{
            {debian, present(fp("bb"))},
            {flathub, {SourceFileState::Unreadable, std::nullopt, "permission denied"}}};
    const SourceAssessment u = assessSources(recorded, unreadable);
    EXPECT_FALSE(u.reimport);
    EXPECT_EQ(statusOf(u, flathub).change, SourceChange::Unreadable);
}

TEST(SourceAssessment, ANewSourceWithoutFileDoesNotBlock)
{
    // A remote just added, whose appstream has not been downloaded yet.
    const Source debian = fixtures::debianSource();
    const Source flathub = fixtures::flathubSource();
    const std::vector<RecordedSource> recorded{{debian, fp("aa"), 12}};
    const std::vector<ObservedSource> observed{{debian, present(fp("aa"))},
                                               {flathub, {SourceFileState::Missing, std::nullopt, "no such file"}}};
    const SourceAssessment a = assessSources(recorded, observed);
    EXPECT_FALSE(a.reimport); // nothing to import yet
    EXPECT_TRUE(a.blocking.empty());
    EXPECT_EQ(a.importable, std::vector<SourceId>{debian.id()});
    EXPECT_EQ(statusOf(a, flathub).change, SourceChange::Missing);
}

TEST(SourceAssessment, NothingReadableMeansNothingToImport)
{
    const Source debian = fixtures::debianSource();
    const std::vector<ObservedSource> observed{{debian, {SourceFileState::Missing, std::nullopt, "no such file"}}};
    const SourceAssessment a = assessSources({}, observed);
    EXPECT_FALSE(a.reimport);
    EXPECT_TRUE(a.importable.empty());
}

TEST(SourceAssessment, ResultDoesNotDependOnOrder)
{
    const Source debian = fixtures::debianSource();
    const Source flathub = fixtures::flathubSource();
    const std::vector<RecordedSource> r1{{debian, fp("aa"), 12}, {flathub, fp("cc"), 30}};
    const std::vector<RecordedSource> r2{{flathub, fp("cc"), 30}, {debian, fp("aa"), 12}};
    const std::vector<ObservedSource> o1{{debian, present(fp("bb"))}, {flathub, present(fp("cc"))}};
    const std::vector<ObservedSource> o2{{flathub, present(fp("cc"))}, {debian, present(fp("bb"))}};
    const SourceAssessment a = assessSources(r1, o1);
    const SourceAssessment b = assessSources(r2, o2);
    ASSERT_EQ(a.sources.size(), b.sources.size());
    for (std::size_t i = 0; i < a.sources.size(); ++i) {
        EXPECT_EQ(a.sources[i].id, b.sources[i].id);
        EXPECT_EQ(a.sources[i].change, b.sources[i].change);
    }
    EXPECT_EQ(a.sources[0].id, flathub.id()); // sorted by id
    EXPECT_EQ(toString(SourceChange::Unreadable), "unreadable");
}

// --- Stored in the catalogue -------------------------------------------------------

TEST(CatalogSources, FingerprintsAreStoredAndReadBack)
{
    TempDir dir;
    const auto file = dir / "main.yml";
    write(file, "File: DEP-11\n");
    const SourceFingerprint real = *probeSource({file, dir / "icons"}).fingerprint;

    const auto path = dir / "catalog.sqlite";
    {
        auto writer = unwrap(CatalogWriter::create(path));
        ASSERT_TRUE(writer.addSource(fixtures::debianSource(), real));
        ASSERT_TRUE(writer.addSource(fixtures::flathubSource())); // unreadable at import: no fingerprint
        ASSERT_TRUE(writer.addComponent(fixtures::debianSource().id(), catalogfixture::deb("org.gimp.GIMP", "gimp", "GIMP")));
        ASSERT_TRUE(writer.finish());
    }
    auto catalog = unwrap(SqliteCatalog::open(path));
    auto sources = unwrap(catalog.sources());
    ASSERT_EQ(sources.size(), 2u);
    const RecordedSource& debian = sources[1].source.id() == fixtures::debianSource().id() ? sources[1] : sources[0];
    const RecordedSource& flathub = &debian == &sources[0] ? sources[1] : sources[0];
    ASSERT_TRUE(debian.fingerprint);
    EXPECT_EQ(*debian.fingerprint, real);
    EXPECT_EQ(debian.offers, 1);
    EXPECT_EQ(debian.source.priority(), 10);
    EXPECT_FALSE(flathub.fingerprint);
    EXPECT_EQ(flathub.offers, 0);

    // Round trip through the assessment: nothing changed.
    const std::vector<ObservedSource> observed{{fixtures::debianSource(), probeSource({file, dir / "icons"}, &real)}};
    const std::vector<RecordedSource> onlyDebian{debian};
    EXPECT_FALSE(assessSources(onlyDebian, observed).reimport);
}

TEST(CatalogSources, InvalidFingerprintsAreRefusedByTheWriter)
{
    TempDir dir;
    auto writer = unwrap(CatalogWriter::create(dir / "catalog.sqlite"));
    SourceFingerprint bad = fp("aa");
    bad.sha256 = "not-hex";
    auto r = writer.addSource(fixtures::debianSource(), bad);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code(), ErrorCode::InvalidArgument);
}

TEST(CatalogSources, AMalformedStoredFingerprintCountsAsUnknown)
{
    TempDir dir;
    const auto path = dir / "catalog.sqlite";
    {
        auto writer = unwrap(CatalogWriter::create(path));
        ASSERT_TRUE(writer.addSource(fixtures::debianSource(), fp("aa")));
        ASSERT_TRUE(writer.finish());
    }
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(path.c_str(), &db), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(db, "UPDATE meta SET value = '{\"path\": 3}' WHERE key LIKE 'fingerprint:%'", nullptr,
                           nullptr, nullptr),
              SQLITE_OK);
    sqlite3_close(db);
    auto sources = unwrap(unwrap(SqliteCatalog::open(path)).sources());
    ASSERT_EQ(sources.size(), 1u);
    EXPECT_FALSE(sources[0].fingerprint); // so the source is imported again
}
