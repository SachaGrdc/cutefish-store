#pragma once

// The Debian profile of the PackageKit backend (P-001, ARCHITECTURE §7.2):
// everything that is Debian's and not PackageKit's. The generic part
// (translate.h) holds no Debian data.

#include <cutefish/store/backend.h>
#include <cutefish/store/result.h>
#include <cutefish/store/source.h>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cutefish::store::packagekit::debian {

/// dpkg's architecture for PackageKit's `DistroId` machine part
/// (`debian;13;x86_64` → `amd64`). Empty when unknown.
std::string dpkgArchitecture(std::string_view distroId);

/// Whether the profile applies: `DistroId` starts with `debian;`. Derived
/// distributions are not assumed compatible (ARCHITECTURE §7.2).
bool applies(std::string_view distroId);

/// A repository id of the APT backend:
/// `<file>:<deb|deb-src>:<uri>:<suite>:<components>`.
struct AptRepository
{
    std::string file;
    std::string type; ///< `deb` or `deb-src`
    std::string uri;
    std::string host;
    std::string suite;
    std::string components;
};

std::optional<AptRepository> parseRepositoryId(std::string_view id);

/// Owner's decision for step F: classified by the URI host. `debian.org`
/// and its subdomains (deb.debian.org, security.debian.org, the
/// ftp.<country>.debian.org mirrors) are the distribution; anything else is
/// a third party.
SourceKind classifyHost(std::string_view host);

/// The source of an enabled `deb` repository; std::nullopt for `deb-src`,
/// a disabled repository or an id the profile cannot read.
std::optional<Source> repositorySource(std::string_view repoId, std::string_view description, bool enabled);

/// The packages whose removal the store refuses, even as a consequence of
/// another removal (ARCHITECTURE §7.2): the desktop meta-package and the
/// store itself.
bool isProtected(std::string_view packageName);

/// The DEP-11 catalogues of the distribution under `swcatalog`
/// (`/var/lib/swcatalog`), one source each:
/// `yaml/<host>_<path>_dists_<suite>_<component>_dep11_Components-<arch>.yml[.gz]`.
/// The source's origin is `<last path element>-<suite>-<component>`
/// (`debian-trixie-main`), which is also the name of its icon directory
/// under `icons/` when that exists. The origin written inside the file is
/// not read (it would need zlib): a file whose name does not follow the
/// pattern is left out.
std::vector<SourceMetadata> catalogSources(const std::filesystem::path& swcatalog);

} // namespace cutefish::store::packagekit::debian
