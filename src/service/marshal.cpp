#include "marshal.h"

#include <cutefish/store/defaultoffer.h>

#include <tuple>

namespace cutefish::store::service {

namespace {

std::string_view iconKind(IconKind kind)
{
    switch (kind) {
    case IconKind::None: return "none";
    case IconKind::LocalPath: return "local";
    case IconKind::RemoteUrl: return "remote";
    case IconKind::StockName: return "stock";
    }
    return "none";
}

std::string_view blockType(DescriptionBlockType type)
{
    switch (type) {
    case DescriptionBlockType::Paragraph: return "p";
    case DescriptionBlockType::ListItem: return "li";
    case DescriptionBlockType::OrderedListItem: return "oli";
    }
    return "p";
}

std::vector<std::string> backendNames(BackendSet set)
{
    std::vector<std::string> out;
    for (const BackendKind k : {BackendKind::PackageKit, BackendKind::Flatpak})
        if (set.contains(k))
            out.emplace_back(toString(k));
    return out;
}

Error wrongType(const std::string& key, const char* expected)
{
    return Error::invalidArgument("option " + key + " must be of type " + expected);
}

} // namespace

DbusError toDbusError(const Error& e)
{
    switch (e.code()) {
    case ErrorCode::InvalidArgument: return {errors::kInvalidArgs, e.message()};
    case ErrorCode::NotFound: return {errors::kNotFound, e.message()};
    case ErrorCode::NotSupported: return {errors::kNotSupported, e.message()};
    case ErrorCode::Conflict: return {errors::kBusy, e.message()};
    case ErrorCode::NotAllowed: return {errors::kNotAllowed, e.message()};
    case ErrorCode::Backend:
        if (e.backendFailure() == BackendFailure::Unavailable)
            return {errors::kBackendUnavailable, e.message()};
        return {errors::kFailed, e.message()};
    case ErrorCode::Cancelled:
    case ErrorCode::Internal:
        return {errors::kFailed, e.message()};
    }
    return {errors::kFailed, e.message()};
}

Result<SearchOptions> searchOptions(const dbus::VariantMap& options)
{
    SearchOptions o;
    for (const auto& [key, value] : options) {
        if (key == "limit") {
            const auto v = value.get<std::uint32_t>();
            if (!v)
                return wrongType(key, "u");
            o.limit = *v;
        } else if (key == "offset") {
            const auto v = value.get<std::uint32_t>();
            if (!v)
                return wrongType(key, "u");
            o.offset = *v;
        } else if (key == "category") {
            const auto v = value.get<std::string>();
            if (!v)
                return wrongType(key, "s");
            o.category = *v;
        } else if (key == "backend") {
            const auto v = value.get<std::string>();
            if (!v)
                return wrongType(key, "s");
            const auto b = parseBackendKind(*v);
            if (!b)
                return Error::invalidArgument("unknown backend " + *v);
            o.backend = *b;
        } else if (key == "installed-only") {
            const auto v = value.get<bool>();
            if (!v)
                return wrongType(key, "b");
            o.installedOnly = *v;
        } else {
            return Error::invalidArgument("unknown option " + key);
        }
    }
    return o;
}

Result<std::optional<BackendKind>> installedOptions(const dbus::VariantMap& options)
{
    std::optional<BackendKind> backend;
    for (const auto& [key, value] : options) {
        if (key != "backend")
            return Error::invalidArgument("unknown option " + key);
        const auto v = value.get<std::string>();
        if (!v)
            return wrongType(key, "s");
        backend = parseBackendKind(*v);
        if (!backend)
            return Error::invalidArgument("unknown backend " + *v);
    }
    return backend;
}

std::string newInstallRefusal(const OfferId& offer)
{
    auto address = decodeOfferId(offer);
    if (!address)
        return {}; // not this rule's concern: the offer is refused further on
    if (address->scope != Scope::System
        || !ConfirmationPolicy::defaultSharedSystemScope().contains(address->backend))
        return {};
    return "new " + std::string(toString(address->backend))
            + " installations go to the user installation; " + offer.str()
            + " is in the system installation";
}

dbus::VariantMap toMap(const AppSummary& s)
{
    return {{"id", s.id.str()},
            {"name", s.name},
            {"summary", s.summary},
            {"icon", s.icon.value},
            {"icon-kind", std::string(iconKind(s.icon.kind))},
            {"backends", backendNames(s.backends)},
            {"installed", s.installed},
            {"has-update", s.hasUpdate},
            {"verified", s.verified},
            {"developer", s.developer}};
}

dbus::VariantMap toMap(const CategoryCount& c)
{
    return {{"id", c.category}, {"count", c.applications}};
}

dbus::VariantMap toMap(const InstalledItem& i)
{
    dbus::VariantMap m{{"offer-id", i.offer().str()},
                       {"app-id", i.app() ? i.app()->str() : std::string()},
                       {"backend", std::string(toString(i.backend()))},
                       {"scope", std::string(toString(i.scope()))},
                       {"origin", i.origin()}};
    if (i.version())
        m.emplace("version", i.version()->str());
    return m;
}

dbus::VariantMap toMap(const AvailableUpdate& u, BackendKind backend)
{
    dbus::VariantMap m{{"offer-id", u.offer.str()},
                       {"backend", std::string(toString(backend))},
                       {"kind", std::string(toString(u.kind))},
                       {"download-size", u.downloadSize}};
    if (u.app)
        m.emplace("app-id", u.app->str());
    if (u.fromVersion)
        m.emplace("from-version", u.fromVersion->str());
    if (u.toVersion)
        m.emplace("to-version", u.toVersion->str());
    return m;
}

dbus::VariantMap planMap(const Plan& plan, const ConfirmationReasons& reasons,
                         std::span<const ConfirmationConcern> concerns)
{
    std::vector<std::string> all;
    for (const ConfirmationReason r : reasons.list())
        all.emplace_back(toString(r));
    std::vector<dbus::VariantMap> operations;
    for (std::size_t i = 0; i < plan.items().size(); ++i) {
        const PlanItem& item = plan.items()[i];
        std::vector<std::string> itemReasons;
        for (const ConfirmationConcern& c : concerns)
            if (c.item == i)
                itemReasons.emplace_back(toString(c.reason));
        dbus::VariantMap m{{"operation", std::string(toString(item.operation()))},
                           {"kind", std::string(toString(item.kind()))},
                           {"package", item.package()},
                           {"requested", item.requested()},
                           {"download-size", item.downloadSize()},
                           {"reasons", itemReasons}};
        if (item.fromVersion())
            m.emplace("from-version", item.fromVersion()->str());
        if (item.toVersion())
            m.emplace("to-version", item.toVersion()->str());
        if (!item.origin().empty())
            m.emplace("origin", item.origin());
        operations.push_back(std::move(m));
    }
    return {{"kind", std::string(toString(plan.kind()))},
            {"backend", std::string(toString(plan.backend()))},
            {"scope", std::string(toString(plan.scope()))},
            {"requires-confirmation", !reasons.empty()},
            {"reasons", all},
            {"download-size", plan.downloadSize()},
            {"installed-size-change", plan.installedSizeChange()},
            {"operations", operations}};
}

dbus::VariantMap backendMap(BackendKind kind, const BackendCapabilities& caps, std::optional<bool> available,
                            const std::string& reason, const std::string& inventoryError,
                            const ExternalActivity& activity)
{
    std::vector<std::string> scopes;
    if (caps.systemScope)
        scopes.emplace_back(toString(Scope::System));
    if (caps.userScope)
        scopes.emplace_back(toString(Scope::User));
    // One format per backend in v1 (D-003): deb through PackageKit.
    const PackageFormat format = kind == BackendKind::PackageKit ? PackageFormat::Deb : PackageFormat::Flatpak;
    dbus::VariantMap m{{"id", std::string(toString(kind))},
                       {"available", available.value_or(false)},
                       {"reason", available ? reason : std::string("not checked yet")},
                       {"scopes", scopes},
                       {"formats", std::vector<std::string>{std::string(toString(format))}}};
    // Only when the last inventory could not be recorded (H.2): what
    // GetInstalled lists for this backend is then older than the backend.
    if (!inventoryError.empty())
        m.emplace("inventory-error", inventoryError);
    // What the package manager runs for other programs (H.2): `changing`
    // means the store's actions on this backend wait for it to end.
    m.emplace("activity", std::string(toString(activity.kind)));
    // What may be done to its sources (H.5): those of the user installation.
    m.emplace("source-operations", caps.userSourceChanges
                                           ? std::vector<std::string>{"enable", "disable", "remove"}
                                           : std::vector<std::string>{});
    if (activity.kind != ExternalActivity::Kind::None)
        m.emplace("activity-role", activity.role);
    return m;
}

dbus::VariantMap toMap(const RecordedSource& r, std::optional<UnixSeconds> lastRefresh)
{
    const Source& s = r.source;
    dbus::VariantMap m{{"id", s.id().str()},
                       {"backend", std::string(toString(s.backend()))},
                       {"name", s.label().empty() ? s.origin() : s.label()},
                       {"origin", s.origin()},
                       {"url", s.url()},
                       {"scope", std::string(toString(s.scope()))},
                       {"kind", std::string(toString(s.kind()))},
                       {"enabled", s.enabled()},
                       // As the catalogue knows it: changed only through the
                       // backend that reports it (sourcelist.h, H.5).
                       {"can-toggle", false},
                       {"suggested", false},
                       {"offers", static_cast<std::uint32_t>(r.offers)}};
    if (lastRefresh)
        m.emplace("last-refresh", static_cast<std::uint64_t>(*lastRefresh));
    return m;
}

dbus::VariantMap errorMap(const Error& e)
{
    const std::string code = e.code() == ErrorCode::Backend ? std::string(toString(e.backendFailure()))
                                                            : std::string(toString(e.code()));
    // `message` is meant to be translated by the interface from the code;
    // the core's text goes to `details`, for diagnosis only.
    return {{"code", code}, {"message", code}, {"details", e.message()}};
}

dbus::VariantMap toMap(const TransactionRecord& r)
{
    std::vector<std::string> offers;
    std::vector<std::string> apps;
    for (const TransactionOffer& o : r.offers) {
        offers.push_back(o.offer.str());
        if (o.app)
            apps.push_back(o.app->str());
    }
    dbus::VariantMap m{{"id", r.id},
                       {"kind", std::string(toString(r.snapshot.kind))},
                       {"backend", std::string(toString(r.backend))},
                       {"scope", std::string(toString(r.scope))},
                       {"state", std::string(toString(r.snapshot.state))},
                       {"offer-ids", offers},
                       {"app-ids", apps},
                       {"created", static_cast<std::uint64_t>(r.created)}};
    if (r.finished)
        m.emplace("finished", static_cast<std::uint64_t>(*r.finished));
    if (r.snapshot.error)
        m.emplace("error", errorMap(*r.snapshot.error));
    return m;
}

dbus::VariantMap details(const AppSummary& summary, const Application& app,
                         std::span<const InstalledItem> installedForApp)
{
    dbus::VariantMap m = toMap(summary);
    std::vector<std::tuple<std::string, std::string>> description;
    for (const DescriptionBlock& b : app.description())
        description.emplace_back(std::string(blockType(b.type)), b.text);
    m.emplace("description", description);
    m.emplace("categories", std::vector<std::string>(app.categories().begin(), app.categories().end()));
    std::vector<std::string> aliases;
    for (const AppId& a : app.aliases())
        aliases.push_back(a.str());
    m.emplace("aliases", aliases);

    std::vector<OfferId> installedIds;
    for (const InstalledItem& i : installedForApp)
        installedIds.push_back(i.offer());
    const Offer& chosen = defaultOffer(app, installedIds);
    std::vector<dbus::VariantMap> offers;
    for (const Offer& o : app.offers()) {
        const InstalledItem* installed = nullptr;
        for (const InstalledItem& i : installedForApp)
            if (i.offer() == o.id())
                installed = &i;
        dbus::VariantMap om{{"id", o.id().str()},
                            {"backend", std::string(toString(o.backend()))},
                            {"format", std::string(toString(o.format()))},
                            {"origin", o.origin()},
                            {"source-id", o.source().str()},
                            {"source-kind", std::string(toString(o.sourceKind()))},
                            {"scope", std::string(toString(o.scope()))},
                            {"package", o.package()},
                            {"architecture", o.architecture()},
                            {"version", o.version() ? o.version()->str() : std::string()},
                            {"installed", installed != nullptr},
                            {"default", o.id() == chosen.id()}};
        if (o.version())
            om.emplace("version-scheme", std::string(toString(o.version()->scheme())));
        if (installed && installed->version())
            om.emplace("installed-version", installed->version()->str());
        offers.push_back(std::move(om));
    }
    m.emplace("offers", offers);
    return m;
}

} // namespace cutefish::store::service
