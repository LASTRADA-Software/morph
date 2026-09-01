// SPDX-License-Identifier: Apache-2.0
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <glaze/glaze.hpp>
#include <morph/forms/forms.hpp>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include "kanban/core/errors.hpp"
#include "kanban/core/types.hpp"
#include "kanban/dto/board_dto.hpp"

TEST_CASE("OpenBoard requires an engaged projectId", "[kanban][dto]") {
    CHECK_FALSE(kanban::OpenBoard{.projectId = {}}.validate());
    CHECK(kanban::OpenBoard{.projectId = kanban::ProjectId{1}}.validate());
}

TEST_CASE("CreateColumn requires a non-empty, bounded name", "[kanban][dto]") {
    CHECK_FALSE(kanban::CreateColumn{.name = ""}.validate());
    CHECK_FALSE(kanban::CreateColumn{.name = std::string(101, 'x')}.validate());
    CHECK(kanban::CreateColumn{.name = "To Do", .wipLimit = 0}.validate());
}

TEST_CASE("CreateTask requires engaged columnId/swimlaneId and a bounded title", "[kanban][dto]") {
    kanban::CreateTask valid{.columnId = kanban::ColumnId{1}, .swimlaneId = kanban::SwimlaneId{1}, .title = "Fix bug"};
    CHECK(valid.validate());

    kanban::CreateTask noColumn = valid;
    noColumn.columnId = {};
    CHECK_FALSE(noColumn.validate());

    kanban::CreateTask emptyTitle = valid;
    emptyTitle.title = "";
    CHECK_FALSE(emptyTitle.validate());
}

TEST_CASE("MoveTaskPosition requires an engaged taskId/columnId/swimlaneId and a non-negative position",
          "[kanban][dto]") {
    kanban::MoveTaskPosition valid{.taskId = kanban::TaskId{1},
                                   .columnId = kanban::ColumnId{1},
                                   .swimlaneId = kanban::SwimlaneId{1},
                                   .position = 0};
    CHECK(valid.validate());

    kanban::MoveTaskPosition negative = valid;
    negative.position = -1;
    CHECK_FALSE(negative.validate());

    kanban::MoveTaskPosition noTask = valid;
    noTask.taskId = {};
    CHECK_FALSE(noTask.validate());
}

TEST_CASE("AddComment requires an engaged taskId and non-empty body", "[kanban][dto]") {
    CHECK_FALSE(kanban::AddComment{.taskId = {}, .body = "hi"}.validate());
    CHECK_FALSE(kanban::AddComment{.taskId = kanban::TaskId{1}, .body = ""}.validate());
    CHECK(kanban::AddComment{.taskId = kanban::TaskId{1}, .body = "hi"}.validate());
}

TEST_CASE("kanban::BoardEventId: fromRowId rejects the one value it cannot represent", "[kanban][types]") {
    // 0 is BoardEventId's "not entered" sentinel, so an event id of 0 would
    // arrive as *absent* and a real event would read as "no event"
    // (morph#215). Row ids start at 1, so this never fires in practice -- it
    // turns a silent collapse into a loud failure at the boundary.
    CHECK_THROWS_AS(kanban::BoardEventId::fromRowId(0), kanban::KanbanError);
}

TEST_CASE("kanban::BoardEventId: fromRowId wraps an ordinary row id unchanged", "[kanban][types]") {
    // Control case: without it the check above would pass against a factory
    // that rejected every input.
    auto const event = kanban::BoardEventId::fromRowId(42);
    CHECK(event.hasValue());
    CHECK(*event == 42);
}

// ─── The schema declarations the board's rendered forms rest on ────────────
//
// `optionalFields`, `explicitSubmit` and `fieldMetadata` are only meaningful
// through `morph::forms::schemaJson<A>()`'s output, which is what
// `DynamicForm` actually reads (bookmarks' test_bookmark_dto.cpp makes the
// same distinction for the same reason). Checking the C++ declarations without
// checking the emitted schema would prove nothing about the screen.

TEST_CASE("Every board form's schema opts out of auto-submit", "[kanban][dto][forms][issue344]") {
    // Without `x-submitMode: explicit` the renderer fires the moment every
    // required field is engaged -- which for a one-field form means submitting
    // on the first typed character. Every action below is side-effectful.
    for (const auto& schema :
         {::morph::forms::schemaJson<kanban::CreateColumn>(), ::morph::forms::schemaJson<kanban::CreateSwimlane>(),
          ::morph::forms::schemaJson<kanban::CreateTask>(), ::morph::forms::schemaJson<kanban::AddComment>()}) {
        CAPTURE(schema);
        glz::generic_u64 dom{};
        REQUIRE_FALSE(glz::read_json(dom, schema));
        REQUIRE(dom.contains("x-submitMode"));
        CHECK(dom["x-submitMode"].get_string() == "explicit");
    }
}

TEST_CASE("CreateColumn's schema leaves wipLimit out of required", "[kanban][dto][forms][issue344]") {
    // `wipLimit = 0` means unlimited and is the aggregate default, so the form
    // must not gate its Submit button on a number the user has no reason to
    // type. Left blank, DynamicForm omits the key and the default applies.
    const auto schema = ::morph::forms::schemaJson<kanban::CreateColumn>();
    CAPTURE(schema);
    glz::generic_u64 dom{};
    REQUIRE_FALSE(glz::read_json(dom, schema));
    REQUIRE(dom.contains("required"));
    const auto& required = dom["required"].get_array();
    CHECK(std::ranges::none_of(required, [](const auto& entry) { return entry.get_string() == "wipLimit"; }));
    // `name` genuinely is required, so this is not vacuously passing on an
    // empty or absent list.
    CHECK(std::ranges::any_of(required, [](const auto& entry) { return entry.get_string() == "name"; }));
}

TEST_CASE("The context ids CreateTask and AddComment carry are hidden, and still required",
          "[kanban][dto][forms][issue344]") {
    // Hidden means "the view supplies this, not the user" -- x-hidden on the
    // property, and *still* in `required`, because the payload must carry it.
    // A hidden field dropped from `required` would let a form submit with no
    // column at all; a required field that is not hidden would put a raw row
    // id on screen for a user to type.
    struct Case {
        std::string schema;
        std::vector<std::string_view> hidden;
        std::string_view visible;
    };
    const std::vector<Case> cases{
        {::morph::forms::schemaJson<kanban::CreateTask>(), {"columnId", "swimlaneId"}, "title"},
        {::morph::forms::schemaJson<kanban::AddComment>(), {"taskId"}, "body"},
    };
    for (const auto& testCase : cases) {
        CAPTURE(testCase.schema);
        glz::generic_u64 dom{};
        REQUIRE_FALSE(glz::read_json(dom, testCase.schema));
        const auto& required = dom["required"].get_array();
        for (const auto& field : testCase.hidden) {
            const auto& property = dom["properties"][std::string{field}];
            REQUIRE(property.contains("x-hidden"));
            CHECK(property["x-hidden"].get_boolean());
            CHECK(std::ranges::any_of(required, [&](const auto& entry) { return entry.get_string() == field; }));
        }
        const auto& shown = dom["properties"][std::string{testCase.visible}];
        CHECK_FALSE(shown.contains("x-hidden"));
        CHECK(
            std::ranges::any_of(required, [&](const auto& entry) { return entry.get_string() == testCase.visible; }));
    }
}
