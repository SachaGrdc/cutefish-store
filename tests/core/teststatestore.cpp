#include "catalogfixture.h"
#include "planfixtures.h"

#include <cutefish/store/statestore.h>

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <fstream>
#include <iterator>
#include <thread>

#include <sys/wait.h>
#include <unistd.h>

using namespace cutefish::store;
using namespace planfixtures;
using catalogfixture::TempDir;
using fixtures::appId;
using fixtures::unwrap;

namespace {

constexpr UnixSeconds kDay = 24 * 60 * 60;
constexpr UnixSeconds kT0 = 1'800'000'000;

std::string contents(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

/// Direct SQLite access, to inspect or damage the file as another program
/// would.
void rawExec(const std::filesystem::path& path, const char* sql)
{
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open(path.c_str(), &db), SQLITE_OK);
    char* message = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &message);
    const std::string text = message ? message : "";
    sqlite3_free(message);
    sqlite3_close(db);
    ASSERT_EQ(rc, SQLITE_OK) << text;
}

std::string rawQuery(const std::filesystem::path& path, const char* sql)
{
    sqlite3* db = nullptr;
    EXPECT_EQ(sqlite3_open(path.c_str(), &db), SQLITE_OK);
    sqlite3_stmt* stmt = nullptr;
    std::string out;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW)
        out = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return out;
}

InstalledItem flatpakItem(const std::string& name, Scope scope = Scope::User, const std::string& version = "1.0")
{
    const std::string offer = "flatpak:" + std::string(toString(scope)) + ":flathub:app/org.example." + name
            + "/x86_64/stable";
    return unwrap(InstalledItem::create({.offer = unwrap(OfferId::parse(offer)),
                                         .app = appId(("org.example." + name).c_str()),
                                         .backend = BackendKind::Flatpak,
                                         .scope = scope,
                                         .version = unwrap(Version::upstream(version)),
                                         .origin = "flathub"}));
}

InstalledItem debItem(const std::string& package, const char* app = nullptr)
{
    InstalledItemSpec spec{.offer = unwrap(OfferId::parse("packagekit:" + package)),
                           .backend = BackendKind::PackageKit,
                           .scope = Scope::System,
                           .version = unwrap(Version::debian("1.0-1")),
                           .origin = "debian-stable-main"};
    if (app)
        spec.app = appId(app);
    return unwrap(InstalledItem::create(std::move(spec)));
}

/// A finished, queued or awaiting record for tests.
TransactionRecord record(const std::string& id, TransactionState state, UnixSeconds created,
                         std::optional<UnixSeconds> finished = std::nullopt)
{
    TransactionRecord r{.id = id,
                        .backend = BackendKind::Flatpak,
                        .scope = Scope::User,
                        .offers = {{unwrap(OfferId::parse("flatpak:user:flathub:" + std::string(kApp))),
                                    appId("org.example.App")}},
                        .snapshot = {.kind = TransactionKind::Install, .state = state},
                        .created = created,
                        .updated = finished.value_or(created),
                        .finished = finished};
    if (state == TransactionState::Failed)
        r.snapshot.error = Error::backend(BackendFailure::Network, "timeout");
    if (state == TransactionState::AwaitingConfirmation || state == TransactionState::Running) {
        auto app = flatpakApp(PlanOperation::Install);
        app.permissions = {"shared=network"};
        r.snapshot.plan = flatpakPlan(TransactionKind::Install, Scope::User, {app});
        if (state == TransactionState::Running)
            r.snapshot.acceptedPlan = r.snapshot.plan;
    }
    return r;
}

TransactionRecord finishedRecord(int i, UnixSeconds finished)
{
    return record("tx-" + std::to_string(1000 + i), TransactionState::Succeeded, finished - 10, finished);
}

void expectSamePlan(const Plan& a, const Plan& b)
{
    EXPECT_EQ(a.kind(), b.kind());
    EXPECT_EQ(a.backend(), b.backend());
    EXPECT_EQ(a.scope(), b.scope());
    EXPECT_EQ(a.installedSizeChange(), b.installedSizeChange());
    ASSERT_EQ(a.items().size(), b.items().size());
    for (std::size_t i = 0; i < a.items().size(); ++i) {
        const PlanItem& x = a.items()[i];
        const PlanItem& y = b.items()[i];
        EXPECT_EQ(x.operation(), y.operation());
        EXPECT_EQ(x.kind(), y.kind());
        EXPECT_EQ(x.requested(), y.requested());
        EXPECT_EQ(x.package(), y.package());
        EXPECT_EQ(x.fromVersion(), y.fromVersion());
        EXPECT_EQ(x.toVersion(), y.toVersion());
        EXPECT_EQ(x.origin(), y.origin());
        EXPECT_EQ(x.revision(), y.revision());
        EXPECT_EQ(x.downloadSize(), y.downloadSize());
        EXPECT_TRUE(std::equal(x.permissions().begin(), x.permissions().end(), y.permissions().begin(),
                               y.permissions().end()));
        EXPECT_TRUE(std::equal(x.oldPermissions().begin(), x.oldPermissions().end(), y.oldPermissions().begin(),
                               y.oldPermissions().end()));
        EXPECT_EQ(x.addsSource(), y.addsSource());
        EXPECT_EQ(x.rebaseFrom(), y.rebaseFrom());
        EXPECT_EQ(x.trusted(), y.trusted());
    }
}

