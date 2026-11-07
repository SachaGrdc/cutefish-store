# cutefish-store
A universal Linux software store for the Cutefish desktop environment.

## Status

The core library (`store-core`, version 0.2: business models, interfaces
and the SQLite catalogue) and the AppStream indexer exist; there is no user
interface, D-Bus service or package backend yet.

- Design: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) and
  [docs/DECISIONS.md](docs/DECISIONS.md)
- Core: [docs/CORE.md](docs/CORE.md)
- Catalogue and indexer: [docs/CATALOG.md](docs/CATALOG.md)

## Build and test

Requires CMake ≥ 3.22, a C++20 compiler, pkg-config and SQLite with FTS5;
the indexer also needs libappstream and libxml2, and the tests GoogleTest.

```bash
cmake -S . -B build -G Ninja -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```
