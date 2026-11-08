# Tests of the Flatpak backend: the pure translation logic and the Backend
# over a deterministic double of FlatpakStore (fakestore.h). No libflatpak and
# no real installation are used.

add_executable(store-flatpak-tests testflatpak.cpp fakestore.h)
target_compile_options(store-flatpak-tests PRIVATE -Wall -Wextra)
target_link_libraries(store-flatpak-tests PRIVATE store-backend-flatpak GTest::gtest_main Threads::Threads)
gtest_discover_tests(store-flatpak-tests)