class StateStoreTest : public ::testing::Test
{
protected:
    StateStore open(UnixSeconds now = kT0) { return unwrap(StateStore::open(path, now)); }

    TempDir dir;
    const std::filesystem::path path = dir / "state.sqlite";
};

} // namespace

// --- Opening and schema --------------------------------------------------------

TEST_F(StateStoreTest, CreatesTheSchemaInWalMode)
{
    {
        auto store = open();
        const std::vector<InstalledItem> debs{debItem("gimp")};
        ASSERT_TRUE(store.replaceInstalled(BackendKind::PackageKit, Scope::System, debs, kT0));
    }
    EXPECT_EQ(rawQuery(path, "PRAGMA user_version"), "1");
    EXPECT_EQ(rawQuery(path, "PRAGMA journal_mode"), "wal");
    EXPECT_EQ(rawQuery(path, "SELECT value FROM meta WHERE key = 'created'"), std::to_string(kT0));

    auto again = open(kT0 + 100);
    auto items = again.items(std::nullopt);
    ASSERT_TRUE(items);
    ASSERT_EQ(items->size(), 1u);
    // The creation date is not rewritten on reopening.
    EXPECT_EQ(rawQuery(path, "SELECT value FROM meta WHERE key = 'created'"), std::to_string(kT0));
}

TEST_F(StateStoreTest, RefusesANewerSchemaWithoutTouchingIt)
{
    {
        auto store = open();
    }
    rawExec(path, "PRAGMA user_version = 99");
    const std::string before = contents(path);
    auto r = StateStore::open(path, kT0);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code(), ErrorCode::NotSupported);
    EXPECT_EQ(contents(path), before);
    auto reader = SqliteInstalledState::open(path);
    ASSERT_FALSE(reader);
    EXPECT_EQ(reader.error().code(), ErrorCode::NotSupported);
}

TEST_F(StateStoreTest, RefusesFilesThatAreNotStateDatabases)
{
    rawExec(path, "CREATE TABLE unrelated (x)");
    const std::string before = contents(path);
    auto r = StateStore::open(path, kT0);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(contents(path), before);

    const auto text = dir / "notes.txt";
    std::ofstream(text) << "not a database, just some text that is long enough to have a header";
    const std::string textBefore = contents(text);
    auto t = StateStore::open(text, kT0);
    ASSERT_FALSE(t);
    EXPECT_EQ(t.error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(contents(text), textBefore);
}

TEST_F(StateStoreTest, RefusesADamagedFile)
{
    {
        auto store = open();
        std::vector<InstalledItem> many;
        for (int i = 0; i < 300; ++i)
            many.push_back(flatpakItem("App" + std::to_string(i)));
        ASSERT_TRUE(store.replaceInstalled(BackendKind::Flatpak, Scope::User, many, kT0));
    }
    rawExec(path, "PRAGMA wal_checkpoint(TRUNCATE)");
    // Overwrite the middle of the file (b-tree pages, not the header).
    {
        std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
        f.seekp(static_cast<std::streamoff>(std::filesystem::file_size(path) / 2));
        const std::string garbage(4096, '\x5a');
        f.write(garbage.data(), static_cast<std::streamsize>(garbage.size()));
    }
    auto r = StateStore::open(path, kT0);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code(), ErrorCode::InvalidArgument) << r.error().message();
}

// --- Installed state (a cache) ---------------------------------------------------

