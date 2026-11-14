#include "debianprofile.h"

#include <algorithm>
#include <system_error>

namespace cutefish::store::packagekit::debian {

std::string dpkgArchitecture(std::string_view distroId)
{
    const std::size_t last = distroId.rfind(';');
    const std::string_view machine = last == std::string_view::npos ? distroId : distroId.substr(last + 1);
    static constexpr std::pair<std::string_view, std::string_view> kArch[] = {
        {"x86_64", "amd64"}, {"aarch64", "arm64"}, {"i686", "i386"}, {"i586", "i386"}, {"i386", "i386"},
        {"armv7l", "armhf"}, {"ppc64le", "ppc64el"}, {"riscv64", "riscv64"}, {"s390x", "s390x"},
        {"loongarch64", "loong64"}};
    for (const auto& [m, a] : kArch)
        if (machine == m)
            return std::string(a);
    return {};
}

bool applies(std::string_view distroId)
{
    return distroId.starts_with("debian;");
}

std::optional<AptRepository> parseRepositoryId(std::string_view id)
{
    // The file name holds no ':' here; the URI does ("http://...").
    const std::size_t typeStart = id.find(':');
    if (typeStart == std::string_view::npos)
        return std::nullopt;
    AptRepository r;
    r.file = std::string(id.substr(0, typeStart));
    std::string_view rest = id.substr(typeStart + 1);
    const std::size_t typeEnd = rest.find(':');
    if (typeEnd == std::string_view::npos)
        return std::nullopt;
    r.type = std::string(rest.substr(0, typeEnd));
    rest = rest.substr(typeEnd + 1);
    // The components and the suite are the last two fields.
    const std::size_t lastColon = rest.rfind(':');
    if (lastColon == std::string_view::npos)
        return std::nullopt;
    r.components = std::string(rest.substr(lastColon + 1));
    rest = rest.substr(0, lastColon);
    const std::size_t suiteColon = rest.rfind(':');
    if (suiteColon == std::string_view::npos)
        return std::nullopt;
    r.suite = std::string(rest.substr(suiteColon + 1));
    r.uri = std::string(rest.substr(0, suiteColon));
    const std::size_t scheme = r.uri.find("://");
    if (scheme == std::string::npos)
        return std::nullopt;
    const std::size_t hostStart = scheme + 3;
    r.host = r.uri.substr(hostStart, r.uri.find_first_of("/:", hostStart) - hostStart);
    if (r.host.empty() || r.suite.empty())
        return std::nullopt;
    return r;
}

SourceKind classifyHost(std::string_view host)
{
    return host == "debian.org" || host.ends_with(".debian.org") ? SourceKind::Distribution : SourceKind::ThirdParty;
}

std::optional<Source> repositorySource(std::string_view repoId, std::string_view description, bool enabled)
{
    const auto repo = parseRepositoryId(repoId);
    if (!repo || repo->type != "deb")
        return std::nullopt;
    auto id = SourceId::parse("packagekit:" + std::string(repoId));
    if (!id)
        return std::nullopt;
    auto source = Source::create({.id = std::move(id).value(),
                                  .backend = BackendKind::PackageKit,
                                  .scope = Scope::System,
                                  .kind = classifyHost(repo->host),
                                  .origin = std::string(repoId),
                                  .label = std::string(description),
                                  .url = repo->uri,
                                  .enabled = enabled,
                                  .priority = 10});
    if (!source)
        return std::nullopt;
    return std::move(source).value();
}

std::vector<SourceId> catalogueSourceIds(std::string_view repoId)
{
    const auto repo = parseRepositoryId(repoId);
    if (!repo || repo->type != "deb")
        return {};
    // The path of the URI, without its scheme and host.
    const std::size_t scheme = repo->uri.find("://");
    const std::size_t pathStart = repo->uri.find('/', scheme + 3);
    std::string_view path = pathStart == std::string::npos ? std::string_view() : std::string_view(repo->uri).substr(pathStart);
    while (!path.empty() && path.back() == '/')
        path.remove_suffix(1);
    const std::size_t last = path.rfind('/');
    const std::string archive(last == std::string_view::npos || path.empty() ? std::string_view(repo->host)
                                                                              : path.substr(last + 1));
    std::vector<SourceId> out;
    std::string_view components = repo->components;
    while (!components.empty()) {
        const std::size_t comma = components.find(',');
        const std::string_view component = components.substr(0, comma);
        if (!component.empty())
            if (auto id = SourceId::parse("packagekit:" + archive + "-" + repo->suite + "-" + std::string(component)))
                out.push_back(std::move(id).value());
        if (comma == std::string_view::npos)
            break;
        components.remove_prefix(comma + 1);
    }
    return out;
}

bool isProtected(std::string_view packageName)
{
    // dpkg may qualify a foreign architecture ("name:i386").
    const std::string_view name = packageName.substr(0, packageName.find(':'));
    return name == "cutefish-desktop" || name == "cutefish-store";
}

std::vector<SourceMetadata> catalogSources(const std::filesystem::path& swcatalog)
{
    std::vector<SourceMetadata> out;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(swcatalog / "yaml", ec)) {
        const std::string name = entry.path().filename().string();
        if (!(name.ends_with(".yml.gz") || name.ends_with(".yml") || name.ends_with(".yaml.gz")))
            continue;
        const std::size_t dists = name.find("_dists_");
        const std::size_t dep11 = name.find("_dep11_");
        if (dists == std::string::npos || dep11 == std::string::npos || dep11 < dists)
            continue;
        const std::string before = name.substr(0, dists);              // deb.debian.org_debian
        const std::string within = name.substr(dists + 7, dep11 - dists - 7); // trixie_main
        const std::size_t sep = within.rfind('_');
        if (sep == std::string::npos)
            continue;
        const std::string suite = within.substr(0, sep);
        const std::string component = within.substr(sep + 1);
        const std::string host = before.substr(0, before.find('_'));
        const std::size_t lastPath = before.rfind('_');
        const std::string archive = lastPath == std::string::npos ? host : before.substr(lastPath + 1);
        const std::string origin = archive + "-" + suite + "-" + component;
        auto id = SourceId::parse("packagekit:" + origin);
        if (!id)
            continue;
        auto source = Source::create({.id = std::move(id).value(),
                                      .backend = BackendKind::PackageKit,
                                      .scope = Scope::System,
                                      .kind = classifyHost(host),
                                      .origin = origin,
                                      .label = origin,
                                      .url = {},
                                      .enabled = true,
                                      .priority = 10});
        if (!source)
            continue;
        std::filesystem::path icons = swcatalog / "icons" / origin;
        if (!std::filesystem::is_directory(icons, ec))
            icons.clear();
        out.push_back({std::move(source).value(), {entry.path(), std::move(icons)}});
    }
    std::sort(out.begin(), out.end(),
              [](const SourceMetadata& a, const SourceMetadata& b) { return a.source.id() < b.source.id(); });
    return out;
}

} // namespace cutefish::store::packagekit::debian
