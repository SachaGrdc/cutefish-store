# Importer tests on small, committed AppStream fixtures (data/). No network,
# no system catalogue. Compressed copies are generated at configure time.

set(INDEXER_TEST_DATA ${CMAKE_CURRENT_SOURCE_DIR}/data)
set(INDEXER_TEST_GENERATED ${CMAKE_CURRENT_BINARY_DIR}/data)
file(MAKE_DIRECTORY ${INDEXER_TEST_GENERATED})
foreach(fixture debian.yml flathub.xml)
    file(COPY ${INDEXER_TEST_DATA}/${fixture} DESTINATION ${INDEXER_TEST_GENERATED})
    file(ARCHIVE_CREATE OUTPUT ${INDEXER_TEST_GENERATED}/${fixture}.gz
         PATHS ${INDEXER_TEST_GENERATED}/${fixture} FORMAT raw COMPRESSION GZip)
endforeach()

add_executable(store-indexer-tests testcli.cpp testimport.cpp testmarkup.cpp)
target_include_directories(store-indexer-tests PRIVATE ${CMAKE_SOURCE_DIR}/tests/core)
target_compile_definitions(store-indexer-tests PRIVATE
    INDEXER_TEST_DATA="${INDEXER_TEST_DATA}"
    INDEXER_TEST_GENERATED="${INDEXER_TEST_GENERATED}"
    INDEXER_BINARY="$<TARGET_FILE:cutefish-store-indexer>")
add_dependencies(store-indexer-tests cutefish-store-indexer)
target_compile_options(store-indexer-tests PRIVATE -Wall -Wextra)
target_link_libraries(store-indexer-tests PRIVATE store-indexer GTest::gtest_main)
gtest_discover_tests(store-indexer-tests)
