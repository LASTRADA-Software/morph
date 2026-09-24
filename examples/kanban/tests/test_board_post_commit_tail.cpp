// SPDX-License-Identifier: Apache-2.0
//
// A `MoveTaskPosition` whose *post-commit* work throws must not report the move
// as failed -- the row it wrote is already committed.
//
// The symptom is a board whose `execute(MoveTaskPosition)` threw and whose move
// was nonetheless observed applied (`test_kanban_offline.cpp:672`, `movedCount
// == 1` under 32-way contention). That needs no race inside the
// commit path: `execute()` commits, then runs `logAction`, `evaluateRules` and a
// final `buildState` -- every one of which can throw -- and without the shield
// nothing between them and the caller distinguishes "the move did not happen"
// from "the move happened and the follow-on work did not".
//
// The contended `SQLITE_BUSY` that produces it under CI's parallelism is not
// reproducible on demand. The *shape* is, deterministically: make the first step
// of the tail (`logAction`) throw, and assert the caller is told the truth.

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

// ── The other nine handlers ──────────────────────────────────────────────────
//
// The case above covers `MoveTaskPosition`. All sixteen `BoardModel::execute`
// overloads classify as:
//
//   * six are read-only and open no transaction at all -- `OpenBoard`,
//     `GetBoardState`, `GetAttachments`, `GetRules`, `GetEventsSince`,
//     `GetActivity`. Nothing to shield; wrapping them would add a branch no
//     input can take.
//   * one is `MoveTaskPosition`, above.
//   * **nine commit and then keep working**, and every one of those nine runs
//     `logAction` after its `Commit()`. Four of them (`CreateColumn`,
//     `CreateSwimlane`, `CreateTask`, `AddComment`) also re-read the board
//     with `buildState` for their return value; those reads sit *inside* the
//     transaction rather than being shielded, so a failed re-read rolls the
//     write back instead of leaving a committed mutation with nothing truthful
//     to report. What is left after the commit is `logAction` alone, and that
//     is what `runPostCommitTail` contains at all nine.
//
// `ApplyTagMutation` is the one whose commit is not visible in the handler --
// `applyTagMutationImpl` owns the transaction -- but its `logAction` is
// post-commit all the same, which is why it is in the list.
//
// Each case below asserts the same two things the case above does: the call does
// not throw, and `appendAttempts` proves the tail genuinely ran and genuinely
// failed. Without the second, every one of these would pass on a tree where
// `runPostCommitTail` had been deleted and the log never consulted.

namespace {

/// @brief A board owned by "alice" with two columns, a swimlane and a task,
///        all built *before* any throwing log is attached.
///
/// Member order is the construction order the setup needs: the database first,
/// then a project (whose own principal scope opens and closes inside its
/// initialiser), then the ambient principal the model's RBAC gates read.
struct ShieldedBoard {
    DbFixture db;
    kanban::ProjectId projectId = createProjectOwnedBy("alice");
    ScopedPrincipal alice{"alice"};
    kanban::BoardModel model;
    kanban::ColumnId columnA;
    kanban::ColumnId columnB;
    kanban::SwimlaneId swimlane;
    kanban::TaskId task;

    ShieldedBoard() {
        model.execute(kanban::OpenBoard{.projectId = projectId});
        columnA = model.execute(kanban::CreateColumn{.name = "Todo", .wipLimit = 0}).columns.back().id;
        columnB = model.execute(kanban::CreateColumn{.name = "Doing", .wipLimit = 0}).columns.back().id;
        swimlane = model.execute(kanban::CreateSwimlane{.name = "Default"}).swimlanes.back().id;
        task = model.execute(kanban::CreateTask{.columnId = columnA, .swimlaneId = swimlane, .title = "A"})
                   .tasks.back()
                   .id;
    }

    /// @brief Attaches a log whose `append()` throws, so the next call's
    ///        post-commit tail fails at its first statement.
    /// @return The log, for its `appendAttempts` counter.
    [[nodiscard]] std::shared_ptr<ThrowingActionLog> armThrowingLog() {
        auto log = std::make_shared<ThrowingActionLog>();
        model.attachActionLog(log, std::to_string(*projectId));
        return log;
    }

    /// @brief Detaches the throwing log, so the read-backs below can journal
    ///        their own entries without hitting it.
    void disarm() { model.attachActionLog(nullptr, std::to_string(*projectId)); }
};

}  // namespace

TEST_CASE("CreateColumn reports success when only its post-commit tail fails", "[kanban][board][morph#751]") {
    ShieldedBoard board;
    auto log = board.armThrowingLog();

    const auto after = board.model.execute(kanban::CreateColumn{.name = "Done", .wipLimit = 0});

    CHECK(log->appendAttempts >= 1);
    CHECK(std::ranges::any_of(after.columns, [](const auto& col) { return col.name == "Done"; }));

    board.disarm();
    const auto reread = board.model.execute(kanban::GetBoardState{});
    CHECK(std::ranges::any_of(reread.columns, [](const auto& col) { return col.name == "Done"; }));
}

TEST_CASE("CreateSwimlane reports success when only its post-commit tail fails", "[kanban][board][morph#751]") {
    ShieldedBoard board;
    auto log = board.armThrowingLog();

    const auto after = board.model.execute(kanban::CreateSwimlane{.name = "Expedite"});

    CHECK(log->appendAttempts >= 1);
    CHECK(std::ranges::any_of(after.swimlanes, [](const auto& row) { return row.name == "Expedite"; }));

    board.disarm();
    const auto reread = board.model.execute(kanban::GetBoardState{});
    CHECK(std::ranges::any_of(reread.swimlanes, [](const auto& row) { return row.name == "Expedite"; }));
}

