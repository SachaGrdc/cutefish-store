#pragma once

// The Flatpak backend (ARCHITECTURE §7.3, step G): the core's Backend
// contract over a FlatpakStore (flatpakstore.h). It owns a single worker
// thread (worker.h) on which all libflatpak objects live and all store calls
// run; completions and events are posted to the host's main loop through the
// `post` function the host gave it, in order, never from inside the call
// (CORE.md, Backend contract). Independent of Cutefish and D-Bus (P-006).

#include "flatpakstore.h"
#include "worker.h"

#include <cutefish/store/backend.h>

#include <atomic>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace cutefish::store::flatpak {

class FlatpakBackend final : public Backend
{
public:
    struct Options
    {
        /// Posts a task to the host's main loop (P-006: the only injection
        /// point). Dropped when the loop is gone.
        std::function<void(std::function<void()>)> post;
    };

    /// `store` is used only on the worker thread; the caller must not touch
    /// it afterwards. In production it is a LibFlatpakStore; tests pass a
    /// double.
    FlatpakBackend(Options options, std::unique_ptr<FlatpakStore> store);
    ~FlatpakBackend() override;

    BackendKind kind() const noexcept override { return BackendKind::Flatpak; }
    BackendCapabilities capabilities() const noexcept override;

    void availability(Completion<void> done) override;
    void queryInstalled(Completion<std::vector<InstalledItem>> done) override;
    void sources(Completion<std::vector<Source>> done) override;
    void catalogSources(Completion<std::vector<SourceMetadata>> done) override;
    void queryUpdates(Completion<std::vector<AvailableUpdate>> done) override;
    void permissions(const OfferId& offer, Completion<std::vector<std::string>> done) override;
    void plan(const std::string& operationId, const PlanRequest& request, Completion<Plan> done) override;
    void execute(const std::string& operationId, const PlanRequest& request, const Plan& accepted,
                 OperationEvents events, Completion<ExecutionResult> done) override;
    void reattach(const std::string& operationId, const std::string& backendRef, OperationEvents events,
                  Completion<ExecutionResult> done) override;
    void refresh(const std::string& operationId, OperationEvents events, Completion<void> done) override;
    Result<void> cancel(const std::string& operationId) override;
    void setChangeListener(std::function<void(const BackendChanges&)> listener) override;

private:
    /// Runs a store call on the worker thread and delivers the result on the
    /// main loop.
    template <typename T>
    void run(std::function<Result<T>()> work, Completion<T> done)
    {
        auto alive = m_alive;
        auto post = m_options.post;
        m_worker.run([alive, post, work = std::move(work), done = std::move(done)]() mutable {
            Result<T> result = [&work]() -> Result<T> {
                try {
                    return work();
                } catch (const std::exception& e) {
                    return Error::internal(std::string("Flatpak backend: ") + e.what());
                } catch (...) {
                    return Error::internal("Flatpak backend: unknown exception");
                }
            }();
            post([alive, done = std::move(done), result = std::move(result)]() mutable {
                if (*alive)
                    done(std::move(result));
            });
        });
    }

    Options m_options;
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
    std::mutex m_mutex; // guards m_cancelled (main thread vs. worker thread)
    std::map<std::string, std::shared_ptr<std::atomic<bool>>> m_cancelled; // by operation id
    std::function<void(const BackendChanges&)> m_listener;
    std::unique_ptr<FlatpakStore> m_store;
    /// Destroyed last (reverse member order): the thread is joined before the
    /// store and the options go away.
    SerialExecutor m_worker;
};

} // namespace cutefish::store::flatpak
