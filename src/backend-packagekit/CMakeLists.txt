# store-backend-packagekit (P-001, D-007): the PackageKit logic. No D-Bus:
# the host's transport (src/service) fills plain structures (pkdata.h) from
# the daemon's signals, and this library turns them into the core's types.
# The Debian profile (debianprofile.h) is the only Debian-specific part.

add_library(store-backend-packagekit STATIC
    debianprofile.cpp
    debianprofile.h
    packageid.cpp
    packageid.h
    pkdata.h
    pkenums.cpp
    pkenums.h
    translate.cpp
    translate.h
)
target_include_directories(store-backend-packagekit PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(store-backend-packagekit PRIVATE -Wall -Wextra)
target_link_libraries(store-backend-packagekit PUBLIC store-core)
