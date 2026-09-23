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
//
// morph#757 widened that from the four domain refusals to *any* pre-commit
// failure. Until then every handler ended `catch (const KanbanError&)`, so
// `Outcome::Failed` was written for this rung's own refusals and for nothing
// else -- an `std::invalid_argument` out of `std::stoull`, one of
// Lightweight's SQL exceptions from a pre-commit query, or a `std::bad_alloc`
// all reached the caller having journalled nothing at all. Nothing in
// "a rejected attempt is itself audit-worthy" depends on the exception's
// type; only the `catch` clause did. The last three cases in this file cover
// the widened path, including the failure mode widening it introduces: the
// journal write is itself fallible, and on a failure path a throw from it
// must not replace the exception the caller came for.

#include <Lightweight/Lightweight.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <exception>
#include <memory>
#include <morph/journal/action_log.hpp>
#include <morph/session/session.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "kanban/models/board_model.hpp"
#include "kanban/models/project_admin_model.hpp"
#include "testkit/db_busy_fixture.hpp"
#include "testkit/db_fixture.hpp"
#include "testkit/db_pool_drain.hpp"

using morph::ladder::testkit::DbBusyFixture;
using morph::ladder::testkit::DbFixture;
using morph::ladder::testkit::drainPoolIdleMappers;

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

/// @brief Installs a short SQLite `busy_timeout` on every connection opened
///        while it is alive, and restores the default afterwards.
///
/// A verbatim copy of `pastebin`'s `test_paste_model.cpp` helper of the same
/// name, for the same reason it exists there: `SqlConnection::PostConnect()`
/// issues `PRAGMA busy_timeout = 60000` on every new SQLite connection, so a
/// statement colliding with `DbBusyFixture`'s held lock would block for a
/// real minute before SQLite gave up. `BoardModel` acquires its connection
/// from `GlobalDataMapperPool()` inside `execute()`, which no test can reach
/// directly; the post-connected hook is the seam that works from outside.
/// Pair it with `drainPoolIdleMappers()` -- see `db_busy_fixture.hpp`'s
/// "`SetPostConnectedHook` and `GlobalDataMapperPool()`" note -- or the hook
/// may never fire on the acquisition that matters.
class ScopedShortBusyTimeout {
public:
    explicit ScopedShortBusyTimeout(int milliseconds) {
        ::Lightweight::SqlConnection::SetPostConnectedHook([milliseconds](::Lightweight::SqlConnection& connection) {
            ::Lightweight::SqlStatement stmt{connection};
            (void)stmt.ExecuteDirect("PRAGMA busy_timeout = " + std::to_string(milliseconds));
        });
    }
    ~ScopedShortBusyTimeout() { ::Lightweight::SqlConnection::ResetPostConnectedHook(); }

    ScopedShortBusyTimeout(const ScopedShortBusyTimeout&) = delete;
    ScopedShortBusyTimeout& operator=(const ScopedShortBusyTimeout&) = delete;
    ScopedShortBusyTimeout(ScopedShortBusyTimeout&&) = delete;
    ScopedShortBusyTimeout& operator=(ScopedShortBusyTimeout&&) = delete;
};

/// @brief An action log whose `append()` throws -- the same stand-in
///        `test_board_post_commit_tail.cpp` uses, here armed on the *failure*
///        path instead of the post-commit one.
///
/// This is the shape that makes widening the catch dangerous: `logFailure`
/// writes to the journal while the original exception is still in flight, so
/// a journal that is itself failing (a `std::bad_alloc`, or a `SQLITE_BUSY`
/// still active against a file-backed log) throws from inside the handler for
/// the original throw. Unless that is contained, the caller stops being told
/// what went wrong and is told about the logging instead.
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

/// @brief How a call under test ended, as a value rather than as control flow.
enum class Ending : std::uint8_t {
    /// @brief It returned. For the cases below that is always a failure of the
    ///        arrangement, never a pass.
    Returned,
    /// @brief It threw one of this rung's four `KanbanError` types -- the only
    ///        thing `Outcome::Failed` was written for before morph#757.
    DomainRefusal,
    /// @brief It threw something else. This is the class morph#757 is about.
    NonDomainFailure,
};

/// @brief Runs @p call and reports how it ended.
///
/// A free function rather than a `try`/`catch` written out in each case: every
/// Catch2 assertion macro expands to a `try`/`catch` of its own, so a handler
/// written inline pushes the enclosing `TEST_CASE` over
/// `readability-function-cognitive-complexity`'s threshold on its own.
/// @tparam Call Nullary callable.
/// @param call The call under test.
/// @return Which of the three endings happened.
template <typename Call>
[[nodiscard]] Ending classify(Call&& call) {
    try {
        std::forward<Call>(call)();
    } catch (const kanban::KanbanError&) {
        return Ending::DomainRefusal;
    } catch (const std::exception&) {
        return Ending::NonDomainFailure;
    }
    return Ending::Returned;
}

