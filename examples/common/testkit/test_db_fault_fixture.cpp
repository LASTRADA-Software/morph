// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include "testkit/db_fault_fixture.hpp"
#include "testkit/db_fixture.hpp"

#include <Lightweight/Lightweight.hpp>
#include <Lightweight/SqlScopedLock.hpp>

#include <chrono>
#include <stdexcept>

// Mirrors Lightweight's own MigrationLockTests.cpp: two distinct
// `SqlConnection` instances are required to prove genuine cross-session
// contention. SQL Server's `sp_getapplock` (with `@LockOwner=Session`) and
// PostgreSQL's `pg_advisory_lock` are both reentrant on the same connection,
// so acquiring twice through one session would succeed — cross-session
// contention is the path that throws on every backend (including SQLite,
// whose lock table just rejects the duplicate). `DbFaultFixture`'s own
// `_lockingConnection` and each test's `secondConn`/`thirdConn` below are
// always separate `SqlConnection` instances for exactly this reason.

TEST_CASE("DbFaultFixture: a second session contending on the same lock name throws",
          "[ladder][testkit][db][fault]") {
    morph::ladder::testkit::DbFaultFixture fault{"probe_lock"};

    Lightweight::SqlConnection secondConn;
    REQUIRE_THROWS_AS(
        (Lightweight::SqlScopedLock{secondConn, "probe_lock", std::chrono::milliseconds{50}}),
        std::runtime_error);
}

TEST_CASE("DbFaultFixture::lockName() reports the name it was constructed with", "[ladder][testkit][db][fault]") {
    morph::ladder::testkit::DbFaultFixture fault{"probe_lock_named"};
    REQUIRE(fault.lockName() == "probe_lock_named");

    // A test can use lockName() to name the exact lock it holds when
    // contending against it, instead of hard-coding the string twice.
    Lightweight::SqlConnection secondConn;
    REQUIRE_THROWS_AS(
        (Lightweight::SqlScopedLock{secondConn, fault.lockName(), std::chrono::milliseconds{50}}),
        std::runtime_error);
}

TEST_CASE("DbFaultFixture: a different lock name is unaffected", "[ladder][testkit][db][fault]") {
    morph::ladder::testkit::DbFaultFixture fault{"probe_lock_a"};

    Lightweight::SqlConnection secondConn;
    Lightweight::SqlScopedLock other{secondConn, "probe_lock_b", std::chrono::milliseconds{50}};
    REQUIRE(other.IsLocked());
}

TEST_CASE("DbFaultFixture: releasing the fixture (going out of scope) lets a later acquisition succeed",
          "[ladder][testkit][db][fault]") {
    {
        morph::ladder::testkit::DbFaultFixture fault{"probe_lock_scoped"};
        Lightweight::SqlConnection secondConn;
        REQUIRE_THROWS_AS(
            (Lightweight::SqlScopedLock{secondConn, "probe_lock_scoped", std::chrono::milliseconds{50}}),
            std::runtime_error);
    }
    // fault is destroyed here — its SqlScopedLock releases.
    Lightweight::SqlConnection thirdConn;
    Lightweight::SqlScopedLock reacquire{thirdConn, "probe_lock_scoped", std::chrono::milliseconds{50}};
    REQUIRE(reacquire.IsLocked());
}
