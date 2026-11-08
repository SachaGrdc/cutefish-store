# cutefish-store
A universal Linux software store for the Cutefish desktop environment.

## Status

The core library (`store-core`, version 0.3: business models, interfaces,
the SQLite catalogue with source fingerprints, browsing and installed
state, transaction plans, the confirmation policy, the state database and
the full backend contract) and the AppStream indexer exist, with a
read-only D-Bus service (`cutefish-store-service`) and a PackageKit backend.
The service uses the backend for reading only (installed packages, updates,
repository refresh); installing, removing and updating are not exposed yet.
There is no user interface or Flatpak backend yet.

- Design: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) and
  [docs/DECISIONS.md](docs/DECISIONS.md)
- Core: [docs/CORE.md](docs/CORE.md)
- Catalogue and indexer: [docs/CATALOG.md](docs/CATALOG.md)
- D-Bus service: [docs/SERVICE.md](docs/SERVICE.md), §16 for what v0.1
  does, §17 for the PackageKit backend

## Build and test

On Debian 13:

```bash
sudo apt install build-essential cmake ninja-build pkg-config \
    libsqlite3-dev libappstream-dev libxml2-dev libgtest-dev
```

Details, and how the list was checked: [docs/CATALOG.md](docs/CATALOG.md),
"Dépendances". The service also needs Cutefish Toolkit (package
`cutefish-toolkit`; `-DCUTEFISH_STORE_BUILD_SERVICE=OFF` builds without it),
and its D-Bus tests need `dbus-daemon`, which they run on a private bus of
their own, without privileges.

The service talks to PackageKit on the system bus (`--packagekit
system|session|off`). It indexes the distribution's DEP-11 metadata found in
`/var/lib/swcatalog` and the sources listed in
`$XDG_CONFIG_HOME/cutefish-store/sources.conf` (docs/DECISIONS.md, D-020,
D-028). The tests never reach the system's packagekitd: they use a simulated
one on a private bus.

```bash
cmake -S . -B build -G Ninja -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Optional, outside ctest: queries and simulations (never a real transaction)
against the system's packagekitd (D-026):

```bash
CUTEFISH_STORE_REAL_PACKAGEKIT=1 build/tests/service/store-pkbackend-real-tests
```
