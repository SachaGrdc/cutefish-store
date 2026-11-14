#pragma once

// The checks the Flatpak backend makes before it adds a remote (D-040): a
// `.flatpakrepo` carrying its GPG key and an https URL, and a plain remote
// name. Pure logic: no GLib, no libflatpak; libflatpak still parses the
// file itself, this only refuses what the store never adds.

#include <cutefish/store/result.h>

#include <string>
#include <string_view>

namespace cutefish::store::flatpak {

/// What the store reads of a `.flatpakrepo` before handing it to libflatpak.
struct FlatpakRepoFile
{
    std::string url;
};

/// Checks a `.flatpakrepo` (a key file, group `[Flatpak Repo]`) before the
/// store adds the remote it describes (D-040): InvalidArgument for a text
/// over 64 KiB, without the group, or whose `Url` is not `https://`;
/// Backend/Untrusted without a `GPGKey`. libflatpak still parses the file
/// itself; this only refuses what the store never adds.
Result<FlatpakRepoFile> checkFlatpakRepo(std::string_view text);

/// A remote name the store may create: 1 to 64 bytes of ASCII letters,
/// digits, `.`, `_` and `-`, not starting with `.` or `-`. InvalidArgument
/// otherwise.
Result<void> checkRemoteName(std::string_view name);

} // namespace cutefish::store::flatpak
