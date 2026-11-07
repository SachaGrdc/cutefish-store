#include <cutefish/store/application.h>

#include "text.h"

#include <algorithm>

namespace cutefish::store {

namespace {

Result<void> checkIcon(const IconRef& icon)
{
    switch (icon.kind) {
    case IconKind::None:
        if (!icon.value.empty())
            return Error::invalidArgument("icon without kind has a value");
        return {};
    case IconKind::LocalPath:
        if (icon.value.empty() || icon.value.front() != '/')
            return Error::invalidArgument("local icon path must be absolute");
        if (icon.value.find("/../") != std::string::npos || icon.value.ends_with("/.."))
            return Error::invalidArgument("local icon path must not contain ..");
        return detail::checkText(icon.value, "local icon path", 4096, true);
    case IconKind::RemoteUrl:
        if (!icon.value.starts_with("https://"))
            return Error::invalidArgument("remote icon must be an https URL");
        return detail::checkAsciiToken(icon.value, "remote icon URL", 2048);
    case IconKind::StockName:
        return detail::checkOpaqueIdentifier(icon.value, "stock icon name");
    }
    return Error::invalidArgument("unknown icon kind");
}

// Small lists (a few categories, aliases or offers): a quadratic scan is
// cheaper than building a set.
template <typename T, typename Key>
bool hasDuplicate(const std::vector<T>& items, Key key)
{
    for (std::size_t i = 0; i < items.size(); ++i)
        for (std::size_t j = i + 1; j < items.size(); ++j)
            if (key(items[i]) == key(items[j]))
                return true;
    return false;
}

} // namespace

Result<Application> Application::create(ApplicationSpec spec)
{
    if (auto ok = detail::checkText(spec.name, "application name", 512, true); !ok)
        return std::move(ok).error();
    if (auto ok = detail::checkText(spec.summary, "application summary", 1024, false); !ok)
        return std::move(ok).error();
    for (const DescriptionBlock& block : spec.description)
        if (auto ok = detail::checkText(block.text, "description block", 64 * 1024, true, true); !ok)
            return std::move(ok).error();
    if (auto ok = checkIcon(spec.icon); !ok)
        return std::move(ok).error();
    for (const std::string& category : spec.categories)
        if (auto ok = detail::checkOpaqueIdentifier(category, "category"); !ok)
            return std::move(ok).error();
    if (hasDuplicate(spec.categories, [](const std::string& c) -> const std::string& { return c; }))
        return Error::invalidArgument("duplicate category");
    for (const AppId& alias : spec.aliases)
        if (alias == spec.id)
            return Error::invalidArgument("an alias repeats the application id");
    if (hasDuplicate(spec.aliases, [](const AppId& a) -> const AppId& { return a; }))
        return Error::invalidArgument("duplicate alias");
    if (spec.offers.empty())
        return Error::invalidArgument("an application needs at least one offer");
    if (hasDuplicate(spec.offers, [](const Offer& o) -> const OfferId& { return o.id(); }))
        return Error::invalidArgument("duplicate offer id");
    return Application(std::move(spec));
}

const Offer* Application::findOffer(const OfferId& id) const noexcept
{
    const auto it = std::find_if(m_spec.offers.begin(), m_spec.offers.end(),
                                 [&](const Offer& o) { return o.id() == id; });
    return it == m_spec.offers.end() ? nullptr : &*it;
}

bool Application::isKnownAs(const AppId& id) const noexcept
{
    return id == m_spec.id || std::find(m_spec.aliases.begin(), m_spec.aliases.end(), id) != m_spec.aliases.end();
}

bool Application::hasCategory(std::string_view category) const noexcept
{
    return std::find(m_spec.categories.begin(), m_spec.categories.end(), category) != m_spec.categories.end();
}

bool Application::hasBackend(BackendKind backend) const noexcept
{
    return std::any_of(m_spec.offers.begin(), m_spec.offers.end(),
                       [&](const Offer& o) { return o.backend() == backend; });
}

} // namespace cutefish::store
