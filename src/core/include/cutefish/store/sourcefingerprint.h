#pragma once

#include <cutefish/store/identifiers.h>
#include <cutefish/store/result.h>
#include <cutefish/store/source.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cutefish::store {

/// Identity of the metadata file a source was imported from (ARCHITECTURE
/// §6.2, step 1). Three notions are kept apart:
/// - the fingerprint says **what** was imported (this file, this content);
/// - the date of the last successful refresh (state.sqlite, D-015) says
///   **when** the store last refreshed, whether or not anything changed;
/// - the provenance of an offer (its source id, origin, scope) says **where
///   from**, and does not change with the content.
struct SourceFingerprint
{
    /// Absolute path of the metadata file (DEP-11 YAML or AppStream XML).
    std::string path;
    std::uint64_t size = 0;
    /// Modification time, nanoseconds since the Unix epoch. With the size,
    /// only a shortcut: unchanged, the previous hash is reused.
    std::int64_t mtimeNs = 0;
    /// SHA-256 of the content, 64 lowercase hexadecimal characters: what
    /// decides whether the content changed.
    std::string sha256;
    /// Directory of the cached icons, as given to the indexer; empty if none.
    /// Its content is not hashed (see docs/CATALOG.md).
    std::string iconsPath = {};

    bool operator==(const SourceFingerprint&) const = default;
};

/// The files a source is read from.
struct SourceFiles
{
    std::filesystem::path metadata;
    std::filesystem::path icons = {};
};

enum class SourceFileState : std::uint8_t {
    Present,    ///< Read and fingerprinted.
    Missing,    ///< No such file (yet, or any more).
    Unreadable, ///< Exists but cannot be read (permissions, a directory, I/O error).
};

struct SourceProbe
{
    SourceFileState state;
    std::optional<SourceFingerprint> fingerprint = {}; ///< Set when Present.
    std::string detail = {};                           ///< Why, when not Present.
};

/// Fingerprints the metadata file. When `previous` names the same path with
/// the same size and modification time, its hash is reused without reading
/// the file; otherwise the whole file is read and hashed.
SourceProbe probeSource(const SourceFiles& files, const SourceFingerprint* previous = nullptr);

/// What the catalogue in service recorded about one source.
struct RecordedSource
{
    Source source;
    /// Absent when the source could not be read at the last import, or for
    /// a catalogue written before fingerprints existed.
    std::optional<SourceFingerprint> fingerprint = {};
    /// Offers imported from it.
    std::int64_t offers = 0;
};

/// A source as configured now, with the probe of its files.
struct ObservedSource
{
    Source source;
    SourceProbe probe;
};

enum class SourceChange : std::uint8_t {
    Unchanged,  ///< Same descriptor, same content (size or time may differ).
    Modified,   ///< Descriptor or content changed, or nothing was recorded.
    Added,      ///< Configured and readable, not in the catalogue.
    Removed,    ///< In the catalogue, no longer configured.
    Missing,    ///< Configured, file absent.
    Unreadable, ///< Configured, file present but unreadable.
};

/// "unchanged", "modified", "added", "removed", "missing", "unreadable".
std::string_view toString(SourceChange change) noexcept;

struct SourceStatus
{
    SourceId id;
    SourceChange change;
    bool inCatalog;
    /// Fingerprint to give the indexer for this source; set when the file
    /// is present.
    std::optional<SourceFingerprint> current = {};
    std::string detail = {};
};

/// What a refresh should do with the catalogue (ARCHITECTURE §6.2).
struct SourceAssessment
{
    /// One entry per recorded or configured source, sorted by id.
    std::vector<SourceStatus> sources;
    /// A new import is worth doing: a source was added, modified or removed,
    /// nothing blocks it, and at least one source can be read (the indexer
    /// needs one).
    bool reimport = false;
    /// Sources of the catalogue in service whose file is missing or
    /// unreadable. A new import would lose their offers and be refused by
    /// replaceCatalog() (minimum share), so it is not proposed: the current
    /// catalogue stays in service until the file comes back or the source
    /// is removed from the configuration.
    std::vector<SourceId> blocking;
    /// To pass to replaceCatalog() as removed on purpose.
    std::vector<SourceId> removed;
    /// Configured sources whose file can be read: the ones to give the
    /// indexer. A configured source that is missing or unreadable and was
    /// never imported is left out, without blocking.
    std::vector<SourceId> importable;
};

/// Compares the catalogue in service (`recorded`; empty if there is none)
/// with the configured sources (`observed`). A descriptor change (kind,
/// origin, scope, label, URL, priority, enabled) is a modification, since
/// it changes the stored rows or the text priority. Pure function: no file
/// is read.
SourceAssessment assessSources(std::span<const RecordedSource> recorded, std::span<const ObservedSource> observed);

} // namespace cutefish::store
