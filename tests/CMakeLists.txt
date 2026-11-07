find_package(GTest REQUIRED)
include(GoogleTest)

add_subdirectory(core)
if(CUTEFISH_STORE_BUILD_INDEXER)
    add_subdirectory(indexer)
endif()
