# cutefish-store
A universal Linux software store for the Cutefish desktop environment.

## Status

Only the core library (`store-core`, version 0.1) exists: business models
and interfaces, without user interface, D-Bus service or backend yet.

- Design: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) and
  [docs/DECISIONS.md](docs/DECISIONS.md)
- Core: [docs/CORE.md](docs/CORE.md)

## Build and test

Requires CMake ≥ 3.22, a C++20 compiler and, for the tests, GoogleTest.

```bash
cmake -S . -B build -G Ninja -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```
