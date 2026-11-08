#include "inventory.h"

#include <cutefish/store/applicationlinks.h>
#include <cutefish/store/sqlitecatalog.h>
#include <cutefish/store/statestore.h>

#include <base/logging.h>

#include <algorithm>
#include <ctime>
#include <map>
#include <set>
#include <system_error>
#include <tuple>

namespace cutefish::store::service {

namespace {

base::LogCategory cfInventory("cutefish.store.inventory");

UnixSeconds now()
{
    return static_cast<UnixSeconds>(std::time(nullptr));
}

/// The applications of the catalogue in service for these offers; none
/// without a catalogue (the items then stay unlinked, for display only).
std::vector<OfferApplication> linksFor(const std::filesystem::path& catalog, std::uint64_t generation,
                                       const std::vector<OfferId>& offers)
{
    std::error_code ec;
    if (generation == 0 || offers.empty() || !std::filesystem::exists(catalog, ec))
        return {};
    auto c = SqliteCatalog::open(catalog);
    if (!c) {
        CF_WARNING(cfInventory) << "cannot read the catalogue to link: " << c.error().message();
        return {};
    }
    auto links = c->offerApplications(offers);
    if (!links) {
        CF_WARNING(cfInventory) << "cannot link to the catalogue: " << links.error().message();
        return {};
    }
    return std::move(links).value();
}

std::string text(const std::optional<AppId>& a)
{
    return a ? a->str() : std::string();
}

std::string text(const std::optional<Version>& v)
{
    return v ? v->str() : std::string();
}

auto key(const AvailableUpdate& u)
{
    return std::tuple(u.offer.str(), text(u.app), u.kind, text(u.fromVersion), text(u.toVersion), u.downloadSize);
}

bool same(const std::vector<AvailableUpdate>& a, const std::vector<AvailableUpdate>& b)
{
    return std::equal(a.begin(), a.end(), b.begin(), b.end(),
                      [](const AvailableUpdate& x, const AvailableUpdate& y) { return key(x) == key(y); });
}

struct InstalledOutcome
{
    bool changed = false;
    std::vector<std::string> apps;
};

} // namespace

/// The worker thread's connection to state.sqlite, opened at its first use.
struct Inventory::Disk
{
    std::optional<StateStore> store;
};

Inventory::Inventory(Backend& backend, std::filesystem::path catalog, std::filesystem::path state)
    : m_backend(backend), m_catalogPath(std::move(catalog)), m_statePath(std::move(state)),
      m_disk(std::make_shared<Disk>())
{
    m_backend.setChangeListener([this](const BackendChanges& changes) {
        // A daemon that came back, or a change made outside the store.
        if (m_available != true)
            return check();
        if (changes.installed)
            reconcileInstalled();
        if (changes.installed || changes.updates)
            reconcileUpdates();
        // Repositories reach the catalogue at the next refresh (catalogSources).
        if (changes.sources) {
            CF_INFO(cfInventory) << toString(m_backend.kind()) << ": repositories changed";
        }
    });
}

Inventory::~Inventory()
{
    *m_alive = false;
    m_backend.setChangeListener({});
}

void Inventory::setAvailable(bool available, std::string reason)
{
    const bool changed = m_available != available || m_reason != reason;
    m_available = available;
    m_reason = std::move(reason);
    if (changed && onAvailabilityChanged)
        onAvailabilityChanged();
}

void Inventory::check()
{
    if (m_checking)
        return;
    m_checking = true;
    m_backend.availability([this, alive = m_alive](Result<void> r) {
        if (!*alive)
            return;
        m_checking = false;
        if (!r) {
            CF_WARNING(cfInventory) << toString(m_backend.kind()) << " unavailable: " << r.error().message();
            setAvailable(false, r.error().message());
            // The recorded inventory stays: it is the last one seen. The
            // updates cannot be acted on without the backend.
            if (!m_updates.empty()) {
                m_updates.clear();
                if (onUpdatesChanged)
                    onUpdatesChanged();
            }
            return;
        }
        setAvailable(true, {});
        reconcileInstalled();
        reconcileUpdates();
    });
}

void Inventory::setGeneration(std::uint64_t generation)
{
    if (generation == m_generation)
        return;
    m_generation = generation;
    if (m_available == true) {
        reconcileInstalled();
        reconcileUpdates();
    }
}

void Inventory::reconcileInstalled()
{
    if (m_installedRun.running) {
        m_installedRun.again = true;
        return;
    }
    m_installedRun.running = true;
    m_backend.queryInstalled([this, alive = m_alive](Result<std::vector<InstalledItem>> items) {
        if (!*alive)
            return;
        if (!items) {
            CF_WARNING(cfInventory) << "installed state not read: " << items.error().message();
            return installedDone();
        }
        const BackendKind kind = m_backend.kind();
        const BackendCapabilities caps = m_backend.capabilities();
        std::set<Scope> scopes;
        if (caps.systemScope)
            scopes.insert(Scope::System);
        if (caps.userScope)
            scopes.insert(Scope::User);
        for (const InstalledItem& i : *items)
            scopes.insert(i.scope());
        m_installedJob = m_io.run(
                [disk = m_disk, catalog = m_catalogPath, state = m_statePath, generation = m_generation, kind,
                 scopes, items = std::move(items).value()]() mutable -> Result<InstalledOutcome> {
                    if (!disk->store) {
                        auto s = StateStore::open(state, now());
                        if (!s)
                            return std::move(s).error();
                        disk->store.emplace(std::move(s).value());
                    }
                    std::vector<OfferId> offers;
                    for (const InstalledItem& i : items)
                        offers.push_back(i.offer());
                    const auto links = linksFor(catalog, generation, offers);
                    std::vector<InstalledItem> linked = linkInstalled(std::move(items), links);

                    auto previous = disk->store->items(kind);
                    if (!previous)
                        return std::move(previous).error();
                    const UnixSeconds t = now();
                    for (Scope scope : scopes) {
                        std::vector<InstalledItem> part;
                        for (const InstalledItem& i : linked)
                            if (i.scope() == scope)
                                part.push_back(i);
                        if (auto ok = disk->store->replaceInstalled(kind, scope, part, t); !ok)
                            return std::move(ok).error();
                    }

                    // What changed, by offer: the applications to tell about.
                    using Seen = std::pair<std::string, std::string>; // app, version
                    std::map<std::string, Seen> before;
                    std::map<std::string, Seen> after;
                    for (const InstalledItem& i : *previous)
                        before.emplace(i.offer().str(), Seen{text(i.app()), text(i.version())});
                    for (const InstalledItem& i : linked)
                        after.emplace(i.offer().str(), Seen{text(i.app()), text(i.version())});
                    InstalledOutcome out;
                    std::set<std::string> apps;
                    auto note = [&](const std::map<std::string, Seen>& from, const std::map<std::string, Seen>& to) {
                        for (const auto& [offer, seen] : from) {
                            const auto it = to.find(offer);
                            if (it != to.end() && it->second == seen)
                                continue;
                            out.changed = true;
                            if (!seen.first.empty())
                                apps.insert(seen.first);
                        }
                    };
                    note(before, after);
                    note(after, before);
                    out.apps.assign(apps.begin(), apps.end());
                    return out;
                },
                [this](Result<InstalledOutcome> outcome) {
                    if (!outcome)
                        CF_WARNING(cfInventory) << "installed state not recorded: " << outcome.error().message();
                    else if (outcome->changed && onInstalledChanged)
                        onInstalledChanged(outcome->apps);
                    installedDone();
                });
    });
}

void Inventory::installedDone()
{
    m_installedRun.running = false;
    if (std::exchange(m_installedRun.again, false))
        reconcileInstalled();
}

void Inventory::reconcileUpdates()
{
    if (m_updatesRun.running) {
        m_updatesRun.again = true;
        return;
    }
    m_updatesRun.running = true;
    m_backend.queryUpdates([this, alive = m_alive](Result<std::vector<AvailableUpdate>> updates) {
        if (!*alive)
            return;
        if (!updates) {
            CF_WARNING(cfInventory) << "updates not read: " << updates.error().message();
            return updatesDone();
        }
        m_updatesJob = m_io.run(
                [catalog = m_catalogPath, generation = m_generation,
                 updates = std::move(updates).value()]() mutable -> std::vector<AvailableUpdate> {
                    std::vector<OfferId> offers;
                    for (const AvailableUpdate& u : updates)
                        offers.push_back(u.offer);
                    const auto links = linksFor(catalog, generation, offers);
                    // D-010: what the backend sees, filtered by the catalogue.
                    return applicationUpdates(std::move(updates), links);
                },
                [this](std::vector<AvailableUpdate> list) {
                    const bool changed = !same(list, m_updates);
                    m_updates = std::move(list);
                    if (changed && onUpdatesChanged)
                        onUpdatesChanged();
                    updatesDone();
                });
    });
}

void Inventory::updatesDone()
{
    m_updatesRun.running = false;
    if (std::exchange(m_updatesRun.again, false))
        reconcileUpdates();
}

} // namespace cutefish::store::service
