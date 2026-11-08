#include "pkenums.h"

namespace cutefish::store::packagekit {

namespace {

constexpr const char* kStatus[] = {
    "unknown", "wait", "setup", "running", "query", "info", "remove", "refresh-cache", "download",
    "install", "update", "cleanup", "obsolete", "dep-resolve", "sig-check", "test-commit", "commit",
    "request", "finished", "cancel", "download-repository", "download-packagelist", "download-filelist",
    "download-changelog", "download-group", "download-updateinfo", "repackaging", "loading-cache",
    "scan-applications", "generate-package-list", "waiting-for-lock", "waiting-for-auth",
    "scan-process-list", "check-executable-files", "check-libraries", "copy-files", "run-hook"};

constexpr const char* kExit[] = {
    "unknown", "success", "failed", "cancelled", "key-required", "eula-required", "killed",
    "media-change-required", "need-untrusted", "cancelled-priority", "skip-transaction", "repair-required"};

constexpr const char* kError[] = {
    "unknown", "oom", "no-network", "not-supported", "internal-error", "gpg-failure",
    "package-id-invalid", "package-not-installed", "package-not-found", "package-already-installed",
    "package-download-failed", "group-not-found", "group-list-invalid", "dep-resolution-failed",
    "filter-invalid", "create-thread-failed", "transaction-error", "transaction-cancelled", "no-cache",
    "repo-not-found", "cannot-remove-system-package", "process-kill", "failed-initialization",
    "failed-finalise", "failed-config-parsing", "cannot-cancel", "cannot-get-lock",
    "no-packages-to-update", "cannot-write-repo-config", "local-install-failed", "bad-gpg-signature",
    "missing-gpg-signature", "cannot-install-source-package", "repo-configuration-error",
    "no-license-agreement", "file-conflicts", "package-conflicts", "repo-not-available",
    "invalid-package-file", "package-install-blocked", "package-corrupt", "all-packages-already-installed",
    "file-not-found", "no-more-mirrors-to-try", "no-distro-upgrade-data", "incompatible-architecture",
    "no-space-on-device", "media-change-required", "not-authorized", "update-not-found",
    "cannot-install-repo-unsigned", "cannot-update-repo-unsigned", "cannot-get-filelist",
    "cannot-get-requires", "cannot-disable-repository", "restricted-download",
    "package-failed-to-configure", "package-failed-to-build", "package-failed-to-install",
    "package-failed-to-remove", "update-failed-due-to-running-process", "package-database-changed",
    "provide-type-not-supported", "install-root-invalid", "cannot-fetch-sources", "cancelled-priority",
    "unfinished-transaction", "lock-required", "repo-already-set"};

template <std::size_t N>
std::string_view lookup(const char* const (&table)[N], std::uint32_t v) noexcept
{
    return v < N ? std::string_view(table[v]) : std::string_view("unknown");
}

} // namespace

std::string_view errorName(std::uint32_t code) noexcept { return lookup(kError, code); }
std::string_view statusName(std::uint32_t status) noexcept { return lookup(kStatus, status); }
std::string_view exitName(std::uint32_t exit) noexcept { return lookup(kExit, exit); }

} // namespace cutefish::store::packagekit
