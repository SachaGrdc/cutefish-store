#pragma once

// Conversion of AppStream description markup into the core's description
// blocks, and text clean-up. Uses libxml2.

#include <cutefish/store/application.h>

#include <string>
#include <string_view>
#include <vector>

namespace cutefish::store::indexer {

/// Collapses runs of whitespace (spaces, tabs, line breaks) into one space
/// and trims both ends. AppStream texts keep the indentation of their XML or
/// YAML source; displayed text does not need it.
std::string normalizeSpace(std::string_view text);

/// `<p>`, `<ul><li>`, `<ol><li>` become paragraphs, list items and ordered
/// list items; inline markup (`<em>`, `<code>`) keeps only its text. Text
/// outside any block becomes a paragraph. Markup that libxml2 cannot parse
/// is kept as one paragraph with the tags removed. Empty blocks are dropped.
std::vector<DescriptionBlock> descriptionBlocks(std::string_view markup);

} // namespace cutefish::store::indexer