TEST_F(StateStoreTest, InventoriesAreReplacedPerBackendAndInstallation)
{
    auto store = open();
    const std::vector<InstalledItem> user{flatpakItem("A"), flatpakItem("B")};
    const std::vector<InstalledItem> system{flatpakItem("A", Scope::System)};
    const std::vector<InstalledItem> debs{debItem("gimp", "org.gimp.GIMP"), debItem("libfoo1")};
    ASSERT_TRUE(store.replaceInstalled(BackendKind::Flatpak, Scope::User, user, kT0));
    ASSERT_TRUE(store.replaceInstalled(BackendKind::Flatpak, Scope::System, system, kT0 + 1));
    ASSERT_TRUE(store.replaceInstalled(BackendKind::PackageKit, Scope::System, debs, kT0 + 2));
    EXPECT_EQ(store.items(std::nullopt)->size(), 5u);
    EXPECT_EQ(store.items(BackendKind::Flatpak)->size(), 3u);
    EXPECT_EQ(store.forApplication(appId("org.example.A"))->size(), 2u);

    // A new user inventory leaves the system one and the debs alone.
    const std::vector<InstalledItem> fewer{flatpakItem("B", Scope::User, "2.0")};
    ASSERT_TRUE(store.replaceInstalled(BackendKind::Flatpak, Scope::User, fewer, kT0 + 10));
    EXPECT_EQ(store.items(std::nullopt)->size(), 4u);
    EXPECT_FALSE(store.find(flatpakItem("A").offer())->has_value());
    auto b = store.find(flatpakItem("B").offer());
    ASSERT_TRUE(b && b->has_value());
    EXPECT_EQ((*b)->version()->str(), "2.0");
    EXPECT_TRUE(store.find(flatpakItem("A", Scope::System).offer())->has_value());
    auto gimp = store.find(debItem("gimp").offer());
    ASSERT_TRUE(gimp && gimp->has_value());
    EXPECT_EQ((*gimp)->app()->str(), "org.gimp.GIMP");
    EXPECT_FALSE((*store.find(debItem("libfoo1").offer()))->app());

    EXPECT_EQ(*store.lastReconciled(BackendKind::Flatpak, Scope::User), std::optional<UnixSeconds>(kT0 + 10));
    EXPECT_EQ(*store.lastReconciled(BackendKind::Flatpak, Scope::System), std::optional<UnixSeconds>(kT0 + 1));
    EXPECT_EQ(*store.lastReconciled(BackendKind::PackageKit, Scope::User), std::optional<UnixSeconds>());

    // An empty inventory is an inventory: nothing installed any more.
    ASSERT_TRUE(store.replaceInstalled(BackendKind::Flatpak, Scope::System, {}, kT0 + 20));
    EXPECT_FALSE(store.find(flatpakItem("A", Scope::System).offer())->has_value());
}

TEST_F(StateStoreTest, InvalidInventoriesChangeNothing)
{
    auto store = open();
    const std::vector<InstalledItem> user{flatpakItem("A")};
    ASSERT_TRUE(store.replaceInstalled(BackendKind::Flatpak, Scope::User, user, kT0));

    const std::vector<InstalledItem> wrongScope{flatpakItem("B", Scope::System)};
    auto r = store.replaceInstalled(BackendKind::Flatpak, Scope::User, wrongScope, kT0 + 1);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code(), ErrorCode::InvalidArgument);

    const std::vector<InstalledItem> twice{flatpakItem("C"), flatpakItem("C")};
    EXPECT_FALSE(store.replaceInstalled(BackendKind::Flatpak, Scope::User, twice, kT0 + 1));

    ASSERT_EQ(store.items(std::nullopt)->size(), 1u);
    EXPECT_TRUE(store.find(flatpakItem("A").offer())->has_value());
    EXPECT_EQ(*store.lastReconciled(BackendKind::Flatpak, Scope::User), std::optional<UnixSeconds>(kT0));
}

TEST_F(StateStoreTest, ReadersSeeCommittedChanges)
{
    auto store = open();
    auto reader = unwrap(SqliteInstalledState::open(path));
    EXPECT_TRUE(reader.items(std::nullopt)->empty());
    const std::vector<InstalledItem> user{flatpakItem("A"), flatpakItem("B")};
    ASSERT_TRUE(store.replaceInstalled(BackendKind::Flatpak, Scope::User, user, kT0));
    EXPECT_EQ(reader.items(BackendKind::Flatpak)->size(), 2u);
    EXPECT_TRUE(reader.find(flatpakItem("B").offer())->has_value());
    EXPECT_EQ(reader.forApplication(appId("org.example.A"))->size(), 1u);
    EXPECT_TRUE(reader.items(BackendKind::PackageKit)->empty());
}

TEST_F(StateStoreTest, ReaderNeverWrites)
{
    {
        auto store = open();
    }
    auto reader = unwrap(SqliteInstalledState::open(dir / "state.sqlite"));
    EXPECT_FALSE(SqliteInstalledState::open(dir / "missing.sqlite"));
    EXPECT_FALSE(std::filesystem::exists(dir / "missing.sqlite"));
}

// --- Refresh dates (D-015) ---------------------------------------------------------

