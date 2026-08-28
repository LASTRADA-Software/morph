// SPDX-License-Identifier: Apache-2.0
//
// Coverage for this rung's journaled outcomes (`morph::journal::Outcome`,
// `morph/journal/action_log.hpp`).
//
// `BoardModel::logAction` stamps every entry `Outcome::Succeeded`
// unconditionally -- it runs only at the end of a successful `execute()`. A
// caught domain exception (`Forbidden`, `NotFound`, `ValidationError`,
// `Conflict`) previously left `execute()` before reaching that call, so the
// refused attempt left **no journal entry at all** -- on the rung
// `LADDER.md` designates the single polished showcase. lims's
// `SelfJournal::recordFailure` (`include/lims/core/self_journal.hpp`)
// demonstrates the correct pattern: a rejected attempt is itself
// audit-worthy, so it gets its own entry with `Outcome::Failed` and the
// rejecting exception's text in `error`.
//
// These cases pin that a refused mutating action now leaves exactly that
// entry.

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <morph/journal/action_log.hpp>
#include <morph/session/session.hpp>
#include <string>

#include "kanban/models/board_model.hpp"
#include "kanban/models/project_admin_model.hpp"
#include "testkit/db_fixture.hpp"

using morph::ladder::testkit::DbFixture;

namespace {

/// @brief See `test_board_model.cpp`'s identical `contextFor`/
///        `ScopedPrincipal` pair for why this is not a designated
///        initializer.
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

[[nodiscard]] kanban::ProjectId createProjectAs(const std::string& principal, const std::string& name) {
    const ScopedPrincipal p{principal};
    kanban::ProjectAdminModel admin;
    return admin.execute(kanban::CreateProject{.name = name}).id;
}

}  // namespace

TEST_CASE("A WIP-limit Conflict on MoveTaskPosition leaves a Failed journal entry", "[kanban][journal][outcome]") {
    DbFixture fixture;
    const auto projectId = createProjectAs("alice", "Sprint Board");
    kanban::BoardModel model;
    const ScopedPrincipal alice{"alice"};
    model.execute(kanban::OpenBoard{.projectId = projectId});
    const auto col1 = model.execute(kanban::CreateColumn{.name = "To Do", .wipLimit = 0}).columns.front().id;
    const auto afterCol2 = model.execute(kanban::CreateColumn{.name = "Done", .wipLimit = 1});
    const auto col2 = afterCol2.columns.back().id;
    const auto swimlaneId = model.execute(kanban::CreateSwimlane{.name = "Default"}).swimlanes.front().id;
    const auto taskA =
        model.execute(kanban::CreateTask{.columnId = col1, .swimlaneId = swimlaneId, .title = "A"}).tasks.back().id;
    const auto taskB =
        model.execute(kanban::CreateTask{.columnId = col1, .swimlaneId = swimlaneId, .title = "B"}).tasks.back().id;

    // Filling col2 (limit 1) to capacity first.
    model.execute(kanban::MoveTaskPosition{
        .taskId = taskA, .columnId = col2, .swimlaneId = swimlaneId, .position = 0, .opId = ""});

    auto log = std::make_shared<morph::journal::InMemoryActionLog>();
    model.attachActionLog(log, std::to_string(*projectId));

    REQUIRE_THROWS_AS(model.execute(kanban::MoveTaskPosition{
                          .taskId = taskB, .columnId = col2, .swimlaneId = swimlaneId, .position = 1, .opId = ""}),
                      kanban::Conflict);

    auto entries = log->entries();
    REQUIRE(entries.size() == 1);
    CHECK(entries.front().outcome == morph::journal::Outcome::Failed);
    CHECK_FALSE(entries.front().error.empty());
    CHECK(entries.front().result.empty());
    CHECK(entries.front().actionType == std::string{morph::model::ActionTraits<kanban::MoveTaskPosition>::typeId()});
}

TEST_CASE("A ValidationError refusal on CreateColumn leaves a Failed journal entry", "[kanban][journal][outcome]") {
    DbFixture fixture;
    const auto projectId = createProjectAs("alice", "Sprint Board");
    kanban::BoardModel model;
    const ScopedPrincipal alice{"alice"};
    model.execute(kanban::OpenBoard{.projectId = projectId});

    auto log = std::make_shared<morph::journal::InMemoryActionLog>();
    model.attachActionLog(log, std::to_string(*projectId));

    // An empty name fails action.validate() before requireRole/the database
    // are ever reached.
    REQUIRE_THROWS_AS(model.execute(kanban::CreateColumn{.name = "", .wipLimit = 0}), kanban::ValidationError);

    auto entries = log->entries();
    REQUIRE(entries.size() == 1);
    CHECK(entries.front().outcome == morph::journal::Outcome::Failed);
    CHECK_FALSE(entries.front().error.empty());
}
