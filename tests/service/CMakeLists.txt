# Service tests. The unit tests need nothing; the bus tests start a private
# dbus-daemon of their own (busfixture.h): no system bus, no systemd, no
# privilege.

add_executable(store-service-tests testserviceunits.cpp)
target_compile_options(store-service-tests PRIVATE -Wall -Wextra)
target_link_libraries(store-service-tests PRIVATE store-service GTest::gtest_main)
gtest_discover_tests(store-service-tests)

find_program(DBUS_DAEMON_EXECUTABLE dbus-daemon)
if(NOT DBUS_DAEMON_EXECUTABLE OR NOT CUTEFISH_STORE_BUILD_INDEXER)
    message(STATUS "D-Bus tests of the service skipped: they need dbus-daemon and the indexer")
    return()
endif()

add_executable(store-service-bus-tests testservicebus.cpp busfixture.h)
target_include_directories(store-service-bus-tests PRIVATE ${CMAKE_SOURCE_DIR}/tests/core)
target_compile_definitions(store-service-bus-tests PRIVATE
    SERVICE_BINARY="$<TARGET_FILE:cutefish-store-service>"
    INDEXER_BINARY="$<TARGET_FILE:cutefish-store-indexer>"
    SERVICE_TEST_DATA="${CMAKE_SOURCE_DIR}/tests/indexer/data"
    DBUS_DAEMON="${DBUS_DAEMON_EXECUTABLE}")
add_dependencies(store-service-bus-tests cutefish-store-service cutefish-store-indexer)
target_compile_options(store-service-bus-tests PRIVATE -Wall -Wextra)
target_link_libraries(store-service-bus-tests PRIVATE store-service GTest::gtest_main Threads::Threads)
# Each test starts its own bus and service: a few seconds each.
gtest_discover_tests(store-service-bus-tests PROPERTIES TIMEOUT 60)

# The generic manager with two scripted backends: the partial failure of an
# UpdateAll parent needs two children, which the service cannot have yet.
add_executable(store-transaction-manager-tests testtransactionmanager.cpp privatebus.h)
target_include_directories(store-transaction-manager-tests PRIVATE ${CMAKE_SOURCE_DIR}/tests/core)
target_compile_definitions(store-transaction-manager-tests PRIVATE DBUS_DAEMON="${DBUS_DAEMON_EXECUTABLE}")
target_compile_options(store-transaction-manager-tests PRIVATE -Wall -Wextra)
target_link_libraries(store-transaction-manager-tests PRIVATE store-service GTest::gtest_main Threads::Threads)
gtest_discover_tests(store-transaction-manager-tests PROPERTIES TIMEOUT 60)

# The PackageKit backend against a simulated PackageKit on a private bus.
if(TARGET store-backend-packagekit)
    add_executable(store-pkbackend-tests testpkbackend.cpp fakepackagekit.h privatebus.h)
    target_include_directories(store-pkbackend-tests PRIVATE ${CMAKE_SOURCE_DIR}/tests/core)
    target_compile_definitions(store-pkbackend-tests PRIVATE DBUS_DAEMON="${DBUS_DAEMON_EXECUTABLE}")
    target_compile_options(store-pkbackend-tests PRIVATE -Wall -Wextra)
    target_link_libraries(store-pkbackend-tests PRIVATE store-service GTest::gtest_main Threads::Threads)
    gtest_discover_tests(store-pkbackend-tests PROPERTIES TIMEOUT 60)
endif()

# The service with its PackageKit backend, against a simulated PackageKit on
# the test's bus (--packagekit session): never the system's packagekitd.
if(TARGET store-backend-packagekit)
    add_executable(store-service-pk-tests testservicepk.cpp busfixture.h fakepackagekit.h)
    target_include_directories(store-service-pk-tests PRIVATE ${CMAKE_SOURCE_DIR}/tests/core)
    target_compile_definitions(store-service-pk-tests PRIVATE
        SERVICE_BINARY="$<TARGET_FILE:cutefish-store-service>"
        INDEXER_BINARY="$<TARGET_FILE:cutefish-store-indexer>"
        SERVICE_TEST_DATA="${CMAKE_SOURCE_DIR}/tests/indexer/data"
        DBUS_DAEMON="${DBUS_DAEMON_EXECUTABLE}")
    add_dependencies(store-service-pk-tests cutefish-store-service cutefish-store-indexer)
    target_compile_options(store-service-pk-tests PRIVATE -Wall -Wextra)
    target_link_libraries(store-service-pk-tests PRIVATE store-service GTest::gtest_main Threads::Threads)
    gtest_discover_tests(store-service-pk-tests PROPERTIES TIMEOUT 60)
endif()

# Opt-in, NOT registered with ctest: the backend against the system's real
# packagekitd, queries and simulations only (testpkreal.cpp says how to run
# it). Skips everything unless CUTEFISH_STORE_REAL_PACKAGEKIT=1.
if(TARGET store-backend-packagekit)
    add_executable(store-pkbackend-real-tests testpkreal.cpp)
    target_compile_options(store-pkbackend-real-tests PRIVATE -Wall -Wextra)
    target_link_libraries(store-pkbackend-real-tests PRIVATE store-service GTest::gtest_main Threads::Threads)
endif()
