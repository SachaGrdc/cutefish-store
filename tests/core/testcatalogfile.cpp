#include "catalogfixture.h"

#include <cutefish/store/catalogfile.h>
#include <cutefish/store/sqlitecatalog.h>

#include <gtest/gtest.h>

#include <atomic>
#include <fstream>
#include <iterator>
#include <thread>

#include <sys/wait.h>
#include <unistd.h>

using namespace cutefish::store;
using catalogfixture::TempDir;
using fixtures::unwrap;

namespace {

std::string contents(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::uint64_t generationOf(const std::filesystem::path& path)
{
    return unwrap(SqliteCatalog::open(path)).generation();
}

class ReplaceTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        catalogfixture::build(candidate, 1);
        ASSERT_TRUE(replaceCatalog(candidate, active));
        before = contents(active);
    }
    void expectActiveUnchanged() const { EXPECT_EQ(contents(active), before); }

    TempDir dir;
    const std::filesystem::path active = dir / "catalog.sqlite";
    const std::filesystem::path candidate = dir / "catalog.sqlite.tmp";
    std::string before;
};

} // namespace

TEST_F(ReplaceTest, ValidCandidateReplacesTheActiveCatalogue)
{
    EXPECT_FALSE(std::filesystem::exists(candidate));
    EXPECT_EQ(generationOf(active), 1u);

    catalogfixture::build(candidate, 2);
    auto info = replaceCatalog(candidate, active);
    ASSERT_TRUE(info) << info.error().message();
    EXPECT_EQ(info->generation, 2u);
    EXPECT_FALSE(std::filesystem::exists(candidate));
    EXPECT_EQ(generationOf(active), 2u);
}

TEST_F(ReplaceTest, MissingCandidateIsRefused)
{
    auto r = replaceCatalog(candidate, active);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code(), ErrorCode::NotFound);
    expectActiveUnchanged();
}

TEST_F(ReplaceTest, InterruptedBuildIsRefused)
{
    {
        auto writer = unwrap(CatalogWriter::create(candidate, {.generation = 2}));
        ASSERT_TRUE(writer.addSource(fixtures::debianSource()));
        ASSERT_TRUE(writer.addComponent(fixtures::debianSource().id(), catalogfixture::deb("org.a.b", "ab", "AB")));
        // No finish(): the import stopped.
    }
    auto r = replaceCatalog(candidate, active);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code(), ErrorCode::InvalidArgument);
    EXPECT_NE(r.error().message().find("schema version 0"), std::string::npos) << r.error().message();
    expectActiveUnchanged();
    EXPECT_TRUE(std::filesystem::exists(candidate)); // left for diagnosis, never used
}

TEST_F(ReplaceTest, ProcessKilledDuringTheBuildIsRefused)
{
    // The child writes part of a catalogue and dies without cleaning up,
    // as a crashed or killed indexer would.
    const pid_t pid = ::fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        auto writer = CatalogWriter::create(candidate, {.generation = 2});
        if (writer) {
            (void) writer->addSource(fixtures::debianSource());
            for (int i = 0; i < 500; ++i) {
                const std::string id = "org.example.App" + std::to_string(i);
                (void) writer->addComponent(fixtures::debianSource().id(),
                                            catalogfixture::deb(id.c_str(), ("pkg" + std::to_string(i)).c_str(), "App"));
            }
        }
        ::_exit(9);
    }
    int status = 0;
    ASSERT_EQ(::waitpid(pid, &status, 0), pid);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 9);

    auto r = replaceCatalog(candidate, active);
    ASSERT_FALSE(r);
    expectActiveUnchanged();
}

TEST_F(ReplaceTest, TruncatedCandidateIsRefused)
{
    catalogfixture::build(candidate, 2);
    std::filesystem::resize_file(candidate, std::filesystem::file_size(candidate) / 2);
    auto r = replaceCatalog(candidate, active);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code(), ErrorCode::InvalidArgument);
    expectActiveUnchanged();
}

TEST_F(ReplaceTest, CorruptedCandidateIsRefused)
{
    catalogfixture::build(candidate, 2);
    {
        std::fstream f(candidate, std::ios::in | std::ios::out | std::ios::binary);
        f.seekp(static_cast<std::streamoff>(std::filesystem::file_size(candidate) / 2));
        const std::string garbage(4096 * 3, '\xA5');
        f.write(garbage.data(), static_cast<std::streamsize>(garbage.size()));
    }
    auto r = replaceCatalog(candidate, active);
    ASSERT_FALSE(r);
    expectActiveUnchanged();
}

TEST_F(ReplaceTest, CandidateWithAJournalIsRefused)
{
    catalogfixture::build(candidate, 2);
    std::ofstream(candidate.string() + "-journal") << "x";
    auto r = replaceCatalog(candidate, active);
    ASSERT_FALSE(r);
    EXPECT_NE(r.error().message().find("-journal"), std::string::npos);
    expectActiveUnchanged();
}

