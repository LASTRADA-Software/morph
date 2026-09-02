// SPDX-License-Identifier: Apache-2.0
//
// A `BoardModel` that was registered but never pointed at a board.
//
// This is the state a client reaches by registering the handler and
// dispatching anything before `OpenBoard` -- `BoardModel` is keyed by project
// id (`BRIDGE_MODEL_KEY` on `OpenBoard::projectId`, `AllowShared`), so a fresh
// registration is attached to no board at all. Every `execute()` overload
// already opens with a `_projectIdStr.has_value()` guard carrying the right
// message, the one `polls::PollModel` answers with.
//
// Those guards were dead over a server, and the reason is not in this file's
// subject at all. `ModelFactory::create` attaches the process-wide default
// action log to every newly constructed holder *with an empty `entityKey`*
// (`include/morph/core/model.hpp`, and kanban's `App` installs such a log via
// `morph::journal::setActionLog`), and `attachActionLog` used to write that
// key straight into `_projectIdStr` -- the same member that records which
// board the handler is attached to. The optional came out *engaged with an
// empty string*, so `has_value()` was true, every guard fell through, and the
// first `std::stoull(*_projectIdStr)` threw `std::invalid_argument{"stoull"}`,
// which went on the wire as though it were a domain error.
//
// The in-process tests missed it because they all attach before acting, and
// the two writers of `_projectIdStr` only disagree when they do not. So these
// cases enter that state deliberately: `attachActionLog(log, {})` is character
// for character the call `ModelFactory::create` makes.
//
// Filed as morph#368; the out-of-process counterpart is
// `scripts/scenario/scenarios/kanban/a-board-must-be-opened-before-it-answers.scenario`.

#include <catch2/catch_test_macros.hpp>
#include <exception>
#include <memory>
#include <morph/journal/action_log.hpp>
#include <morph/session/session.hpp>
#include <string>
#include <utility>

#include "kanban/core/errors.hpp"
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

/// @brief Runs @p call and returns the `what()` of whatever it threw, or the
///        empty string if it returned normally.
///
/// Catches `std::exception`, not `kanban::NotFound`, on purpose. The defect
/// this file pins threw `std::invalid_argument{"stoull"}` from
/// `std::stoull("")` -- outside kanban's own hierarchy entirely -- so a
/// `CHECK_THROWS_AS(..., kanban::NotFound)` would have reported it as an
/// escaping exception rather than as the wrong message, and the assertion
/// that matters here is *which text a client receives*. Returning the text
/// rather than asserting inside lets each case below read as one comparison.
template <typename Fn>
[[nodiscard]] std::string errorTextOf(Fn&& call) {
    try {
        std::forward<Fn>(call)();
    } catch (const std::exception& error) {
        return std::string{error.what()};
    }
    return {};
}

/// @brief A handler in exactly the state a fresh registration leaves it in:
///        default-constructed, then handed the process-wide default action
///        log with an empty `entityKey`, the way `ModelFactory::create` does.
class UnattachedBoard {
public:
    UnattachedBoard() { _model.attachActionLog(std::make_shared<morph::journal::InMemoryActionLog>(), {}); }

    [[nodiscard]] kanban::BoardModel& get() { return _model; }

private:
    kanban::BoardModel _model;
};

}  // namespace

// The two guards issue #368 singles out: they are written, they are correct,
// and over a server they never fired. Split into their own cases so a
// regression in either is named by the failing test rather than by an
// assertion line.
TEST_CASE("GetActivity on an unattached handler names the action and OpenBoard", "[kanban][model][unattached]") {
    DbFixture fixture;
    UnattachedBoard board;
    const ScopedPrincipal alice{"alice"};

    CHECK(errorTextOf([&] { return board.get().execute(kanban::GetActivity{}); })
          == "GetActivity: handler was never attached via OpenBoard");
}

TEST_CASE("GetRules on an unattached handler names the action and OpenBoard", "[kanban][model][unattached]") {
    DbFixture fixture;
    const auto projectId = createProjectAs("alice", "Unopened");
    UnattachedBoard board;
    const ScopedPrincipal alice{"alice"};

    // A real, existing projectId in the action: the refusal is about the
    // *handler* never having been attached, not about the argument.
    CHECK(errorTextOf([&] { return board.get().execute(kanban::GetRules{.projectId = projectId}); })
          == "GetRules: handler was never attached via OpenBoard");
}

