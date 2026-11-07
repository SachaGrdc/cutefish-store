#pragma once

// The catalogue schema (docs/CATALOG.md), shared by the writer, the reader
// and the validation. Internal to store-core.

#include <cstdint>

namespace cutefish::store::detail {

/// Values of texts.field.
enum class TextField : std::int64_t { Name = 0, Summary = 1, Description = 2, Keywords = 3 };

/// Description blocks are stored as one text: blocks separated by U+001E,
/// each block starting with a marker character then U+001F. The markers are
/// punctuation, so that the search tokenizer ignores them.
inline constexpr char kBlockSeparator = '\x1E';
inline constexpr char kBlockTextStart = '\x1F';
inline constexpr char kParagraphMarker = '.';
inline constexpr char kListItemMarker = '*';
inline constexpr char kOrderedItemMarker = '#';
/// Keywords of one language, one per line.
inline constexpr char kKeywordSeparator = '\n';

inline constexpr const char* kCatalogSchema = R"SQL(
CREATE TABLE meta (
    key TEXT PRIMARY KEY,
    value TEXT NOT NULL
) WITHOUT ROWID;

CREATE TABLE sources (
    id INTEGER PRIMARY KEY,
    source_id TEXT NOT NULL UNIQUE,
    backend TEXT NOT NULL CHECK (backend IN ('packagekit', 'flatpak')),
    scope TEXT NOT NULL CHECK (scope IN ('system', 'user')),
    kind TEXT NOT NULL CHECK (kind IN ('distribution', 'flathub', 'third-party')),
    origin TEXT NOT NULL,
    label TEXT NOT NULL DEFAULT '',
    url TEXT NOT NULL DEFAULT '',
    priority INTEGER NOT NULL,
    enabled INTEGER NOT NULL CHECK (enabled IN (0, 1))
);

CREATE TABLE apps (
    id INTEGER PRIMARY KEY,
    app_id TEXT NOT NULL UNIQUE,
    verified INTEGER NOT NULL DEFAULT 0 CHECK (verified IN (0, 1))
);

CREATE TABLE aliases (
    alias TEXT PRIMARY KEY,
    app INTEGER NOT NULL REFERENCES apps (id)
) WITHOUT ROWID;
CREATE INDEX aliases_app ON aliases (app);

CREATE TABLE app_categories (
    category TEXT NOT NULL,
    app INTEGER NOT NULL REFERENCES apps (id),
    PRIMARY KEY (category, app)
) WITHOUT ROWID;
CREATE INDEX app_categories_app ON app_categories (app);

CREATE TABLE offers (
    id INTEGER PRIMARY KEY,
    offer_id TEXT NOT NULL,
    app INTEGER NOT NULL REFERENCES apps (id),
    source INTEGER NOT NULL REFERENCES sources (id),
    component_id TEXT NOT NULL,
    format TEXT NOT NULL CHECK (format IN ('deb', 'flatpak')),
    package TEXT NOT NULL,
    version TEXT,
    architecture TEXT NOT NULL DEFAULT '',
    developer TEXT NOT NULL DEFAULT '',
    icon_kind INTEGER NOT NULL DEFAULT 0,
    icon TEXT NOT NULL DEFAULT '',
    UNIQUE (app, offer_id)
);
CREATE INDEX offers_source ON offers (source);

CREATE TABLE texts (
    offer INTEGER NOT NULL REFERENCES offers (id),
    field INTEGER NOT NULL CHECK (field BETWEEN 0 AND 3),
    locale TEXT NOT NULL,
    value TEXT NOT NULL,
    PRIMARY KEY (offer, field, locale)
) WITHOUT ROWID;
CREATE INDEX texts_names ON texts (value COLLATE NOCASE) WHERE field = 0;

CREATE VIRTUAL TABLE search USING fts5(
    name, keywords, summary, description, developer,
    content = '',
    tokenize = 'unicode61 remove_diacritics 2',
    prefix = '2 3'
);
CREATE VIRTUAL TABLE search_names USING fts5(
    name,
    content = '',
    tokenize = 'trigram remove_diacritics 1'
);
)SQL";

/// Tables a valid catalogue must contain.
inline constexpr const char* kRequiredTables[] = {"meta", "sources", "apps", "aliases", "app_categories",
                                                  "offers", "texts", "search", "search_names"};

} // namespace cutefish::store::detail
