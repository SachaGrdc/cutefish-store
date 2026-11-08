cmake_minimum_required(VERSION 3.22)

project(cutefish-store
    VERSION 0.1.0
    DESCRIPTION "Software store for the Cutefish desktop"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)

# Tests are off by default, as in the Cutefish projects (P-013).
option(BUILD_TESTING "Build the GoogleTest suites" OFF)
option(CUTEFISH_STORE_SANITIZERS "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

if(CUTEFISH_STORE_SANITIZERS)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

option(CUTEFISH_STORE_BUILD_INDEXER "Build cutefish-store-indexer (needs libappstream and libxml2)" ON)
option(CUTEFISH_STORE_BUILD_SERVICE "Build cutefish-store-service (needs Cutefish Toolkit)" ON)
option(CUTEFISH_STORE_BUILD_PACKAGEKIT "Build the PackageKit backend (no extra dependency)" ON)

# store-core depends on SQLite only; libcurl will come with the image cache.
add_subdirectory(src/core)
if(CUTEFISH_STORE_BUILD_INDEXER)
    add_subdirectory(src/indexer)
endif()
if(CUTEFISH_STORE_BUILD_PACKAGEKIT)
    add_subdirectory(src/backend-packagekit)
endif()
if(CUTEFISH_STORE_BUILD_SERVICE)
    add_subdirectory(src/service)
endif()

if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()
