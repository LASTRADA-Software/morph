// SPDX-License-Identifier: Apache-2.0
//
// morph#566: a `MoveTaskPosition` whose *post-commit* work throws must not
// report the move as failed -- the row it wrote is already committed.
//
// The CI observation this pins was a board whose `execute(MoveTaskPosition)`
// threw and whose move was nonetheless observed applied
// (`test_kanban_offline.cpp:672`, `movedCount == 1` under 32-way contention).
// That needs no race inside the commit path: `execute()` commits, then runs
// `logAction`, `evaluateRules` and a final `buildState` -- every one of which
// can throw -- and before this fix nothing between them and the caller
// distinguished "the move did not happen" from "the move happened and the
// follow-on work did not".
//
// The contended `SQLITE_BUSY` that produced it in CI is not reproducible on
// demand. The *shape* is, deterministically: make the first step of the tail
// (`logAction`) throw, and assert the caller is told the truth.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <morph/journal/action_log.hpp>
#include <morph/session/session.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "kanban/models/board_model.hpp"
#include "kanban/models/project_admin_model.hpp"
#include "testkit/db_fixture.hpp"

using morph::ladder::testkit::DbFixture;

namespace {

/// @brief See `test_board_model.cpp`'s identical `contextFor`/`ScopedPrincipal`
///        pair for why this is not a designated initializer.
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

/// @brief An action log whose `append()` throws.
///
/// `BoardModel::logAction` is the first statement of `MoveTaskPosition`'s
/// post-commit tail, so a throwing `append()` is the cheapest deterministic
/// stand-in for the contended `SQLITE_BUSY` that CI hit further down it. What
/// is being tested is the containment of the whole tail, not this particular
/// step: `evaluateRules` and the final `buildState` sit behind the same
/// `try`.
class ThrowingActionLog : public morph::journal::IActionLog {
public:
    void append(morph::journal::LogEntry /*entry*/) override {
        ++appendAttempts;
        throw std::runtime_error{"ThrowingActionLog: append refused"};
    }

    void flush() override {}

    [[nodiscard]] std::vector<morph::journal::LogEntry> entries(std::string_view /*entityKey*/ = {}) const override {
        return {};
    }

    int appendAttempts = 0;
};

/// @brief Creates a project owned by @p principal, named after it.
/// @param principal The owning principal; also supplies the project's name, so
///        this takes one string rather than two interchangeable ones.
/// @return The new project's id.
[[nodiscard]] kanban::ProjectId createProjectOwnedBy(const std::string& principal) {
    const ScopedPrincipal principalScope{principal};
    kanban::ProjectAdminModel admin;
    return admin.execute(kanban::CreateProject{.name = principal + "'s board"}).id;
}

/// @brief The column a task currently sits in, read back from the database
///        rather than from the result the call under test returned.
[[nodiscard]] kanban::ColumnId columnOfTask(kanban::BoardModel& model, kanban::TaskId task) {
    const auto state = model.execute(kanban::GetBoardState{});
    const auto found = std::ranges::find_if(state.tasks, [task](const auto& row) { return row.id == task; });
    REQUIRE(found != state.tasks.end());
    return found->columnId;
}

}  // namespace

TEST_CASE("MoveTaskPosition reports success when only its post-commit tail fails", "[kanban][board][morph#566]") {
    const DbFixture fixture;
    const auto projectId = createProjectOwnedBy("alice");

    kanban::BoardModel model;
    const ScopedPrincipal alice{"alice"};
    model.execute(kanban::OpenBoard{.projectId = projectId});

    const auto columnA = model.execute(kanban::CreateColumn{.name = "Todo", .wipLimit = 0}).columns.back().id;
    const auto columnB = model.execute(kanban::CreateColumn{.name = "Doing", .wipLimit = 0}).columns.back().id;
    const auto swimlane = model.execute(kanban::CreateSwimlane{.name = "Default"}).swimlanes.back().id;
    const auto task =
        model.execute(kanban::CreateTask{.columnId = columnA, .swimlaneId = swimlane, .title = "A"}).tasks.back().id;

    REQUIRE(columnOfTask(model, task) == columnA);

    auto log = std::make_shared<ThrowingActionLog>();
    model.attachActionLog(log, std::to_string(*projectId));

    // The call must not throw: whatever the tail does, the move itself
    // committed before the tail began.
    const auto moved = model.execute(kanban::MoveTaskPosition{
        .taskId = task, .columnId = columnB, .swimlaneId = swimlane, .position = 0, .opId = ""});

    // The tail really did fail -- otherwise this case would pass for the
    // uninteresting reason that nothing threw.
    CHECK(log->appendAttempts >= 1);

    // And the result the caller got describes the move it actually made.
    const auto movedRow = std::ranges::find_if(moved.tasks, [task](const auto& row) { return row.id == task; });
    REQUIRE(movedRow != moved.tasks.end());
    CHECK(movedRow->columnId == columnB);

    // Detached before the read-back below, so `GetBoardState`'s own journal
    // append does not hit the throwing log.
    model.attachActionLog(nullptr, std::to_string(*projectId));
    CHECK(columnOfTask(model, task) == columnB);
}
