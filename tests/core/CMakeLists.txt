# Unit tests of store-core: no network, no privileges, no Cutefish session.

add_executable(store-core-tests
    fakes.h
    fixtures.h
    testapplication.cpp
    testerror.cpp
    testidentifiers.cpp
    testinstalled.cpp
    testinterfaces.cpp
    testoffer.cpp
    testsearch.cpp
    testsource.cpp
    testversion.cpp
)
target_link_libraries(store-core-tests PRIVATE store-core GTest::gtest_main)
target_compile_options(store-core-tests PRIVATE -Wall -Wextra)
gtest_discover_tests(store-core-tests)

# Every public header must compile on its own: one translation unit per
# header, which includes nothing else.
set(header_checks)
foreach(header IN LISTS STORE_CORE_PUBLIC_HEADERS)
    string(REPLACE "include/" "" include_path "${header}")
    string(MAKE_C_IDENTIFIER "${include_path}" name)
    set(source "${CMAKE_CURRENT_BINARY_DIR}/headercheck/${name}.cpp")
    file(WRITE "${source}.in" "#include <${include_path}>\n")
    configure_file("${source}.in" "${source}" COPYONLY)
    list(APPEND header_checks "${source}")
endforeach()
add_library(store-core-header-check OBJECT ${header_checks})
target_link_libraries(store-core-header-check PRIVATE store-core)
target_compile_options(store-core-header-check PRIVATE -Wall -Wextra)