TEST_F(StateStoreTest, RefreshDates)
{
    auto store = open();
    const SourceId debian = unwrap(SourceId::parse("packagekit:debian-stable-main"));
    const SourceId flathub = unwrap(SourceId::parse("flatpak:user:flathub"));
    EXPECT_EQ(*store.lastRefresh(), std::optional<UnixSeconds>());
    EXPECT_EQ(*store.lastRefresh(debian), std::optional<UnixSeconds>());

    const std::vector<SourceId> both{debian, flathub};
    ASSERT_TRUE(store.recordRefresh(both, kT0));
    const std::vector<SourceId> one{flathub};
    ASSERT_TRUE(store.recordRefresh(one, kT0 + kDay));
    EXPECT_EQ(*store.lastRefresh(), std::optional<UnixSeconds>(kT0 + kDay));
    EXPECT_EQ(*store.lastRefresh(debian), std::optional<UnixSeconds>(kT0));
    EXPECT_EQ(*store.lastRefresh(flathub), std::optional<UnixSeconds>(kT0 + kDay));

    auto again = open(kT0 + 2 * kDay);
    EXPECT_EQ(*again.lastRefresh(), std::optional<UnixSeconds>(kT0 + kDay));
}

// --- Transaction journal ----------------------------------------------------------

TEST_F(StateStoreTest, TransactionsRoundTrip)
{
    auto store = open();
    // An update that installs a system runtime, awaiting a new confirmation
    // after the plan changed (D-014, D-017), with every plan field set.
    auto app = flatpakApp(PlanOperation::Update);
    app.permissions = {"shared=network", "filesystems=xdg-documents/My \"Folder\"", "devices=dri ✓"};
    app.oldPermissions = {"shared=network"};
    app.rebaseFrom = "app/org.example.Old/x86_64/stable";
    app.addsSource = true;
    auto runtime = flatpakRuntime(PlanOperation::Install);
    runtime.trusted = false;
    Plan accepted = flatpakPlan(TransactionKind::Update, Scope::System, {flatpakApp(PlanOperation::Update)});
    Plan current = unwrap(Plan::create({.kind = TransactionKind::Update,
                                        .backend = BackendKind::Flatpak,
                                        .scope = Scope::System,
                                        .items = {app, runtime},
                                        .installedSizeChange = -12345}));
    Transaction t(TransactionKind::Update);
    ASSERT_TRUE(t.startPlanning());
    ASSERT_TRUE(t.planReady(accepted));
    ASSERT_TRUE(t.verifyExecutionPlan(current));
    ASSERT_EQ(t.state(), TransactionState::AwaitingConfirmation);

    const TransactionRecord in{.id = "6f1c2a9e-0000-4000-8000-000000000001",
                               .backend = BackendKind::Flatpak,
                               .scope = Scope::System,
                               .offers = {{unwrap(OfferId::parse("flatpak:system:flathub:" + std::string(kApp))),
                                           appId("org.example.App")},
                                          {unwrap(OfferId::parse("flatpak:system:flathub:app/org.x.Y/x86_64/stable"))}},
                               .snapshot = t.snapshot(),
                               .created = kT0,
                               .updated = kT0 + 5};
    ASSERT_TRUE(store.saveTransaction(in));

    auto out = store.transaction(in.id);
    ASSERT_TRUE(out) << out.error().message();
    ASSERT_TRUE(out->has_value());
    const TransactionRecord& r = **out;
    EXPECT_EQ(r.backend, BackendKind::Flatpak);
    EXPECT_EQ(r.scope, Scope::System);
    EXPECT_EQ(r.offers, in.offers);
    EXPECT_EQ(r.created, kT0);
    EXPECT_EQ(r.updated, kT0 + 5);
    EXPECT_FALSE(r.finished);
    EXPECT_EQ(r.snapshot.state, TransactionState::AwaitingConfirmation);
    EXPECT_TRUE(r.snapshot.planChanged);
    ASSERT_TRUE(r.snapshot.plan && r.snapshot.acceptedPlan);
    expectSamePlan(*r.snapshot.plan, current);
    expectSamePlan(*r.snapshot.acceptedPlan, accepted);

    // Restored, it asks the same confirmation, pointing at the runtime.
    auto restored = unwrap(Transaction::restore(r.snapshot));
    EXPECT_EQ(restored.reasons(), t.reasons());
    EXPECT_EQ(restored.concerns(), t.concerns());

    EXPECT_FALSE(store.transaction("unknown")->has_value());
}

