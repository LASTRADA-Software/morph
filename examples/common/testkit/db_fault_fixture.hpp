// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "testkit/db_fixture.hpp"

#include <Lightweight/Lightweight.hpp>
#include <Lightweight/SqlScopedLock.hpp>

#include <chrono>
#include <string>
#include <string_view>

/// @file
/// Genuine cross-session lock contention for the ladder's store-error
/// coverage (examples/IMPLEMENTATION.md rule 5), built directly on
/// Lightweight's own shipped, already-tested `SqlScopedLock` — see this
/// file's class doc comment and the Task 4 design precedent note in the plan
/// this was built from for why that beats a hand-rolled mock or raw SQL.

namespace morph::ladder::testkit {

/// @brief Wraps a `DbFixture` and holds a real `SqlScopedLock` on a second,
///        independent `SqlConnection` to the same shared database, so any
///        code that takes the same-named lock on a *different* connection
///        (the fixture's own default-connection `SqlStatement`s, or a
///        model's `DataMapper`) observes a genuine contention failure.
class DbFaultFixture {
  public:
    /// @param lockName Advisory lock name to contend on — pick one that
    ///        matches what the code under test actually locks (e.g. a
    ///        model's own `SqlScopedLock` name), or a dedicated probe name
    ///        for testing the fixture itself.
    explicit DbFaultFixture(std::string lockName = "morph_ladder_db_fault_fixture")
        : _fixture{}, _lockingConnection{}, _lock{_lockingConnection, lockName, std::chrono::milliseconds{50}} {}

    DbFaultFixture(const DbFaultFixture&) = delete;
    DbFaultFixture& operator=(const DbFaultFixture&) = delete;
    DbFaultFixture(DbFaultFixture&&) = delete;
    DbFaultFixture& operator=(DbFaultFixture&&) = delete;
    ~DbFaultFixture() = default;

    /// @brief The lock name this fixture holds, so a test can attempt to
    ///        acquire the *same* name on its own connection and assert it
    ///        throws. `SqlScopedLock::Name()` itself returns a
    ///        `std::string_view` bound to the lock's own storage, so this
    ///        mirrors that return type rather than the brief's illustrative
    ///        `const std::string&` (which cannot bind to a `string_view`).
    [[nodiscard]] std::string_view lockName() const noexcept { return _lock.Name(); }

  private:
    DbFixture _fixture;
    ::Lightweight::SqlConnection _lockingConnection;
    ::Lightweight::SqlScopedLock _lock;
};

}  // namespace morph::ladder::testkit
