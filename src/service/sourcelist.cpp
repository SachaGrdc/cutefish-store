#include "sourcelist.h"

#include "marshal.h"

#include <algorithm>
#include <map>
#include <set>

namespace cutefish::store::service {

namespace {

bool canToggle(const BackendSource& s, bool userSourceChanges)
{
    if (!userSourceChanges || s.source.scope() != Scope::User)
        return false;
    // Disabling is always possible; enabling only a source whose signatures
    // are checked (D-044).
    return s.source.enabled() || !untrustedEnable(s);
}

bool canRemove(const BackendSource& s, bool userSourceChanges)
{
    return userSourceChanges && s.source.scope() == Scope::User && s.installedItems == std::optional<std::uint32_t>(0);
}

} // namespace

std::optional<Error> sourceRequestRefusal(const BackendSource& source, bool userSourceChanges,
                                          SourceRequest::Action action)
{
    const Source& s = source.source;
    if (!userSourceChanges)
        return Error::notSupported("the " + std::string(toString(s.backend())) + " sources are read-only");
    if (s.scope() != Scope::User)
        return Error::notAllowed("the sources of the system installation are read-only (D-011)");
    switch (action) {
    case SourceRequest::Action::Enable:
        if (s.enabled())
            return Error::notAllowed(s.id().str() + " is already enabled");
        return std::nullopt;
    case SourceRequest::Action::Disable:
        if (!s.enabled())
            return Error::notAllowed(s.id().str() + " is already disabled");
        return std::nullopt;
    case SourceRequest::Action::Remove:
        if (!source.installedItems)
            return Error::notAllowed("cannot tell what was installed from " + s.id().str());
        if (*source.installedItems > 0)
            return Error::notAllowed(s.id().str() + " is used by " + std::to_string(*source.installedItems)
                                     + " installed item(s) (D-041)");
        return std::nullopt;
    }
    return Error::internal("unknown source request");
}

bool untrustedEnable(const BackendSource& source)
{
    return source.verifiesSignatures != std::optional<bool>(true);
}

std::vector<base::dbus::VariantMap> sourceList(const std::vector<BackendSources>& backends,
                                               const std::vector<CatalogueSource>& catalogue)
{
    std::map<std::string, const CatalogueSource*> byId;
    for (const CatalogueSource& c : catalogue)
        byId.emplace(c.recorded.source.id().str(), &c);
    std::set<std::string> fed; // catalogue sources a configured source accounts for
    std::map<std::string, base::dbus::VariantMap> out;

    for (const BackendSources& b : backends) {
        for (const BackendSource& s : b.sources) {
            std::vector<std::string> feeds;
            if (s.catalogueSources.empty())
                feeds.push_back(s.source.id().str());
            for (const SourceId& id : s.catalogueSources)
                feeds.push_back(id.str());
            std::int64_t offers = 0;
            std::optional<UnixSeconds> lastRefresh;
            bool allRefreshed = true;
            bool anyFed = false;
            for (const std::string& id : feeds) {
                const auto it = byId.find(id);
                if (it == byId.end())
                    continue;
                anyFed = true;
                fed.insert(id);
                offers += it->second->recorded.offers;
                if (!it->second->lastRefresh)
                    allRefreshed = false;
                else if (!lastRefresh || *it->second->lastRefresh < *lastRefresh)
                    lastRefresh = it->second->lastRefresh;
            }
            const Source& src = s.source;
            base::dbus::VariantMap m{{"id", src.id().str()},
                                     {"backend", std::string(toString(src.backend()))},
                                     {"name", src.label().empty() ? src.origin() : src.label()},
                                     {"origin", src.origin()},
                                     {"url", src.url()},
                                     {"scope", std::string(toString(src.scope()))},
                                     {"kind", std::string(toString(src.kind()))},
                                     {"enabled", src.enabled()},
                                     {"configured", true},
                                     {"can-toggle", canToggle(s, b.userSourceChanges)},
                                     {"can-remove", canRemove(s, b.userSourceChanges)},
                                     {"suggested", false},
                                     {"offers", static_cast<std::uint32_t>(std::max<std::int64_t>(offers, 0))}};
            // The oldest refresh of what it feeds; absent when one of them
            // was never refreshed, or nothing is in the catalogue.
            if (anyFed && allRefreshed && lastRefresh)
                m.emplace("last-refresh", static_cast<std::uint64_t>(*lastRefresh));
            if (s.verifiesSignatures)
                m.emplace("gpg-verify", *s.verifiesSignatures);
            if (s.installedItems)
                m.emplace("installed-count", *s.installedItems);
            out.insert_or_assign(src.id().str(), std::move(m));
        }
    }
    for (const CatalogueSource& c : catalogue) {
        const std::string id = c.recorded.source.id().str();
        if (fed.contains(id) || out.contains(id))
            continue;
        base::dbus::VariantMap m = toMap(c.recorded, c.lastRefresh);
        m.insert_or_assign("configured", false);
        m.insert_or_assign("can-remove", false);
        out.emplace(id, std::move(m));
    }
    std::vector<base::dbus::VariantMap> list;
    list.reserve(out.size());
    for (auto& [id, m] : out)
        list.push_back(std::move(m));
    return list;
}

} // namespace cutefish::store::service
