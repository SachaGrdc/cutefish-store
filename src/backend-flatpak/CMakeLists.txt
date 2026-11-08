# store-backend-flatpak (ARCHITECTURE §7.3, step G): the Flatpak backend and
# its pure translation logic. The core stays independent of Flatpak (P-006);
# only this library links libflatpak, and only when it is found. Without it,
# the logic and the backend still build (the backend can be driven by a test
# double); the real LibFlatpakStore is disabled.

add_library(store-backend-flatpak STATIC
    flatpakbackend.cpp
    flatpakbackend.h
    flatpakstore.h
    flatpaktypes.h
    translate.cpp
    translate.h
    worker.h
)
target_include_directories(store-backend-flatpak PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(store-backend-flatpak PRIVATE -Wall -Wextra)
target_link_libraries(store-backend-flatpak PUBLIC store-core)

find_package(Threads REQUIRED)
target_link_libraries(store-backend-flatpak PRIVATE Threads::Threads)

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(FLATPAK QUIET IMPORTED_TARGET flatpak)
endif()
if(FLATPAK_FOUND)
    target_sources(store-backend-flatpak PRIVATE libflatpakstore.cpp libflatpakstore.h)
    target_link_libraries(store-backend-flatpak PRIVATE PkgConfig::FLATPAK)
    target_compile_definitions(store-backend-flatpak PUBLIC CUTEFISH_STORE_HAVE_FLATPAK=1)
    message(STATUS "Flatpak backend: libflatpak ${FLATPAK_VERSION} found")
else()
    message(STATUS "Flatpak backend: libflatpak not found; the real store is disabled "
                   "(the logic and the backend still build)")
endif()
