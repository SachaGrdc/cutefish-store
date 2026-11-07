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

# The core has no external dependency in v0.1: SQLite and libcurl will be
# added with the catalogue and the image cache, when they are needed.
add_subdirectory(src/core)

if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()
