#pragma once

// PackageKit 1.3.1 enumerations, as sent on D-Bus. The values come from
// lib/packagekit-glib2/pk-enum.h at tag v1.3.1 (all sequential from 0, a
// bitfield value being 1 << enum), the same table the prototype used; no
// development header of PackageKit is installed. Codes observed in real
// replies agree (prototype 4: info 11, 12, 13, 327685; exit 1, 2).

#include <cstdint>
#include <string>
#include <string_view>

namespace cutefish::store::packagekit {

enum class Role : std::uint32_t {
    Unknown = 0, Cancel = 1, GetPackages = 5, GetRepoList = 6, GetUpdates = 9, InstallPackages = 11,
    RefreshCache = 13, RemovePackages = 14, Resolve = 17, UpdatePackages = 22,
};

enum class Status : std::uint32_t {
    Unknown = 0, Wait = 1, Setup = 2, Running = 3, Query = 4, Info = 5, Remove = 6, RefreshCache = 7,
    Download = 8, Install = 9, Update = 10, Cleanup = 11, Obsolete = 12, DepResolve = 13, SigCheck = 14,
    TestCommit = 15, Commit = 16, Request = 17, Finished = 18, Cancel = 19, DownloadRepository = 20,
    DownloadPackagelist = 21, DownloadFilelist = 22, DownloadChangelog = 23, DownloadGroup = 24,
    DownloadUpdateinfo = 25, Repackaging = 26, LoadingCache = 27, ScanApplications = 28,
    GeneratePackageList = 29, WaitingForLock = 30, WaitingForAuth = 31,
};

enum class Exit : std::uint32_t {
    Unknown = 0, Success = 1, Failed = 2, Cancelled = 3, KeyRequired = 4, EulaRequired = 5, Killed = 6,
    MediaChangeRequired = 7, NeedUntrusted = 8, CancelledPriority = 9, SkipTransaction = 10, RepairRequired = 11,
};

/// The low 16 bits of a package's `info` (the high ones hold an update's
/// severity, ARCHITECTURE §7.2).
enum class Info : std::uint32_t {
    Unknown = 0, Installed = 1, Available = 2, Low = 3, Enhancement = 4, Normal = 5, Bugfix = 6,
    Important = 7, Security = 8, Blocked = 9, Downloading = 10, Updating = 11, Installing = 12,
    Removing = 13, Cleanup = 14, Obsoleting = 15, CollectionInstalled = 16, CollectionAvailable = 17,
    Finished = 18, Reinstalling = 19, Downgrading = 20, Preparing = 21, Decompressing = 22, Untrusted = 23,
    Trusted = 24, Unavailable = 25, Critical = 26, Install = 27, Remove = 28, Obsolete = 29, Downgrade = 30,
};

/// The PkErrorEnum names, indexed by value.
std::string_view errorName(std::uint32_t code) noexcept;
std::string_view statusName(std::uint32_t status) noexcept;
std::string_view exitName(std::uint32_t exit) noexcept;

/// Bit values of PkFilterEnum and PkTransactionFlagEnum.
namespace filter {
inline constexpr std::uint64_t kNone = 1ull << 1;
inline constexpr std::uint64_t kInstalled = 1ull << 2;
inline constexpr std::uint64_t kNotInstalled = 1ull << 3;
inline constexpr std::uint64_t kNewest = 1ull << 16;
inline constexpr std::uint64_t kArch = 1ull << 18;
} // namespace filter
namespace flag {
inline constexpr std::uint64_t kOnlyTrusted = 1ull << 1;
inline constexpr std::uint64_t kSimulate = 1ull << 2;
} // namespace flag

constexpr std::uint64_t bit(Role role) noexcept { return 1ull << static_cast<std::uint32_t>(role); }

inline Info infoOf(std::uint32_t info) noexcept { return static_cast<Info>(info & 0xFFFF); }

} // namespace cutefish::store::packagekit
