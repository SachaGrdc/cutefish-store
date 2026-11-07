#include <cutefish/store/catalogfile.h>

#include <cutefish/store/catalogwriter.h>

#include "catalogschema.h"
#include "sqlite.h"
#include "sqliteuri.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <system_error>

#include <fcntl.h>
#include <unistd.h>

namespace cutefish::store {

using detail::Database;
using detail::Statement;

namespace {

Result<void> fsyncPath(const std::filesystem::path& path, int flags)
{
    const int fd = ::open(path.c_str(), flags | O_CLOEXEC);
    if (fd < 0)
        return Error::internal("open " + path.string() + ": " + std::strerror(errno));
    const int rc = ::fsync(fd);
    const int saved = errno;
    ::close(fd);
    if (rc != 0)
        return Error::internal("fsync " + path.string() + ": " + std::strerror(saved));
    return {};
}

} // namespace

Result<CatalogFileInfo> inspectCatalog(const std::filesystem::path& path)
{
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec))
        return Error::notFound("no catalogue file " + path.string());
    for (const char* suffix : {"-journal", "-wal"})
        if (std::filesystem::exists(path.string() + suffix, ec))
            return Error::invalidArgument("catalogue " + path.string() + " has a " + suffix + " file: interrupted write");

    auto db = Database::open(detail::readOnlyUri(path, false), SQLITE_OPEN_READONLY);
    if (!db)
        return std::move(db).error();
    auto version = db->queryInt("PRAGMA user_version");
    if (!version)
        return Error::invalidArgument("not a catalogue: " + version.error().message());
    if (*version != kCatalogSchemaVersion)
        return Error::invalidArgument("incomplete or foreign catalogue: schema version " + std::to_string(*version));

    {
        auto check = Statement::prepare(*db, "PRAGMA quick_check");
        if (!check)
            return Error::invalidArgument("corrupted catalogue: " + check.error().message());
        const int rc = check->step();
        if (rc != SQLITE_ROW || check->columnText(0) != "ok")
            return Error::invalidArgument("corrupted catalogue: " +
                                          std::string(rc == SQLITE_ROW ? check->columnText(0) : "quick_check failed"));
    }
    {
        auto fk = Statement::prepare(*db, "PRAGMA foreign_key_check");
        if (!fk)
            return std::move(fk).error();
        if (fk->step() == SQLITE_ROW)
            return Error::invalidArgument("catalogue violates a foreign key in table " + std::string(fk->columnText(0)));
    }
    {
        auto table = Statement::prepare(*db, "SELECT 1 FROM sqlite_schema WHERE type = 'table' AND name = ?");
        if (!table)
            return std::move(table).error();
        for (const char* name : detail::kRequiredTables) {
            table->bind(1, std::string_view(name));
            const bool present = table->step() == SQLITE_ROW;
            table->reset();
            if (!present)
                return Error::invalidArgument(std::string("catalogue has no table ") + name);
        }
    }
    for (const char* probe : {"SELECT count(*) FROM search WHERE search MATCH 'a*'",
                              "SELECT count(*) FROM search_names WHERE search_names MATCH '\"abc\"'"})
        if (auto ok = db->queryInt(probe); !ok)
            return Error::invalidArgument("catalogue search index unusable: " + ok.error().message());

    CatalogFileInfo info;
    auto apps = db->queryInt("SELECT count(*) FROM apps");
    auto offers = db->queryInt("SELECT count(*) FROM offers");
    if (!apps || !offers)
        return Error::invalidArgument("catalogue cannot be counted");
    info.applications = *apps;
    info.offers = *offers;
    auto meta = Statement::prepare(*db, "SELECT key, value FROM meta");
    if (!meta)
        return std::move(meta).error();
    while (meta->step() == SQLITE_ROW) {
        const std::string_view key = meta->columnText(0);
        const std::string value(meta->columnText(1));
        if (key == "generation")
            info.generation = std::stoull(value);
        else if (key.starts_with("offers:"))
            info.offersPerSource[std::string(key.substr(7))] = std::stoll(value);
    }
    return info;
}

Result<CatalogFileInfo> replaceCatalog(const std::filesystem::path& candidate, const std::filesystem::path& active,
                                       const CatalogReplaceRules& rules)
{
    if (candidate.parent_path() != active.parent_path())
        return Error::invalidArgument("candidate and active catalogue must be in the same directory");
    auto info = inspectCatalog(candidate);
    if (!info)
        return std::move(info).error();

    std::error_code ec;
    if (std::filesystem::exists(active, ec)) {
        auto current = inspectCatalog(active);
        // A broken active catalogue is no reference: any valid candidate is
        // better than it.
        if (current) {
            for (const auto& [source, count] : current->offersPerSource) {
                if (std::find(rules.removedSources.begin(), rules.removedSources.end(), source) !=
                    rules.removedSources.end())
                    continue;
                const auto it = info->offersPerSource.find(source);
                const std::int64_t now = it == info->offersPerSource.end() ? -1 : it->second;
                if (now < 0)
                    return Error::invalidArgument("implausible candidate: source " + source + " is missing");
                if (static_cast<double>(now) < rules.minimumShare * static_cast<double>(count))
                    return Error::invalidArgument("implausible candidate: source " + source + " has " +
                                                  std::to_string(now) + " offers instead of " + std::to_string(count));
            }
        }
    }

    if (auto ok = fsyncPath(candidate, O_RDONLY); !ok)
        return std::move(ok).error();
    std::filesystem::rename(candidate, active, ec);
    if (ec)
        return Error::internal("rename " + candidate.string() + ": " + ec.message());
    const std::filesystem::path directory = active.parent_path().empty() ? "." : active.parent_path();
    if (auto ok = fsyncPath(directory, O_RDONLY | O_DIRECTORY); !ok)
        return std::move(ok).error();
    return info;
}

} // namespace cutefish::store
