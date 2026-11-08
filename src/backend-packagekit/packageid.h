#pragma once

// PackageKit package ids, `name;version;arch;data` (ARCHITECTURE §7.2).

#include <cutefish/store/result.h>

#include <string>
#include <string_view>

namespace cutefish::store::packagekit {

struct PackageId
{
    std::string name;
    std::string version;
    std::string arch;
    /// `installed`, `manual:<origin>`, `auto:<origin>`, `+auto:<origin>`,
    /// or an origin alone (`debian-stable-security-main`).
    std::string data;

    /// The repository origin in `data`, without its markers; `local` for a
    /// package installed from no repository, empty when unknown.
    std::string origin() const;
    /// The `name` as dpkg writes it for this machine: `name` for the native
    /// architecture and `all`, `name:arch` otherwise.
    std::string qualifiedName(std::string_view nativeArch) const;
    std::string str() const { return name + ";" + version + ";" + arch + ";" + data; }
};

/// Exactly four fields, a non-empty name. InvalidArgument otherwise.
Result<PackageId> parsePackageId(std::string_view text);

} // namespace cutefish::store::packagekit
