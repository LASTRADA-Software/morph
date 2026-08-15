// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include "testkit/db_fixture.hpp"
#include "testkit/db_pool_drain.hpp"

#include <Lightweight/SqlConnection.hpp>
#include <Lightweight/SqlStatement.hpp>

#include <atomic>

// drainPoolIdleMappers()'s whole point is making the *next* Acquire()
// trigger Lightweight::SqlConnection::PostConnect() -- there is no direct
// "was this connection fresh" observable exposed to a morph consumer (see
// db_busy_fixture.hpp's own note: Pool::IdleCount()/WaiterCount() exist only
// under Lightweight's internal BUILD_TESTS macro, never defined for code
// linking against Lightweight as a library), so PostConnect firing (or not)
// via SetPostConnectedHook is the only observable morph itself has, and is
// exactly the mechanism the busy-timeout tests this helper protects rely on.

TEST_CASE("drainPoolIdleMappers makes the next Acquire() trigger PostConnect",
          "[ladder][testkit][db][pool]") {
    morph::ladder::testkit::DbFixture fixture;

    // Warm the pool with at least one real, connected mapper first -- an
    // ordinary Acquire()+destroy, so a later Acquire() has something idle to
    // (wrongly) hand back if the drain below did not actually work.
    (void) ::Lightweight::GlobalDataMapperPool().Acquire();

    // The drain itself acquires Config.maxSize mappers, and however many of
    // those the pool did not already have idle each connect for real (firing
    // PostConnect of their own) -- that is drainPoolIdleMappers() doing
    // exactly its job, not noise to suppress, but it means the hook must be
    // installed *after* the drain to isolate the one acquisition this test
    // actually cares about.
    auto drained = morph::ladder::testkit::drainPoolIdleMappers();

    std::atomic<int> postConnectCount{0};
    ::Lightweight::SqlConnection::SetPostConnectedHook(
        [&postConnectCount](::Lightweight::SqlConnection&) { postConnectCount.fetch_add(1); });

    {
        // Still holding every idle mapper drainPoolIdleMappers() acquired --
        // the pool's idle list is empty right now, so this Acquire() must
        // construct a fresh DataMapper, which must connect, which must fire
        // PostConnect() exactly once.
        auto fresh = ::Lightweight::GlobalDataMapperPool().Acquire();
        CHECK(postConnectCount.load() == 1);
        // fresh still in scope here -- released below, after the assertion
        // above already observed the fresh acquisition.
    }

    ::Lightweight::SqlConnection::ResetPostConnectedHook();
}
