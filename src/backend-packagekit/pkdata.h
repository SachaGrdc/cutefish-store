#pragma once

// What one PackageKit transaction answered, as plain data: the host's D-Bus
// transport fills it from the signals of org.freedesktop.PackageKit.
// Transaction, this library reads it. Nothing here knows D-Bus.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace cutefish::store::packagekit {

/// A `Package` signal, or one entry of `Packages(a(uss))`.
struct PkPackage
{
    std::uint32_t info = 0;
    std::string id = {}; ///< name;version;arch;data
    std::string summary = {};
};

/// A `RepoDetail` signal.
struct PkRepo
{
    std::string id = {};
    std::string description = {};
    bool enabled = false;
};

/// The end of a transaction.
struct PkResult
{
    /// From `Finished`; Unknown when the method itself failed on D-Bus.
    std::uint32_t exit = 0;
    std::uint32_t runtimeMs = 0;
    /// From `ErrorCode`, when one came.
    std::optional<std::uint32_t> errorCode = {};
    std::string errorDetails = {};
    /// The D-Bus error of the method or of CreateTransaction, if any
    /// (PackageKit refuses some arguments that way, without Finished).
    std::string dbusError = {};
    std::string dbusErrorMessage = {};
    std::vector<PkPackage> packages = {};
    std::vector<PkRepo> repos = {};
    /// Last DownloadSizeRemaining seen (simulations: the whole plan).
    std::uint64_t downloadSizeRemaining = 0;
};

/// A change of a running transaction's properties.
struct PkProgress
{
    std::uint32_t status = 0;
    /// 0 to 100, or 101 when unknown (PackageKit's convention).
    std::uint32_t percentage = 101;
    bool allowCancel = false;
    std::uint64_t downloadSizeRemaining = 0;
    /// The package an ItemProgress signal is about; empty otherwise.
    std::string item = {};
};

} // namespace cutefish::store::packagekit
