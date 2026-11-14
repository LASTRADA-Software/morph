// SPDX-License-Identifier: Apache-2.0

// Action guards: a check an action declares (`using guard = ...;`), which the framework runs on the decoded action on
// every execution site, after the validator and before Model::execute.

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <chrono>
#include <core/async/Task.hpp>
#include <cstdint>
#include <future>
#include <memory>
#include <morph/core/action_guard.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/coroutine.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/forms/forms.hpp>
#include <morph/journal/action_log.hpp>
#include <morph/journal/journal.hpp>
#include <morph/session/session.hpp>
#include <morph/util/quantity.hpp>
#include <morph/util/rational.hpp>
#include <stdexcept>
#include <string>
#include <vector>

#include "test_support.hpp"

// A miniature unit system, qty * price = total, for the computed-field case. Its own names: a second file-scope enum
// of another test file's name would be an ODR violation.
enum class GuardLineUnit : std::uint8_t { qty, price, total };

template <>
struct morph::units::UnitTraits<GuardLineUnit> {
    static constexpr morph::units::UnitMeta meta(GuardLineUnit unit) noexcept {
        switch (unit) {
            case GuardLineUnit::qty:
                return {.id = "gqty", .display = "qty", .defaultDecimals = 2};
            case GuardLineUnit::price:
                return {.id = "gprice", .display = "$/u", .defaultDecimals = 2};
            case GuardLineUnit::total:
                return {.id = "gtotal", .display = "$", .defaultDecimals = 2};
            default:
                return {.id = "?", .display = "?", .defaultDecimals = 2};
        }
    }
    static constexpr std::array<morph::units::UnitRelation<GuardLineUnit>, 0> relations{};
};

consteval GuardLineUnit operator*(GuardLineUnit lhs, GuardLineUnit rhs) {
    if ((lhs == GuardLineUnit::qty && rhs == GuardLineUnit::price) ||
        (lhs == GuardLineUnit::price && rhs == GuardLineUnit::qty)) {
        return GuardLineUnit::total;
    }
    // NOLINTNEXTLINE(bugprone-std-exception-baseclass): a consteval throw is how a bad product fails to compile.
    throw "unsupported unit product";
}

template <GuardLineUnit U, std::uint32_t Dec = morph::units::UnitTraits<GuardLineUnit>::meta(U).defaultDecimals>
using GuardQ = morph::units::Quantity<U, Dec>;

namespace guard_test {

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables): counters shared between the test thread and the
// strands the actions run on, reset by every case.
namespace {
std::atomic<int> gExecuted{0};
std::atomic<int> gGuardCalls{0};
std::atomic<int> gRefusalsHeard{0};
std::atomic<bool> gNestedRefused{false};
morph::model::detail::IModelHolder* gNestedTarget = nullptr;
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)
}  // namespace

struct WithdrawalLimit;
struct ActiveOnly;
struct ThrowsNonStandard;

/// Guarded on the payload: above 100 needs the "teller" principal.
struct Withdraw {
    using guard = WithdrawalLimit;
    int amount{};
    [[nodiscard]] bool validate() const { return amount > 0; }
};
/// Unguarded.
struct Deposit {
    int amount{};
};
/// Guarded on the instance: refused while the account is frozen.
struct Spend {
    using guard = ActiveOnly;
    int amount{};
};
/// Unguarded: freezes the account.
struct Freeze {};
/// Guarded by a check that throws something that is not a std::exception.
struct Odd {
    using guard = ThrowsNonStandard;
};
/// A Task handler's action, guarded like a withdrawal.
struct Transfer {
    using guard = WithdrawalLimit;
    int amount{};
};
struct Balance {
    int value{};
};

struct TotalAgrees;
/// Carries a computed `total`; its guard refuses a total that disagrees with `qty * price`.
struct Line {
    using guard = TotalAgrees;
    GuardQ<GuardLineUnit::qty> qty;
    GuardQ<GuardLineUnit::price> price;
    GuardQ<GuardLineUnit::total> total;

