// SPDX-License-Identifier: Apache-2.0
//
// Coverage for this rung's payload-shape fingerprints
// (`morph::model::payloadShapeString` / `payloadFingerprint`,
// `morph/core/payload_schema.hpp`).
//
// Every one of kanban's strong ids carries its own `glz::meta` -- on the wire
// each *is* its nullable underlying scalar, which is what makes
// `BRIDGE_REGISTER_ACTION` on a DTO carrying one compile at all
// (`kanban/core/types.hpp`). The cost, stated in
// `morph/core/payload_shape_tag.hpp` and in `docs/spec/journal/journal.md`'s
// "Custom-codec types name themselves", is that a custom-codec type has no
// reflected members for `payloadShape` to decompose: absent a declared
// `morph::model::PayloadShapeTag` it renders as the bare opaque `x`,
// indistinguishable from every other such type.
//
// That is not cosmetic here. `MoveTaskPosition{TaskId, ColumnId, SwimlaneId}`
// -- design spec §1's exactly-once centrepiece, and the action the offline
// queue replays -- is three ids in a row, and `CreateTask{ColumnId,
// SwimlaneId}` two. Without declared tags each renders as a row of
// interchangeable `x`s: exchanging two of those fields' types, which is what
// an id rename or a copy-paste between adjacent lines produces, leaves the
// fingerprint bit-identical. All these ids are `std::optional<std::int64_t>`
// on the wire, so the JSON is byte-identical across such a swap too and no
// decode on any path can catch it -- the shape tag is the only place it is
// visible at all. `journal::replay()`'s mismatch gate then has nothing to
// fire on, and the recorded integers decode into the wrong slots: a task
// filed under a column id, moved into the column named by a swimlane.
//
// These cases pin the tags that close it, and the refusal that follows.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <memory>
#include <morph/core/payload_schema.hpp>
#include <morph/core/registry.hpp>
#include <morph/journal/action_log.hpp>
#include <morph/journal/journal.hpp>
#include <morph/session/session.hpp>
#include <string>
#include <vector>

#include "kanban/core/types.hpp"
#include "kanban/dto/auth_dto.hpp"
#include "kanban/models/board_model.hpp"
#include "kanban/models/project_admin_model.hpp"
#include "testkit/db_fixture.hpp"

using morph::ladder::testkit::DbFixture;
using morph::model::payloadFingerprint;
using morph::model::payloadShapeString;

// A named namespace, not an anonymous one: glaze's traditional reflection
// derives member names from a pointer-to-member mangling that requires the
// reflected type to have linkage, so a payload struct declared in an anonymous
// namespace does not compile -- see ledger's identical fixture namespace
// (`examples/ledger/tests/test_ledger_payload_shape.cpp`) for the same
// rationale, spelled out in full there. The name is this file's own, so the
// fixtures cannot collide with another translation unit's in the one
// ladder_kanban_tests binary.
namespace kanban_payload_shape_fixtures {

/// @brief `MoveTaskPosition` with its `taskId` and `columnId` fields' *types*
///        exchanged and the member names left alone -- the shape a build that
///        made that one edit would stamp its entries with.
///
///        Never registered: it exists only to produce a fingerprint, which is
///        the whole of what a retained journal hands a later reader.
struct MoveTaskPositionIdsSwapped {
    kanban::ColumnId taskId;
    kanban::TaskId columnId;
    kanban::SwimlaneId swimlaneId;
    std::int64_t position = 0;
    std::string opId;
};

/// @brief `CreateTask` with `columnId` and `swimlaneId` exchanged, same idea.
struct CreateTaskIdsSwapped {
    kanban::SwimlaneId columnId;
    kanban::ColumnId swimlaneId;
    std::string title;
};

}  // namespace kanban_payload_shape_fixtures

using kanban_payload_shape_fixtures::CreateTaskIdsSwapped;
using kanban_payload_shape_fixtures::MoveTaskPositionIdsSwapped;

namespace {

/// @brief A `Context` carrying only @p principal -- not a designated
///        initializer, for the reason `test_board_model.cpp`'s own
///        `contextFor` gives (`-Wmissing-designated-field-initializers` under
///        `-Weverything`).
[[nodiscard]] morph::session::Context contextFor(std::string principal) {
    morph::session::Context ctx;
    ctx.principal = std::move(principal);
    return ctx;
}

class ScopedPrincipal {
public:
    explicit ScopedPrincipal(std::string principal) : _ctx{contextFor(std::move(principal))}, _scope{_ctx} {}

private:
    morph::session::Context _ctx;
    morph::session::detail::ScopedContext _scope;
};

}  // namespace

// ── The tags themselves ──────────────────────────────────────────────────────

