find_package(GTest REQUIRED)
find_package(Threads REQUIRED)
include(GoogleTest)

add_subdirectory(core)
if(CUTEFISH_STORE_BUILD_INDEXER)
    add_subdirectory(indexer)
endif()
add_subdirectory(perf)
