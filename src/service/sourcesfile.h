#pragma once

// Sources the service indexes besides those the backends provide through
// Backend::catalogSources() (D-020, D-028): the file's entry wins on a
// shared id. One source per line, in the syntax of the indexer's --source
// option:
//
//   id=ID,backend=packagekit|flatpak,scope=system|user,
//   kind=distribution|flathub|third-party,origin=ORIGIN,priority=N,
//   file=PATH[,icons=DIR][,label=TEXT][,url=URL]
//
// A source to leave out is removed from the file (or commented out).
//
// Blank lines and lines starting with '#' are ignored. Values cannot hold a
// comma.

#include <cutefish/store/result.h>
#include <cutefish/store/source.h>
#include <cutefish/store/sourcefingerprint.h>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace cutefish::store::service {

struct ConfiguredSource
{
    Source source;
    SourceFiles files;
};

/// InvalidArgument with the line number for a malformed line, an invalid
/// source or a duplicate id.
Result<std::vector<ConfiguredSource>> parseSources(std::string_view text);

/// A missing file means no source (an empty list), not an error.
Result<std::vector<ConfiguredSource>> readSources(const std::filesystem::path& path);

/// The indexer's --source value for a source.
std::string indexerArgument(const ConfiguredSource& source);

} // namespace cutefish::store::service
