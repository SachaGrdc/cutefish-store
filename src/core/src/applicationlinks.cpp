#include <cutefish/store/applicationlinks.h>

#include <algorithm>
#include <map>
#include <optional>

namespace cutefish::store {

namespace {

std::map<OfferId, AppId> firstApplication(std::span<const OfferApplication> links)
{
    std::map<OfferId, AppId> out;
    for (const auto& [offer, app] : links) {
        auto [it, inserted] = out.emplace(offer, app);
        if (!inserted && app < it->second)
            it->second = app;
    }
    return out;
}

} // namespace

std::vector<InstalledItem> linkInstalled(std::vector<InstalledItem> items, std::span<const OfferApplication> links)
{
    const auto apps = firstApplication(links);
    std::vector<InstalledItem> out;
    out.reserve(items.size());
    for (InstalledItem& item : items) {
        const auto it = apps.find(item.offer());
        if (item.app() || it == apps.end()) {
            out.push_back(std::move(item));
            continue;
        }
        auto linked = InstalledItem::create({.offer = item.offer(),
                                             .app = it->second,
                                             .backend = item.backend(),
                                             .scope = item.scope(),
                                             .version = item.version(),
                                             .origin = item.origin()});
        // The same fields as a valid item: create() cannot refuse them.
        out.push_back(linked ? std::move(linked).value() : std::move(item));
    }
    return out;
}

std::vector<AvailableUpdate> applicationUpdates(std::vector<AvailableUpdate> updates,
                                                std::span<const OfferApplication> links)
{
    const auto apps = firstApplication(links);
    std::vector<AvailableUpdate> out;
    for (AvailableUpdate& u : updates) {
        if (u.kind == PlanItemKind::Runtime) {
            out.push_back(std::move(u));
            continue;
        }
        const auto it = apps.find(u.offer);
        if (it == apps.end())
            continue;
        if (!u.app)
            u.app = it->second;
        u.kind = PlanItemKind::Application;
        out.push_back(std::move(u));
    }
    return out;
}

} // namespace cutefish::store