    static constexpr auto computedFields =
        morph::forms::computeList(morph::forms::computed<&Line::total, &Line::qty, &Line::price>(
            [](const auto& line) { return line.qty * line.price; }));
};
struct LineModel {
    int execute(Line const&) {
        ++gExecuted;
        return 1;
    }
};
struct TotalAgrees {
    static void check(Line const& line) {
        ++gGuardCalls;
        auto const product = line.qty * line.price;
        if (!line.total.hasValue() || !product.hasValue() || !(*line.total == *product)) {
            throw std::runtime_error{"the total disagrees with qty * price"};
        }
    }
};

struct Account {
    bool active = true;
    Balance execute(Withdraw const& action) {
        ++gExecuted;
        return Balance{.value = -action.amount};
    }
    Balance execute(Deposit const& action) {
        ++gExecuted;
        if (gNestedTarget != nullptr) {
            try {
                static_cast<void>(morph::model::detail::defaultDispatcher().dispatch(
                    "T_GuardAccount", "T_GuardWithdraw", *gNestedTarget, R"({"amount":500})"));
            } catch (std::exception const&) {
                gNestedRefused = true;
            }
        }
        return Balance{.value = action.amount};
    }
    Balance execute(Spend const& action) {
        ++gExecuted;
        return Balance{.value = -action.amount};
    }
    Balance execute(Freeze const&) {
        active = false;
        return Balance{};
    }
    Balance execute(Odd const&) {
        ++gExecuted;
        return Balance{};
    }
    void onGuardRefused(Withdraw const&, std::exception const&) { ++gRefusalsHeard; }
    void onGuardRefused(Spend const&, std::exception const&) { ++gRefusalsHeard; }
};

struct AsyncAccount {
    core::async::Task<Balance> execute(Transfer action) {
        ++gExecuted;
        co_return Balance{.value = -action.amount};
    }
};

struct WithdrawalLimit {
    template <typename Action>
    static void check(Action const& action) {
        ++gGuardCalls;
        auto const* const ctx = morph::session::current();
        if (action.amount > 100 && (ctx == nullptr || ctx->principal != "teller")) {
            throw std::runtime_error{"withdrawal over the limit"};
        }
    }
};

struct ActiveOnly {
    static void check(Account const& account, Spend const&) {
        ++gGuardCalls;
        if (!account.active) {
            throw std::runtime_error{"the account is frozen"};
        }
    }
};

struct ThrowsNonStandard {
    // NOLINTNEXTLINE(hicpp-exception-baseclass,bugprone-std-exception-baseclass): a refusal that is not a std::exception is the case under test.
    static void check(Odd const&) { throw 7; }
};

namespace {
void resetCounters() {
    gExecuted = 0;
    gGuardCalls = 0;
    gRefusalsHeard = 0;
    gNestedRefused = false;
    gNestedTarget = nullptr;
}
}  // namespace

}  // namespace guard_test

BRIDGE_REGISTER_MODEL(guard_test::Account, "T_GuardAccount")
BRIDGE_REGISTER_ACTION(guard_test::Account, guard_test::Withdraw, "T_GuardWithdraw")
BRIDGE_REGISTER_ACTION(guard_test::Account, guard_test::Deposit, "T_GuardDeposit")
BRIDGE_REGISTER_ACTION(guard_test::Account, guard_test::Spend, "T_GuardSpend")
BRIDGE_REGISTER_ACTION(guard_test::Account, guard_test::Freeze, "T_GuardFreeze")
BRIDGE_REGISTER_ACTION(guard_test::Account, guard_test::Odd, "T_GuardOdd")
BRIDGE_REGISTER_MODEL(guard_test::AsyncAccount, "T_GuardAsyncAccount")
BRIDGE_REGISTER_ACTION(guard_test::AsyncAccount, guard_test::Transfer, "T_GuardTransfer")
BRIDGE_REGISTER_MODEL(guard_test::LineModel, "T_GuardLineModel")
BRIDGE_REGISTER_ACTION(guard_test::LineModel, guard_test::Line, "T_GuardLine")

static_assert(morph::model::DeclaresGuard<guard_test::Withdraw>);
static_assert(!morph::model::DeclaresGuard<guard_test::Deposit>);
static_assert(morph::model::GuardRefusalObserver<guard_test::Account, guard_test::Withdraw>);
static_assert(!morph::model::GuardRefusalObserver<guard_test::Account, guard_test::Odd>);

