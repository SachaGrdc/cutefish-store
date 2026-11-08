#include "refresher.h"

#include <cutefish/store/catalogfile.h>
#include <cutefish/store/sqlitecatalog.h>

#include <algorithm>
#include <map>

namespace cutefish::store::service {

struct Refresher::Assessment
{
    Result<void> status = Result<void>::success();
    std::vector<ConfiguredSource> configured;
    SourceAssessment assessment;
    std::filesystem::path catalog;
    std::filesystem::path candidate;
};

namespace {

bool changesSources(const SourceAssessment& a)
{
    return std::any_of(a.sources.begin(), a.sources.end(), [](const SourceStatus& s) {
        return s.change == SourceChange::Added || s.change == SourceChange::Removed;
    });
}

} // namespace

Refresher::Refresher(base::EventLoop& loop, const ServiceOptions& options) : m_loop(loop), m_options(options) {}

Refresher::~Refresher()
{
    if (m_process && m_process->isRunning())
        m_process->kill();
}

void Refresher::start(bool force, std::uint64_t generation, std::function<void(Result<RefreshOutcome>)> done)
{
    m_running = true;
    m_replacing = false;
    m_cancelled = false;
    m_force = force;
    m_generation = generation;
    m_done = std::move(done);

    auto a = std::make_shared<Assessment>();
    a->catalog = m_options.dataDir / "catalog.sqlite";
    a->candidate = m_options.dataDir / "catalog.sqlite.tmp";
    const std::filesystem::path sourcesFile = m_options.sourcesFile;
    // Worker thread: read the configuration, the recorded fingerprints, and
    // probe every file (hashing when size or time changed).
    m_io.run(
                [a, sourcesFile, generation] {
                    auto configured = readSources(sourcesFile);
                    if (!configured) {
                        a->status = std::move(configured).error();
                        return;
                    }
                    a->configured = std::move(configured).value();
                    std::vector<RecordedSource> recorded;
                    if (generation > 0) {
                        if (auto c = SqliteCatalog::open(a->catalog))
                            if (auto r = c->sources())
                                recorded = std::move(r).value();
                    }
                    std::map<std::string, const SourceFingerprint*> previous;
                    for (const RecordedSource& r : recorded)
                        if (r.fingerprint)
                            previous.emplace(r.source.id().str(), &*r.fingerprint);
                    std::vector<ObservedSource> observed;
                    for (const ConfiguredSource& s : a->configured) {
                        const auto it = previous.find(s.source.id().str());
                        observed.push_back({s.source, probeSource(s.files, it == previous.end() ? nullptr : it->second)});
                    }
                    a->assessment = assessSources(recorded, observed);
                },
                [this, a] { assessed(a); })
            .detach();
}

void Refresher::assessed(std::shared_ptr<Assessment> a)
{
    if (m_cancelled)
        return finish(Error::cancelled("refresh cancelled"));
    if (!a->status)
        return finish(a->status.error());
    const SourceAssessment& s = a->assessment;
    if (!s.blocking.empty()) {
        std::string ids;
        for (const SourceId& id : s.blocking)
            ids += (ids.empty() ? "" : ", ") + id.str();
        // The catalogue in service stays (CATALOG §11).
        return finish(Error::notFound("metadata missing or unreadable for " + ids));
    }
    if (s.importable.empty()) {
        if (m_generation == 0)
            return finish(Error::notFound("no readable source to import"));
        return finish(RefreshOutcome{.replaced = false, .generation = m_generation, .sourcesChanged = false, .refreshedSources = {}});
    }
    if (!s.reimport && !m_force) {
        // Everything current: a successful refresh that changes nothing.
        return finish(RefreshOutcome{.replaced = false, .generation = m_generation, .sourcesChanged = false, .refreshedSources = s.importable});
    }

    std::vector<std::string> args{"--output", a->candidate.string(), "--generation", std::to_string(m_generation + 1)};
    for (const std::string& l : m_options.indexLocales) {
        args.push_back("--locale");
        args.push_back(l);
    }
    for (const ConfiguredSource& c : a->configured)
        if (std::find(s.importable.begin(), s.importable.end(), c.source.id()) != s.importable.end()) {
            args.push_back("--source");
            args.push_back(indexerArgument(c));
        }
    m_process = std::make_unique<base::Process>(&m_loop);
    m_process->onFinished = [this, a](int exitCode, bool crashed) {
        // Deferred: the Process must not be destroyed from its own callback.
        m_loop.post([this, a, exitCode, crashed] { indexerFinished(a, exitCode, crashed); });
    };
    if (!m_process->start(m_options.indexer.string(), args))
        return finish(Error::internal("cannot start " + m_options.indexer.string()));
}

void Refresher::indexerFinished(std::shared_ptr<Assessment> a, int exitCode, bool crashed)
{
    const std::string errors = m_process ? m_process->standardError() : std::string();
    m_process.reset();
    if (m_cancelled)
        return finish(Error::cancelled("refresh cancelled"));
    if (crashed || exitCode != 0)
        return finish(Error::internal("indexer " + std::string(crashed ? "killed by signal " : "exited with ")
                                      + std::to_string(exitCode) + (errors.empty() ? "" : ": " + errors)));
    m_replacing = true;
    auto result = std::make_shared<Result<CatalogFileInfo>>(Error::internal("not replaced"));
    CatalogReplaceRules rules;
    for (const SourceId& id : a->assessment.removed)
        rules.removedSources.push_back(id.str());
    m_io.run([a, rules, result] { *result = replaceCatalog(a->candidate, a->catalog, rules); },
             [this, a, result] {
                 if (!*result)
                     return finish(std::move(*result).error());
                 finish(RefreshOutcome{.replaced = true,
                                       .generation = (*result)->generation,
                                       .sourcesChanged = changesSources(a->assessment),
                                       .refreshedSources = a->assessment.importable});
             })
            .detach();
}

bool Refresher::cancel()
{
    if (!cancellable())
        return false;
    m_cancelled = true;
    if (m_process && m_process->isRunning())
        m_process->terminate();
    return true;
}

void Refresher::finish(Result<RefreshOutcome> result)
{
    m_running = false;
    m_replacing = false;
    auto done = std::move(m_done);
    m_done = nullptr;
    if (done)
        done(std::move(result));
}

} // namespace cutefish::store::service
