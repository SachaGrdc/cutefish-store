// cutefish-store-indexer: reads AppStream catalogues and writes a candidate
// catalogue (ARCHITECTURE §4.7, §6.2). Launched by the service; usable by
// hand for tests and measurements.
//
//   cutefish-store-indexer --output CANDIDATE [--generation N]
//       [--locale LOCALE]... [--all-description-locales] [--batch N]
//       --source id=ID,backend=packagekit|flatpak,scope=system|user,
//                kind=distribution|flathub|third-party,origin=ORIGIN,
//                priority=N,file=PATH[,icons=DIR][,label=TEXT][,url=URL]
//       [--source ...] [--replace ACTIVE [--removed-source ID]...]
//
// Exit status: 0 when the candidate was written (and, with --replace,
// installed), 1 when it could not be, 2 on a usage error. Sources that
// cannot be read are reported and do not stop the others.

#include "importer.h"

#include <cutefish/store/catalogfile.h>
#include <cutefish/store/catalogwriter.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

using namespace cutefish::store;
using namespace cutefish::store::indexer;

namespace {

[[noreturn]] void usage(const std::string& message)
{
    std::fprintf(stderr, "cutefish-store-indexer: %s\n(see the header of src/indexer/main.cpp)\n", message.c_str());
    std::exit(2);
}

std::map<std::string, std::string> keyValues(std::string_view text)
{
    std::map<std::string, std::string> out;
    while (!text.empty()) {
        const std::size_t comma = text.find(',');
        const std::string_view item = text.substr(0, comma);
        const std::size_t equal = item.find('=');
        if (equal == std::string_view::npos)
            usage("expected key=value in --source, got " + std::string(item));
        out[std::string(item.substr(0, equal))] = std::string(item.substr(equal + 1));
        text = comma == std::string_view::npos ? std::string_view() : text.substr(comma + 1);
    }
    return out;
}

SourceConfig parseSource(std::string_view text)
{
    auto kv = keyValues(text);
    auto need = [&](const char* key) {
        const auto it = kv.find(key);
        if (it == kv.end() || it->second.empty())
            usage(std::string("--source needs ") + key);
        return it->second;
    };
    auto id = SourceId::parse(need("id"));
    const auto backend = parseBackendKind(need("backend"));
    const auto scope = parseScope(need("scope"));
    const auto kind = parseSourceKind(need("kind"));
    if (!id || !backend || !scope || !kind)
        usage("invalid id, backend, scope or kind in --source");
    auto source = Source::create({.id = std::move(id).value(),
                                  .backend = *backend,
                                  .scope = *scope,
                                  .kind = *kind,
                                  .origin = need("origin"),
                                  .label = kv["label"],
                                  .url = kv["url"],
                                  .priority = static_cast<std::int32_t>(std::stol(need("priority")))});
    if (!source)
        usage("invalid --source: " + source.error().message());
    return {std::move(source).value(), need("file"), kv["icons"]};
}

// Background work (ARCHITECTURE §6.2): lowest CPU priority, idle I/O class.
void lowerPriority()
{
    (void) setpriority(PRIO_PROCESS, 0, 19);
#ifdef SYS_ioprio_set
    constexpr int ioprioWhoProcess = 1;
    constexpr int ioprioClassIdle = 3;
    (void) syscall(SYS_ioprio_set, ioprioWhoProcess, 0, ioprioClassIdle << 13);
#endif
}

} // namespace

int main(int argc, char** argv)
{
    std::string output;
    std::string replace;
    CatalogBuildOptions build;
    ImportOptions import;
    CatalogReplaceRules rules;
    std::vector<SourceConfig> sources;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= argc)
                usage("missing value after " + arg);
            return argv[++i];
        };
        if (arg == "--output")
            output = value();
        else if (arg == "--generation")
            build.generation = std::stoull(value());
        else if (arg == "--locale")
            import.descriptionLocales.push_back(value());
        else if (arg == "--all-description-locales")
            import.allDescriptionLocales = true;
        else if (arg == "--batch")
            import.batchSize = std::stoul(value());
        else if (arg == "--source")
            sources.push_back(parseSource(value()));
        else if (arg == "--replace")
            replace = value();
        else if (arg == "--removed-source")
            rules.removedSources.push_back(value());
        else
            usage("unknown argument " + arg);
    }
    if (output.empty() || sources.empty())
        usage("--output and at least one --source are required");
    build.searchLocales = import.descriptionLocales;
    lowerPriority();

    const auto start = std::chrono::steady_clock::now();
    auto writer = CatalogWriter::create(output, build);
    if (!writer) {
        std::fprintf(stderr, "cannot create %s: %s\n", output.c_str(), writer.error().message().c_str());
        return 1;
    }
    const std::vector<SourceReport> reports = importSources(*writer, sources, import);
    const long rssAfterImport = peakRssKiB();
    const auto finishStart = std::chrono::steady_clock::now();
    auto stats = writer->finish();
    const auto end = std::chrono::steady_clock::now();
    auto seconds = [](auto a, auto b) { return std::chrono::duration<double>(b - a).count(); };

    for (const SourceReport& r : reports) {
        std::printf("source %s readable=%d components=%lld imported=%lld seconds=%.3f%s%s\n", r.sourceId.c_str(),
                    r.readable, static_cast<long long>(r.components), static_cast<long long>(r.imported), r.seconds,
                    r.error.empty() ? "" : " error=", r.error.c_str());
        for (const auto& [reason, count] : r.skipped)
            std::printf("  skipped %lld: %s\n", static_cast<long long>(count), reason.c_str());
        for (const std::string& example : r.examples)
            std::printf("  example: %s\n", example.c_str());
    }
    if (!stats) {
        std::fprintf(stderr, "cannot finish %s: %s\n", output.c_str(), stats.error().message().c_str());
        return 1;
    }
    std::error_code ec;
    std::printf("catalogue %s applications=%lld offers=%lld texts=%lld aliases=%lld ambiguous_aliases=%lld "
                "dropped_texts=%lld dropped_icons=%lld dropped_categories=%lld bytes=%lld import_s=%.3f "
                "finish_s=%.3f total_s=%.3f rss_after_import_kib=%ld max_rss_kib=%ld\n",
                output.c_str(), static_cast<long long>(stats->applications), static_cast<long long>(stats->offers),
                static_cast<long long>(stats->texts), static_cast<long long>(stats->aliases),
                static_cast<long long>(stats->ambiguousAliases), static_cast<long long>(stats->droppedTexts),
                static_cast<long long>(stats->droppedIcons), static_cast<long long>(stats->droppedCategories),
                static_cast<long long>(std::filesystem::file_size(output, ec)), seconds(start, finishStart),
                seconds(finishStart, end), seconds(start, end), rssAfterImport, peakRssKiB());

    if (!replace.empty()) {
        auto replaced = replaceCatalog(output, replace, rules);
        if (!replaced) {
            std::fprintf(stderr, "candidate refused, %s kept: %s\n", replace.c_str(), replaced.error().message().c_str());
            return 1;
        }
        std::printf("replaced %s generation=%llu\n", replace.c_str(),
                    static_cast<unsigned long long>(replaced->generation));
    }
    return 0;
}
