# cutefish-store-indexer (ARCHITECTURE §4.7, P-002): the only part of the
# project that uses libappstream, and with it GLib and libxml2. store-core
# never sees their headers.

find_package(PkgConfig REQUIRED)
pkg_check_modules(APPSTREAM REQUIRED IMPORTED_TARGET appstream>=1.0)
pkg_check_modules(LIBXML2 REQUIRED IMPORTED_TARGET libxml-2.0)

add_library(store-indexer STATIC
    importer.cpp
    importer.h
    markup.cpp
    markup.h
)
target_include_directories(store-indexer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(store-indexer PRIVATE -Wall -Wextra)
target_link_libraries(store-indexer
    PUBLIC store-core
    PRIVATE PkgConfig::APPSTREAM PkgConfig::LIBXML2
)

add_executable(cutefish-store-indexer main.cpp)
target_compile_options(cutefish-store-indexer PRIVATE -Wall -Wextra)
target_link_libraries(cutefish-store-indexer PRIVATE store-indexer)
install(TARGETS cutefish-store-indexer RUNTIME DESTINATION ${CMAKE_INSTALL_LIBEXECDIR})
