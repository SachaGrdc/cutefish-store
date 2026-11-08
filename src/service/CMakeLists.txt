# cutefish-store-service (SERVICE.md): the host layer. The only part of the
# project that uses Cutefish Toolkit (P-006); store-core never sees it.

find_package(CutefishToolkit QUIET)
if(NOT TARGET Cutefish::ToolkitBase)
    message(FATAL_ERROR "cutefish-store-service needs Cutefish Toolkit (Cutefish::ToolkitBase). "
                        "Configure with -DCUTEFISH_STORE_BUILD_SERVICE=OFF to build without the service.")
endif()

set(CUTEFISH_STORE_INDEXER_PATH "${CMAKE_INSTALL_FULL_LIBEXECDIR}/cutefish-store-indexer")
set(CUTEFISH_STORE_SERVICE_PATH "${CMAKE_INSTALL_FULL_LIBEXECDIR}/cutefish-store-service")

add_library(store-service STATIC
    lifecycle.cpp
    lifecycle.h
    marshal.cpp
    marshal.h
    options.cpp
    options.h
    readpool.cpp
    readpool.h
    refresher.cpp
    refresher.h
    refreshtransaction.cpp
    refreshtransaction.h
    sourcesfile.cpp
    sourcesfile.h
    storeservice.cpp
    storeservice.h
)
target_include_directories(store-service PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(store-service PRIVATE -Wall -Wextra)
target_compile_definitions(store-service PRIVATE
    CUTEFISH_STORE_VERSION="${PROJECT_VERSION}"
    CUTEFISH_STORE_INDEXER_PATH="${CUTEFISH_STORE_INDEXER_PATH}")
target_link_libraries(store-service PUBLIC store-core Cutefish::ToolkitBase)
if(TARGET store-backend-packagekit)
    target_sources(store-service PRIVATE pkbackend.cpp pkbackend.h pktransport.cpp pktransport.h)
    target_link_libraries(store-service PUBLIC store-backend-packagekit)
    target_compile_definitions(store-service PUBLIC CUTEFISH_STORE_HAVE_PACKAGEKIT=1)
endif()

add_executable(cutefish-store-service main.cpp)
target_compile_options(cutefish-store-service PRIVATE -Wall -Wextra)
target_link_libraries(cutefish-store-service PRIVATE store-service)

# D-Bus activation and the systemd user unit (P-010). Installed, never run
# by the build or the tests (they use a private bus, tests/service).
configure_file(${PROJECT_SOURCE_DIR}/data/org.cutefish.Store.service.in
               ${CMAKE_CURRENT_BINARY_DIR}/org.cutefish.Store.service @ONLY)
configure_file(${PROJECT_SOURCE_DIR}/data/cutefish-store.service.in
               ${CMAKE_CURRENT_BINARY_DIR}/cutefish-store.service @ONLY)
install(TARGETS cutefish-store-service DESTINATION ${CMAKE_INSTALL_LIBEXECDIR})
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/org.cutefish.Store.service
        DESTINATION ${CMAKE_INSTALL_DATADIR}/dbus-1/services)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/cutefish-store.service
        DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/systemd/user)