namespace {

/// A server, a client bridge over SimulatedRemoteBackend, and a handler, as tests/test_action_validation.cpp builds
/// them.
struct RemoteRig {
    morph::exec::ThreadPoolExecutor pool{2};
    std::shared_ptr<morph::backend::RemoteServer> server = std::make_shared<morph::backend::RemoteServer>(pool);
    morph::exec::MainThreadExecutor owner;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::SimulatedRemoteBackend>(*server), owner};
};

/// A LocalBackend bridge, as tests/test_action_validation.cpp builds it.
struct LocalRig {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::exec::MainThreadExecutor owner;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool), owner};
};

/// A registry-created holder with a log attached, dispatched to directly.
struct Dispatched {
    std::shared_ptr<morph::journal::InMemoryActionLog> log =
        std::make_shared<morph::journal::InMemoryActionLog>(morph::testing::storageOwner());
    std::unique_ptr<morph::model::detail::IModelHolder> holder =
        morph::model::detail::defaultRegistry().create("T_GuardAccount");
    Dispatched() { holder->attachActionLog(log, std::string{}); }
    [[nodiscard]] std::string dispatch(std::string_view actionId, std::string_view body, std::string principal) const {
        morph::session::Context ctx;
        ctx.principal = std::move(principal);
        morph::session::detail::ScopedContext const installed{ctx};
        return morph::model::detail::defaultDispatcher().dispatch("T_GuardAccount", actionId, *holder, body);
    }
};

}  // namespace

// Mutation: remove the runGuardJournaled call from runHandler (the large withdrawal executes on the server path).
TEST_CASE("action guard: the server runner refuses before execute, and runs an unguarded action unchanged",
          "[guard]") {
    guard_test::resetCounters();
    RemoteRig rig;
    morph::bridge::BridgeHandler<guard_test::Account> account{rig.bridge, &rig.owner};
    CHECK_THROWS_WITH(morph::testing::awaitValueOn(rig.owner, account.execute(guard_test::Withdraw{.amount = 500})),
                      Catch::Matchers::ContainsSubstring("over the limit"));
    CHECK(guard_test::gExecuted == 0);
    CHECK(morph::testing::awaitValueOn(rig.owner, account.execute(guard_test::Withdraw{.amount = 50})).value == -50);
    CHECK(morph::testing::awaitValueOn(rig.owner, account.execute(guard_test::Deposit{.amount = 500})).value == 500);
    CHECK(guard_test::gExecuted == 2);
}

// Mutation: remove the runGuardJournaled call from localOp (the large withdrawal executes on the local path).
TEST_CASE("action guard: the in-process bridge refuses before execute, under the call's principal", "[guard]") {
    guard_test::resetCounters();
    LocalRig rig;
    morph::bridge::BridgeHandler<guard_test::Account> account{rig.bridge, &rig.owner};
    CHECK_THROWS_WITH(morph::testing::awaitValueOn(rig.owner, account.execute(guard_test::Withdraw{.amount = 500})),
                      Catch::Matchers::ContainsSubstring("over the limit"));
    CHECK(guard_test::gExecuted == 0);
    CHECK(morph::testing::awaitValueOn(rig.owner, account.execute(guard_test::Deposit{.amount = 500})).value == 500);
    morph::session::Context teller;
    teller.principal = "teller";
    rig.bridge.setDefaultSession(teller);
    CHECK(morph::testing::awaitValueOn(rig.owner, account.execute(guard_test::Withdraw{.amount = 500})).value == -500);
}

// Mutations (each makes the guard count 1 for the invalid withdrawal): in `localOp`, move the runGuardJournaled call
// above the validator check; in `prepareAction`, pass the model in and call the guard just before its `ready` check
// (the server path's validator runs inside `prepareAction`, so `runHandler` cannot be mutated by moving its own call).
TEST_CASE("action guard: the validator runs first, and the guard never sees an invalid action", "[guard]") {
    guard_test::resetCounters();
    LocalRig local;
    morph::bridge::BridgeHandler<guard_test::Account> account{local.bridge, &local.owner};
    CHECK_THROWS_AS(morph::testing::awaitValueOn(local.owner, account.execute(guard_test::Withdraw{.amount = -1})),
                    morph::model::ValidationError);
    Dispatched const dispatched;
    CHECK_THROWS_AS(dispatched.dispatch("T_GuardWithdraw", R"({"amount":-1})", "alice"),
                    morph::model::ValidationError);
    CHECK(guard_test::gGuardCalls == 0);
}

