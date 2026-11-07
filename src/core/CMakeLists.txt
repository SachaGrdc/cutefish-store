# store-core: the business logic of Cutefish Store (ARCHITECTURE §4.3, P-006).
# C++20 and the standard library only: no Cutefish, no GLib, no D-Bus, no
# graphical toolkit, no thread and no event loop. SQLite (with FTS5) is its
# only external dependency, for the catalogue (ARCHITECTURE §12.1).

find_package(PkgConfig REQUIRED)
pkg_check_modules(SQLITE3 IMPORTED_TARGET sqlite3>=3.34)
if(NOT SQLITE3_FOUND)
    message(FATAL_ERROR "store-core needs SQLite >= 3.34 with its development files "
                        "(Debian: libsqlite3-dev). See docs/CATALOG.md, \"Dépendances\".")
endif()

set(STORE_CORE_PUBLIC_HEADERS
    include/cutefish/store/application.h
    include/cutefish/store/backend.h
    include/cutefish/store/catalog.h
    include/cutefish/store/catalogfile.h
    include/cutefish/store/catalogwriter.h
    include/cutefish/store/confirmation.h
    include/cutefish/store/defaultoffer.h
    include/cutefish/store/error.h
    include/cutefish/store/identifiers.h
    include/cutefish/store/installed.h
    include/cutefish/store/installedstate.h
    include/cutefish/store/offer.h
    include/cutefish/store/plan.h
    include/cutefish/store/result.h
    include/cutefish/store/search.h
    include/cutefish/store/source.h
    include/cutefish/store/sqlitecatalog.h
    include/cutefish/store/statestore.h
    include/cutefish/store/transaction.h
    include/cutefish/store/version.h
)

add_library(store-core STATIC
    ${STORE_CORE_PUBLIC_HEADERS}
    src/application.cpp
    src/catalogfile.cpp
    src/catalogschema.h
    src/catalogwriter.cpp
    src/confirmation.cpp
    src/defaultoffer.cpp
    src/error.cpp
    src/identifiers.cpp
    src/installed.cpp
    src/offer.cpp
    src/plan.cpp
    src/planjson.cpp
    src/planjson.h
    src/search.cpp
    src/source.cpp
    src/sqlite.cpp
    src/sqlite.h
    src/sqlitecatalog.cpp
    src/sqliteuri.h
    src/stateschema.h
    src/statestore.cpp
    src/text.cpp
    src/text.h
    src/transaction.cpp
    src/version.cpp
)

target_include_directories(store-core
    PUBLIC include
    PRIVATE src
)
target_compile_features(store-core PUBLIC cxx_std_20)
target_compile_options(store-core PRIVATE -Wall -Wextra)
# PRIVATE: sqlite3.h never reaches the public headers.
target_link_libraries(store-core PRIVATE PkgConfig::SQLITE3)

# Exposed for the header self-containment test (tests/core).
set(STORE_CORE_PUBLIC_HEADERS ${STORE_CORE_PUBLIC_HEADERS} PARENT_SCOPE)