TEST_F(StateStoreTest, SavingAgainReplacesTheRecord)
{
    auto store = open();
    auto r = record("tx-1", TransactionState::Queued, kT0);
    ASSERT_TRUE(store.saveTransaction(r));
    r.snapshot = record("tx-1", TransactionState::Failed, kT0, kT0 + 9).snapshot;
    r.updated = kT0 + 9;
    r.finished = kT0 + 9;
    r.offers.push_back({unwrap(OfferId::parse("flatpak:user:flathub:app/org.x.Z/x86_64/stable"))});
    ASSERT_TRUE(store.saveTransaction(r));

    auto out = unwrap(store.transaction("tx-1"));
    ASSERT_TRUE(out);
    EXPECT_EQ(out->snapshot.state, TransactionState::Failed);
    ASSERT_TRUE(out->snapshot.error);
    EXPECT_EQ(out->snapshot.error->backendFailure(), BackendFailure::Network);
    EXPECT_EQ(out->snapshot.error->message(), "timeout");
    EXPECT_EQ(out->offers.size(), 2u);
    EXPECT_EQ(rawQuery(path, "SELECT count(*) FROM transaction_offers"), "2");
    EXPECT_TRUE(store.openTransactions()->empty());
}

TEST_F(StateStoreTest, InvalidRecordsAreNotWritten)
{
    auto store = open();
    auto refuse = [&](TransactionRecord r) {
        auto ok = store.saveTransaction(r);
        ASSERT_FALSE(ok);
        EXPECT_EQ(ok.error().code(), ErrorCode::InvalidArgument);
    };
    auto noOffer = record("a", TransactionState::Queued, kT0);
    noOffer.offers.clear();
    refuse(noOffer);
    auto twice = record("b", TransactionState::Queued, kT0);
    twice.offers.push_back(twice.offers.front());
    refuse(twice);
    auto finishedButOpen = record("c", TransactionState::Queued, kT0);
    finishedButOpen.finished = kT0;
    refuse(finishedButOpen);
    auto terminalWithoutDate = record("d", TransactionState::Succeeded, kT0, kT0);
    terminalWithoutDate.finished.reset();
    refuse(terminalWithoutDate);
    auto otherScope = record("e", TransactionState::Running, kT0);
    otherScope.scope = Scope::System;
    refuse(otherScope);
    auto userDeb = record("f", TransactionState::Queued, kT0);
    userDeb.backend = BackendKind::PackageKit;
    refuse(userDeb);
    refuse(record("has space", TransactionState::Queued, kT0));
    auto backwards = record("g", TransactionState::Queued, kT0);
    backwards.updated = kT0 - 1;
    refuse(backwards);
    auto inconsistent = record("h", TransactionState::Running, kT0);
    inconsistent.snapshot.acceptedPlan.reset();
    refuse(inconsistent);
    EXPECT_EQ(rawQuery(path, "SELECT count(*) FROM transactions"), "0");
}

TEST_F(StateStoreTest, DamagedStoredPlanIsReportedNotTrusted)
{
    auto store = open();
    ASSERT_TRUE(store.saveTransaction(record("tx-1", TransactionState::AwaitingConfirmation, kT0)));
    rawExec(path, "UPDATE transactions SET current_plan = '{\"v\": 2}'");
    auto r = store.transaction("tx-1");
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code(), ErrorCode::InvalidArgument);

    rawExec(path, "UPDATE transactions SET current_plan = "
                  "'{\"v\":1,\"kind\":\"install\",\"backend\":\"flatpak\",\"scope\":\"user\","
                  "\"installedSizeChange\":0,\"items\":[]}'");
    // A plan without items is refused by Plan::create.
    EXPECT_FALSE(store.transaction("tx-1"));
}

TEST_F(StateStoreTest, OpenTransactionsAndBusyOffers)
{
    auto store = open();
    ASSERT_TRUE(store.saveTransaction(record("late", TransactionState::Queued, kT0 + 50)));
    ASSERT_TRUE(store.saveTransaction(record("early", TransactionState::Planning, kT0)));
    ASSERT_TRUE(store.saveTransaction(record("done", TransactionState::Succeeded, kT0 - 100, kT0 - 50)));

    auto open = unwrap(store.openTransactions());
    ASSERT_EQ(open.size(), 2u);
    EXPECT_EQ(open[0].id, "early");
    EXPECT_EQ(open[1].id, "late");

    const OfferId offer = unwrap(OfferId::parse("flatpak:user:flathub:" + std::string(kApp)));
    EXPECT_EQ(*store.openTransactionFor(offer), std::optional<std::string>("early"));
    EXPECT_EQ(*store.openTransactionFor(unwrap(OfferId::parse("packagekit:gimp"))), std::optional<std::string>());

    auto history = unwrap(store.history(10));
    ASSERT_EQ(history.size(), 1u);
    EXPECT_EQ(history[0].id, "done");
}

// --- Retention (D-018) -----------------------------------------------------------

