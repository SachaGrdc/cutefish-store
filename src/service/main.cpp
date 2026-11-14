// cutefish-store-service: the D-Bus service of Cutefish Store (SERVICE.md).
// Activated on the session bus as org.cutefish.Store; stops when idle.

#include "options.h"
#include "storeservice.h"

#ifdef CUTEFISH_STORE_HAVE_PACKAGEKIT
#include "pkbackend.h"
#endif

#ifdef CUTEFISH_STORE_HAVE_FLATPAK
#include "flatpakbackend.h"
#include "libflatpakstore.h"
#endif

#include <base/dbus.h>
#include <base/event_loop.h>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace cutefish;

namespace {

/// Builds every backend the service was asked for (H.1): PackageKit and
/// Flatpak, each optional and independent. The only place that knows which
/// backends exist.
std::vector<std::unique_ptr<store::Backend>> buildBackends(const store::service::ServiceOptions& options,
                                                           base::EventLoop& loop)
{
    std::vector<std::unique_ptr<store::Backend>> backends;
#ifdef CUTEFISH_STORE_HAVE_PACKAGEKIT
    if (options.packagekit != store::service::PackageKitBus::Off) {
        store::service::PackageKitBackend::Options pk;
        pk.bus = options.packagekit == store::service::PackageKitBus::System
                         ? store::service::PkTransport::Bus::System
                         : store::service::PkTransport::Bus::Session;
        pk.swcatalog = options.swcatalog;
        backends.push_back(std::make_unique<store::service::PackageKitBackend>(loop, std::move(pk)));
    }
#endif
#ifdef CUTEFISH_STORE_HAVE_FLATPAK
    if (options.flatpak != store::service::FlatpakMode::Off) {
        auto post = [&loop](std::function<void()> task) { loop.post(std::move(task)); };
        backends.push_back(std::make_unique<store::flatpak::FlatpakBackend>(
                store::flatpak::FlatpakBackend::Options{.post = std::move(post)},
                std::make_unique<store::flatpak::LibFlatpakStore>(store::flatpak::LibFlatpakStore::Options{})));
    }
#endif
    return backends;
}

} // namespace

int main(int argc, char** argv)
{
    auto options = store::service::parseArguments(std::vector<std::string>(argv + 1, argv + argc));
    if (!options) {
        std::fprintf(stderr, "cutefish-store-service: %s\n", options.error().message().c_str());
        return 2;
    }
    base::EventLoop loop;
    auto connection = base::dbus::Connection::open(base::dbus::Connection::Bus::Session, &loop);
    if (!connection) {
        std::fprintf(stderr, "cutefish-store-service: no session bus\n");
        return 1;
    }
    // The bus gone (session ending, dbus-daemon stopped): nothing left to
    // serve. sd-bus reports it with a local Disconnected signal.
    base::dbus::MatchRule disconnected;
    disconnected.path = "/org/freedesktop/DBus/Local";
    disconnected.interface = "org.freedesktop.DBus.Local";
    disconnected.member = "Disconnected";
    const std::string name = options->busName;
    // Build the backends before moving the options: `options` must still be
    // valid when buildBackends() reads it (the order of evaluation of the
    // arguments below is unspecified, so moving first would be a use-after-move).
    auto backends = buildBackends(*options, loop);
    store::service::StoreService service(loop, *connection, std::move(options).value(), std::move(backends));
    // StoreService stops its inventory writes before calling onIdle, so
    // that an instance started once the name is free never meets one of
    // this instance's writes after its own (SERVICE.md §21.5).
    service.onIdle = [&] {
        connection->releaseName(name);
        loop.quit(0);
    };
    auto lost = connection->subscribe(disconnected, [&loop, &service](const base::dbus::Message&) {
        // Another bus may already serve a new instance.
        service.stopInventoryWrites();
        loop.quit(1);
    });
    if (auto ok = service.start(); !ok) {
        std::fprintf(stderr, "cutefish-store-service: %s\n", ok.error().message().c_str());
        return 1;
    }
    return loop.run();
}