// Mutations: remove recordActionFailure from runGuardJournaled (no entry); skip it when the principal is empty (the
// second entry is missing).
TEST_CASE("action guard: a refusal is journaled as Failed whatever the principal", "[guard][journal]") {
    guard_test::resetCounters();
    Dispatched const dispatched;
    CHECK_THROWS(dispatched.dispatch("T_GuardWithdraw", R"({"amount":500})", "alice"));
    CHECK_THROWS(dispatched.dispatch("T_GuardWithdraw", R"({"amount":500})", ""));
    auto const entries = dispatched.log->entries();
    REQUIRE(entries.size() == 2U);
    CHECK(entries[0].outcome == morph::journal::Outcome::Failed);
    CHECK(entries[0].principal == "alice");
    CHECK_THAT(entries[0].error, Catch::Matchers::ContainsSubstring("over the limit"));
    CHECK(entries[0].result.empty());
    CHECK(entries[1].outcome == morph::journal::Outcome::Failed);
    CHECK(entries[1].principal.empty());
    CHECK(guard_test::gExecuted == 0);
}

// Mutation: widen runGuardJournaled's catch to catch (...) and record "unknown" (an entry appears).
TEST_CASE("action guard: a refusal that is not a std::exception records nothing and reaches the caller",
          "[guard][journal]") {
    guard_test::resetCounters();
    Dispatched const dispatched;
    CHECK_THROWS_AS(dispatched.dispatch("T_GuardOdd", "{}", "alice"), int);
    CHECK(dispatched.log->entries().empty());
    CHECK(guard_test::gExecuted == 0);
}

// Mutations: remove markReconstructing(true) from replay() (the replayed withdrawal is refused and replay throws);
// skip the guard on journal::isReplaying() instead of the holder's mark (the nested withdrawal is not refused).
TEST_CASE("action guard: replay skips the guard for the holder it reconstructs, and only for that holder",
          "[guard][journal]") {
    guard_test::resetCounters();
    auto const other = morph::model::detail::defaultRegistry().create("T_GuardAccount");
    std::vector<morph::journal::LogEntry> entries(2);
    entries[0].modelType = "T_GuardAccount";
    entries[0].actionType = "T_GuardWithdraw";
    entries[0].payload = R"({"amount":500})";
    entries[1].modelType = "T_GuardAccount";
    entries[1].actionType = "T_GuardDeposit";
    entries[1].payload = R"({"amount":1})";
    guard_test::gNestedTarget = other.get();
    auto const replayed = morph::journal::replay("T_GuardAccount", entries);
    guard_test::gNestedTarget = nullptr;
    REQUIRE(replayed != nullptr);
    CHECK_FALSE(replayed->isReconstructing());
    CHECK(guard_test::gNestedRefused);
    CHECK(guard_test::gGuardCalls == 1);
}

// Mutation: call guardAction in evaluateAction (the guard is called).
TEST_CASE("action guard: evaluate does not run the guard", "[guard]") {
    guard_test::resetCounters();
    auto const evaluation =
        morph::model::detail::defaultDispatcher().evaluate("T_GuardAccount", "T_GuardWithdraw", R"({"amount":500})");
    CHECK(evaluation.ready);
    CHECK(guard_test::gGuardCalls == 0);
}