TEST_F(StateStoreTest, KeepsAtMost200FinishedTransactions)
{
    auto store = open();
    ASSERT_TRUE(store.saveTransaction(record("open-old", TransactionState::Queued, kT0 - 400 * kDay)));
    for (int i = 0; i < 205; ++i)
        ASSERT_TRUE(store.saveTransaction(finishedRecord(i, kT0 + i)));

    auto history = unwrap(store.history(1000));
    ASSERT_EQ(history.size(), 200u);
    EXPECT_EQ(history.front().id, "tx-1204"); // most recent first
    EXPECT_EQ(history.back().id, "tx-1005");
    EXPECT_FALSE(store.transaction("tx-1004")->has_value());
    // Never an unfinished one, however old.
    EXPECT_TRUE(store.transaction("open-old")->has_value());
    EXPECT_EQ(rawQuery(path, "SELECT count(*) FROM transaction_offers"), "201");
}

TEST_F(StateStoreTest, KeepsFinishedTransactionsAtMost90Days)
{
    auto store = open();
    ASSERT_TRUE(store.saveTransaction(finishedRecord(1, kT0)));
    ASSERT_TRUE(store.saveTransaction(finishedRecord(2, kT0 + kDay)));
    // Saving a transaction finished exactly 90 days after the first keeps it.
    ASSERT_TRUE(store.saveTransaction(finishedRecord(3, kT0 + 90 * kDay)));
    EXPECT_EQ(store.history(10)->size(), 3u);
    // One second later, it is older than 90 days.
    ASSERT_TRUE(store.saveTransaction(finishedRecord(4, kT0 + 90 * kDay + 1)));
    auto history = unwrap(store.history(10));
    ASSERT_EQ(history.size(), 3u);
    EXPECT_FALSE(store.transaction("tx-1001")->has_value());
}

TEST_F(StateStoreTest, TheStricterLimitWins)
{
    auto store = open();
    // 100 transactions older than 90 days, then 150 recent ones: the age
    // limit removes the old ones although fewer than 200 would remain.
    for (int i = 0; i < 100; ++i)
        ASSERT_TRUE(store.saveTransaction(finishedRecord(i, kT0 + i)));
    for (int i = 100; i < 250; ++i)
        ASSERT_TRUE(store.saveTransaction(finishedRecord(i, kT0 + 100 * kDay + i)));
    auto history = unwrap(store.history(1000));
    EXPECT_EQ(history.size(), 150u);
    EXPECT_EQ(history.back().id, "tx-1100");

    // 250 recent ones: the count limit applies.
    TempDir other;
    auto second = unwrap(StateStore::open(other / "state.sqlite", kT0));
    for (int i = 0; i < 250; ++i)
        ASSERT_TRUE(second.saveTransaction(finishedRecord(i, kT0 + i)));
    EXPECT_EQ(second.history(1000)->size(), 200u);
}

TEST_F(StateStoreTest, PurgeHistoryAtStartup)
{
    auto store = open();
    ASSERT_TRUE(store.saveTransaction(finishedRecord(1, kT0)));
    ASSERT_TRUE(store.saveTransaction(finishedRecord(2, kT0 + 10 * kDay)));
    ASSERT_TRUE(store.saveTransaction(record("open", TransactionState::AwaitingConfirmation, kT0 - 500 * kDay)));
    // Months later, nothing was saved meanwhile.
    auto removed = store.purgeHistory(kT0 + 95 * kDay);
    ASSERT_TRUE(removed);
    EXPECT_EQ(*removed, 1u);
    EXPECT_EQ(store.history(10)->size(), 1u);
    EXPECT_EQ(store.openTransactions()->size(), 1u);
    EXPECT_EQ(*store.purgeHistory(kT0 + 95 * kDay), 0u);
}

// --- Recovery after a restart (SERVICE.md §5.6) ------------------------------------

TEST(Recovery, ActionPerState)
{
    auto r = [](TransactionState s, BackendKind b, std::string ref) {
        TransactionRecord rec{};
        rec.backend = b;
        rec.backendRef = std::move(ref);
        rec.snapshot.state = s;
        return recoveryAction(rec);
    };
    using S = TransactionState;
    EXPECT_EQ(r(S::Queued, BackendKind::Flatpak, ""), RecoveryAction::Interrupt);
    EXPECT_EQ(r(S::Planning, BackendKind::PackageKit, "/1_abc"), RecoveryAction::Interrupt);
    EXPECT_EQ(r(S::AwaitingConfirmation, BackendKind::Flatpak, ""), RecoveryAction::Cancel);
    EXPECT_EQ(r(S::Running, BackendKind::Flatpak, ""), RecoveryAction::Interrupt);
    EXPECT_EQ(r(S::Running, BackendKind::PackageKit, ""), RecoveryAction::Interrupt);
    EXPECT_EQ(r(S::Running, BackendKind::PackageKit, "/1_abc"), RecoveryAction::TryReattach);
    for (S s : {S::Succeeded, S::Failed, S::Cancelled, S::Interrupted})
        EXPECT_EQ(r(s, BackendKind::PackageKit, "/1_abc"), RecoveryAction::None);
}

