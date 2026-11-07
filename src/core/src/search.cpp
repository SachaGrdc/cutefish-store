#include <cutefish/store/search.h>

#include "text.h"

namespace cutefish::store {

Result<SearchQuery> SearchQuery::create(std::string text, SearchOptions options)
{
    if (auto ok = detail::checkText(text, "search text", kMaxSearchTextBytes, false); !ok)
        return std::move(ok).error();
    if (options.limit == 0 || options.limit > kMaxSearchLimit)
        return Error::invalidArgument("search limit must be between 1 and 200");
    if (!options.category.empty())
        if (auto ok = detail::checkOpaqueIdentifier(options.category, "category"); !ok)
            return std::move(ok).error();
    return SearchQuery(std::move(text), std::move(options));
}

Result<BrowseQuery> BrowseQuery::create(SearchOptions options)
{
    // Same option rules as a search, without text.
    auto checked = SearchQuery::create({}, std::move(options));
    if (!checked)
        return std::move(checked).error();
    return BrowseQuery(checked->options());
}

} // namespace cutefish::store
