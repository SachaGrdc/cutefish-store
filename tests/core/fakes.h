#pragma once

// In-memory implementations of the core interfaces, for tests. They show
// what a real implementation must do; they are not part of store-core.

#include <cutefish/store/backend.h>
#include <cutefish/store/catalog.h>
#include <cutefish/store/installedstate.h>

#include <algorithm>
#include <cctype>
#include <deque>
#include <functional>
#include <map>
#include <utility>
#include <vector>

namespace fakes {

using namespace cutefish::store;

inline std::string lower(std::string_view s)
{
    std::string out(s);
    for (char& c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

class FakeInstalledState final : public InstalledState
{
public:
    explicit FakeInstalledState(std::vector<InstalledItem> items = {}) : m_items(std::move(items)) {}

    void setFailure(std::optional<Error> e) { m_failure = std::move(e); }

    Result<std::vector<InstalledItem>> items(std::optional<BackendKind> backend) const override
    {
        if (m_failure)
            return *m_failure;
        std::vector<InstalledItem> out;
        for (const InstalledItem& i : m_items)
            if (!backend || i.backend() == *backend)
                out.push_back(i);
        return out;
    }

    Result<std::optional<InstalledItem>> find(const OfferId& offer) const override
    {
        if (m_failure)
            return *m_failure;
        for (const InstalledItem& i : m_items)
            if (i.offer() == offer)
                return std::optional<InstalledItem>(i);
        return std::optional<InstalledItem>();
    }

    Result<std::vector<InstalledItem>> forApplication(const AppId& app) const override
    {
        if (m_failure)
            return *m_failure;
        std::vector<InstalledItem> out;
        for (const InstalledItem& i : m_items)
            if (i.app() && *i.app() == app)
                out.push_back(i);
        return out;
    }

private:
    std::vector<InstalledItem> m_items;
    std::optional<Error> m_failure;
};

/// Case-insensitive ASCII substring search on names and summaries; the
/// real catalogue uses FTS5.
class FakeCatalog final : public Catalog
{
public:
    FakeCatalog(std::uint64_t generation, std::vector<Application> apps, const InstalledState* installed = nullptr)
        : m_generation(generation), m_apps(std::move(apps)), m_installed(installed)
    {
    }

    std::uint64_t generation() const noexcept override { return m_generation; }

    Result<Application> application(const AppId& id) const override
    {
        for (const Application& a : m_apps)
            if (a.isKnownAs(id))
                return a;
        return Error::notFound("no application " + id.str());
    }

    Result<SearchPage> search(const SearchQuery& query) const override
    {
        SearchPage page{.generation = m_generation, .total = 0, .items = {}};
        if (query.text().empty())
            return page;
        const std::string needle = lower(query.text());
        const SearchOptions& o = query.options();
        for (const Application& a : m_apps) {
            if (lower(a.name()).find(needle) == std::string::npos &&
                lower(a.summary()).find(needle) == std::string::npos)
                continue;
            if (!o.category.empty() && !a.hasCategory(o.category))
                continue;
            if (o.backend && !a.hasBackend(*o.backend))
                continue;
            AppSummary s = summarize(a);
            if (o.installedOnly && !s.installed)
                continue;
            if (page.total >= o.offset && page.items.size() < o.limit)
                page.items.push_back(std::move(s));
            ++page.total;
        }
        return page;
    }

    Result<SearchPage> browse(const BrowseQuery& query) const override
    {
        const SearchOptions& o = query.options();
        std::vector<const Application*> sorted;
        for (const Application& a : m_apps)
            sorted.push_back(&a);
        std::sort(sorted.begin(), sorted.end(), [](const Application* x, const Application* y) {
            const std::string a = lower(x->name()), b = lower(y->name());
            return a != b ? a < b : x->id() < y->id();
        });
        SearchPage page{.generation = m_generation, .total = 0, .items = {}};
        for (const Application* a : sorted) {
            if (!o.category.empty() && !a->hasCategory(o.category))
                continue;
            if (o.backend && !a->hasBackend(*o.backend))
                continue;
            AppSummary s = summarize(*a);
            if (o.installedOnly && !s.installed)
                continue;
            if (page.total >= o.offset && page.items.size() < o.limit)
                page.items.push_back(std::move(s));
            ++page.total;
        }
        return page;
    }

    Result<std::vector<CategoryCount>> listCategories() const override
    {
        std::map<std::string, std::uint32_t> counts;
        for (const Application& a : m_apps)
            for (const std::string& c : a.categories())
                ++counts[c];
        std::vector<CategoryCount> out;
        for (const auto& [c, n] : counts)
            out.push_back({c, n});
        return out;
    }

    /// What sources() returns; set by the test.
    std::vector<RecordedSource> recordedSources;

    Result<std::vector<RecordedSource>> sources() const override { return recordedSources; }

    Result<std::vector<AppSummary>> summaries(std::span<const AppId> ids) const override
    {
        std::vector<AppSummary> out;
        out.reserve(ids.size());
        for (const AppId& id : ids)
            if (auto app = application(id))
                out.push_back(summarize(*app));
        return out;
    }

private:
    AppSummary summarize(const Application& a) const
    {
        AppSummary s{.id = a.id(), .name = a.name(), .summary = a.summary(), .icon = a.icon(), .backends = {}};
        for (const Offer& o : a.offers())
            s.backends.insert(o.backend());
        if (m_installed)
            if (auto items = m_installed->forApplication(a.id()))
                s.installed = !items->empty();
        return s;
    }

    std::uint64_t m_generation;
    std::vector<Application> m_apps;
    const InstalledState* m_installed;
};

/// Stands for the host's `post`: work is queued and run by runAll(), as the
/// host's main loop would do.
class PostQueue
{
public:
    void post(std::function<void()> f) { m_queue.push_back(std::move(f)); }
    std::size_t pending() const { return m_queue.size(); }
    void runAll()
    {
        while (!m_queue.empty()) {
            auto f = std::move(m_queue.front());
            m_queue.pop_front();
            f();
        }
    }

private:
    std::deque<std::function<void()>> m_queue;
};

/// A backend whose answers are set by the test and delivered through the
/// PostQueue, never from inside the call.
class FakeBackend final : public Backend
{
public:
    FakeBackend(BackendKind kind, PostQueue& queue) : m_kind(kind), m_queue(queue) {}

    Result<void> availabilityAnswer = Result<void>::success();
    Result<std::vector<InstalledItem>> installedAnswer = std::vector<InstalledItem>{};
    Result<std::vector<Source>> sourcesAnswer = std::vector<Source>{};
    int calls = 0;

    BackendKind kind() const noexcept override { return m_kind; }

    void availability(Completion<void> done) override { deliver(std::move(done), availabilityAnswer); }
    void queryInstalled(Completion<std::vector<InstalledItem>> done) override
    {
        deliver(std::move(done), installedAnswer);
    }
    void sources(Completion<std::vector<Source>> done) override { deliver(std::move(done), sourcesAnswer); }

private:
    template <typename T>
    void deliver(Completion<T> done, Result<T> answer)
    {
        ++calls;
        m_queue.post([done = std::move(done), answer = std::move(answer)]() mutable { done(std::move(answer)); });
    }

    BackendKind m_kind;
    PostQueue& m_queue;
};

} // namespace fakes