TEST_F(StateStoreTest, RecoverAfterRestart)
{
    {
        auto store = open();
        ASSERT_TRUE(store.saveTransaction(record("queued", TransactionState::Queued, kT0)));
        ASSERT_TRUE(store.saveTransaction(record("planning", TransactionState::Planning, kT0 + 1)));
        ASSERT_TRUE(store.saveTransaction(record("awaiting", TransactionState::AwaitingConfirmation, kT0 + 2)));
        ASSERT_TRUE(store.saveTransaction(record("flatpak-running", TransactionState::Running, kT0 + 3)));

        auto pk = record("pk-running", TransactionState::Running, kT0 + 4);
        pk.backend = BackendKind::PackageKit;
        pk.scope = Scope::System;
        pk.offers = {{unwrap(OfferId::parse("packagekit:gimp"))}};
        pk.snapshot.plan = debPlan(TransactionKind::Install, {debPackage(PlanOperation::Install, "gimp", true)});
        pk.snapshot.acceptedPlan = pk.snapshot.plan;
        pk.backendRef = "/1234_abcdef";
        ASSERT_TRUE(store.saveTransaction(pk));
        ASSERT_TRUE(store.saveTransaction(finishedRecord(1, kT0 - 5)));
    }
    // The service restarts.
    auto store = open(kT0 + 100);
    auto report = unwrap(store.recoverAfterRestart(kT0 + 100));
    EXPECT_EQ(report.interrupted, (std::vector<std::string>{"queued", "planning", "flatpak-running"}));
    EXPECT_EQ(report.cancelled, std::vector<std::string>{"awaiting"});
    ASSERT_EQ(report.toReattach.size(), 1u);
    EXPECT_EQ(report.toReattach[0].id, "pk-running");
    EXPECT_EQ(report.toReattach[0].backendRef, "/1234_abcdef");
    EXPECT_TRUE(report.reconcile); // flatpak-running may have changed the system

    auto awaiting = unwrap(store.transaction("awaiting"));
    EXPECT_EQ(awaiting->snapshot.state, TransactionState::Cancelled);
    EXPECT_EQ(awaiting->finished, std::optional<UnixSeconds>(kT0 + 100));
    EXPECT_EQ(unwrap(store.transaction("flatpak-running"))->snapshot.state, TransactionState::Interrupted);
    EXPECT_EQ(unwrap(store.transaction("pk-running"))->snapshot.state, TransactionState::Running);

    // A second pass only finds what is still to re-attach.
    auto again = unwrap(store.recoverAfterRestart(kT0 + 200));
    EXPECT_TRUE(again.interrupted.empty());
    EXPECT_TRUE(again.cancelled.empty());
    EXPECT_FALSE(again.reconcile);
    EXPECT_EQ(again.toReattach.size(), 1u);
}

TEST_F(StateStoreTest, RecoveryWithoutRunningTransactionNeedsNoReconciliation)
{
    auto store = open();
    ASSERT_TRUE(store.saveTransaction(record("planning", TransactionState::Planning, kT0)));
    auto report = unwrap(store.recoverAfterRestart(kT0 + 1));
    EXPECT_EQ(report.interrupted.size(), 1u);
    // D-017: nothing runs in planning, so nothing changed on the system.
    EXPECT_FALSE(report.reconcile);
}

// --- Concurrency ------------------------------------------------------------------

TEST_F(StateStoreTest, ReadersNeverSeeAHalfReplacedInventory)
{
    auto store = open();
    constexpr int kItems = 50;
    constexpr int kRounds = 150;
    auto inventory = [](int round) {
        std::vector<InstalledItem> items;
        for (int i = 0; i < kItems; ++i)
            items.push_back(flatpakItem("App" + std::to_string(i), Scope::User, "1." + std::to_string(round)));
        return items;
    };
    ASSERT_TRUE(store.replaceInstalled(BackendKind::Flatpak, Scope::User, inventory(0), kT0));

    std::atomic<bool> done{false};
    std::atomic<int> reads{0};
    std::atomic<int> failures{0};
    std::vector<std::thread> readers;
    for (int r = 0; r < 4; ++r) {
        readers.emplace_back([&] {
            auto reader = SqliteInstalledState::open(path);
            if (!reader) {
                ++failures;
                return;
            }
            while (!done.load()) {
                auto items = reader->items(BackendKind::Flatpak);
                if (!items || items->size() != kItems) {
                    ++failures;
                    continue;
                }
                const std::string version = items->front().version()->str();
                for (const InstalledItem& item : *items)
                    if (item.version()->str() != version)
                        ++failures;
                ++reads;
            }
        });
    }
    for (int round = 1; round <= kRounds; ++round)
        ASSERT_TRUE(store.replaceInstalled(BackendKind::Flatpak, Scope::User, inventory(round), kT0 + round));
    // Let the readers see the last state at least once.
    while (reads.load() < 20)
        std::this_thread::yield();
    done = true;
    for (auto& t : readers)
        t.join();
    EXPECT_EQ(failures.load(), 0);
    EXPECT_GT(reads.load(), 0);
}

