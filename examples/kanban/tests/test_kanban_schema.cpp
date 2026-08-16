// SPDX-License-Identifier: Apache-2.0
#include "kanban/db/database.hpp"

#include "testkit/db_fixture.hpp"

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <Lightweight/DataMapper/Pool.hpp>

#include <catch2/catch_test_macros.hpp>

using morph::ladder::testkit::DbFixture;

TEST_CASE("The kanban schema creates all eight tables", "[kanban][schema]") {
    DbFixture fixture;
    auto mapper = ::Lightweight::GlobalDataMapperPool().Acquire();
    // A query against each table must not throw -- proves the table exists
    // and is reachable through Lightweight's ODBC connection, the same
    // smoke-test shape bookmarks'/polls' own schema tests use.
    for (const auto* table :
         {"projects", "project_has_roles", "board_columns", "swimlanes", "tasks", "comments", "board_applied_ops",
          "board_events"}) {
        ::Lightweight::SqlStatement stmt{mapper->Connection()};
        REQUIRE_NOTHROW(stmt.ExecuteDirect(std::string{"SELECT COUNT(*) FROM "} + table));
    }
}