// Mutations: drop the guard from executeChecked (the withdrawal executes); drop onGuardRefused from executeChecked
// (no refusal is heard); drop recomputeAll from executeChecked (the computed-field case below executes).
TEST_CASE("action guard: executeChecked validates, guards and returns execute's result", "[guard]") {
    guard_test::resetCounters();
    guard_test::Account account;
    CHECK_THROWS_WITH(morph::model::executeChecked(account, guard_test::Withdraw{.amount = 500}),
                      Catch::Matchers::ContainsSubstring("over the limit"));
    CHECK_THROWS_AS(morph::model::executeChecked(account, guard_test::Withdraw{.amount = -1}),
                    morph::model::ValidationError);
    CHECK(guard_test::gExecuted == 0);
    CHECK(guard_test::gRefusalsHeard == 1);
    CHECK(morph::model::executeChecked(account, guard_test::Deposit{.amount = 1}).value == 1);
    CHECK(guard_test::gExecuted == 1);

    // A wrong total is recomputed before the guard reads it, so the action executes rather than being refused.
    guard_test::resetCounters();
    guard_test::LineModel lines;
    guard_test::Line line;
    line.qty =
        morph::math::Rational{morph::math::Numerator{3}, morph::math::Denominator{1}, morph::math::DecimalPlaces{2}};
    line.price =
        morph::math::Rational{morph::math::Numerator{5}, morph::math::Denominator{1}, morph::math::DecimalPlaces{2}};
    line.total =
        morph::math::Rational{morph::math::Numerator{999}, morph::math::Denominator{1}, morph::math::DecimalPlaces{2}};
    CHECK(morph::model::executeChecked(lines, line) == 1);
    CHECK(guard_test::gGuardCalls == 1);
    CHECK(guard_test::gExecuted == 1);
}

// Mutation: register every action with guarded = false (Withdraw and Spend report unguarded).
TEST_CASE("action guard: isGuarded answers per registered pair, and false for an unknown one", "[guard]") {
    auto const& dispatcher = morph::model::detail::defaultDispatcher();
    for (auto const& actionId : dispatcher.actionsOf("T_GuardAccount")) {
        CAPTURE(actionId);
        bool const expected = actionId == "T_GuardWithdraw" || actionId == "T_GuardSpend" || actionId == "T_GuardOdd";
        CHECK(morph::model::isGuarded("T_GuardAccount", actionId) == expected);
    }
    CHECK(morph::model::isGuarded("T_GuardAsyncAccount", "T_GuardTransfer"));
    CHECK_FALSE(morph::model::isGuarded("T_GuardAccount", "NoSuchAction"));
    CHECK_FALSE(morph::model::isGuarded("NoSuchModel", "T_GuardWithdraw"));
}

// Mutations: remove the runGuardJournaled call from runTaskHandler (the remote transfer executes); from localTaskOp
// (the local transfer executes).
TEST_CASE("action guard: a Task handler is guarded on both paths, before the Task exists", "[guard][coroutine]") {
    guard_test::resetCounters();
    RemoteRig remote;
    morph::bridge::BridgeHandler<guard_test::AsyncAccount> remoteAccount{remote.bridge, &remote.owner};
    CHECK_THROWS_WITH(
        morph::testing::awaitValueOn(remote.owner, remoteAccount.execute(guard_test::Transfer{.amount = 500})),
        Catch::Matchers::ContainsSubstring("over the limit"));
    LocalRig local;
    morph::bridge::BridgeHandler<guard_test::AsyncAccount> localAccount{local.bridge, &local.owner};
    CHECK_THROWS_WITH(
        morph::testing::awaitValueOn(local.owner, localAccount.execute(guard_test::Transfer{.amount = 500})),
        Catch::Matchers::ContainsSubstring("over the limit"));
    CHECK(guard_test::gExecuted == 0);
    CHECK(morph::testing::awaitValueOn(local.owner, localAccount.execute(guard_test::Transfer{.amount = 5})).value ==
          -5);
}

