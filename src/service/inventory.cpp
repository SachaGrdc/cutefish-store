#include "inventory.h"

#include <cutefish/store/applicationlinks.h>
#include <cutefish/store/offer.h>
#include <cutefish/store/sqlitecatalog.h>
#include <cutefish/store/statestore.h>

#include <base/logging.h>

#include <algorithm>
#include <iterator>
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


auto key(const InstalledItem& i)
{
    return std::tuple(i.offer().str(), text(i.app()), text(i.version()), i.origin(), i.scope());
}

/// The backend an offer id names, if it is an offer id the core builds.
std::optional<BackendKind> backendOf(const OfferId& offer)
{
    auto address = decodeOfferId(offer);
    if (!address)
        return std::nullopt;
    return address->backend;
}

} // namespace

std::vector<InstalledItem> normalizeInstalled(BackendKind kind, std::vector<InstalledItem> items)
{
    std::vector<InstalledItem> out;
    out.reserve(items.size());
    for (InstalledItem& i : items) {
        if (i.backend() != kind || backendOf(i.offer()) != kind) {
            CF_WARNING(cfInventory) << toString(kind) << ": installed item of another backend left out: "
                                    << i.offer().str();
            continue;
        }
        out.push_back(std::move(i));
    }
    std::sort(out.begin(), out.end(), [](const InstalledItem& a, const InstalledItem& b) { return key(a) < key(b); });
    const auto end = std::unique(out.begin(), out.end(),
                                 [](const InstalledItem& a, const InstalledItem& b) { return a.offer() == b.offer(); });
    if (end != out.end()) {
        CF_INFO(cfInventory) << toString(kind) << ": " << std::distance(end, out.end())
                             << " installed offer(s) listed twice, kept once";
    }
    out.erase(end, out.end());
    return out;
}

std::vector<AvailableUpdate> normalizeUpdates(BackendKind kind, std::vector<AvailableUpdate> updates)
{
    std::vector<AvailableUpdate> out;
    out.reserve(updates.size());
    for (AvailableUpdate& u : updates) {
        if (backendOf(u.offer) != kind) {
            CF_WARNING(cfInventory) << toString(kind) << ": update of another backend left out: " << u.offer.str();
            continue;
        }
        out.push_back(std::move(u));
    }
    std::sort(out.begin(), out.end(), [](const AvailableUpdate& a, const AvailableUpdate& b) { return key(a) < key(b); });
    const auto end = std::unique(out.begin(), out.end(),
                                 [](const AvailableUpdate& a, const AvailableUpdate& b) { return a.offer == b.offer; });
    out.erase(end, out.end());
    return out;
}

std::vector<BackendSource> normalizeSources(BackendKind kind, std::vector<BackendSource> sources)
{
    std::vector<BackendSource> out;
    out.reserve(sources.size());
    for (BackendSource& s : sources) {
        if (s.source.backend() != kind) {
            CF_WARNING(cfInventory) << toString(kind) << ": source of another backend left out: " << s.source.id().str();
            continue;
        }
        out.push_back(std::move(s));
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const BackendSource& a, const BackendSource& b) { return a.source.id() < b.source.id(); });
    const auto end = std::unique(out.begin(), out.end(), [](const BackendSource& a, const BackendSource& b) {
        return a.source.id() == b.source.id();
    });
    out.erase(end, out.end());
    return out;
}

bool sameSources(const std::vector<BackendSource>& a, const std::vector<BackendSource>& b)
{
    auto key = [](const BackendSource& x) {
        std::vector<std::string> fed;
        for (const SourceId& id : x.catalogueSources)
            fed.push_back(id.str());
        const Source& s = x.source;
        return std::tuple(s.id().str(), s.backend(), s.scope(), s.kind(), s.origin(), s.label(), s.url(), s.enabled(),
                          s.priority(), fed, x.verifiesSignatures, x.installedItems);
    };
    return std::equal(a.begin(), a.end(), b.begin(), b.end(),
                      [&](const BackendSource& x, const BackendSource& y) { return key(x) == key(y); });
}

Inventory::Inventory(Backend& backend, std::filesystem::path catalog, InventoryIo& io)
    : m_backend(backend), m_catalogPath(std::move(catalog)), m_io(io)
{
    m_retry.setSingleShot(true);
    m_retry.onTimeout = [this] { retryWrite(); };
    m_backend.setChangeListener([this](const BackendChanges& changes) {
        // What the package manager runs for other programs (H.2): only the
        // Backends property changes; nothing to read again.
        if (changes.activity && onAvailabilityChanged)
            onAvailabilityChanged();
        if (!changes.installed && !changes.updates && !changes.sources)
            return;
        // A daemon that came back, or a change made outside the store.
        if (m_available != true)
            return check();
        if (changes.installed)
            reconcileInstalled();
        if (changes.installed || changes.updates)
            reconcileUpdates();
        // The list of sources is read again at once (H.5); their metadata
        // reaches the catalogue at the next refresh (catalogSources).
        if (changes.sources) {
            CF_INFO(cfInventory) << toString(m_backend.kind()) << ": sources changed";
            reconcileSources();
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
            // Nor can its sources be changed: they are listed from the
            // catalogue only until it comes back (H.5).
            setSources({});
            return;
        }
        setAvailable(true, {});
        reconcileInstalled();
        reconcileUpdates();
        reconcileSources();
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
        Pending p;
        if (caps.systemScope)
            p.scopes.insert(Scope::System);
        if (caps.userScope)
            p.scopes.insert(Scope::User);
        p.items = normalizeInstalled(kind, std::move(items).value());
        for (const InstalledItem& i : p.items)
            p.scopes.insert(i.scope());
        p.generation = m_generation;
        m_pending = std::move(p);
        writeInstalled();
    });
}

