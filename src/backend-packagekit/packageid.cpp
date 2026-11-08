#include "packageid.h"

namespace cutefish::store::packagekit {

std::string PackageId::origin() const
{
    std::string_view d = data;
    if (!d.empty() && d.front() == '+')
        d.remove_prefix(1);
    for (std::string_view prefix : {"auto:", "manual:", "installed:"})
        if (d.starts_with(prefix)) {
            d.remove_prefix(prefix.size());
            break;
        }
    if (d == "installed")
        return {};
    return std::string(d);
}

std::string PackageId::qualifiedName(std::string_view nativeArch) const
{
    if (arch.empty() || arch == "all" || arch == nativeArch)
        return name;
    return name + ":" + arch;
}

Result<PackageId> parsePackageId(std::string_view text)
{
    PackageId id;
    std::string* fields[] = {&id.name, &id.version, &id.arch, &id.data};
    std::size_t start = 0;
    for (int i = 0; i < 4; ++i) {
        const std::size_t semi = i < 3 ? text.find(';', start) : std::string_view::npos;
        if (i < 3 && semi == std::string_view::npos)
            return Error::invalidArgument("package id needs four fields: " + std::string(text));
        *fields[i] = std::string(text.substr(start, i < 3 ? semi - start : std::string_view::npos));
        start = semi + 1;
    }
    if (id.name.empty() || id.data.find(';') != std::string::npos)
        return Error::invalidArgument("malformed package id: " + std::string(text));
    return id;
}

} // namespace cutefish::store::packagekit
