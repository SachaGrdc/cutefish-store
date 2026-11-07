# Benchmarks on real catalogues: built with the tests, never run by ctest
# (they need a catalogue; see scripts/measure-catalog.sh).

find_package(PkgConfig REQUIRED)
pkg_check_modules(SQLITE3 REQUIRED IMPORTED_TARGET sqlite3)

add_executable(store-catalog-bench catalogbench.cpp)
target_compile_options(store-catalog-bench PRIVATE -Wall -Wextra)
target_link_libraries(store-catalog-bench PRIVATE store-core PkgConfig::SQLITE3 Threads::Threads)
