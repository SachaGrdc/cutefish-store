#pragma once

// Reads AppStream catalogues (Debian DEP-11 YAML, Flatpak XML) with
// libappstream and writes them into a CatalogWriter. This is the only part
// of the project that uses libappstream, GLib and libxml2 (P-002, P-006).

#include <cutefish/store/catalogwriter.h>
#include <cutefish/store/source.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace cutefish::store::indexer {

struct SourceConfig
{
    Source source;
    /// Catalogue file: DEP-11 YAML (`.yml`, `.yaml`, optionally `.gz`) or
    /// AppStream XML (anything else, optionally `.gz`).
    std::filesystem::path file;
    /// Directory holding the cached icons of this catalogue, with one
    /// `<width>x<height>` subdirectory per size; empty if none.
    std::filesystem::path icons = {};
};

struct ImportOptions
{
    /// Languages whose descriptions are stored, besides `C`. Names,
    /// summaries and keywords are always stored in every language.
    std::vector<std::string> descriptionLocales = {};
    /// Store descriptions in every language the component has a name,
    /// summary or keyword translation for (O-009 measurement mode).
    bool allDescriptionLocales = false;
    /// Components handed to AsMetadata at a time while the file is streamed
    /// (XML with libxml2's reader, DEP-11 YAML split on its `---` document
    /// separators). 0 parses the whole file at once, as prototype 3 did:
    /// much more memory for the same result (docs/CATALOG.md).
    std::size_t batchSize = 20;
};

/// Outcome of one source. Every skipped component is counted under a
/// reason; a few examples are kept for the log.
struct SourceReport
{
    std::string sourceId;
    bool readable = true;
    std::string error = {};
    std::int64_t components = 0;
    std::int64_t imported = 0;
    std::map<std::string, std::int64_t> skipped = {};
    std::vector<std::string> examples = {};
    double seconds = 0;
};

/// Imports every source in order. A source that cannot be read is reported
/// and the others go on: the replacement checks (catalogfile.h) then decide
/// whether the candidate is plausible.
std::vector<SourceReport> importSources(CatalogWriter& writer, const std::vector<SourceConfig>& sources,
                                        const ImportOptions& options);

/// Peak resident memory of the process so far, in KiB.
long peakRssKiB();

} // namespace cutefish::store::indexer