// The rest of the wire surface issue #368 lists. Every one of these answered
// the bare string "stoull" over a server.
TEST_CASE("Every action on an unattached handler is refused by name, never with \"stoull\"",
          "[kanban][model][unattached]") {
    DbFixture fixture;
    const auto projectId = createProjectAs("alice", "Unopened");
    UnattachedBoard board;
    auto& model = board.get();
    const ScopedPrincipal alice{"alice"};

    CHECK(errorTextOf([&] { return model.execute(kanban::GetBoardState{}); })
          == "GetBoardState: handler was never attached via OpenBoard");
    CHECK(errorTextOf([&] { return model.execute(kanban::CreateColumn{.name = "Todo", .wipLimit = 0}); })
          == "CreateColumn: handler was never attached via OpenBoard");
    CHECK(errorTextOf([&] { return model.execute(kanban::CreateSwimlane{.name = "Lane"}); })
          == "CreateSwimlane: handler was never attached via OpenBoard");
    CHECK(errorTextOf([&] {
        return model.execute(
            kanban::CreateTask{.columnId = kanban::ColumnId{1}, .swimlaneId = kanban::SwimlaneId{1}, .title = "None"});
    }) == "CreateTask: handler was never attached via OpenBoard");
    CHECK(errorTextOf([&] {
        return model.execute(kanban::MoveTaskPosition{.taskId = kanban::TaskId{1},
                                                      .columnId = kanban::ColumnId{1},
                                                      .swimlaneId = kanban::SwimlaneId{1},
                                                      .position = 0,
                                                      .opId = ""});
    }) == "MoveTaskPosition: handler was never attached via OpenBoard");
    CHECK(errorTextOf([&] {
        return model.execute(kanban::AddComment{.taskId = kanban::TaskId{1}, .body = "Into the void"});
    }) == "AddComment: handler was never attached via OpenBoard");
    CHECK(errorTextOf([&] { return model.execute(kanban::GetEventsSince{.lastEventId = {}}); })
          == "GetEventsSince: handler was never attached via OpenBoard");
    CHECK(errorTextOf([&] { return model.execute(kanban::GetActivity{}); })
          == "GetActivity: handler was never attached via OpenBoard");
    CHECK(errorTextOf([&] { return model.execute(kanban::GetRules{.projectId = projectId}); })
          == "GetRules: handler was never attached via OpenBoard");
}

// Validation still runs ahead of the attach guard, exactly as it did before:
// a malformed action is refused for being malformed. Pinned here because the
// fix moves *when* the attach guard bites, and the outermost gate must not
// have moved with it.
TEST_CASE("A malformed action on an unattached handler is refused for being malformed",
          "[kanban][model][unattached]") {
    DbFixture fixture;
    UnattachedBoard board;
    auto& model = board.get();
    const ScopedPrincipal alice{"alice"};

    CHECK(errorTextOf([&] { return model.execute(kanban::CreateColumn{.name = "", .wipLimit = 0}); })
          == "CreateColumn: a bounded, non-empty name is required");
    CHECK(errorTextOf([&] { return model.execute(kanban::GetEventsSince{.lastEventId = kanban::BoardEventId{-1}}); })
          == "GetEventsSince: malformed request");
}

// The keyed attach path is the one `attachActionLog` legitimately sets
// `_projectIdStr` from -- `Remote::attachLogIfConfigured` passes a real
// project id as the `entityKey` when the client registered with a non-empty
// contextKey. Refusing the *empty* key must not have disturbed it.
TEST_CASE("A non-empty entityKey still attaches the handler to that board", "[kanban][model][unattached]") {
    DbFixture fixture;
    const auto projectId = createProjectAs("alice", "Keyed");
    kanban::BoardModel model;
    const ScopedPrincipal alice{"alice"};

    model.attachActionLog(std::make_shared<morph::journal::InMemoryActionLog>(), std::to_string(*projectId));

    const auto state = model.execute(kanban::GetBoardState{});
    CHECK(state.projectId == projectId);
    CHECK(state.name == "Keyed");
}

// Attaching a log with an empty key after OpenBoard must not un-attach the
// handler either: the old assignment clobbered a live board id with "", so a
// handler that had been working answered "stoull" from then on.
TEST_CASE("Attaching a log with an empty entityKey does not un-attach an open board",
          "[kanban][model][unattached]") {
    DbFixture fixture;
    const auto projectId = createProjectAs("alice", "Sprint Board");
    kanban::BoardModel model;
    const ScopedPrincipal alice{"alice"};
    model.execute(kanban::OpenBoard{.projectId = projectId});

    model.attachActionLog(std::make_shared<morph::journal::InMemoryActionLog>(), {});

    const auto state = model.execute(kanban::GetBoardState{});
    CHECK(state.projectId == projectId);
}
