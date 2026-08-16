// SPDX-License-Identifier: Apache-2.0
#include "kanban/db/database.hpp"
#include "kanban/db/kanban_entity.hpp"

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

TEST_CASE("A project row round-trips through the DataMapper", "[kanban][schema]") {
    DbFixture fixture;
    auto mapper = ::Lightweight::GlobalDataMapperPool().Acquire();

    kanban::db::ProjectRecord project;
    project.name = "Sprint Board";
    project.archived = false;
    project.createdAtMs = 1000;
    mapper->Create(project);
    REQUIRE(project.id.Value() > 0);

    auto rows = mapper->Query<kanban::db::ProjectRecord>()
                    .Where(::Lightweight::FieldNameOf<&kanban::db::ProjectRecord::id>, "=", project.id.Value())
                    .All();
    REQUIRE(rows.size() == 1);
    CHECK(std::string{rows.front().name.Value()} == "Sprint Board");
    CHECK_FALSE(rows.front().archived.Value());
}

TEST_CASE("TaskRecord has no relation-typed member -- Update() must compile", "[kanban][schema]") {
    // Compile-time proof, mirroring bookmarks::db::BookmarkRecord's identical
    // test: DataMapper::Update()'s non-reflection path calls IsModified() on
    // every member via EnumerateRecordMembers, which does not compile if any
    // member is a HasMany/HasManyThrough relation field.
    DbFixture fixture;
    auto mapper = ::Lightweight::GlobalDataMapperPool().Acquire();
    kanban::db::TaskRecord task;
    task.title = "Do the thing";
    task.position = 0;
    mapper->Create(task);
    task.title = "Do the other thing";
    REQUIRE_NOTHROW(mapper->Update(task));
}
