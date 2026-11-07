#pragma once

#include <cutefish/store/result.h>

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace cutefish::store {

/// What a valid catalogue file contains.
struct CatalogFileInfo
{
    std::uint64_t generation = 0;
    std::int64_t applications = 0;
    std::int64_t offers = 0;
    /// Offers per source id, as recorded by the writer.
    std::map<std::string, std::int64_t> offersPerSource = {};
};

struct CatalogReplaceRules
{
    /// Each source of the active catalogue must keep at least this share of
    /// its offers in the candidate (ARCHITECTURE §6.2).
    double minimumShare = 0.5;
    /// Sources removed on purpose since the active catalogue was built:
    /// their absence from the candidate is not an error.
    std::vector<std::string> removedSources = {};
};

/// Checks one catalogue file, read-only, without using the `immutable` flag:
/// - no `-journal` or `-wal` file next to it (a write was interrupted);
/// - SQLite opens it and `user_version` is kCatalogSchemaVersion, which the
///   writer sets last, in a transaction of its own;
/// - `PRAGMA quick_check` and `PRAGMA foreign_key_check` find nothing;
/// - every table of the schema exists and both search indexes answer.
Result<CatalogFileInfo> inspectCatalog(const std::filesystem::path& path);

/// Validates `candidate`, compares it with `active` when that file exists
/// (plausibility rules above), then replaces `active` with it:
/// fsync of the candidate, rename() over the active file, fsync of the
/// directory. Both files must be in the same directory. On any failure the
/// active catalogue is left untouched, and so is the candidate.
///
/// Readers that already opened the active file keep reading the old
/// snapshot until they close it (POSIX keeps the inode alive); readers
/// opened after the rename see the new one.
Result<CatalogFileInfo> replaceCatalog(const std::filesystem::path& candidate, const std::filesystem::path& active,
                                       const CatalogReplaceRules& rules = {});

} // namespace cutefish::store
