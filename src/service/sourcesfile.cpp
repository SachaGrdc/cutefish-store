#include "sourcesfile.h"

#include <charconv>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace cutefish::store::service {

namespace {

Result<ConfiguredSource> parseLine(std::string_view line)
{
    std::map<std::string, std::string> kv;
    while (!line.empty()) {
        const std::size_t comma = line.find(',');
        const std::string_view item = line.substr(0, comma);
        const std::size_t equal = item.find('=');
        if (equal == std::string_view::npos || equal == 0)
            return Error::invalidArgument("expected key=value, got " + std::string(item));
        const std::string key(item.substr(0, equal));
        if (!kv.emplace(key, std::string(item.substr(equal + 1))).second)
            return Error::invalidArgument("key given twice: " + key);
        line = comma == std::string_view::npos ? std::string_view() : line.substr(comma + 1);
    }
    for (const auto& [key, value] : kv) {
        static const std::set<std::string> known{"id", "backend", "scope", "kind", "origin", "priority",
                                                 "file", "icons", "label", "url"};
        if (!known.contains(key))
            return Error::invalidArgument("unknown key " + key);
    }
    auto need = [&](const char* key) -> Result<std::string> {
        const auto it = kv.find(key);
        if (it == kv.end() || it->second.empty())
            return Error::invalidArgument(std::string("missing ") + key);
        return it->second;
    };
    for (const char* key : {"id", "backend", "scope", "kind", "origin", "priority", "file"})
        if (auto v = need(key); !v)
            return std::move(v).error();
    auto id = SourceId::parse(kv["id"]);
    const auto backend = parseBackendKind(kv["backend"]);
    const auto scope = parseScope(kv["scope"]);
    const auto kind = parseSourceKind(kv["kind"]);
    if (!id || !backend || !scope || !kind)
        return Error::invalidArgument("invalid id, backend, scope or kind");
    std::int32_t priority = 0;
    const std::string& p = kv["priority"];
    if (auto [end, ec] = std::from_chars(p.data(), p.data() + p.size(), priority);
        ec != std::errc() || end != p.data() + p.size())
        return Error::invalidArgument("invalid priority " + p);
    std::filesystem::path file = kv["file"];
    if (!file.is_absolute())
        return Error::invalidArgument("file must be an absolute path");
    auto source = Source::create({.id = std::move(id).value(),
                                  .backend = *backend,
                                  .scope = *scope,
                                  .kind = *kind,
                                  .origin = kv["origin"],
                                  .label = kv["label"],
                                  .url = kv["url"],
                                  .priority = priority});
    if (!source)
        return std::move(source).error();
    return ConfiguredSource{std::move(source).value(), {std::move(file), kv["icons"]}};
}

} // namespace

Result<std::vector<ConfiguredSource>> parseSources(std::string_view text)
{
    std::vector<ConfiguredSource> out;
    std::set<std::string> ids;
    std::size_t number = 0;
    while (!text.empty()) {
        ++number;
        const std::size_t newline = text.find('\n');
        std::string_view line = text.substr(0, newline);
        text = newline == std::string_view::npos ? std::string_view() : text.substr(newline + 1);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
            line.remove_suffix(1);
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
            line.remove_prefix(1);
        if (line.empty() || line.front() == '#')
            continue;
        auto source = parseLine(line);
        if (!source)
            return Error::invalidArgument("sources line " + std::to_string(number) + ": " + source.error().message());
        if (!ids.insert(source->source.id().str()).second)
            return Error::invalidArgument("sources line " + std::to_string(number) + ": duplicate id "
                                          + source->source.id().str());
        out.push_back(std::move(source).value());
    }
    return out;
}

Result<std::vector<ConfiguredSource>> readSources(const std::filesystem::path& path)
{
    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
        return std::vector<ConfiguredSource>{};
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return Error::invalidArgument("cannot read " + path.string());
    std::ostringstream text;
    text << in.rdbuf();
    return parseSources(text.str());
}

std::string indexerArgument(const ConfiguredSource& s)
{
    const Source& src = s.source;
    std::string out = "id=" + src.id().str() + ",backend=" + std::string(toString(src.backend()))
            + ",scope=" + std::string(toString(src.scope())) + ",kind=" + std::string(toString(src.kind()))
            + ",origin=" + src.origin() + ",priority=" + std::to_string(src.priority())
            + ",file=" + s.files.metadata.string();
    if (!s.files.icons.empty())
        out += ",icons=" + s.files.icons.string();
    if (!src.label().empty())
        out += ",label=" + src.label();
    if (!src.url().empty())
        out += ",url=" + src.url();
    return out;
}

} // namespace cutefish::store::service