void Inventory::writeInstalled()
{
    // The inventory read from the backend is kept (m_pending) until it is
    // recorded: a failed attempt is tried again from it, without asking the
    // backend again.
    ++m_pending->attempts;
    m_installedJob = m_io.write(
            [catalog = m_catalogPath, kind = m_backend.kind(), p = *m_pending](StateStore& store) mutable
            -> Result<InstalledOutcome> {
                std::vector<OfferId> offers;
                for (const InstalledItem& i : p.items)
                    offers.push_back(i.offer());
                const auto links = linksFor(catalog, p.generation, offers);
                std::vector<InstalledItem> linked = linkInstalled(std::move(p.items), links);

                auto previous = store.items(kind);
                if (!previous)
                    return std::move(previous).error();
                // Every scope of the backend in one SQLite transaction: an
                // attempt is applied entirely or not at all, so trying it
                // again never applies half of it twice, and the difference
                // with `previous` (read on this thread, the only writer of
                // these rows) is exact even after a failed attempt.
                const std::vector<Scope> scopes(p.scopes.begin(), p.scopes.end());
                if (auto ok = store.replaceInstalled(kind, scopes, linked, now()); !ok)
                    return std::move(ok).error();

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
            [this](Result<InstalledOutcome> outcome) { installedWritten(std::move(outcome)); });
}

void Inventory::installedWritten(Result<InstalledOutcome> outcome)
{
    if (outcome) {
        m_pending.reset();
        setRecordError({});
        if (outcome->changed && onInstalledChanged)
            onInstalledChanged(outcome->apps);
        return installedDone();
    }
    const Error& e = outcome.error();
    // The writer was stopped (the service is shutting down): the write was
    // abandoned on purpose; nothing to report or try again.
    if (e.code() == ErrorCode::Cancelled && m_io.stopping()) {
        m_pending.reset();
        m_installedRun.again = false;
        return installedDone();
    }
    const bool temporary = e.code() == ErrorCode::Conflict;
    const auto& delays = m_io.options().retryDelays;
    const int attempts = m_pending->attempts;
    // A newer reconciliation was asked meanwhile: it supersedes this one
    // (installedDone() runs it, from a new answer of the backend).
    if (m_installedRun.again) {
        m_pending.reset();
        return installedDone();
    }
    if (temporary && static_cast<std::size_t>(attempts) <= delays.size()) {
        const auto delay = delays[static_cast<std::size_t>(attempts) - 1];
        CF_INFO(cfInventory) << toString(m_backend.kind()) << ": installed state not recorded yet (attempt "
                             << attempts << "): " << e.message() << "; trying again in " << delay.count() << " ms";
        // Still running: a request meanwhile only marks `again`.
        m_retry.start(delay);
        return;
    }
    // A permanent error, or a temporary one that lasted past every attempt:
    // reported (the Backends property carries it) and kept until a
    // reconciliation goes through. The previous inventory stays recorded.
    CF_WARNING(cfInventory) << toString(m_backend.kind()) << ": installed state not recorded after " << attempts
                            << " attempt(s)" << (temporary ? " (the file stayed locked)" : "") << ": "
                            << e.message();
    m_pending.reset();
    setRecordError(e.message());
    installedDone();
}

void Inventory::retryWrite()
{
    if (!m_pending)
        return;
    if (m_installedRun.again) {
        m_pending.reset();
        return installedDone();
    }
    writeInstalled();
}

void Inventory::setRecordError(std::string error)
{
    if (error == m_recordError)
        return;
    m_recordError = std::move(error);
    if (onAvailabilityChanged)
        onAvailabilityChanged();
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
                [catalog = m_catalogPath, generation = m_generation, kind = m_backend.kind(),
                 updates = std::move(updates).value()]() mutable -> std::vector<AvailableUpdate> {
                    updates = normalizeUpdates(kind, std::move(updates));
                    std::vector<OfferId> offers;
                    for (const AvailableUpdate& u : updates)
                        offers.push_back(u.offer);
                    const auto links = linksFor(catalog, generation, offers);
                    // D-010: what the backend sees, filtered by the catalogue.
                    // The filter keeps the order: still one entry per offer,
                    // sorted by offer id.
                    return applicationUpdates(std::move(updates), links);
                },
                [this](std::vector<AvailableUpdate> list) {
                    // The backend became unusable while the list was being
                    // read: its updates cannot be acted on (check() cleared
                    // them), a late answer must not bring them back.
                    if (m_available != true)
                        list.clear();
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

void Inventory::reconcileSources()
{
    if (m_available != true)
        return;
    if (m_sourcesRun.running) {
        m_sourcesRun.again = true;
        return;
    }
    m_sourcesRun.running = true;
    m_backend.sources([this, alive = m_alive](Result<std::vector<BackendSource>> sources) {
        if (!*alive)
            return;
        if (!sources) {
            // The last list stays: it is the last one seen.
            CF_WARNING(cfInventory) << toString(m_backend.kind()) << ": sources not read: " << sources.error().message();
            return sourcesDone();
        }
        // Like the updates: a late answer of a backend that became unusable
        // meanwhile does not bring its sources back.
        if (m_available == true)
            setSources(normalizeSources(m_backend.kind(), std::move(sources).value()));
        sourcesDone();
    });
}

void Inventory::sourcesDone()
{
    m_sourcesRun.running = false;
    if (std::exchange(m_sourcesRun.again, false))
        reconcileSources();
}

void Inventory::setSources(std::vector<BackendSource> sources)
{
    if (sameSources(sources, m_sources))
        return;
    m_sources = std::move(sources);
    if (onSourcesChanged)
        onSourcesChanged();
}

} // namespace cutefish::store::service
