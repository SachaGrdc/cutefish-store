# store-core: the business logic of Cutefish Store (ARCHITECTURE §4.3, P-006).
# C++20 and the standard library only: no Cutefish, no GLib, no D-Bus, no
# graphical toolkit, no thread and no event loop.

set(STORE_CORE_PUBLIC_HEADERS
    include/cutefish/store/error.h
    include/cutefish/store/identifiers.h
    include/cutefish/store/result.h
    include/cutefish/store/version.h
)

add_library(store-core STATIC
    ${STORE_CORE_PUBLIC_HEADERS}
    src/error.cpp
    src/identifiers.cpp
    src/text.cpp
    src/text.h
    src/version.cpp
)

target_include_directories(store-core
    PUBLIC include
    PRIVATE src
)
target_compile_features(store-core PUBLIC cxx_std_20)
target_compile_options(store-core PRIVATE -Wall -Wextra)

# Exposed for the header self-containment test (tests/core).
set(STORE_CORE_PUBLIC_HEADERS ${STORE_CORE_PUBLIC_HEADERS} PARENT_SCOPE)