TEST_F(StateStoreTest, TwoWritersWaitForEachOther)
{
    // The service is the only writer, but a second process (a crashed
    // instance still exiting) must not corrupt anything: SQLite serialises
    // the writes, waiting up to the busy timeout.
    auto a = open();
    auto b = open();
    std::thread other([&] {
        for (int i = 0; i < 50; ++i)
            ASSERT_TRUE(b.saveTransaction(finishedRecord(i, kT0 + i)));
    });
    for (int i = 50; i < 100; ++i)
        ASSERT_TRUE(a.saveTransaction(finishedRecord(i, kT0 + i)));
    other.join();
    EXPECT_EQ(a.history(1000)->size(), 100u);
}

// --- Abrupt stops --------------------------------------------------------------

TEST_F(StateStoreTest, UncommittedChangesOfAKilledProcessDisappear)
{
    {
        auto store = open();
        const std::vector<InstalledItem> user{flatpakItem("A"), flatpakItem("B")};
        ASSERT_TRUE(store.replaceInstalled(BackendKind::Flatpak, Scope::User, user, kT0));
    }
    const pid_t pid = ::fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        sqlite3* db = nullptr;
        sqlite3_open(path.c_str(), &db);
        sqlite3_exec(db, "BEGIN IMMEDIATE; DELETE FROM installed; DELETE FROM meta;", nullptr, nullptr, nullptr);
        ::_exit(3); // dies inside the transaction
    }
    int status = 0;
    ASSERT_EQ(::waitpid(pid, &status, 0), pid);
    ASSERT_TRUE(WIFEXITED(status));

    auto store = open();
    EXPECT_EQ(store.items(std::nullopt)->size(), 2u);
    EXPECT_EQ(rawQuery(path, "PRAGMA integrity_check"), "ok");
}

TEST_F(StateStoreTest, CommittedChangesSurviveAKilledProcess)
{
    {
        auto store = open();
    }
    const pid_t pid = ::fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        auto store = StateStore::open(path, kT0);
        const bool ok = store && store->saveTransaction(record("tx-1", TransactionState::Running, kT0));
        ::_exit(ok ? 0 : 1); // no destructor, no checkpoint, no close
    }
    int status = 0;
    ASSERT_EQ(::waitpid(pid, &status, 0), pid);
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0);

    auto store = open();
    auto r = unwrap(store.transaction("tx-1"));
    ASSERT_TRUE(r);
    EXPECT_EQ(r->snapshot.state, TransactionState::Running);
}

TEST_F(StateStoreTest, KillingAWriterAtRandomLeavesAConsistentState)
{
    constexpr int kItems = 200;
    {
        auto store = open();
    }
    for (int attempt = 0; attempt < 6; ++attempt) {
        const pid_t pid = ::fork();
        ASSERT_GE(pid, 0);
        if (pid == 0) {
            auto store = StateStore::open(path, kT0);
            if (!store)
                ::_exit(1);
            for (int round = 0;; ++round) {
                std::vector<InstalledItem> items;
                for (int i = 0; i < kItems; ++i)
                    items.push_back(flatpakItem("App" + std::to_string(i), Scope::User, "1." + std::to_string(round)));
                (void) store->replaceInstalled(BackendKind::Flatpak, Scope::User, items, kT0 + round);
                (void) store->saveTransaction(finishedRecord(round % 300, kT0 + round));
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20 + 37 * attempt));
        ::kill(pid, SIGKILL);
        int status = 0;
        ASSERT_EQ(::waitpid(pid, &status, 0), pid);
        ASSERT_TRUE(WIFSIGNALED(status));

        auto store = StateStore::open(path, kT0);
        ASSERT_TRUE(store) << store.error().message();
        auto items = unwrap(store->items(BackendKind::Flatpak));
        ASSERT_TRUE(items.empty() || items.size() == kItems) << items.size();
        for (const InstalledItem& item : items)
            ASSERT_EQ(item.version()->str(), items.front().version()->str());
        ASSERT_TRUE(store->history(1000));
        EXPECT_LE(store->history(1000)->size(), kMaxFinishedTransactions);
    }
    EXPECT_EQ(rawQuery(path, "PRAGMA integrity_check"), "ok");
}
