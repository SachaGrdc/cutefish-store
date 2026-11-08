#include "options.h"

#include <base/paths.h>

#include <charconv>
#include <cstdlib>

#ifndef CUTEFISH_STORE_INDEXER_PATH
#define CUTEFISH_STORE_INDEXER_PATH "cutefish-store-indexer"
#endif

namespace cutefish::store::service {

namespace {

Result<std::chrono::milliseconds> milliseconds(const std::string& text)
{
    long long value = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc() || end != text.data() + text.size() || value <= 0)
        return Error::invalidArgument("expected a positive number of milliseconds, got " + text);
    return std::chrono::milliseconds(value);
}

} // namespace

std::vector<std::string> environmentLocales()
{
    std::vector<std::string> out;
    auto add = [&](std::string value) {
        if (!value.empty() && value != "C" && value != "POSIX")
            out.push_back(std::move(value));
    };
    // LANGUAGE is a colon-separated preference list (gettext).
    if (const char* language = std::getenv("LANGUAGE")) {
        std::string_view rest = language;
        while (!rest.empty()) {
            const auto colon = rest.find(':');
            add(std::string(rest.substr(0, colon)));
            rest = colon == std::string_view::npos ? std::string_view() : rest.substr(colon + 1);
        }
    }
    for (const char* name : {"LC_ALL", "LC_MESSAGES", "LANG"})
        if (const char* value = std::getenv(name); value && *value) {
            add(value);
            break;
        }
    return out;
}

Result<ServiceOptions> parseArguments(const std::vector<std::string>& arguments)
{
    ServiceOptions o;
    o.dataDir = std::filesystem::path(base::paths::stateHome()) / "cutefish-store";
    o.sourcesFile = std::filesystem::path(base::paths::configHome()) / "cutefish-store" / "sources.conf";
    o.indexer = CUTEFISH_STORE_INDEXER_PATH;
    bool localesGiven = false;
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const std::string& arg = arguments[i];
        if (i + 1 >= arguments.size())
            return Error::invalidArgument("missing value after " + arg);
        const std::string& value = arguments[++i];
        if (arg == "--data-dir") {
            o.dataDir = value;
        } else if (arg == "--sources") {
            o.sourcesFile = value;
        } else if (arg == "--indexer") {
            o.indexer = value;
        } else if (arg == "--idle-timeout-ms" || arg == "--retention-ms" || arg == "--confirmation-timeout-ms") {
            auto ms = milliseconds(value);
            if (!ms)
                return std::move(ms).error();
            (arg == "--idle-timeout-ms" ? o.idleTimeout
             : arg == "--retention-ms"  ? o.transactionRetention
                                        : o.confirmationTimeout) = *ms;
        } else if (arg == "--packagekit") {
            if (value == "system")
                o.packagekit = PackageKitBus::System;
            else if (value == "session")
                o.packagekit = PackageKitBus::Session;
            else if (value == "off")
                o.packagekit = PackageKitBus::Off;
            else
                return Error::invalidArgument("--packagekit expects system, session or off, got " + value);
        } else if (arg == "--swcatalog") {
            o.swcatalog = value;
        } else if (arg == "--flatpak") {
            if (value == "auto")
                o.flatpak = FlatpakMode::Auto;
            else if (value == "off")
                o.flatpak = FlatpakMode::Off;
            else
                return Error::invalidArgument("--flatpak expects auto or off, got " + value);
        } else if (arg == "--locale") {
            o.readLocales.push_back(value);
            localesGiven = true;
        } else {
            return Error::invalidArgument("unknown argument " + arg);
        }
    }
    if (!localesGiven)
        o.readLocales = environmentLocales();
    return o;
}

} // namespace cutefish::store::service
