# cutefish-store
A universal Linux software store for the Cutefish desktop environment.

## Status

The core library (`store-core`, version 0.3: business models, interfaces,
the SQLite catalogue with source fingerprints, browsing and installed
state, transaction plans, the confirmation policy and the state database)
and the AppStream indexer exist; there is no user
interface, D-Bus service or package backend yet.

- Design: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) and
  [docs/DECISIONS.md](docs/DECISIONS.md)
- Core: [docs/CORE.md](docs/CORE.md)
- Catalogue and indexer: [docs/CATALOG.md](docs/CATALOG.md)

## Build and test

On Debian 13:

```bash
sudo apt install build-essential cmake ninja-build pkg-config \
    libsqlite3-dev libappstream-dev libxml2-dev libgtest-dev
```

Details, and how the list was checked: [docs/CATALOG.md](docs/CATALOG.md),
"Dépendances".

```bash
cmake -S . -B build -G Ninja -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```
