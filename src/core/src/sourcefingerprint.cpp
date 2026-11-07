#include <cutefish/store/sourcefingerprint.h>

#include "sha256.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <map>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace cutefish::store {

namespace {

bool sameDescriptor(const Source& a, const Source& b) noexcept
{
    return a.backend() == b.backend() && a.scope() == b.scope() && a.kind() == b.kind() && a.origin() == b.origin()
            && a.label() == b.label() && a.url() == b.url() && a.priority() == b.priority()
            && a.enabled() == b.enabled();
}

/// Same content: path and hash (size and time are only a shortcut), and the
/// same icon directory, since icon paths are stored in the catalogue.
bool sameContent(const SourceFingerprint& a, const SourceFingerprint& b) noexcept
{
    return a.path == b.path && a.sha256 == b.sha256 && a.iconsPath == b.iconsPath;
}

} // namespace

std::string_view toString(SourceChange change) noexcept
{
    switch (change) {
    case SourceChange::Unchanged: return "unchanged";
    case SourceChange::Modified: return "modified";
    case SourceChange::Added: return "added";
    case SourceChange::Removed: return "removed";
    case SourceChange::Missing: return "missing";
    case SourceChange::Unreadable: return "unreadable";
    }
    return "modified";
}

SourceProbe probeSource(const SourceFiles& files, const SourceFingerprint* previous)
{
    const std::string path = files.metadata.string();
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) {
        const int e = errno;
        if (e == ENOENT || e == ENOTDIR)
            return {SourceFileState::Missing, std::nullopt, "no such file " + path};
        return {SourceFileState::Unreadable, std::nullopt, path + ": " + std::strerror(e)};
    }
    if (!S_ISREG(st.st_mode))
        return {SourceFileState::Unreadable, std::nullopt, path + " is not a regular file"};

    SourceFingerprint fp{.path = path,
                         .size = static_cast<std::uint64_t>(st.st_size),
                         .mtimeNs = static_cast<std::int64_t>(st.st_mtim.tv_sec) * 1'000'000'000
                                 + static_cast<std::int64_t>(st.st_mtim.tv_nsec),
                         .sha256 = {},
                         .iconsPath = files.icons.string()};
    if (previous && previous->path == fp.path && previous->size == fp.size && previous->mtimeNs == fp.mtimeNs
        && previous->sha256.size() == 64) {
        fp.sha256 = previous->sha256;
        return {SourceFileState::Present, std::move(fp), {}};
    }

    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        const int e = errno;
        if (e == ENOENT)
            return {SourceFileState::Missing, std::nullopt, "no such file " + path};
        return {SourceFileState::Unreadable, std::nullopt, path + ": " + std::strerror(e)};
    }
    detail::Sha256 hash;
    std::uint64_t total = 0;
    char buffer[1 << 16];
    for (;;) {
        const ssize_t n = ::read(fd, buffer, sizeof buffer);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            const int e = errno;
            ::close(fd);
            return {SourceFileState::Unreadable, std::nullopt, path + ": " + std::strerror(e)};
        }
        if (n == 0)
            break;
        hash.update(buffer, static_cast<std::size_t>(n));
        total += static_cast<std::uint64_t>(n);
    }
    ::close(fd);
    fp.sha256 = hash.hexDigest();
    // What was hashed is what counts: the file may have grown or shrunk
    // since stat().
    fp.size = total;
    return {SourceFileState::Present, std::move(fp), {}};
}

SourceAssessment assessSources(std::span<const RecordedSource> recorded, std::span<const ObservedSource> observed)
{
    std::map<std::string, const RecordedSource*> before;
    for (const RecordedSource& r : recorded)
        before.emplace(r.source.id().str(), &r);
    std::map<std::string, const ObservedSource*> now;
    for (const ObservedSource& o : observed)
        now.emplace(o.source.id().str(), &o);

    SourceAssessment out;
    bool changed = false;
    for (const auto& [id, o] : now) {
        const auto it = before.find(id);
        const RecordedSource* r = it == before.end() ? nullptr : it->second;
        SourceStatus s{.id = o->source.id(), .change = SourceChange::Unchanged, .inCatalog = r != nullptr,
                       .current = o->probe.fingerprint, .detail = o->probe.detail};
        switch (o->probe.state) {
        case SourceFileState::Missing:
        case SourceFileState::Unreadable:
            s.change = o->probe.state == SourceFileState::Missing ? SourceChange::Missing : SourceChange::Unreadable;
            if (r)
                out.blocking.push_back(s.id);
            break;
        case SourceFileState::Present:
            out.importable.push_back(s.id);
            if (!r)
                s.change = SourceChange::Added;
            else if (!sameDescriptor(r->source, o->source) || !r->fingerprint || !o->probe.fingerprint
                     || !sameContent(*r->fingerprint, *o->probe.fingerprint))
                s.change = SourceChange::Modified;
            changed = changed || s.change != SourceChange::Unchanged;
            break;
        }
        out.sources.push_back(std::move(s));
    }
    for (const auto& [id, r] : before) {
        if (now.contains(id))
            continue;
        out.sources.push_back({.id = r->source.id(), .change = SourceChange::Removed, .inCatalog = true});
        out.removed.push_back(r->source.id());
        changed = true;
    }
    std::sort(out.sources.begin(), out.sources.end(),
              [](const SourceStatus& a, const SourceStatus& b) { return a.id < b.id; });
    out.reimport = changed && out.blocking.empty() && !out.importable.empty();
    return out;
}

} // namespace cutefish::store