/// @brief Runs @p call and returns the `what()` of whatever it threw.
///
/// Sibling of `classify` above, for the one case where the exception's *text*
/// is what distinguishes the original failure from the journal's.
/// @tparam Call Nullary callable.
/// @param call The call under test.
/// @return The thrown exception's `what()`, or an empty string if @p call
///         returned normally.
template <typename Call>
[[nodiscard]] std::string messageFrom(Call&& call) {
    try {
        std::forward<Call>(call)();
    } catch (const std::exception& error) {
        return error.what();
    }
    return {};
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

// ── morph#757: the failure that journalled nothing ───────────────────────────

TEST_CASE("A non-KanbanError pre-commit failure leaves a Failed journal entry", "[kanban][journal][morph#757]") {
    DbFixture fixture;
    const auto projectId = createProjectAs("alice", "Sprint Board");
    kanban::BoardModel model;
    const ScopedPrincipal alice{"alice"};
    model.execute(kanban::OpenBoard{.projectId = projectId});

    auto log = std::make_shared<morph::journal::InMemoryActionLog>();
    model.attachActionLog(log, std::to_string(*projectId));

    // The failure morph#566 observed in CI on this rung, arranged rather than
    // waited for: a real second connection holding a real write lock, so the
    // handler's own pre-commit statement takes a genuine `SQLITE_BUSY` past a
    // genuine (shortened) busy timeout. Not a mock and not a fault-injection
    // seam in the model -- the handler cannot tell this apart from the
    // contention that produced the original report.
    const ScopedShortBusyTimeout shortTimeout{200};
    auto drained = drainPoolIdleMappers();
    const DbBusyFixture busy{"board_columns"};

    // Not REQUIRE_THROWS_AS: the point of the case is the exception's type,
    // so "it was not a KanbanError" has to be asserted rather than assumed
    // from the name of whatever Lightweight throws. A `DomainRefusal` here
    // would mean the case was testing nothing morph#757 changed.
    REQUIRE(classify([&] { (void)model.execute(kanban::CreateColumn{.name = "To Do", .wipLimit = 0}); }) ==
            Ending::NonDomainFailure);

    // The forced-fresh acquisition this case needed has happened, so the
    // drained mappers can go back (`drainPoolIdleMappers`'s own doc comment).
    drained.clear();

    // Before morph#757 there were zero entries here: the handler's catch
    // named `KanbanError` and this exception is not one, so it left with the
    // journal untouched.
    const auto entries = log->entries();
    REQUIRE(entries.size() == 1);
    CHECK(entries.front().outcome == morph::journal::Outcome::Failed);
    CHECK_FALSE(entries.front().error.empty());
    CHECK(entries.front().result.empty());
    CHECK(entries.front().actionType == std::string{morph::model::ActionTraits<kanban::CreateColumn>::typeId()});
    CHECK(entries.front().principal == "alice");
    // Pins *which* clause wrote the entry. `LogEntry` has no field separating
    // "the board refused you" from "the board fell over", so the non-domain
    // clause prefixes its text; without this the case would pass just as well
    // on an entry written by the domain-refusal clause, which is not what
    // happened here.
    CHECK(entries.front().error.starts_with("unexpected failure: "));
}

TEST_CASE("A journal that throws on the failure path does not replace the refusal", "[kanban][journal][morph#757]") {
    DbFixture fixture;
    const auto projectId = createProjectAs("alice", "Sprint Board");
    kanban::BoardModel model;
    const ScopedPrincipal alice{"alice"};
    model.execute(kanban::OpenBoard{.projectId = projectId});

    auto log = std::make_shared<ThrowingActionLog>();
    model.attachActionLog(log, std::to_string(*projectId));

    // An empty name is a ValidationError, thrown before the database is
    // touched. Journalling it is what fails here, and `ValidationError` --
    // not `ThrowingActionLog`'s `std::runtime_error` -- is what the caller
    // must still receive.
    REQUIRE_THROWS_AS(model.execute(kanban::CreateColumn{.name = "", .wipLimit = 0}), kanban::ValidationError);

    // Without this the case would pass on a tree where the journal was never
    // consulted on the failure path at all -- which is the tree morph#757
    // started from.
    CHECK(log->appendAttempts >= 1);
}

TEST_CASE("A journal that throws on the failure path does not replace a non-domain failure",
          "[kanban][journal][morph#757]") {
    DbFixture fixture;
    const auto projectId = createProjectAs("alice", "Sprint Board");
    kanban::BoardModel model;
    const ScopedPrincipal alice{"alice"};
    model.execute(kanban::OpenBoard{.projectId = projectId});

    auto log = std::make_shared<ThrowingActionLog>();
    model.attachActionLog(log, std::to_string(*projectId));

    const ScopedShortBusyTimeout shortTimeout{200};
    auto drained = drainPoolIdleMappers();
    const DbBusyFixture busy{"board_columns"};

    // The compounded case: a non-domain failure -- the one that only reaches
    // the journal at all after morph#757 -- while the journal it now writes
    // to is itself failing. `ValidationError` above proves containment by the
    // exception's type; here there is no type to lean on, so the message is
    // what distinguishes the original failure from the logging one.
    const auto reached =
        messageFrom([&] { (void)model.execute(kanban::CreateColumn{.name = "To Do", .wipLimit = 0}); });
    drained.clear();

    CHECK(log->appendAttempts >= 1);
    // Empty would mean it returned instead of throwing, which the held write
    // lock makes impossible -- checked so that the membership test below
    // cannot pass by having nothing to search.
    CHECK_FALSE(reached.empty());
    CHECK_FALSE(reached.contains("ThrowingActionLog"));
}
