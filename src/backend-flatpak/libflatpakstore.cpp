#include "libflatpakstore.h"

#include "translate.h"

#include <flatpak.h>

#include <cstring>

namespace cutefish::store::flatpak {

namespace {

/// The FlatpakRefKind of a parsed ref.
FlatpakRefKind refKindOf(RefKind kind)
{
    return kind == RefKind::App ? FLATPAK_REF_KIND_APP : FLATPAK_REF_KIND_RUNTIME;
}

RefKind fromFlatpakKind(FlatpakRefKind kind)
{
    return kind == FLATPAK_REF_KIND_APP ? RefKind::App : RefKind::Runtime;
}

Error errorOf(const GError* error)
{
    if (error == nullptr)
        return Error::internal("no error given");
    std::string_view domain;
    if (error->domain == FLATPAK_ERROR)
        domain = "flatpak";
    else if (error->domain == G_IO_ERROR)
        domain = "g-io";
    else if (error->domain == G_DBUS_ERROR)
        domain = "g-dbus";
    else
        domain = "flatpak";
    return mapFlatpakError(domain, error->code, error->message ? error->message : "");
}

FlatpakRef refFrom(const char* full)
{
    auto parsed = parseRef(full ? full : "");
    return parsed ? std::move(parsed).value() : FlatpakRef{};
}

/// The [Context] section and the bus policies of a metadata file, as stable
/// `key=value` strings (ARCHITECTURE §5.4, §7.3).
std::vector<std::string> permissionsOf(GKeyFile* metadata)
{
    std::vector<std::string> out;
    if (metadata == nullptr)
        return out;
    auto add = [&](const char* group) {
        gsize n = 0;
        g_auto(GStrv) keys = g_key_file_get_keys(metadata, group, &n, nullptr);
        for (gsize i = 0; keys && i < n; ++i) {
            gsize values = 0;
            g_auto(GStrv) list = g_key_file_get_string_list(metadata, group, keys[i], &values, nullptr);
            for (gsize j = 0; list && j < values; ++j)
                out.emplace_back(std::string(keys[i]) + "=" + list[j]);
        }
    };
    add("Context");
    add("Session Bus Policy");
    add("System Bus Policy");
    return normalisePermissions(std::move(out));
}

} // namespace

LibFlatpakStore::LibFlatpakStore(Options options) : m_options(std::move(options)) {}

LibFlatpakStore::~LibFlatpakStore()
{
    // FlatpakInstallation objects are released on the worker thread before the
    // backend joins it; no explicit unref is needed beyond the object owner.
}

std::vector<Scope> LibFlatpakStore::scopes()
{
    std::vector<Scope> out;
    if (m_options.user) {
        auto handle = installation(Scope::User);
        if (handle)
            out.push_back(Scope::User);
    }
    if (m_options.system) {
        auto handle = installation(Scope::System);
        if (handle)
            out.push_back(Scope::System);
    }
    return out;
}

Result<void*> LibFlatpakStore::installation(Scope scope)
{
    const bool user = scope == Scope::User;
    void*& cached = user ? m_user : m_system;
    bool& failed = user ? m_userFailed : m_systemFailed;
    if (failed)
        return Error::backend(BackendFailure::Unavailable, "the installation could not be opened");
    if (cached)
        return cached;

    GError* error = nullptr;
    FlatpakInstallation* handle = nullptr;
    if (user && m_options.userPath) {
        g_autoptr(GFile) path = g_file_new_for_path(m_options.userPath->c_str());
        handle = flatpak_installation_new_for_path(path, TRUE, nullptr, &error);
    } else if (!user && m_options.systemPath) {
        g_autoptr(GFile) path = g_file_new_for_path(m_options.systemPath->c_str());
        handle = flatpak_installation_new_for_path(path, FALSE, nullptr, &error);
    } else if (user) {
        handle = flatpak_installation_new_user(nullptr, &error);
    } else {
        handle = flatpak_installation_new_system(nullptr, &error);
    }
    if (handle == nullptr) {
        failed = true;
        g_clear_error(&error);
        return Error::backend(BackendFailure::Unavailable,
                              user ? "no user installation" : "no system installation");
    }
    cached = handle;
    return cached;
}

Result<std::vector<RemoteInfo>> LibFlatpakStore::remotes(Scope scope)
{
    auto handle = installation(scope);
    if (!handle)
        return std::move(handle).error();
    GError* error = nullptr;
    g_autoptr(GPtrArray) list = flatpak_installation_list_remotes(static_cast<FlatpakInstallation*>(*handle), nullptr, &error);
    if (list == nullptr)
        return errorOf(error);
    std::vector<RemoteInfo> out;
    const char* arch = flatpak_get_default_arch();
    for (guint i = 0; i < list->len; ++i) {
        auto* remote = static_cast<FlatpakRemote*>(g_ptr_array_index(list, i));
        RemoteInfo info{.name = flatpak_remote_get_name(remote),
                        .url = flatpak_remote_get_url(remote),
                        .disabled = flatpak_remote_get_disabled(remote) != FALSE,
                        .gpgVerify = flatpak_remote_get_gpg_verify(remote) != FALSE,
                        .metadataPath = {},
                        .iconsPath = {}};
        g_autoptr(GFile) dir = flatpak_remote_get_appstream_dir(remote, arch);
        if (dir) {
            g_autofree char* path = g_file_get_path(dir);
            if (path) {
                info.metadataPath = std::string(path) + "/appstream.xml.gz";
                info.iconsPath = std::string(path) + "/icons";
            }
        }
        out.push_back(std::move(info));
    }
    return out;
}

Result<void> LibFlatpakStore::addRemote(Scope scope, const std::string& name, const std::string& url)
{
    auto handle = installation(scope);
    if (!handle)
        return std::move(handle).error();
    GError* error = nullptr;
    g_autoptr(FlatpakRemote) remote = flatpak_remote_new(name.c_str());
    flatpak_remote_set_url(remote, url.c_str());
    if (!flatpak_installation_add_remote(static_cast<FlatpakInstallation*>(*handle), remote, FALSE, nullptr, &error))
        return errorOf(error);
    return Result<void>::success();
}

Result<void> LibFlatpakStore::removeRemote(Scope scope, const std::string& name)
{
    auto handle = installation(scope);
    if (!handle)
        return std::move(handle).error();
    GError* error = nullptr;
    if (!flatpak_installation_remove_remote(static_cast<FlatpakInstallation*>(*handle), name.c_str(), nullptr, &error))
        return errorOf(error);
    return Result<void>::success();
}

Result<void> LibFlatpakStore::setRemoteDisabled(Scope scope, const std::string& name, bool disabled)
{
    auto handle = installation(scope);
    if (!handle)
        return std::move(handle).error();
    GError* error = nullptr;
    g_autoptr(FlatpakRemote) remote =
            flatpak_installation_get_remote_by_name(static_cast<FlatpakInstallation*>(*handle), name.c_str(), nullptr, &error);
    if (remote == nullptr)
        return errorOf(error);
    flatpak_remote_set_disabled(remote, disabled);
    if (!flatpak_installation_modify_remote(static_cast<FlatpakInstallation*>(*handle), remote, nullptr, &error))
        return errorOf(error);
    return Result<void>::success();
}

namespace {

std::optional<InstalledRef> installedRefOf(FlatpakInstalledRef* ref)
{
    auto* base = reinterpret_cast<::FlatpakRef*>(ref);
    const char* name = flatpak_ref_get_name(base);
    const char* arch = flatpak_ref_get_arch(base);
    const char* branch = flatpak_ref_get_branch(base);
    const char* origin = flatpak_installed_ref_get_origin(ref);
    if (name == nullptr || arch == nullptr || branch == nullptr)
        return std::nullopt;
    InstalledRef out;
    out.ref.kind = fromFlatpakKind(flatpak_ref_get_kind(base));
    out.ref.name = name;
    out.ref.arch = arch;
    out.ref.branch = branch;
    if (origin)
        out.origin = origin;
    return out;
}

} // namespace

Result<std::vector<InstalledRef>> LibFlatpakStore::installed(Scope scope)
{
    auto handle = installation(scope);
    if (!handle)
        return std::move(handle).error();
    GError* error = nullptr;
    g_autoptr(GPtrArray) list =
            flatpak_installation_list_installed_refs(static_cast<FlatpakInstallation*>(*handle), nullptr, &error);
    if (list == nullptr)
        return errorOf(error);
    std::vector<InstalledRef> out;
    for (guint i = 0; i < list->len; ++i)
        if (auto ref = installedRefOf(static_cast<FlatpakInstalledRef*>(g_ptr_array_index(list, i))))
            out.push_back(std::move(*ref));
    return out;
}

Result<std::vector<UpdateRef>> LibFlatpakStore::updates(Scope scope)
{
    auto handle = installation(scope);
    if (!handle)
        return std::move(handle).error();
    GError* error = nullptr;
    g_autoptr(GPtrArray) list =
            flatpak_installation_list_installed_refs_for_update(static_cast<FlatpakInstallation*>(*handle), nullptr, &error);
    if (list == nullptr)
        return errorOf(error);
    std::vector<UpdateRef> out;
    for (guint i = 0; i < list->len; ++i) {
        auto* ref = static_cast<FlatpakInstalledRef*>(g_ptr_array_index(list, i));
        if (auto item = installedRefOf(ref)) {
            const char* commit = flatpak_installed_ref_get_latest_commit(ref);
            UpdateRef u{.ref = std::move(item->ref), .origin = std::move(item->origin), .newCommit = {}};
            if (commit)
                u.newCommit = commit;
            out.push_back(std::move(u));
        }
    }
    return out;
}

Result<RefMetadata> LibFlatpakStore::metadata(Scope scope, const std::string& remote, const FlatpakRef& ref)
{
    auto handle = installation(scope);
    if (!handle)
        return std::move(handle).error();
    GError* error = nullptr;
    g_autoptr(FlatpakRemoteRef) remoteRef = flatpak_installation_fetch_remote_ref_sync_full(
            static_cast<FlatpakInstallation*>(*handle), remote.c_str(), refKindOf(ref.kind), ref.name.c_str(),
            ref.arch.c_str(), ref.branch.c_str(), FLATPAK_QUERY_FLAGS_NONE, nullptr, &error);
    if (remoteRef == nullptr)
        return errorOf(error);
    RefMetadata out;
    out.downloadSize = flatpak_remote_ref_get_download_size(remoteRef);
    out.installedSize = flatpak_remote_ref_get_installed_size(remoteRef);
    const char* eol = flatpak_remote_ref_get_eol(remoteRef);
    if (eol)
        out.eol = true;
    const char* rebase = flatpak_remote_ref_get_eol_rebase(remoteRef);
    if (rebase)
        out.eolRebase = rebase;
    g_autoptr(GBytes) bytes = flatpak_remote_ref_get_metadata(remoteRef);
    if (bytes) {
        g_autoptr(GKeyFile) keyFile = g_key_file_new();
        gsize size = 0;
        const char* data = static_cast<const char*>(g_bytes_get_data(bytes, &size));
        if (data && size && g_key_file_load_from_data(keyFile, data, size, G_KEY_FILE_NONE, nullptr))
            out.permissions = permissionsOf(keyFile);
    }
    return out;
}

namespace {

struct TransactionState
{
    TransactionSpec spec;
    std::vector<FlatpakOp> ops;
    std::optional<Error> error;
    bool changed = false;
    bool cancelled = false;
};

/// Copies the operations the transaction resolved from its `ready` signal.
void collectOps(FlatpakTransaction* transaction, std::vector<FlatpakOp>& ops)
{
    ops.clear();
    GList* list = flatpak_transaction_get_operations(transaction);
    for (GList* it = list; it != nullptr; it = it->next) {
        auto* op = static_cast<FlatpakTransactionOperation*>(it->data);
        FlatpakOp out;
        switch (flatpak_transaction_operation_get_operation_type(op)) {
        case FLATPAK_TRANSACTION_OPERATION_INSTALL: out.type = OperationType::Install; break;
        case FLATPAK_TRANSACTION_OPERATION_UPDATE: out.type = OperationType::Update; break;
        case FLATPAK_TRANSACTION_OPERATION_UNINSTALL: out.type = OperationType::Uninstall; break;
        default: continue; // a bundle or flatpakref: not produced here
        }
        out.ref = refFrom(flatpak_transaction_operation_get_ref(op));
        if (out.ref.name.empty())
            continue;
        const char* remote = flatpak_transaction_operation_get_remote(op);
        if (remote)
            out.remote = remote;
        const char* commit = flatpak_transaction_operation_get_commit(op);
        if (commit)
            out.commit = commit;
        out.downloadSize = flatpak_transaction_operation_get_download_size(op);
        out.installedSize = flatpak_transaction_operation_get_installed_size(op);
        out.permissions = permissionsOf(flatpak_transaction_operation_get_metadata(op));
        out.oldPermissions = permissionsOf(flatpak_transaction_operation_get_old_metadata(op));
        ops.push_back(std::move(out));
    }
}

gboolean onReady(FlatpakTransaction* transaction, gpointer user_data)
{
    auto* state = static_cast<TransactionState*>(user_data);
    collectOps(transaction, state->ops);
    return FALSE; // the caller decides: resolve() always stops here
}

} // namespace

Result<std::vector<FlatpakOp>> LibFlatpakStore::resolve(const TransactionSpec& spec)
{
    auto handle = installation(spec.scope);
    if (!handle)
        return std::move(handle).error();
    GError* error = nullptr;
    g_autoptr(FlatpakTransaction) tx =
            flatpak_transaction_new_for_installation(static_cast<FlatpakInstallation*>(*handle), nullptr, &error);
    if (tx == nullptr)
        return errorOf(error);
    for (const RefRequest& r : spec.refs) {
        const std::string full = r.ref.full();
        gboolean ok = FALSE;
        switch (spec.kind) {
        case TransactionKind::Install:
            ok = flatpak_transaction_add_install(tx, r.remote.c_str(), full.c_str(), nullptr, &error);
            break;
        case TransactionKind::Remove:
            ok = flatpak_transaction_add_uninstall(tx, full.c_str(), &error);
            break;
        case TransactionKind::Update:
            ok = flatpak_transaction_add_update(tx, full.c_str(), nullptr, nullptr, &error);
            break;
        }
        if (!ok)
            return errorOf(error);
    }
    flatpak_transaction_set_no_interaction(tx, TRUE);
    flatpak_transaction_add_default_dependency_sources(tx);

    TransactionState state;
    state.spec = spec;
    g_signal_connect(tx, "ready", G_CALLBACK(onReady), &state);
    g_autoptr(GCancellable) cancel = g_cancellable_new();
    gboolean ran = flatpak_transaction_run(tx, cancel, &error);
    if (!ran) {
        // The first pass always aborts in `ready` (FLATPAK_ERROR_ABORTED): the
        // operations are the result, not an error (ARCHITECTURE §7.3).
        if (error && error->domain == FLATPAK_ERROR && error->code == FLATPAK_ERROR_ABORTED)
            return state.ops;
        return errorOf(error);
    }
    return state.ops;
}

Result<ExecutionOutcome> LibFlatpakStore::execute(const TransactionSpec& spec, ExecuteControl control)
{
    auto handle = installation(spec.scope);
    if (!handle)
        return std::move(handle).error();
    GError* error = nullptr;
    g_autoptr(FlatpakTransaction) tx =
            flatpak_transaction_new_for_installation(static_cast<FlatpakInstallation*>(*handle), nullptr, &error);
    if (tx == nullptr)
        return errorOf(error);
    for (const RefRequest& r : spec.refs) {
        const std::string full = r.ref.full();
        gboolean ok = FALSE;
        switch (spec.kind) {
        case TransactionKind::Install:
            ok = flatpak_transaction_add_install(tx, r.remote.c_str(), full.c_str(), nullptr, &error);
            break;
        case TransactionKind::Remove:
            ok = flatpak_transaction_add_uninstall(tx, full.c_str(), &error);
            break;
        case TransactionKind::Update:
            ok = flatpak_transaction_add_update(tx, full.c_str(), nullptr, nullptr, &error);
            break;
        }
        if (!ok)
            return errorOf(error);
    }
    // A user action on the system installation lets polkit ask; the user
    // installation and background work never show a dialog (ARCHITECTURE §7.3).
    flatpak_transaction_set_no_interaction(tx, spec.scope == Scope::User);
    flatpak_transaction_add_default_dependency_sources(tx);

    struct ExecuteState
    {
        ExecuteControl control;
        std::vector<FlatpakOp> ops;
        GCancellable* cancel = nullptr;
        bool changed = false;
        bool cancelled = false;
    };
    ExecuteState state;
    state.control = std::move(control);
    g_autoptr(GCancellable) cancel = g_cancellable_new();
    state.cancel = cancel;

    // The comparison happens here: the plan is the one this transaction will
    // execute (D-017). Returning false from `ready` stops it without any
    // download or deploy.
    g_signal_connect(tx, "ready",
                     G_CALLBACK(+[](FlatpakTransaction* t, gpointer d) -> gboolean {
                         auto* s = static_cast<ExecuteState*>(d);
                         collectOps(t, s->ops);
                         if (s->control.cancelled && s->control.cancelled()) {
                             s->cancelled = true;
                             return FALSE;
                         }
                         if (!s->control.onReady(s->ops)) {
                             s->changed = true;
                             return FALSE;
                         }
                         return TRUE;
                     }),
                     &state);

    // A cancellation request reaches the GCancellable of the running pass.
    g_signal_connect(tx, "new-operation",
                     G_CALLBACK(+[](FlatpakTransaction*, FlatpakTransactionOperation* op, FlatpakTransactionProgress*,
                                    gpointer d) {
                         auto* s = static_cast<ExecuteState*>(d);
                         if (s->control.cancelled && s->control.cancelled())
                             g_cancellable_cancel(s->cancel);
                         if (!s->control.progress)
                             return;
                         const char* ref = flatpak_transaction_operation_get_ref(op);
                         s->control.progress(FlatpakProgress{.fraction = -1.0, .status = "resolving",
                                                             .ref = ref ? ref : "", .cancellable = true});
                     }),
                     &state);

    gboolean ran = flatpak_transaction_run(tx, cancel, &error);
    if (state.changed) {
        // ready returned false: nothing was executed; the fresh operations are
        // the plan that would have run.
        return ExecutionOutcome{.type = ExecutionOutcome::Type::PlanChanged, .ops = std::move(state.ops)};
    }
    if (state.cancelled)
        return Error::cancelled("cancelled");
    if (!ran) {
        if (error && error->domain == FLATPAK_ERROR && error->code == FLATPAK_ERROR_ABORTED)
            return Error::cancelled("aborted");
        return errorOf(error);
    }
    return ExecutionOutcome{.type = ExecutionOutcome::Type::Completed, .ops = {}};
}

} // namespace cutefish::store::flatpak
