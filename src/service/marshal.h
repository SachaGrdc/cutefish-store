#pragma once

// Conversions between the core's types and the D-Bus API (ARCHITECTURE
// §5.4, SERVICE.md §4.6), and the mapping of core errors to D-Bus errors
// (SERVICE.md §10). Pure functions: no bus, no thread.

#include <cutefish/store/application.h>
#include <cutefish/store/backend.h>
#include <cutefish/store/catalog.h>
#include <cutefish/store/confirmation.h>
#include <cutefish/store/error.h>
#include <cutefish/store/installed.h>
#include <cutefish/store/search.h>
#include <cutefish/store/sourcefingerprint.h>
#include <cutefish/store/statestore.h>

#include <base/dbus_types.h>

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace cutefish::store::service {

namespace dbus = cutefish::base::dbus;

/// D-Bus error names (ARCHITECTURE §5.6).
namespace errors {
inline constexpr const char* kInvalidArgs = "org.freedesktop.DBus.Error.InvalidArgs";
inline constexpr const char* kFailed = "org.freedesktop.DBus.Error.Failed";
inline constexpr const char* kNotFound = "org.cutefish.Store1.Error.NotFound";
inline constexpr const char* kNotSupported = "org.cutefish.Store1.Error.NotSupported";
inline constexpr const char* kNotReady = "org.cutefish.Store1.Error.NotReady";
inline constexpr const char* kBusy = "org.cutefish.Store1.Error.Busy";
inline constexpr const char* kNotAllowed = "org.cutefish.Store1.Error.NotAllowed";
inline constexpr const char* kBackendUnavailable = "org.cutefish.Store1.Error.BackendUnavailable";
} // namespace errors

struct DbusError
{
    std::string name;
    std::string message;
};

/// InvalidArgument → InvalidArgs, NotFound → NotFound, NotSupported →
/// NotSupported, Conflict → Busy, Backend/Unavailable → BackendUnavailable,
/// everything else → Failed. The message is the core's, for diagnosis.
DbusError toDbusError(const Error& error);

/// The `options` of Search and Browse: `limit` (u), `offset` (u),
/// `category` (s), `backend` (s), `installed-only` (b). An unknown key or a
/// wrong type is an InvalidArgument (ARCHITECTURE §5.1).
Result<SearchOptions> searchOptions(const dbus::VariantMap& options);

/// The `options` of GetInstalled: `backend` (s).
Result<std::optional<BackendKind>> installedOptions(const dbus::VariantMap& options);

/// Why Install refuses this offer before any transaction, or empty when it
/// may be installed. A new installation goes to the user installation
/// (D-005); the system installation of a backend whose system installation
/// is shared by every user (ConfirmationPolicy::sharedSystemScope: Flatpak)
/// is only managed, never added to (D-011, O-013 open). Distribution
/// packages are system-wide by nature and are not concerned.
std::string newInstallRefusal(const OfferId& offer);

dbus::VariantMap toMap(const AppSummary& summary);
dbus::VariantMap toMap(const CategoryCount& category);
dbus::VariantMap toMap(const InstalledItem& item);
/// A source of the catalogue, with its last successful refresh if known.
dbus::VariantMap toMap(const RecordedSource& source, std::optional<UnixSeconds> lastRefresh);
/// A finished transaction of the journal.
dbus::VariantMap toMap(const TransactionRecord& record);
/// An available update (GetUpdates): `offer-id`, `app-id` (absent for a
/// runtime), `backend`, `kind`, `from-version`, `to-version` when known,
/// `download-size` (t, 0 when unknown).
dbus::VariantMap toMap(const AvailableUpdate& update, BackendKind backend);
/// An entry of the Backends property (ARCHITECTURE §5.2): `id`,
/// `available` (b, false while not known yet), `reason` (s), `scopes` (as),
/// `formats` (as), `inventory-error` (s) only when the backend's last
/// inventory could not be recorded (H.2), `activity` (s: none, checking,
/// refreshing, changing) and, unless none, `activity-role` (s): what its
/// package manager runs for other programs (H.2, SERVICE.md §22).
dbus::VariantMap backendMap(BackendKind kind, const BackendCapabilities& caps, std::optional<bool> available,
                            const std::string& reason,
                            const std::string& inventoryError = {}, const ExternalActivity& activity = {});
/// The `Plan` of a transaction (SERVICE.md §4.6): `kind`, `backend`,
/// `scope`, `requires-confirmation` (b), `reasons` (as), `download-size`
/// (t), `installed-size-change` (x), and `operations` (aa{sv}): `operation`,
/// `kind`, `package`, `requested` (b), `from-version`, `to-version`,
/// `origin` when known, `download-size` (t), `reasons` (as, the concerns of
/// that item). Reasons only while a confirmation is pending.
dbus::VariantMap planMap(const Plan& plan, const ConfirmationReasons& reasons,
                         std::span<const ConfirmationConcern> concerns);
/// `code`, `message`, `details` (ARCHITECTURE §5.3, property `Error`).
dbus::VariantMap errorMap(const Error& error);

/// GetDetails: the summary keys, `description` (a(ss)), `categories`,
/// `aliases`, and `offers` with their installed state and `default`
/// (D-009, computed here from the installed offers).
dbus::VariantMap details(const AppSummary& summary, const Application& app,
                         std::span<const InstalledItem> installedForApp);

} // namespace cutefish::store::service
