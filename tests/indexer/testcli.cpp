// The exit-status contract of cutefish-store-indexer (src/indexer/main.cpp,
// docs/CATALOG.md "Contrat de l'indexeur"), checked by running the binary.

#include "catalogfixture.h"

#include <cutefish/store/catalogfile.h>
#include <cutefish/store/sqlitecatalog.h>

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <sys/wait.h>

using namespace cutefish::store;
using catalogfixture::TempDir;

namespace {

const std::string kIndexer = INDEXER_BINARY;
const std::filesystem::path kGenerated = INDEXER_TEST_GENERATED;

struct Outcome
{
    int status = -1;
    std::string out;

    std::string resultLine() const
    {
        const std::size_t at = out.rfind("result=");
        return at == std::string::npos ? std::string() : out.substr(at, out.find('\n', at) - at);
    }
};

std::string quote(const std::string& s)
{
    std::string q = "'";
    for (const char c : s) {
        if (c == '\'')
            q += "'\\''";
        else
            q += c;
    }
    return q + "'";
}

Outcome run(const std::vector<std::string>& args)
{
    std::string command = quote(kIndexer);
    for (const std::string& a : args)
        command += " " + quote(a);
    command += " 2>/dev/null";
    Outcome r;
    FILE* pipe = ::popen(command.c_str(), "r");
    if (!pipe)
        return r;
    char buffer[4096];
    std::size_t n;
    while ((n = std::fread(buffer, 1, sizeof buffer, pipe)) > 0)
        r.out.append(buffer, n);
    const int raw = ::pclose(pipe);
    r.status = WIFEXITED(raw) ? WEXITSTATUS(raw) : -1;
    return r;
}

std::string debian(const std::filesystem::path& file = kGenerated / "debian.yml")
{
    return "id=packagekit:test-main,backend=packagekit,scope=system,kind=distribution,origin=test-main,priority=10,file=" +
           file.string();
}

std::string flathub(const std::filesystem::path& file = kGenerated / "flathub.xml")
{
    return "id=flatpak:user:flathub,backend=flatpak,scope=user,kind=flathub,origin=flathub,priority=20,file=" +
           file.string();
}

std::string contents(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

} // namespace

TEST(IndexerCli, ZeroWithoutReplaceMeansWrittenNotValidatedNotInstalled)
{
    TempDir dir;
    const auto candidate = dir / "catalog.sqlite.tmp";
    const auto active = dir / "catalog.sqlite";
    const Outcome r = run({"--output", candidate.string(), "--source", debian(), "--source", flathub()});
    EXPECT_EQ(r.status, 0) << r.out;
    EXPECT_EQ(r.resultLine(), "result=written sources=2 unreadable_sources=0");
    // Complete, so a caller can validate it...
    EXPECT_TRUE(inspectCatalog(candidate));
    // ...but nothing was installed: that is the caller's next step.
    EXPECT_FALSE(std::filesystem::exists(active));
}

TEST(IndexerCli, ZeroWithReplaceMeansInstalled)
{
    TempDir dir;
    const auto candidate = dir / "catalog.sqlite.tmp";
    const auto active = dir / "catalog.sqlite";
    const Outcome r = run({"--output", candidate.string(), "--replace", active.string(), "--source", debian(),
                       "--source", flathub()});
    EXPECT_EQ(r.status, 0) << r.out;
    EXPECT_EQ(r.resultLine(), "result=installed sources=2 unreadable_sources=0");
    EXPECT_FALSE(std::filesystem::exists(candidate));
    EXPECT_TRUE(SqliteCatalog::open(active));
}

TEST(IndexerCli, ThreeMeansWrittenButRefusedActiveUnchanged)
{
    TempDir dir;
    const auto candidate = dir / "catalog.sqlite.tmp";
    const auto active = dir / "catalog.sqlite";
    ASSERT_EQ(run({"--output", candidate.string(), "--replace", active.string(), "--source", debian(), "--source",
                   flathub()})
                      .status,
              0);
    const std::string before = contents(active);

    // Flathub's file has disappeared: the candidate is written, without
    // Flathub offers, and the validation refuses it.
    const Outcome r = run({"--output", candidate.string(), "--replace", active.string(), "--source", debian(),
                       "--source", flathub(dir / "missing.xml")});
    EXPECT_EQ(r.status, 3) << r.out;
    EXPECT_EQ(r.resultLine(), "result=refused sources=2 unreadable_sources=1");
    EXPECT_EQ(contents(active), before);
    EXPECT_TRUE(inspectCatalog(candidate)); // written and complete, kept for diagnosis
}

TEST(IndexerCli, UnreadableSourceDoesNotChangeTheStatusWithoutReplace)
{
    // Exit 0 is about the file being written, not about every source being
    // read: the result line and the report say what happened.
    TempDir dir;
    const Outcome r = run({"--output", (dir / "c.sqlite").string(), "--source", debian(), "--source",
                       flathub(dir / "missing.xml")});
    EXPECT_EQ(r.status, 0) << r.out;
    EXPECT_EQ(r.resultLine(), "result=written sources=2 unreadable_sources=1");
    EXPECT_NE(r.out.find("source flatpak:user:flathub readable=0"), std::string::npos) << r.out;
}

TEST(IndexerCli, OneMeansNoUsableCandidate)
{
    TempDir dir;
    const auto candidate = dir / "no-such-directory" / "c.sqlite";
    const Outcome r = run({"--output", candidate.string(), "--source", debian()});
    EXPECT_EQ(r.status, 1) << r.out;
    EXPECT_EQ(r.resultLine(), "result=failed sources=1 unreadable_sources=0");
    EXPECT_FALSE(inspectCatalog(candidate));
}

TEST(IndexerCli, TwoMeansUsageErrorAndNothingWritten)
{
    TempDir dir;
    const auto candidate = dir / "c.sqlite";
    const std::vector<std::vector<std::string>> cases = {
        {},
        {"--output", candidate.string()},
        {"--source", debian()},
        {"--output", candidate.string(), "--source", debian(), "--unknown"},
        {"--output", candidate.string(), "--source", debian(), "--batch", "many"},
        {"--output", candidate.string(), "--source", "id=x,backend=apt"},
        {"--output", candidate.string(), "--source", debian(), "--generation"},
    };
    for (const auto& args : cases) {
        const Outcome r = run(args);
        EXPECT_EQ(r.status, 2) << r.out;
        EXPECT_FALSE(std::filesystem::exists(candidate));
    }
}

TEST(IndexerCli, OutputCannotBeTheActiveCatalogue)
{
    TempDir dir;
    const auto active = dir / "catalog.sqlite";
    ASSERT_EQ(run({"--output", (dir / "c.tmp").string(), "--replace", active.string(), "--source", debian()}).status, 0);
    const std::string before = contents(active);
    for (const std::string& same : {active.string(), (dir.path() / "." / "catalog.sqlite").string()}) {
        const Outcome r = run({"--output", same, "--replace", active.string(), "--source", debian()});
        EXPECT_EQ(r.status, 2) << same;
        EXPECT_EQ(contents(active), before);
    }
}
