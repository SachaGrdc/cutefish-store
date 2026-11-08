// cutefish-store-service: the D-Bus service of Cutefish Store (SERVICE.md).
// Activated on the session bus as org.cutefish.Store; stops when idle.

#include "options.h"
#include "storeservice.h"

#ifdef CUTEFISH_STORE_HAVE_PACKAGEKIT
#include "pkbackend.h"
#endif

#include <base/dbus.h>
#include <base/event_loop.h>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace cutefish;

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
    auto lost = connection->subscribe(disconnected, [&loop](const base::dbus::Message&) { loop.quit(1); });
    const std::string name = options->busName;
    // The only place that knows which backend the service uses.
    std::unique_ptr<store::Backend> backend;
#ifdef CUTEFISH_STORE_HAVE_PACKAGEKIT
    if (options->packagekit != store::service::PackageKitBus::Off) {
        store::service::PackageKitBackend::Options pk;
        pk.bus = options->packagekit == store::service::PackageKitBus::System ? store::service::PkTransport::Bus::System
                                                                               : store::service::PkTransport::Bus::Session;
        pk.swcatalog = options->swcatalog;
        backend = std::make_unique<store::service::PackageKitBackend>(loop, std::move(pk));
    }
#endif
    store::service::StoreService service(loop, *connection, std::move(options).value(), std::move(backend));
    service.onIdle = [&] {
        connection->releaseName(name);
        loop.quit(0);
    };
    if (auto ok = service.start(); !ok) {
        std::fprintf(stderr, "cutefish-store-service: %s\n", ok.error().message().c_str());
        return 1;
    }
    return loop.run();
}