TEST_CASE("Every kanban strong id and the auth token render a distinct payload shape",
          "[kanban][journal][payload_shape]") {
    INFO("ProjectId    -> " << payloadShapeString<kanban::ProjectId>());
    INFO("ColumnId     -> " << payloadShapeString<kanban::ColumnId>());
    INFO("TaskId       -> " << payloadShapeString<kanban::TaskId>());
    INFO("SwimlaneId   -> " << payloadShapeString<kanban::SwimlaneId>());
    INFO("TagId        -> " << payloadShapeString<kanban::TagId>());
    INFO("RuleId       -> " << payloadShapeString<kanban::RuleId>());
    INFO("AttachmentId -> " << payloadShapeString<kanban::AttachmentId>());
    INFO("BoardEventId -> " << payloadShapeString<kanban::BoardEventId>());
    INFO("AuthToken    -> " << payloadShapeString<kanban::AuthToken>());

    const std::vector<std::string> shapes{
        payloadShapeString<kanban::ProjectId>(),    payloadShapeString<kanban::ColumnId>(),
        payloadShapeString<kanban::TaskId>(),       payloadShapeString<kanban::SwimlaneId>(),
        payloadShapeString<kanban::TagId>(),        payloadShapeString<kanban::RuleId>(),
        payloadShapeString<kanban::AttachmentId>(), payloadShapeString<kanban::BoardEventId>(),
        payloadShapeString<kanban::AuthToken>(),
    };

    // None of them may still be the bare opaque tag, and no two may collide.
    for (const auto& shape : shapes) {
        CHECK(shape != "x");
    }
    auto sorted = shapes;
    std::ranges::sort(sorted);
    CHECK(std::ranges::adjacent_find(sorted) == sorted.end());
}

// ── What the tags buy: the swaps the fingerprint can now see ─────────────────

TEST_CASE("MoveTaskPosition's taskId and columnId are not interchangeable", "[kanban][journal][payload_shape]") {
    INFO("MoveTaskPosition -> " << payloadShapeString<kanban::MoveTaskPosition>());
    INFO("ids swapped      -> " << payloadShapeString<MoveTaskPositionIdsSwapped>());

    CHECK(payloadShapeString<kanban::MoveTaskPosition>() != payloadShapeString<MoveTaskPositionIdsSwapped>());
    CHECK(payloadFingerprint<kanban::MoveTaskPosition>() != payloadFingerprint<MoveTaskPositionIdsSwapped>());
}

TEST_CASE("CreateTask's columnId and swimlaneId are not interchangeable", "[kanban][journal][payload_shape]") {
    INFO("CreateTask  -> " << payloadShapeString<kanban::CreateTask>());
    INFO("ids swapped -> " << payloadShapeString<CreateTaskIdsSwapped>());

    CHECK(payloadShapeString<kanban::CreateTask>() != payloadShapeString<CreateTaskIdsSwapped>());
    CHECK(payloadFingerprint<kanban::CreateTask>() != payloadFingerprint<CreateTaskIdsSwapped>());
}

// ── The refusal ──────────────────────────────────────────────────────────────

TEST_CASE("replay() refuses a MoveTaskPosition entry stamped by a build whose ids were swapped",
          "[kanban][journal][payload_shape]") {
    // A real recorded entry, not a hand-built one: this also pins that
    // BoardModel::logAction() stamps the fingerprint this build computes.
    DbFixture fixture;
    kanban::ProjectId projectId;
    {
        const ScopedPrincipal alice{"alice"};
        kanban::ProjectAdminModel admin;
        projectId = admin.execute(kanban::CreateProject{.name = "Sprint Board"}).id;
    }

    kanban::BoardModel model;
    const ScopedPrincipal alice{"alice"};
    model.execute(kanban::OpenBoard{.projectId = projectId});
    const auto columnId = model.execute(kanban::CreateColumn{.name = "To Do", .wipLimit = 0}).columns.front().id;
    const auto swimlaneId = model.execute(kanban::CreateSwimlane{.name = "Default"}).swimlanes.front().id;
    const auto taskId = model.execute(kanban::CreateTask{.columnId = columnId, .swimlaneId = swimlaneId, .title = "A"})
                            .tasks.back()
                            .id;

    // Attach the log only now, so it holds the move and nothing that had to
    // happen first.
    auto log = std::make_shared<morph::journal::InMemoryActionLog>();
    model.attachActionLog(log, std::to_string(*projectId));
    model.execute(kanban::MoveTaskPosition{
        .taskId = taskId, .columnId = columnId, .swimlaneId = swimlaneId, .position = 0, .opId = ""});

    auto entries = log->entries();
    const auto moveType = std::string{morph::model::ActionTraits<kanban::MoveTaskPosition>::typeId()};
    auto recorded = std::ranges::find_if(entries, [&](const auto& e) { return e.actionType == moveType; });
    REQUIRE(recorded != entries.end());
    REQUIRE(recorded->schema == payloadFingerprint<kanban::MoveTaskPosition>());

    // Re-stamp it as the swapped-id build would have. The payload bytes are
    // byte-identical either way -- three JSON integers under three field names
    // -- so the fingerprint is the only evidence that the recorded `taskId` is
    // not this build's `taskId`, and replay() must refuse rather than decode
    // one id into the other's slot.
    std::vector<morph::journal::LogEntry> mismatched{*recorded};
    mismatched.front().schema = payloadFingerprint<MoveTaskPositionIdsSwapped>();
    REQUIRE_THROWS_AS(morph::journal::replay("BoardModel", mismatched), morph::journal::SchemaMismatchError);
}
