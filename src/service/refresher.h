#pragma once

// Catalogue refresh in service v0.1 (D-015, SERVICE.md §3.5): read the
// configured sources, probe and assess them (CATALOG §11), run the indexer
// as a separate process if something changed, then validate and put the
// candidate in service with replaceCatalog(). The refresher itself downloads
// nothing: it rereads the metadata files on disk (the host asks a backend to
// refresh its repositories first, when it can, step F).
//
// Disk work (probing, hashing, replacement with its fsync calls) runs on a
// worker thread; the indexer runs in its own process; the main loop only
// starts things and receives their results.

#include "options.h"
#include "sourcesfile.h"

#include <cutefish/store/result.h>
#include <cutefish/store/sourcefingerprint.h>

#include <base/event_loop.h>
#include <base/process.h>
#include <base/thread_pool.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cutefish::store::service {

struct RefreshOutcome
{
    /// A new catalogue is in service.
    bool replaced = false;
    std::uint64_t generation = 0;
    /// The set of sources, or a source's descriptor, changed.
    bool sourcesChanged = false;
    /// Sources whose content was read and found current, or imported.
    std::vector<SourceId> refreshedSources;
};

class Refresher
{
public:
    Refresher(base::EventLoop& loop, const ServiceOptions& options);
    ~Refresher();

    bool running() const noexcept { return m_running; }
    /// Before the indexer has replaced the catalogue; afterwards the
    /// replacement goes to its end.
    bool cancellable() const noexcept { return m_running && !m_replacing; }

    /// Starts a refresh. `force` imports even when nothing changed (a manual
    /// refresh, or the initial import). `generation` is the one in service
    /// (0 if none). `provided`: the sources the backends report
    /// (Backend::catalogSources), indexed with those of the sources file.
    /// The file's entry wins on a shared id, so that it can override a
    /// backend's; a second reference to a metadata file already listed
    /// (under another id) is ignored, with a warning. Precondition: not
    /// running.
    void start(bool force, std::uint64_t generation, std::vector<ConfiguredSource> provided,
               std::function<void(Result<RefreshOutcome>)> done);
    /// Asks to stop: the indexer is terminated; done() then reports
    /// Cancelled. False when there is nothing left to cancel.
    bool cancel();

private:
    struct Assessment;
    void assessed(std::shared_ptr<Assessment> a);
    void indexerFinished(std::shared_ptr<Assessment> a, int exitCode, bool crashed);
    void finish(Result<RefreshOutcome> result);

    base::EventLoop& m_loop;
    const ServiceOptions& m_options;
    base::ThreadPool m_io{1};
    std::unique_ptr<base::Process> m_process;
    std::function<void(Result<RefreshOutcome>)> m_done;
    std::uint64_t m_generation = 0;
    bool m_force = false;
    bool m_running = false;
    bool m_replacing = false;
    bool m_cancelled = false;
};

} // namespace cutefish::store::service