TEST_CASE("CreateTask reports success when only its post-commit tail fails", "[kanban][board][morph#751]") {
    ShieldedBoard board;
    auto log = board.armThrowingLog();

    const auto after =
        board.model.execute(kanban::CreateTask{.columnId = board.columnB, .swimlaneId = board.swimlane, .title = "B"});

    CHECK(log->appendAttempts >= 1);
    CHECK(std::ranges::any_of(after.tasks, [](const auto& row) { return row.title == "B"; }));

    board.disarm();
    const auto reread = board.model.execute(kanban::GetBoardState{});
    CHECK(std::ranges::any_of(reread.tasks, [](const auto& row) { return row.title == "B"; }));
}

TEST_CASE("AddComment reports success when only its post-commit tail fails", "[kanban][board][morph#751]") {
    ShieldedBoard board;
    auto log = board.armThrowingLog();

    const auto after = board.model.execute(kanban::AddComment{.taskId = board.task, .body = "looking into it"});

    CHECK(log->appendAttempts >= 1);
    CHECK(std::ranges::any_of(after.comments, [](const auto& row) { return row.body == "looking into it"; }));

    board.disarm();
    const auto reread = board.model.execute(kanban::GetBoardState{});
    CHECK(std::ranges::any_of(reread.comments, [](const auto& row) { return row.body == "looking into it"; }));
}

TEST_CASE("AddAttachment reports success when only its post-commit tail fails", "[kanban][board][morph#751]") {
    ShieldedBoard board;
    auto log = board.armThrowingLog();

    board.model.execute(kanban::AddAttachment{.taskId = board.task,
                                              .filename = "report.pdf",
                                              .contentType = "application/pdf",
                                              .sizeBytes = 1024,
                                              .storageKey = "abc123"});

    CHECK(log->appendAttempts >= 1);

    board.disarm();
    const auto listed = board.model.execute(kanban::GetAttachments{.taskId = board.task});
    REQUIRE(listed.attachments.size() == 1);
    CHECK(listed.attachments.front().filename == "report.pdf");
}

TEST_CASE("RemoveAttachment reports success when only its post-commit tail fails", "[kanban][board][morph#751]") {
    ShieldedBoard board;
    board.model.execute(kanban::AddAttachment{.taskId = board.task,
                                              .filename = "report.pdf",
                                              .contentType = "application/pdf",
                                              .sizeBytes = 1024,
                                              .storageKey = "abc123"});
    const auto attachmentId = board.model.execute(kanban::GetAttachments{.taskId = board.task}).attachments.front().id;

    auto log = board.armThrowingLog();

    board.model.execute(kanban::RemoveAttachment{.attachmentId = attachmentId});

    CHECK(log->appendAttempts >= 1);

    board.disarm();
    CHECK(board.model.execute(kanban::GetAttachments{.taskId = board.task}).attachments.empty());
}

TEST_CASE("CreateRule reports success when only its post-commit tail fails", "[kanban][board][morph#751]") {
    ShieldedBoard board;
    auto log = board.armThrowingLog();

    const auto created = board.model.execute(kanban::CreateRule{.projectId = board.projectId,
                                                                .triggerColumnId = *board.columnB,
                                                                .mutationType = kanban::RuleMutationType::AddTag,
                                                                .mutationValue = "closed"});

    CHECK(log->appendAttempts >= 1);
    // The id the caller was handed has to be the row's, not a default -- the
    // read that produces it now runs before the commit, so this also pins that
    // reordering.
    CHECK(created.ruleId.hasValue());

    board.disarm();
    const auto listed = board.model.execute(kanban::GetRules{.projectId = board.projectId});
    REQUIRE(listed.rules.size() == 1);
    CHECK(listed.rules.front().id == created.ruleId);
}

TEST_CASE("DeleteRule reports success when only its post-commit tail fails", "[kanban][board][morph#751]") {
    ShieldedBoard board;
    const auto ruleId = board.model
                            .execute(kanban::CreateRule{.projectId = board.projectId,
                                                        .triggerColumnId = *board.columnB,
                                                        .mutationType = kanban::RuleMutationType::AddTag,
                                                        .mutationValue = "closed"})
                            .ruleId;

    auto log = board.armThrowingLog();

    board.model.execute(kanban::DeleteRule{.ruleId = ruleId});

    CHECK(log->appendAttempts >= 1);

    board.disarm();
    CHECK(board.model.execute(kanban::GetRules{.projectId = board.projectId}).rules.empty());
}

TEST_CASE("ApplyTagMutation reports success when only its post-commit tail fails", "[kanban][board][morph#751]") {
    ShieldedBoard board;
    auto log = board.armThrowingLog();

    board.model.execute(kanban::ApplyTagMutation{
        .taskId = board.task, .mutationType = kanban::RuleMutationType::AddTag, .tag = "urgent"});

    CHECK(log->appendAttempts >= 1);

    board.disarm();
    const auto reread = board.model.execute(kanban::GetBoardState{});
    const auto tagged = std::ranges::find_if(reread.tasks, [&](const auto& row) { return row.id == board.task; });
    REQUIRE(tagged != reread.tasks.end());
    CHECK(std::ranges::find(tagged->tags, "urgent") != tagged->tags.end());
}
