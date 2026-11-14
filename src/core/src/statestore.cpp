#include <cutefish/store/statestore.h>

#include "planjson.h"
#include "sqlite.h"
#include "sqliteuri.h"
#include "stateschema.h"
#include "text.h"

#include <algorithm>
#include <set>
#include <string>
#include <utility>

namespace cutefish::store {

using detail::Database;
using detail::Statement;

namespace {

// --- Connection setup ----------------------------------------------------------

constexpr int kBusyTimeoutMs = 5000;

Result<std::string> queryText(Database& db, const char* sql)
{
    auto stmt = Statement::prepare(db, sql);
    if (!stmt)
        return std::move(stmt).error();
    if (stmt->step() != SQLITE_ROW)
        return db.error(sql);
    return std::string(stmt->columnText(0));
}

Result<void> quickCheck(Database& db)
{
    auto r = queryText(db, "PRAGMA quick_check");
    if (!r)
        return Error::invalidArgument("damaged state database: " + r.error().message());
    if (*r != "ok")
        return Error::invalidArgument("damaged state database: " + *r);
    return {};
}

/// BEGIN IMMEDIATE ... COMMIT; rolled back unless committed.
class WriteTransaction
{
public:
    static Result<WriteTransaction> begin(Database& db)
    {
        if (auto ok = db.exec("BEGIN IMMEDIATE"); !ok)
            return std::move(ok).error();
        return WriteTransaction(db);
    }
    WriteTransaction(WriteTransaction&& o) noexcept : m_db(std::exchange(o.m_db, nullptr)) {}
    WriteTransaction& operator=(WriteTransaction&&) = delete;
    ~WriteTransaction()
    {
        if (m_db)
            (void) m_db->exec("ROLLBACK");
    }
    Result<void> commit()
    {
        auto ok = m_db->exec("COMMIT");
        if (ok)
            m_db = nullptr;
        return ok;
    }

private:
    explicit WriteTransaction(Database& db) : m_db(&db) {}
    Database* m_db;
};

// --- Installed items -----------------------------------------------------------

constexpr const char* kInstalledColumns = "offer_id, backend, scope, app_id, version, origin";

Result<InstalledItem> installedFromRow(const Statement& s)
{
    auto offer = OfferId::parse(s.columnText(0));
    const auto backend = parseBackendKind(s.columnText(1));
    const auto scope = parseScope(s.columnText(2));
    if (!offer || !backend || !scope)
        return Error::invalidArgument("stored installed item is malformed");
    InstalledItemSpec spec{.offer = std::move(offer).value(), .backend = *backend, .scope = *scope};
    if (!s.columnIsNull(3)) {
        auto app = AppId::parse(s.columnText(3));
        if (!app)
            return std::move(app).error();
        spec.app = std::move(app).value();
    }
    if (!s.columnIsNull(4)) {
        auto v = *backend == BackendKind::PackageKit ? Version::debian(s.columnText(4))
                                                     : Version::upstream(s.columnText(4));
        if (!v)
            return std::move(v).error();
        spec.version = std::move(v).value();
    }
    spec.origin = std::string(s.columnText(5));
    return InstalledItem::create(std::move(spec));
}

Result<std::vector<InstalledItem>> collectInstalled(Database& db, Statement& stmt)
{
    std::vector<InstalledItem> out;
    int rc;
    while ((rc = stmt.step()) == SQLITE_ROW) {
        auto item = installedFromRow(stmt);
        if (!item)
            return std::move(item).error();
        out.push_back(std::move(item).value());
    }
    if (rc != SQLITE_DONE)
        return db.error("reading installed items");
    return out;
}

Result<std::vector<InstalledItem>> readItems(Database& db, std::optional<BackendKind> backend)
{
    const std::string sql = std::string("SELECT ") + kInstalledColumns
            + " FROM installed WHERE ?1 IS NULL OR backend = ?1 ORDER BY offer_id";
    auto stmt = Statement::prepare(db, sql.c_str());
    if (!stmt)
        return std::move(stmt).error();
    if (backend)
        stmt->bind(1, toString(*backend));
    return collectInstalled(db, *stmt);
}

Result<std::optional<InstalledItem>> readOne(Database& db, const OfferId& offer)
{
    const std::string sql = std::string("SELECT ") + kInstalledColumns + " FROM installed WHERE offer_id = ?1";
    auto stmt = Statement::prepare(db, sql.c_str());
    if (!stmt)
        return std::move(stmt).error();
    stmt->bind(1, offer.str());
    auto items = collectInstalled(db, *stmt);
    if (!items)
        return std::move(items).error();
    if (items->empty())
        return std::optional<InstalledItem>();
    return std::optional<InstalledItem>(std::move(items->front()));
}

Result<std::vector<InstalledItem>> readForApp(Database& db, const AppId& app)
{
    const std::string sql =
            std::string("SELECT ") + kInstalledColumns + " FROM installed WHERE app_id = ?1 ORDER BY offer_id";
    auto stmt = Statement::prepare(db, sql.c_str());
    if (!stmt)
        return std::move(stmt).error();
    stmt->bind(1, app.str());
    return collectInstalled(db, *stmt);
}

Result<void> insertInstalled(Database& db, const InstalledItem& item)
{
    auto stmt = Statement::prepare(db, "INSERT INTO installed (offer_id, backend, scope, app_id, version, origin)"
                                       " VALUES (?1, ?2, ?3, ?4, ?5, ?6)");
    if (!stmt)
        return std::move(stmt).error();
    stmt->bind(1, item.offer().str()).bind(2, toString(item.backend())).bind(3, toString(item.scope()));
    if (item.app())
        stmt->bind(4, item.app()->str());
    if (item.version())
        stmt->bind(5, item.version()->str());
    stmt->bind(6, item.origin());
    return stmt->run();
}

// --- Meta ----------------------------------------------------------------------

Result<void> setMetaInt(Database& db, std::string_view key, std::int64_t value)
{
    auto stmt = Statement::prepare(db, "INSERT OR REPLACE INTO meta (key, value) VALUES (?1, ?2)");
    if (!stmt)
        return std::move(stmt).error();
    const std::string text = std::to_string(value);
    stmt->bind(1, key).bind(2, std::string_view(text));
    return stmt->run();
}

Result<std::optional<std::int64_t>> getMetaInt(Database& db, std::string_view key)
{
    auto stmt = Statement::prepare(db, "SELECT CAST(value AS INTEGER) FROM meta WHERE key = ?1");
    if (!stmt)
        return std::move(stmt).error();
    stmt->bind(1, key);
    const int rc = stmt->step();
    if (rc == SQLITE_ROW)
        return std::optional<std::int64_t>(stmt->columnInt(0));
    if (rc == SQLITE_DONE)
        return std::optional<std::int64_t>();
    return db.error("reading meta");
}

std::string reconciledKey(BackendKind backend, Scope scope)
{
    return "reconciled:" + std::string(toString(backend)) + ":" + std::string(toString(scope));
}

// --- Transactions --------------------------------------------------------------

Result<void> checkRecord(const TransactionRecord& r)
{
    if (auto ok = detail::checkOpaqueIdentifier(r.id, "transaction id"); !ok)
        return ok;
    if (!r.backendRef.empty())
        if (auto ok = detail::checkOpaqueIdentifier(r.backendRef, "transaction backend reference"); !ok)
            return ok;
    if (r.offers.empty())
        return Error::invalidArgument("a transaction acts on at least one offer");
    for (std::size_t i = 0; i < r.offers.size(); ++i)
        for (std::size_t j = i + 1; j < r.offers.size(); ++j)
            if (r.offers[i].offer == r.offers[j].offer)
                return Error::invalidArgument("duplicate offer in transaction " + r.id);
    if (r.backend == BackendKind::PackageKit && r.scope != Scope::System)
        return Error::invalidArgument("a PackageKit transaction is always system-wide");
    if (r.finished.has_value() != isTerminal(r.snapshot.state))
        return Error::invalidArgument("a transaction has a finish date exactly when it is finished");
    if (r.created < 0 || r.updated < r.created || (r.finished && *r.finished < r.created))
        return Error::invalidArgument("inconsistent transaction dates");
    for (const auto* p : {&r.snapshot.plan, &r.snapshot.acceptedPlan})
        if (*p && ((*p)->backend() != r.backend || (*p)->scope() != r.scope))
            return Error::invalidArgument("a plan does not match the transaction backend or scope");
    // Same rules as a restart: a record that could not be restored is not
    // written.
    if (auto t = Transaction::restore(r.snapshot); !t)
        return std::move(t).error();
    return {};
}

Result<void> writeRecord(Database& db, const TransactionRecord& r)
{
    auto del = Statement::prepare(db, "DELETE FROM transactions WHERE id = ?1");
    if (!del)
        return std::move(del).error();
    del->bind(1, r.id);
    if (auto ok = del->run(); !ok)
        return ok;

    auto ins = Statement::prepare(
            db, "INSERT INTO transactions (id, kind, backend, scope, state, terminal, confirmed_by_user,"
                " plan_changed, current_plan, accepted_plan, error_code, error_failure, error_message, backend_ref,"
                " created, updated, finished)"
                " VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15, ?16, ?17)");
    if (!ins)
        return std::move(ins).error();
    const TransactionSnapshot& s = r.snapshot;
    const std::string current = s.plan ? detail::planToJson(*s.plan) : std::string();
    const std::string accepted = s.acceptedPlan ? detail::planToJson(*s.acceptedPlan) : std::string();
    ins->bind(1, r.id)
            .bind(2, toString(s.kind))
            .bind(3, toString(r.backend))
            .bind(4, toString(r.scope))
            .bind(5, toString(s.state))
            .bind(6, std::int64_t(isTerminal(s.state)))
            .bind(7, std::int64_t(s.confirmedByUser))
            .bind(8, std::int64_t(s.planChanged));
    if (s.plan)
        ins->bind(9, std::string_view(current));
    if (s.acceptedPlan)
        ins->bind(10, std::string_view(accepted));
    if (s.error) {
        ins->bind(11, toString(s.error->code()))
                .bind(12, toString(s.error->backendFailure()))
                .bind(13, std::string_view(s.error->message()));
    }
    ins->bind(14, std::string_view(r.backendRef)).bind(15, r.created).bind(16, r.updated);
    if (r.finished)
        ins->bind(17, *r.finished);
    if (auto ok = ins->run(); !ok)
        return ok;

    auto off = Statement::prepare(
            db, "INSERT INTO transaction_offers (transaction_id, position, offer_id, app_id) VALUES (?1, ?2, ?3, ?4)");
    if (!off)
        return std::move(off).error();
    for (std::size_t i = 0; i < r.offers.size(); ++i) {
        off->bind(1, r.id).bind(2, std::int64_t(i)).bind(3, r.offers[i].offer.str());
        if (r.offers[i].app)
            off->bind(4, r.offers[i].app->str());
        if (auto ok = off->run(); !ok)
            return ok;
    }
    return {};
}

Result<std::optional<TransactionRecord>> readRecord(Database& db, std::string_view id)
{
    auto stmt = Statement::prepare(
            db, "SELECT kind, backend, scope, state, confirmed_by_user, plan_changed, current_plan, accepted_plan,"
                " error_code, error_failure, error_message, backend_ref, created, updated, finished"
                " FROM transactions WHERE id = ?1");
    if (!stmt)
        return std::move(stmt).error();
    stmt->bind(1, id);
    const int rc = stmt->step();
    if (rc == SQLITE_DONE)
        return std::optional<TransactionRecord>();
    if (rc != SQLITE_ROW)
        return db.error("reading a transaction");

    const auto kind = parseTransactionKind(stmt->columnText(0));
    const auto backend = parseBackendKind(stmt->columnText(1));
    const auto scope = parseScope(stmt->columnText(2));
    const auto state = parseTransactionState(stmt->columnText(3));
    if (!kind || !backend || !scope || !state)
        return Error::invalidArgument("stored transaction is malformed: " + std::string(id));

    TransactionSnapshot snap{.kind = *kind,
                             .state = *state,
                             .planChanged = stmt->columnInt(5) != 0,
                             .confirmedByUser = stmt->columnInt(4) != 0};
    for (const auto& [column, target] : {std::pair{6, &snap.plan}, std::pair{7, &snap.acceptedPlan}}) {
        if (stmt->columnIsNull(column))
            continue;
        auto plan = detail::planFromJson(db, std::string(stmt->columnText(column)));
        if (!plan)
            return std::move(plan).error();
        *target = std::move(plan).value();
    }
    if (!stmt->columnIsNull(8)) {
        const auto code = parseErrorCode(stmt->columnText(8));
        const auto failure = parseBackendFailure(stmt->columnText(9));
        if (!code || !failure)
            return Error::invalidArgument("stored transaction error is malformed: " + std::string(id));
        std::string message(stmt->columnText(10));
        snap.error = *code == ErrorCode::Backend ? Error(*failure, std::move(message)) : Error(*code, std::move(message));
    }

    TransactionRecord record{.id = std::string(id),
                             .backend = *backend,
                             .scope = *scope,
                             .offers = {},
                             .snapshot = std::move(snap),
                             .backendRef = std::string(stmt->columnText(11)),
                             .created = stmt->columnInt(12),
                             .updated = stmt->columnInt(13)};
    if (!stmt->columnIsNull(14))
        record.finished = stmt->columnInt(14);

    auto offers = Statement::prepare(
            db, "SELECT offer_id, app_id FROM transaction_offers WHERE transaction_id = ?1 ORDER BY position");
    if (!offers)
        return std::move(offers).error();
    offers->bind(1, id);
    int orc;
    while ((orc = offers->step()) == SQLITE_ROW) {
        auto offer = OfferId::parse(offers->columnText(0));
        if (!offer)
            return std::move(offer).error();
        TransactionOffer entry{.offer = std::move(offer).value()};
        if (!offers->columnIsNull(1)) {
            auto app = AppId::parse(offers->columnText(1));
            if (!app)
                return std::move(app).error();
            entry.app = std::move(app).value();
        }
        record.offers.push_back(std::move(entry));
    }
    if (orc != SQLITE_DONE)
        return db.error("reading transaction offers");

    if (auto ok = checkRecord(record); !ok)
        return Error::invalidArgument("stored transaction " + std::string(id) + " is inconsistent: "
                                      + ok.error().message());
    return std::optional<TransactionRecord>(std::move(record));
}

Result<std::vector<TransactionRecord>> readRecords(Database& db, const char* idQuery, std::int64_t limit)
{
    auto stmt = Statement::prepare(db, idQuery);
    if (!stmt)
        return std::move(stmt).error();
    stmt->bind(1, limit);
    std::vector<std::string> ids;
    int rc;
    while ((rc = stmt->step()) == SQLITE_ROW)
        ids.emplace_back(stmt->columnText(0));
    if (rc != SQLITE_DONE)
        return db.error("listing transactions");
    std::vector<TransactionRecord> out;
    out.reserve(ids.size());
    for (const std::string& id : ids) {
        auto r = readRecord(db, id);
        if (!r)
            return std::move(r).error();
        if (*r)
            out.push_back(std::move(**r));
    }
    return out;
}

Result<std::size_t> purge(Database& db, UnixSeconds now)
{
    auto stmt = Statement::prepare(
            db, "DELETE FROM transactions WHERE terminal = 1 AND (finished < ?1 OR id NOT IN"
                " (SELECT id FROM transactions WHERE terminal = 1 ORDER BY finished DESC, id DESC LIMIT ?2))");
    if (!stmt)
        return std::move(stmt).error();
    stmt->bind(1, now - kMaxTransactionAge).bind(2, std::int64_t(kMaxFinishedTransactions));
    if (auto ok = stmt->run(); !ok)
        return std::move(ok).error();
    return static_cast<std::size_t>(sqlite3_changes(db.handle()));
}

// --- Opening -------------------------------------------------------------------

Result<void> configure(Database& db, bool writer)
{
    sqlite3_busy_timeout(db.handle(), kBusyTimeoutMs);
    return db.exec(writer ? "PRAGMA foreign_keys = ON; PRAGMA synchronous = FULL"
                          : "PRAGMA foreign_keys = ON; PRAGMA query_only = ON");
}

Result<void> migrate(Database& db, std::int64_t from, UnixSeconds now)
{
    auto tx = WriteTransaction::begin(db);
    if (!tx)
        return std::move(tx).error();
    // Read again under the write lock: another process may have created the
    // schema meanwhile.
    auto version = db.queryInt("PRAGMA user_version");
    if (!version)
        return std::move(version).error();
    if (*version != from)
        return tx->commit();
    if (from == 0) {
        if (auto ok = db.exec(detail::kStateSchemaV1); !ok)
            return ok;
        if (auto ok = setMetaInt(db, "created", now); !ok)
            return ok;
    }
    // Future versions: one step per version, in order.
    if (auto ok = db.exec(("PRAGMA user_version = " + std::to_string(detail::kStateSchemaVersion)).c_str()); !ok)
        return ok;
    return tx->commit();
}

} // namespace

RecoveryAction recoveryAction(const TransactionRecord& record) noexcept
{
    switch (record.snapshot.state) {
    case TransactionState::Queued:
    case TransactionState::Planning:
        return RecoveryAction::Interrupt;
    case TransactionState::AwaitingConfirmation:
        return RecoveryAction::Cancel;
    case TransactionState::Running:
        return record.backend == BackendKind::PackageKit && !record.backendRef.empty() ? RecoveryAction::TryReattach
                                                                                        : RecoveryAction::Interrupt;
    case TransactionState::Succeeded:
    case TransactionState::Failed:
    case TransactionState::Cancelled:
    case TransactionState::Interrupted:
        return RecoveryAction::None;
    }
    return RecoveryAction::None;
}

// --- StateStore ------------------------------------------------------------------

struct StateStore::Impl
{
    Database db;
    std::function<bool()> mayCommit;
};

StateStore::StateStore(std::unique_ptr<Impl> impl) : d(std::move(impl)) {}
StateStore::StateStore(StateStore&&) noexcept = default;
StateStore& StateStore::operator=(StateStore&&) noexcept = default;
StateStore::~StateStore() = default;

Result<StateStore> StateStore::open(const std::filesystem::path& path, UnixSeconds now)
{
    auto db = Database::open(detail::fileUri(path), SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
    if (!db)
        return std::move(db).error();
    auto impl = std::make_unique<Impl>();
    impl->db = std::move(db).value();
    Database& h = impl->db;
    // Before the first statement: another connection of the process (the
    // inventory writer, a read thread) may hold a lock on the file; without
    // a busy timeout the checks and the switch to WAL below would fail at
    // once with SQLITE_BUSY instead of waiting for it.
    sqlite3_busy_timeout(h.handle(), kBusyTimeoutMs);

    // Nothing below writes before the file is known to be ours.
    auto version = h.queryInt("PRAGMA user_version");
    if (!version)
        return Error::invalidArgument("not a state database: " + version.error().message());
    if (*version > detail::kStateSchemaVersion)
        return Error::notSupported("state database schema version " + std::to_string(*version) + " is newer than "
                                   + std::to_string(detail::kStateSchemaVersion));
    if (*version == 0) {
        auto objects = h.queryInt("SELECT count(*) FROM sqlite_schema");
        if (!objects)
            return Error::invalidArgument("not a state database: " + objects.error().message());
        if (*objects != 0)
            return Error::invalidArgument("not a state database: " + path.string());
    } else if (auto ok = quickCheck(h); !ok) {
        return std::move(ok).error();
    }

    auto mode = queryText(h, "PRAGMA journal_mode = WAL");
    if (!mode)
        return std::move(mode).error();
    if (*mode != "wal")
        return Error::internal("cannot use the WAL journal for " + path.string());
    if (auto ok = configure(h, true); !ok)
        return std::move(ok).error();
    if (*version < detail::kStateSchemaVersion)
        if (auto ok = migrate(h, *version, now); !ok)
            return std::move(ok).error();
    return StateStore(std::move(impl));
}

Result<void> StateStore::replaceInstalled(BackendKind backend, Scope scope, std::span<const InstalledItem> items,
                                          UnixSeconds now)
{
    const Scope one[] = {scope};
    return replaceInstalled(backend, std::span<const Scope>(one), items, now);
}

Result<void> StateStore::replaceInstalled(BackendKind backend, std::span<const Scope> scopes,
                                          std::span<const InstalledItem> items, UnixSeconds now)
{
    std::vector<std::string_view> ids;
    ids.reserve(items.size());
    for (const InstalledItem& item : items) {
        if (item.backend() != backend || std::find(scopes.begin(), scopes.end(), item.scope()) == scopes.end())
            return Error::invalidArgument("installed item " + item.offer().str() + " belongs to another backend or "
                                          "installation");
        ids.push_back(item.offer().str());
    }
    std::sort(ids.begin(), ids.end());
    if (auto dup = std::adjacent_find(ids.begin(), ids.end()); dup != ids.end())
        return Error::invalidArgument("duplicate installed item: " + std::string(*dup));

    auto tx = WriteTransaction::begin(d->db);
    if (!tx)
        return std::move(tx).error();
    auto del = Statement::prepare(d->db, "DELETE FROM installed WHERE backend = ?1 AND scope = ?2");
    if (!del)
        return std::move(del).error();
    for (const Scope scope : scopes) {
        del->bind(1, toString(backend)).bind(2, toString(scope));
        if (auto ok = del->run(); !ok)
            return ok;
    }
    for (const InstalledItem& item : items)
        if (auto ok = insertInstalled(d->db, item); !ok)
            return ok;
    for (const Scope scope : scopes)
        if (auto ok = setMetaInt(d->db, reconciledKey(backend, scope), now); !ok)
            return ok;
    return tx->commit();
}

void StateStore::setBusyTimeout(std::chrono::milliseconds timeout) noexcept
{
    sqlite3_busy_timeout(d->db.handle(), static_cast<int>(timeout.count()));
}

void StateStore::setCommitGuard(std::function<bool()> mayCommit)
{
    d->mayCommit = std::move(mayCommit);
    if (!d->mayCommit) {
        sqlite3_commit_hook(d->db.handle(), nullptr, nullptr);
        return;
    }
    // Impl does not move (owned through a unique_ptr): its address stays
    // valid for the connection's lifetime, which it bounds.
    sqlite3_commit_hook(
            d->db.handle(),
            [](void* impl) -> int { return static_cast<Impl*>(impl)->mayCommit() ? 0 : 1; },
            d.get());
}

Result<void> StateStore::setDurableCommits(bool durable)
{
    return d->db.exec(durable ? "PRAGMA synchronous = FULL" : "PRAGMA synchronous = NORMAL");
}

Result<std::optional<UnixSeconds>> StateStore::lastReconciled(BackendKind backend, Scope scope) const
{
    return getMetaInt(d->db, reconciledKey(backend, scope));
}

Result<std::vector<InstalledItem>> StateStore::items(std::optional<BackendKind> backend) const
{
    return readItems(d->db, backend);
}

Result<std::optional<InstalledItem>> StateStore::find(const OfferId& offer) const
{
    return readOne(d->db, offer);
}

Result<std::vector<InstalledItem>> StateStore::forApplication(const AppId& app) const
{
    return readForApp(d->db, app);
}

Result<void> StateStore::recordRefresh(std::span<const SourceId> sources, UnixSeconds now)
{
    auto tx = WriteTransaction::begin(d->db);
    if (!tx)
        return std::move(tx).error();
    auto stmt = Statement::prepare(d->db, "INSERT OR REPLACE INTO source_state (source_id, last_refresh) VALUES (?1, ?2)");
    if (!stmt)
        return std::move(stmt).error();
    for (const SourceId& source : sources) {
        stmt->bind(1, source.str()).bind(2, now);
        if (auto ok = stmt->run(); !ok)
            return ok;
    }
    if (auto ok = setMetaInt(d->db, "last_refresh", now); !ok)
        return ok;
    return tx->commit();
}

Result<std::optional<UnixSeconds>> StateStore::lastRefresh() const
{
    return getMetaInt(d->db, "last_refresh");
}

Result<std::optional<UnixSeconds>> StateStore::lastRefresh(const SourceId& source) const
{
    auto stmt = Statement::prepare(d->db, "SELECT last_refresh FROM source_state WHERE source_id = ?1");
    if (!stmt)
        return std::move(stmt).error();
    stmt->bind(1, source.str());
    const int rc = stmt->step();
    if (rc == SQLITE_ROW)
        return std::optional<UnixSeconds>(stmt->columnInt(0));
    if (rc == SQLITE_DONE)
        return std::optional<UnixSeconds>();
    return d->db.error("reading a refresh date");
}

Result<void> StateStore::saveTransaction(const TransactionRecord& record)
{
    if (auto ok = checkRecord(record); !ok)
        return ok;
    auto tx = WriteTransaction::begin(d->db);
    if (!tx)
        return std::move(tx).error();
    if (auto ok = writeRecord(d->db, record); !ok)
        return ok;
    if (record.finished)
        if (auto purged = purge(d->db, *record.finished); !purged)
            return std::move(purged).error();
    return tx->commit();
}

Result<void> StateStore::saveTransactions(std::span<const TransactionRecord> records)
{
    std::set<std::string_view> ids;
    for (const TransactionRecord& r : records) {
        if (auto ok = checkRecord(r); !ok)
            return ok;
        if (!ids.insert(r.id).second)
            return Error::invalidArgument("transaction " + r.id + " is given twice");
    }
    if (records.empty())
        return {};
    auto tx = WriteTransaction::begin(d->db);
    if (!tx)
        return std::move(tx).error();
    std::optional<UnixSeconds> latest;
    for (const TransactionRecord& r : records) {
        if (auto ok = writeRecord(d->db, r); !ok)
            return ok;
        if (r.finished && (!latest || *r.finished > *latest))
            latest = r.finished;
    }
    if (latest)
        if (auto purged = purge(d->db, *latest); !purged)
            return std::move(purged).error();
    return tx->commit();
}

Result<std::optional<TransactionRecord>> StateStore::transaction(std::string_view id) const
{
    return readRecord(d->db, id);
}

Result<std::vector<TransactionRecord>> StateStore::openTransactions() const
{
    return readRecords(d->db, "SELECT id FROM transactions WHERE terminal = 0 ORDER BY created, id LIMIT ?1", -1);
}

Result<std::vector<TransactionRecord>> StateStore::history(std::size_t limit) const
{
    return readRecords(d->db, "SELECT id FROM transactions WHERE terminal = 1 ORDER BY finished DESC, id DESC LIMIT ?1",
                       static_cast<std::int64_t>(std::min<std::size_t>(limit, kMaxFinishedTransactions)));
}

Result<std::optional<std::string>> StateStore::openTransactionFor(const OfferId& offer) const
{
    auto stmt = Statement::prepare(d->db, "SELECT t.id FROM transaction_offers o JOIN transactions t"
                                          " ON t.id = o.transaction_id WHERE o.offer_id = ?1 AND t.terminal = 0"
                                          " ORDER BY t.created, t.id LIMIT 1");
    if (!stmt)
        return std::move(stmt).error();
    stmt->bind(1, offer.str());
    const int rc = stmt->step();
    if (rc == SQLITE_ROW)
        return std::optional<std::string>(std::string(stmt->columnText(0)));
    if (rc == SQLITE_DONE)
        return std::optional<std::string>();
    return d->db.error("looking for an open transaction");
}

Result<std::size_t> StateStore::purgeHistory(UnixSeconds now)
{
    auto tx = WriteTransaction::begin(d->db);
    if (!tx)
        return std::move(tx).error();
    auto removed = purge(d->db, now);
    if (!removed)
        return removed;
    if (auto ok = tx->commit(); !ok)
        return std::move(ok).error();
    return removed;
}

Result<RecoveryReport> StateStore::recoverAfterRestart(UnixSeconds now)
{
    auto tx = WriteTransaction::begin(d->db);
    if (!tx)
        return std::move(tx).error();
    auto open = openTransactions();
    if (!open)
        return std::move(open).error();

    RecoveryReport report;
    for (TransactionRecord& record : *open) {
        const RecoveryAction action = recoveryAction(record);
        if (action == RecoveryAction::TryReattach) {
            report.toReattach.push_back(std::move(record));
            continue;
        }
        if (action == RecoveryAction::None)
            continue;
        auto t = Transaction::restore(record.snapshot);
        if (!t)
            return std::move(t).error();
        const bool wasRunning = record.snapshot.state == TransactionState::Running;
        auto ok = action == RecoveryAction::Cancel ? t->cancel() : t->interrupt();
        if (!ok)
            return std::move(ok).error();
        record.snapshot = t->snapshot();
        record.updated = std::max(now, record.created);
        record.finished = record.updated;
        if (auto w = writeRecord(d->db, record); !w)
            return std::move(w).error();
        if (action == RecoveryAction::Cancel) {
            report.cancelled.push_back(record.id);
        } else {
            report.interrupted.push_back(record.id);
            report.reconcile = report.reconcile || wasRunning;
        }
    }
    if (auto purged = purge(d->db, now); !purged)
        return std::move(purged).error();
    if (auto ok = tx->commit(); !ok)
        return std::move(ok).error();
    return report;
}

// --- SqliteInstalledState --------------------------------------------------------

struct SqliteInstalledState::Impl
{
    Database db;
};

SqliteInstalledState::SqliteInstalledState(std::unique_ptr<Impl> impl) : d(std::move(impl)) {}
SqliteInstalledState::SqliteInstalledState(SqliteInstalledState&&) noexcept = default;
SqliteInstalledState& SqliteInstalledState::operator=(SqliteInstalledState&&) noexcept = default;
SqliteInstalledState::~SqliteInstalledState() = default;

Result<SqliteInstalledState> SqliteInstalledState::open(const std::filesystem::path& path)
{
    // Read-write without CREATE, then query_only: a WAL reader may need to
    // create the shared-memory file, which a read-only connection cannot.
    auto db = Database::open(detail::fileUri(path), SQLITE_OPEN_READWRITE);
    if (!db)
        return std::move(db).error();
    auto impl = std::make_unique<Impl>();
    impl->db = std::move(db).value();
    if (auto ok = configure(impl->db, false); !ok)
        return std::move(ok).error();
    auto version = impl->db.queryInt("PRAGMA user_version");
    if (!version)
        return Error::invalidArgument("not a state database: " + version.error().message());
    if (*version != detail::kStateSchemaVersion)
        return Error::notSupported("state database schema version " + std::to_string(*version) + ", expected "
                                   + std::to_string(detail::kStateSchemaVersion));
    return SqliteInstalledState(std::move(impl));
}

Result<std::vector<InstalledItem>> SqliteInstalledState::items(std::optional<BackendKind> backend) const
{
    return readItems(d->db, backend);
}

Result<std::optional<InstalledItem>> SqliteInstalledState::find(const OfferId& offer) const
{
    return readOne(d->db, offer);
}

Result<std::vector<InstalledItem>> SqliteInstalledState::forApplication(const AppId& app) const
{
    return readForApp(d->db, app);
}

} // namespace cutefish::store