TEST_F(ReplaceTest, ImplausibleCandidateIsRefused)
{
    // Flathub vanished (unreachable remote, incomplete download).
    catalogfixture::build(candidate, 2, /*withFlathub=*/false);
    auto r = replaceCatalog(candidate, active);
    ASSERT_FALSE(r);
    EXPECT_NE(r.error().message().find("flatpak:user:flathub"), std::string::npos) << r.error().message();
    expectActiveUnchanged();

    // Removed on purpose: accepted.
    auto ok = replaceCatalog(candidate, active, {.removedSources = {"flatpak:user:flathub"}});
    ASSERT_TRUE(ok) << ok.error().message();
    EXPECT_EQ(generationOf(active), 2u);
}

TEST_F(ReplaceTest, SourceThatLostMostOfItsOffersIsRefused)
{
    {
        auto writer = unwrap(CatalogWriter::create(candidate, {.generation = 2}));
        ASSERT_TRUE(writer.addSource(fixtures::debianSource()));
        ASSERT_TRUE(writer.addSource(fixtures::flathubSource()));
        ASSERT_TRUE(writer.addComponent(fixtures::debianSource().id(), catalogfixture::deb("org.a.b", "ab", "AB")));
        ASSERT_TRUE(writer.addComponent(fixtures::flathubSource().id(),
                                        catalogfixture::flatpak("org.a.b", "app/org.a.b/x86_64/stable", "AB")));
        ASSERT_TRUE(writer.finish());
    }
    auto r = replaceCatalog(candidate, active);
    ASSERT_FALSE(r);
    EXPECT_NE(r.error().message().find("offers instead of"), std::string::npos) << r.error().message();
    expectActiveUnchanged();
    // A lower threshold accepts it.
    EXPECT_TRUE(replaceCatalog(candidate, active, {.minimumShare = 0.1}));
}

TEST_F(ReplaceTest, BrokenActiveCatalogueIsReplacedByAValidOne)
{
    std::filesystem::resize_file(active, 1000);
    catalogfixture::build(candidate, 2);
    EXPECT_TRUE(replaceCatalog(candidate, active));
    EXPECT_EQ(generationOf(active), 2u);
}

TEST_F(ReplaceTest, BothFilesMustShareADirectory)
{
    TempDir other;
    catalogfixture::build(other / "c.sqlite", 2);
    auto r = replaceCatalog(other / "c.sqlite", active);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code(), ErrorCode::InvalidArgument);
}

TEST_F(ReplaceTest, OpenReadersKeepTheirSnapshotNewReadersSeeTheNewOne)
{
    const SqliteCatalog old = unwrap(SqliteCatalog::open(active));
    catalogfixture::build(candidate, 2, /*withFlathub=*/false);
    ASSERT_TRUE(replaceCatalog(candidate, active, {.removedSources = {"flatpak:user:flathub"}}));

    // The old reader still reads generation 1, with its Flathub offers.
    EXPECT_EQ(old.generation(), 1u);
    auto gimp = old.application(fixtures::appId("org.gimp.GIMP"));
    ASSERT_TRUE(gimp) << gimp.error().message();
    EXPECT_EQ(gimp->offers().size(), 2u);
    EXPECT_EQ(unwrap(old.search(unwrap(SearchQuery::create("inkscape")))).total, 1u);

    // A new reader sees generation 2, without them.
    const SqliteCatalog fresh = unwrap(SqliteCatalog::open(active));
    EXPECT_EQ(fresh.generation(), 2u);
    EXPECT_EQ(unwrap(fresh.application(fixtures::appId("org.gimp.GIMP"))).offers().size(), 1u);
    EXPECT_EQ(unwrap(fresh.search(unwrap(SearchQuery::create("inkscape")))).total, 0u);
}

TEST_F(ReplaceTest, ConcurrentReadersDuringReplacements)
{
    // Readers open, search and read applications in a loop while the
    // catalogue is replaced repeatedly. Each reader must see a consistent
    // snapshot: no error, and its generation never changes while open.
    std::atomic<bool> stop{false};
    std::atomic<int> errors{0};
    std::atomic<int> reads{0};
    std::vector<std::thread> readers;
    for (int t = 0; t < 4; ++t) {
        readers.emplace_back([&] {
            while (!stop) {
                auto c = SqliteCatalog::open(active);
                if (!c) {
                    ++errors;
                    continue;
                }
                const std::uint64_t generation = c->generation();
                for (int i = 0; i < 20; ++i) {
                    auto page = c->search(unwrap(SearchQuery::create(i % 2 ? "gimp" : "editor")));
                    auto app = c->application(fixtures::appId("org.gimp.GIMP"));
                    if (!page || !app || page->generation != generation || page->total == 0)
                        ++errors;
                    ++reads;
                }
            }
        });
    }
    for (std::uint64_t g = 2; g < 22; ++g) {
        catalogfixture::build(candidate, g);
        auto r = replaceCatalog(candidate, active);
        EXPECT_TRUE(r) << (r ? "" : r.error().message());
    }
    stop = true;
    for (std::thread& t : readers)
        t.join();
    EXPECT_EQ(errors.load(), 0);
    EXPECT_GT(reads.load(), 0);
    EXPECT_EQ(generationOf(active), 21u);
}
