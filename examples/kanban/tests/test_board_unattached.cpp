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

    // Both halves matter and they are separate claims: the *type* is what a
    // caller's `.onError(...)` branches on, and it was `std::invalid_argument`
    // -- outside kanban's hierarchy altogether -- while the guard was dead.
    CHECK_THROWS_AS(board.get().execute(kanban::GetActivity{}), kanban::NotFound);
    CHECK(errorTextOf([&] { return board.get().execute(kanban::GetActivity{}); }) ==
          "GetActivity: handler was never attached via OpenBoard");
}

TEST_CASE("GetRules on an unattached handler names the action and OpenBoard", "[kanban][model][unattached]") {
    DbFixture fixture;
    const auto projectId = createProjectAs("alice", "Unopened");
    UnattachedBoard board;
    const ScopedPrincipal alice{"alice"};

    // A real, existing projectId in the action: the refusal is about the
    // *handler* never having been attached, not about the argument.
    CHECK_THROWS_AS(board.get().execute(kanban::GetRules{.projectId = projectId}), kanban::NotFound);
    CHECK(errorTextOf([&] { return board.get().execute(kanban::GetRules{.projectId = projectId}); }) ==
          "GetRules: handler was never attached via OpenBoard");
}

// All fifteen guards, in one case: the nine actions issue #368 lists -- every
// one of which answered the bare string "stoull" over a server -- and the six
// it does not.
TEST_CASE("Every action on an unattached handler is refused by name, never with \"stoull\"",
          "[kanban][model][unattached]") {
    DbFixture fixture;
    const auto projectId = createProjectAs("alice", "Unopened");
    UnattachedBoard board;
    auto& model = board.get();
    const ScopedPrincipal alice{"alice"};

    // `NotFound` for the whole surface, spot-checked on one representative
    // action -- the per-action assertions below are about the text.
    CHECK_THROWS_AS(model.execute(kanban::GetBoardState{}), kanban::NotFound);

    CHECK(errorTextOf([&] { return model.execute(kanban::GetBoardState{}); }) ==
          "GetBoardState: handler was never attached via OpenBoard");
    CHECK(errorTextOf([&] { return model.execute(kanban::CreateColumn{.name = "Todo", .wipLimit = 0}); }) ==
          "CreateColumn: handler was never attached via OpenBoard");
    CHECK(errorTextOf([&] { return model.execute(kanban::CreateSwimlane{.name = "Lane"}); }) ==
          "CreateSwimlane: handler was never attached via OpenBoard");
    CHECK(errorTextOf([&] {
              return model.execute(kanban::CreateTask{
                  .columnId = kanban::ColumnId{1}, .swimlaneId = kanban::SwimlaneId{1}, .title = "None"});
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
    CHECK(errorTextOf([&] { return model.execute(kanban::GetEventsSince{.lastEventId = {}}); }) ==
          "GetEventsSince: handler was never attached via OpenBoard");
    CHECK(errorTextOf([&] { return model.execute(kanban::GetActivity{}); }) ==
          "GetActivity: handler was never attached via OpenBoard");
    CHECK(errorTextOf([&] { return model.execute(kanban::GetRules{.projectId = projectId}); }) ==
          "GetRules: handler was never attached via OpenBoard");

    // The rest of the fifteen. Not in issue #368's list and not driven by the
    // scenario either -- which is exactly why they are here: a regression that
    // re-opened the fall-through in one of these six would otherwise pass
    // every file written to prevent it.
    CHECK(errorTextOf([&] {
              return model.execute(kanban::AddAttachment{.taskId = kanban::TaskId{1},
                                                         .filename = "spec.pdf",
                                                         .contentType = "application/pdf",
                                                         .sizeBytes = 1,
                                                         .storageKey = "blob-1"});
          }) == "AddAttachment: handler was never attached via OpenBoard");
    CHECK(errorTextOf([&] { return model.execute(kanban::GetAttachments{.taskId = kanban::TaskId{1}}); }) ==
          "GetAttachments: handler was never attached via OpenBoard");
    CHECK(errorTextOf([&] {
              return model.execute(kanban::RemoveAttachment{.attachmentId = kanban::AttachmentId{1}});
          }) == "RemoveAttachment: handler was never attached via OpenBoard");
    CHECK(errorTextOf([&] {
              return model.execute(kanban::CreateRule{.projectId = projectId,
                                                      .triggerColumnId = kanban::ColumnId{1},
                                                      .mutationType = kanban::RuleMutationType::AddTag,
                                                      .mutationValue = "urgent"});
          }) == "CreateRule: handler was never attached via OpenBoard");
    CHECK(errorTextOf([&] { return model.execute(kanban::DeleteRule{.ruleId = kanban::RuleId{1}}); }) ==
          "DeleteRule: handler was never attached via OpenBoard");
    CHECK(errorTextOf([&] {
              return model.execute(kanban::ApplyTagMutation{
                  .taskId = kanban::TaskId{1}, .mutationType = kanban::RuleMutationType::AddTag, .tag = "urgent"});
          }) == "ApplyTagMutation: handler was never attached via OpenBoard");
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

    CHECK(errorTextOf([&] { return model.execute(kanban::CreateColumn{.name = "", .wipLimit = 0}); }) ==
          "CreateColumn: a bounded, non-empty name is required");
    CHECK(errorTextOf([&] {
              return model.execute(kanban::GetEventsSince{.lastEventId = kanban::BoardEventId{-1}});
          }) == "GetEventsSince: malformed request");
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

// The second key that reaches `attachActionLog` is the client's `contextKey`,
// forwarded verbatim off the wire by `Remote::attachLogIfConfigured` -- so it
// is arbitrary text, not necessarily a project id. Registering with
// `contextKey=foo` used to make `_projectIdStr == "foo"`, pass all fifteen
// guards, and reach `std::stoull("foo")`: the same bare "stoull" reply as the
// empty key, one step over. Reproduced over a live server before this was
// fixed.
TEST_CASE("A contextKey that is not a project id does not attach the handler", "[kanban][model][unattached]") {
    DbFixture fixture;
    kanban::BoardModel model;
    const ScopedPrincipal alice{"alice"};

    model.attachActionLog(std::make_shared<morph::journal::InMemoryActionLog>(), "foo");

    CHECK_THROWS_AS(model.execute(kanban::GetBoardState{}), kanban::NotFound);
    CHECK(errorTextOf([&] { return model.execute(kanban::GetBoardState{}); }) ==
          "GetBoardState: handler was never attached via OpenBoard");
}

// A partially-numeric key is the dangerous one, and the reason the check is
// `std::from_chars` over the whole string rather than a `stoull` in a `try`:
// `stoull("5x")` succeeds, returning 5, so the handler would have silently
// attached to project 5 -- a board the client never named. Per-action
// `requireRole` still gates access, so this was a wrong-target bug rather than
// an authorization hole, but a client cannot be told which board it got.
TEST_CASE("A partly-numeric contextKey does not attach to the board its prefix names", "[kanban][model][unattached]") {
    DbFixture fixture;
    const auto projectId = createProjectAs("alice", "Not yours to guess");
    kanban::BoardModel model;
    const ScopedPrincipal alice{"alice"};

    // `<real project id>x` -- `stoull` would have parsed the prefix and
    // attached to exactly this project.
    model.attachActionLog(std::make_shared<morph::journal::InMemoryActionLog>(), std::to_string(*projectId) + "x");

    CHECK(errorTextOf([&] { return model.execute(kanban::GetBoardState{}); }) ==
          "GetBoardState: handler was never attached via OpenBoard");
}

// A zero-padded key parses, and that is exactly the problem: `OpenBoard`
// always stores `std::to_string` of a real row id, so the attach invariant
// ("engaged implies it parses as a project id, spelled the way `OpenBoard`
// would spell it") assumes one canonical spelling per project. A handler
// attached as "007" would answer for project 7 under a spelling no
// `OpenBoard` call for that project would ever produce, two spellings of one
// attach state where the invariant assumes there is only ever one. So the
// spelling has to be canonical, not merely parseable.
TEST_CASE("A zero-padded contextKey does not attach the handler", "[kanban][model][unattached]") {
    DbFixture fixture;
    const auto projectId = createProjectAs("alice", "Padded");
    kanban::BoardModel model;
    const ScopedPrincipal alice{"alice"};

    model.attachActionLog(std::make_shared<morph::journal::InMemoryActionLog>(), "00" + std::to_string(*projectId));

    CHECK(errorTextOf([&] { return model.execute(kanban::GetBoardState{}); }) ==
          "GetBoardState: handler was never attached via OpenBoard");
}

// Attaching a log with an empty key after OpenBoard must not un-attach the
// handler either: the old assignment clobbered a live board id with "", so a
// handler that had been working answered "stoull" from then on.
TEST_CASE("Attaching a log with an empty entityKey does not un-attach an open board", "[kanban][model][unattached]") {
    DbFixture fixture;
    const auto projectId = createProjectAs("alice", "Sprint Board");
    kanban::BoardModel model;
    const ScopedPrincipal alice{"alice"};
    model.execute(kanban::OpenBoard{.projectId = projectId});

    auto log = std::make_shared<morph::journal::InMemoryActionLog>();
    model.attachActionLog(log, {});

    const auto state = model.execute(kanban::GetBoardState{});
    CHECK(state.projectId == projectId);

    // #422's own version of this hazard: an empty entityKey must not pull
    // _entityKeyStr away from the board _projectIdStr still names either --
    // otherwise every entry logged after this second attach call would carry
    // entityKey="" while execute(GetActivity) keeps reading entries by
    // *_projectIdStr, making them invisible to it despite the handler still
    // answering as attached. Proven by round-tripping through GetActivity
    // itself, not by reading the member directly.
    model.execute(kanban::CreateColumn{.name = "Doing", .wipLimit = 0});
    const auto activity = model.execute(kanban::GetActivity{});
    REQUIRE(activity.events.size() == 1);
    CHECK(activity.events.front().actionType == "CreateColumn");
}

// #422's own residue: a `contextKey` the attach guard rejects used to produce
// two different journal entity keys for the *same* attach call -- the raw
// key from a holder-wrapped instance's `_contextKey` (`IModelHolder`,
// `morph/core/model.hpp`, set unconditionally), and the empty string from
// this `BoardModel`'s own `_projectIdStr`, which the guard had correctly
// left disengaged. `_entityKeyStr` closes that gap by taking the same
// unconditional assignment `_contextKey` does, so the two now agree even
// though the attach guard still refuses "foo" as a board.
TEST_CASE("A rejected contextKey still produces the raw key as the journal entityKey, matching IModelHolder",
          "[kanban][model][unattached]") {
    DbFixture fixture;
    kanban::BoardModel model;
    const ScopedPrincipal alice{"alice"};
    auto log = std::make_shared<morph::journal::InMemoryActionLog>();

    model.attachActionLog(log, "foo");

    // CreateColumn, not GetBoardState: GetBoardState is Loggable::No and its
    // execute() has no try/catch at all, so it never reaches logFailure.
    // CreateColumn's own attach guard throw is caught by its execute()'s
    // KanbanError handler, which does. The attach guard still refuses "foo"
    // as a board either way -- this is the existing #368 behavior, unaffected
    // by #422's split.
    CHECK_THROWS_AS(model.execute(kanban::CreateColumn{.name = "To Do", .wipLimit = 0}), kanban::NotFound);

    // The refusal above is a KanbanError, so CreateColumn's own catch block
    // already ran logFailure for it before rethrowing -- exactly the path
    // that used to stamp "" instead of "foo".
    const auto entries = log->entries();
    REQUIRE(entries.size() == 1);
    CHECK(entries.front().entityKey == "foo");
    CHECK(entries.front().outcome == morph::journal::Outcome::Failed);
}
