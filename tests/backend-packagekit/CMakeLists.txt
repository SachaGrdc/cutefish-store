# Unit tests of the PackageKit logic: no daemon, no bus. The inputs are the
# data the prototype received from PackageKit 1.3.1 (prototype 4).

add_executable(store-backend-packagekit-tests testpackagekitlogic.cpp)
target_include_directories(store-backend-packagekit-tests PRIVATE ${CMAKE_SOURCE_DIR}/tests/core)
target_compile_options(store-backend-packagekit-tests PRIVATE -Wall -Wextra)
target_link_libraries(store-backend-packagekit-tests PRIVATE store-backend-packagekit GTest::gtest_main)
gtest_discover_tests(store-backend-packagekit-tests)