// Mutation: as runTaskHandler's pre-start catch, call done without runGuardJournaled's record (no entry).
TEST_CASE("action guard: a Task handler's refusal is journaled", "[guard][coroutine][journal]") {
    guard_test::resetCounters();
    auto const log = std::make_shared<morph::journal::InMemoryActionLog>(morph::testing::storageOwner());

    // runTaskHandler, driven by id as tests/test_coroutine_model.cpp drives a Task handler.
    {
        morph::exec::ThreadPoolExecutor pool{2};
        auto const strands = std::make_shared<morph::exec::detail::ModelStrands>(pool);
        morph::exec::detail::ModelId const key{21};
        auto executor = std::make_shared<morph::exec::detail::TaskResumer>(strands, key, morph::session::Context{});
        auto holder = morph::model::detail::defaultRegistry().create("T_GuardAsyncAccount");
        holder->attachActionLog(log, std::string{});
        std::promise<std::exception_ptr> outcome;
        auto settled = outcome.get_future();
        strands->post(key, [&] {
            morph::model::detail::defaultDispatcher().dispatchAsync(
                "T_GuardAsyncAccount", "T_GuardTransfer", *holder, R"({"amount":500})", executor,
                core::async::StopToken{},
                [&](std::string const&, std::exception_ptr const& error) { outcome.set_value(error); });
        });
        REQUIRE(settled.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
        CHECK(settled.get() != nullptr);
        strands->close();
    }
    morph::testing::storageOwner().drain();
    {
        auto const entries = log->entries();
        REQUIRE(entries.size() == 1U);
        CHECK(entries[0].outcome == morph::journal::Outcome::Failed);
        CHECK_THAT(entries[0].error, Catch::Matchers::ContainsSubstring("over the limit"));
        CHECK(entries[0].result.empty());
    }

    // localTaskOp, through LocalBackend with the process default log installed so the holder it creates carries it.
    {
        morph::journal::ScopedActionLog const installed{log};
        LocalRig local;
        morph::bridge::BridgeHandler<guard_test::AsyncAccount> account{local.bridge, &local.owner};
        CHECK_THROWS(morph::testing::awaitValueOn(local.owner, account.execute(guard_test::Transfer{.amount = 500})));
    }
    morph::testing::storageOwner().drain();
    auto const entries = log->entries();
    REQUIRE(entries.size() == 2U);
    CHECK(entries[1].outcome == morph::journal::Outcome::Failed);
    CHECK_THAT(entries[1].error, Catch::Matchers::ContainsSubstring("over the limit"));
    CHECK(guard_test::gExecuted == 0);
}

// Mutation: pass a default-constructed Account to the guard instead of the holder's instance (the frozen account's
// spend executes).
TEST_CASE("action guard: the model form reads the instance the action is addressed to", "[guard]") {
    guard_test::resetCounters();
    for (bool const remote : {false, true}) {
        CAPTURE(remote);
        std::unique_ptr<RemoteRig> remoteRig = remote ? std::make_unique<RemoteRig>() : nullptr;
        std::unique_ptr<LocalRig> localRig = remote ? nullptr : std::make_unique<LocalRig>();
        auto& bridge = remote ? remoteRig->bridge : localRig->bridge;
        auto& owner = remote ? remoteRig->owner : localRig->owner;
        morph::bridge::BridgeHandler<guard_test::Account> account{bridge, &owner};
        CHECK(morph::testing::awaitValueOn(owner, account.execute(guard_test::Spend{.amount = 3})).value == -3);
        static_cast<void>(morph::testing::awaitValueOn(owner, account.execute(guard_test::Freeze{})));
        CHECK_THROWS_WITH(morph::testing::awaitValueOn(owner, account.execute(guard_test::Spend{.amount = 3})),
                          Catch::Matchers::ContainsSubstring("frozen"));
    }
    guard_test::Account direct;
    direct.active = false;
    CHECK_THROWS_WITH(morph::model::executeChecked(direct, guard_test::Spend{.amount = 3}),
                      Catch::Matchers::ContainsSubstring("frozen"));
}

// Mutation: remove the onGuardRefused call from runGuardJournaled (nothing is heard on the dispatch paths).
TEST_CASE("action guard: the model hears every refusal, an outbox-managed holder's included", "[guard][journal]") {
    guard_test::resetCounters();
    Dispatched const dispatched;
    dispatched.holder->setOutboxManaged(true);
    CHECK_THROWS(dispatched.dispatch("T_GuardWithdraw", R"({"amount":500})", "alice"));
    CHECK(dispatched.log->entries().empty());
    CHECK(guard_test::gRefusalsHeard == 1);
    LocalRig local;
    morph::bridge::BridgeHandler<guard_test::Account> account{local.bridge, &local.owner};
    CHECK_THROWS(morph::testing::awaitValueOn(local.owner, account.execute(guard_test::Withdraw{.amount = 500})));
    CHECK(guard_test::gRefusalsHeard == 2);
}
