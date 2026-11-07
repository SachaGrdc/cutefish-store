#pragma once

// Schema of state.sqlite (SERVICE.md §8.2). Internal to store-core.

namespace cutefish::store::detail {

/// PRAGMA user_version of the current schema. Each older version has a
/// migration step in statestore.cpp; a newer one is refused.
inline constexpr int kStateSchemaVersion = 1;

/// Version 1, created on an empty file.
inline constexpr const char* kStateSchemaV1 = R"SQL(
CREATE TABLE meta (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
) WITHOUT ROWID;

-- A cache of the last inventory of each backend and installation, for
-- display. PackageKit and libflatpak stay the source of truth.
CREATE TABLE installed (
    offer_id TEXT PRIMARY KEY,
    backend  TEXT NOT NULL CHECK (backend IN ('packagekit', 'flatpak')),
    scope    TEXT NOT NULL CHECK (scope IN ('system', 'user')),
    app_id   TEXT,
    version  TEXT,
    origin   TEXT NOT NULL DEFAULT ''
) WITHOUT ROWID;
CREATE INDEX installed_app ON installed (app_id) WHERE app_id IS NOT NULL;
CREATE INDEX installed_backend ON installed (backend, scope);

-- The transaction journal (D-017, D-018). Plans are JSON written by
-- planjson.cpp and validated again by Plan::create when read.
CREATE TABLE transactions (
    id                TEXT PRIMARY KEY,
    kind              TEXT NOT NULL CHECK (kind IN ('install', 'remove', 'update')),
    backend           TEXT NOT NULL CHECK (backend IN ('packagekit', 'flatpak')),
    scope             TEXT NOT NULL CHECK (scope IN ('system', 'user')),
    state             TEXT NOT NULL CHECK (state IN ('queued', 'planning', 'awaiting-confirmation', 'running',
                                                     'succeeded', 'failed', 'cancelled', 'interrupted')),
    terminal          INTEGER NOT NULL CHECK (terminal IN (0, 1)),
    confirmed_by_user INTEGER NOT NULL CHECK (confirmed_by_user IN (0, 1)),
    plan_changed      INTEGER NOT NULL CHECK (plan_changed IN (0, 1)),
    current_plan      TEXT CHECK (current_plan IS NULL OR json_valid(current_plan)),
    accepted_plan     TEXT CHECK (accepted_plan IS NULL OR json_valid(accepted_plan)),
    error_code        TEXT,
    error_failure     TEXT,
    error_message     TEXT,
    backend_ref       TEXT NOT NULL DEFAULT '',
    created           INTEGER NOT NULL,
    updated           INTEGER NOT NULL,
    finished          INTEGER,
    CHECK ((terminal = 1) = (finished IS NOT NULL)),
    CHECK ((state = 'failed') = (error_code IS NOT NULL))
) WITHOUT ROWID;
CREATE INDEX transactions_open ON transactions (created, id) WHERE terminal = 0;
CREATE INDEX transactions_finished ON transactions (finished, id) WHERE terminal = 1;

CREATE TABLE transaction_offers (
    transaction_id TEXT NOT NULL REFERENCES transactions (id) ON DELETE CASCADE,
    position       INTEGER NOT NULL,
    offer_id       TEXT NOT NULL,
    app_id         TEXT,
    PRIMARY KEY (transaction_id, position)
) WITHOUT ROWID;
CREATE INDEX transaction_offers_offer ON transaction_offers (offer_id);

-- Last successful refresh of each source (D-015).
CREATE TABLE source_state (
    source_id    TEXT PRIMARY KEY,
    last_refresh INTEGER NOT NULL
) WITHOUT ROWID;
)SQL";

} // namespace cutefish::store::detail
