// SPDX-License-Identifier: Apache-2.0
#include "kanban/db/database.hpp"
#include "kanban/db/kanban_entity.hpp"
#include "kanban/dto/rule_dto.hpp"

#include "testkit/db_fixture.hpp"

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <Lightweight/DataMapper/Pool.hpp>

#include <catch2/catch_test_macros.hpp>

using morph::ladder::testkit::DbFixture;

TEST_CASE("The kanban schema creates all ten tables", "[kanban][schema]") {
    DbFixture fixture;
    auto mapper = ::Lightweight::GlobalDataMapperPool().Acquire();
    // A query against each table must not throw -- proves the table exists
    // and is reachable through Lightweight's ODBC connection, the same
    // smoke-test shape bookmarks'/polls' own schema tests use.
    for (const auto* table :
         {"projects", "project_has_roles", "board_columns", "swimlanes", "tasks", "comments", "board_applied_ops",
          "board_events", "rules", "task_tags"}) {
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

TEST_CASE("A rules table row round-trips through the DataMapper", "[kanban][schema]") {
    DbFixture fixture;
    auto mapper = ::Lightweight::GlobalDataMapperPool().Acquire();

    kanban::db::ProjectRecord project;
    project.name = "Automation Board";
    project.archived = false;
    project.createdAtMs = 1000;
    mapper->Create(project);
    REQUIRE(project.id.Value() > 0);

    kanban::db::RuleRecord rule;
    rule.project = project.id.Value();
    rule.triggerEvent = "TaskMovedToColumn";
    rule.conditionField = "columnId";
    rule.conditionValue = "42";
    rule.mutationType = "AddTag";
    rule.mutationValue = "urgent";
    mapper->Create(rule);
    REQUIRE(rule.id.Value() > 0);

    auto rows = mapper->Query<kanban::db::RuleRecord>()
                    .Where(::Lightweight::FieldNameOf<&kanban::db::RuleRecord::id>, "=", rule.id.Value())
                    .All();
    REQUIRE(rows.size() == 1);
    CHECK(rows.front().project.Value() == project.id.Value());
    CHECK(std::string{rows.front().triggerEvent.Value()} == "TaskMovedToColumn");
    CHECK(std::string{rows.front().conditionField.Value()} == "columnId");
    CHECK(std::string{rows.front().conditionValue.Value()} == "42");
    CHECK(std::string{rows.front().mutationType.Value()} == "AddTag");
    CHECK(std::string{rows.front().mutationValue.Value()} == "urgent");
}

TEST_CASE("A task_tags row round-trips through the DataMapper", "[kanban][schema]") {
    // Task 14: the smallest storage answer that makes RuleMutationType::
    // AddTag/RemoveTag mean something concrete -- see board_model.cpp's
    // execute(ApplyTagMutation) for the model-level behavior this table backs.
    DbFixture fixture;
    auto mapper = ::Lightweight::GlobalDataMapperPool().Acquire();

    kanban::db::ProjectRecord project;
    project.name = "Tag Board";
    project.archived = false;
    project.createdAtMs = 1000;
    mapper->Create(project);

    kanban::db::ColumnRecord column;
    column.project = project.id.Value();
    column.name = "Done";
    column.wipLimit = 0;
    column.sortOrder = 0;
    mapper->Create(column);

    kanban::db::SwimlaneRecord swimlane;
    swimlane.project = project.id.Value();
    swimlane.name = "Default";
    swimlane.sortOrder = 0;
    mapper->Create(swimlane);

    kanban::db::TaskRecord task;
    task.project = project.id.Value();
    task.column = column.id.Value();
    task.swimlane = swimlane.id.Value();
    task.title = "Ship it";
    task.position = 0;
    task.createdAtMs = 1000;
    mapper->Create(task);
    REQUIRE(task.id.Value() > 0);

    kanban::db::TaskTagRecord tag;
    tag.task = task.id.Value();
    tag.tag = "closed";
    mapper->Create(tag);
    REQUIRE(tag.id.Value() > 0);

    auto rows = mapper->Query<kanban::db::TaskTagRecord>()
                    .Where(::Lightweight::FieldNameOf<&kanban::db::TaskTagRecord::id>, "=", tag.id.Value())
                    .All();
    REQUIRE(rows.size() == 1);
    CHECK(rows.front().task.Value() == task.id.Value());
    CHECK(std::string{rows.front().tag.Value()} == "closed");
}

TEST_CASE("CreateRule/GetRules/DeleteRule validate() and enum string round-trips", "[kanban][schema]") {
    // DTO-only compile/behavior proof for this task -- CreateRule/GetRules/
    // DeleteRule's actual model-level execute() is a later task's job (rule
    // evaluation), not this one.
    kanban::CreateRule createRule{
        .projectId = kanban::ProjectId{1}, .triggerColumnId = kanban::ColumnId{2},
        .mutationType = kanban::RuleMutationType::AddTag, .mutationValue = "urgent"};
    CHECK(createRule.validate());

    kanban::CreateRule missingColumn{.projectId = kanban::ProjectId{1}, .mutationValue = "urgent"};
    CHECK_FALSE(missingColumn.validate());

    kanban::CreateRule emptyValue{
        .projectId = kanban::ProjectId{1}, .triggerColumnId = kanban::ColumnId{2}};
    CHECK_FALSE(emptyValue.validate());

    kanban::GetRules getRules{.projectId = kanban::ProjectId{1}};
    CHECK(getRules.validate());
    CHECK_FALSE(kanban::GetRules{}.validate());

    kanban::DeleteRule deleteRule{.ruleId = kanban::RuleId{7}};
    CHECK(deleteRule.validate());
    CHECK_FALSE(kanban::DeleteRule{}.validate());

    CHECK(kanban::ruleMutationTypeToString(kanban::RuleMutationType::AddTag) == "AddTag");
    CHECK(kanban::ruleMutationTypeToString(kanban::RuleMutationType::RemoveTag) == "RemoveTag");
    CHECK(kanban::ruleMutationTypeFromString("AddTag") == kanban::RuleMutationType::AddTag);
    CHECK(kanban::ruleMutationTypeFromString("RemoveTag") == kanban::RuleMutationType::RemoveTag);
    CHECK(kanban::ruleMutationTypeFromString("bogus") == kanban::RuleMutationType::AddTag);

    CHECK(kanban::ruleTriggerEventToString(kanban::RuleTriggerEvent::TaskMovedToColumn) == "TaskMovedToColumn");
    CHECK(kanban::ruleTriggerEventFromString("TaskMovedToColumn") == kanban::RuleTriggerEvent::TaskMovedToColumn);

    kanban::GetRulesResult result;
    result.rules.push_back(kanban::RuleView{
        .id = kanban::RuleId{7}, .triggerColumnId = kanban::ColumnId{2},
        .mutationType = kanban::RuleMutationType::AddTag, .mutationValue = "urgent"});
    CHECK(result.rules.size() == 1);

    kanban::CreateRuleResult createResult{.ruleId = kanban::RuleId{7}};
    CHECK(createResult.ruleId == kanban::RuleId{7});
}
