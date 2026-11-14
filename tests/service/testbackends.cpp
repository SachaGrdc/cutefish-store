// The Backends registry (H.1): registration, lookup by BackendKind, absence,
// and destruction. Uses scripted backends (fakebackend.h); no D-Bus, no
// package manager.

#include "backends.h"
#include "fakebackend.h"

#include <gtest/gtest.h>

#include <base/event_loop.h>

#include <memory>
#include <optional>
#include <vector>

using namespace cutefish::store;
using namespace cutefish::store::service;
using namespace cutefish::store::service::testutil;
namespace base = cutefish::base;

namespace {

TEST(Backends, IsEmptyUntilBackendsAreAdded)
{
    Backends backends("catalog.sqlite", "state.sqlite");
    EXPECT_TRUE(backends.empty());
    EXPECT_TRUE(backends.kinds().empty());
}

TEST(Backends, RegistersAndLooksUpByKind)
{
    base::EventLoop loop;
    Backends backends("catalog.sqlite", "state.sqlite");
    backends.add(std::make_unique<FakeBackend>(loop, BackendKind::PackageKit));
    backends.add(std::make_unique<FakeBackend>(loop, BackendKind::Flatpak));

    EXPECT_FALSE(backends.empty());
    EXPECT_EQ(backends.kinds(), (std::vector<BackendKind>{BackendKind::PackageKit, BackendKind::Flatpak}));
    EXPECT_NE(backends.backend(BackendKind::PackageKit), nullptr);
    EXPECT_NE(backends.backend(BackendKind::Flatpak), nullptr);
    EXPECT_NE(backends.inventory(BackendKind::PackageKit), nullptr);
    EXPECT_NE(backends.inventory(BackendKind::Flatpak), nullptr);
}

TEST(Backends, ReturnsNullForAnAbsentBackend)
{
    base::EventLoop loop;
    Backends backends("catalog.sqlite", "state.sqlite");
    backends.add(std::make_unique<FakeBackend>(loop, BackendKind::PackageKit));
    EXPECT_EQ(backends.backend(BackendKind::Flatpak), nullptr);
    EXPECT_EQ(backends.inventory(BackendKind::Flatpak), nullptr);
}

TEST(Backends, IsNotUnavailableBeforeAnInventoryAnswers)
{
    base::EventLoop loop;
    Backends backends("catalog.sqlite", "state.sqlite");
    backends.add(std::make_unique<FakeBackend>(loop, BackendKind::PackageKit));
    // No check() has run: availability is unknown, not "unavailable".
    EXPECT_FALSE(backends.allUnavailable());
}

TEST(Backends, DestroysBackendsAndInventories)
{
    base::EventLoop loop;
    {
        Backends backends("catalog.sqlite", "state.sqlite");
        backends.add(std::make_unique<FakeBackend>(loop, BackendKind::PackageKit));
        backends.add(std::make_unique<FakeBackend>(loop, BackendKind::Flatpak));
    }
    // The scope ended: the registry and everything it owns were destroyed.
    SUCCEED();
}

} // namespace
