// SPDX-License-Identifier: Apache-2.0
//
// Tests for `morph::async::CallbackScope` / `CallbackToken` (issue #138): the
// lifetime-and-stop gate a receiver holds as a *data member* (not a base class)
// and hands to the callbacks it attaches.
//
// Verification strategy, carried over from #150 and required by the issue: the
// call counters live in `shared_ptr`s that **outlive the receiver**, so "the
// callback body did not run" is directly observable rather than resting on a
// sanitizer noticing UB after the fact. A test that destroys the receiver and
// then delivers therefore fails loudly on a broken gate instead of passing
// everywhere except under ASan.

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/callback_scope.hpp>
#include <morph/core/completion.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
#include <stdexcept>
#include <string>
#include <thread>

#include "test_support.hpp"

// Model/action/result types need external linkage: glaze's plain-aggregate
// reflection cannot see into an anonymous namespace, and the BRIDGE_REGISTER_*
// macros specialise templates at global scope.
// NOLINTBEGIN(misc-use-internal-linkage)
/// State type a gated subscription renders.
struct CbsCounterState {
    std::int64_t value = 0;
};

/// Action bumping the counter, so a subscription has something to hear.
struct CbsBump {
    std::int64_t id = 0;
    std::int64_t by = 0;
};

/// Stateful model behind the subscription test.
struct CbsCounterModel {
    std::int64_t value = 0;

    /// @brief Applies @p act and yields the new state.
    /// @param act Bump to apply.
    /// @return The counter's new state.
    CbsCounterState execute(const CbsBump& act) {
        value += act.by;
        return {.value = value};
    }
};

BRIDGE_REGISTER_MODEL(CbsCounterModel, "CBS_CounterModel")
BRIDGE_REGISTER_ACTION(CbsCounterModel, CbsBump, "CBS_Bump")
BRIDGE_MODEL_KEY(CbsCounterModel, CbsBump, &CbsBump::id);
// NOLINTEND(misc-use-internal-linkage)

namespace {

using morph::async::CallbackScope;
using morph::async::CallbackStatus;
using morph::async::CallbackToken;
using morph::testing::StepExecutor;

using Counter = std::shared_ptr<std::atomic<int>>;

Counter makeCounter() { return std::make_shared<std::atomic<int>>(0); }

/// A receiver that owns a `CallbackScope` as a plain member and, deliberately,
/// derives from nothing. `_hits` is a `shared_ptr` the *test* also holds, so a
/// callback that wrongly runs after `~Receiver` is observable as a count rather
/// than as undefined behaviour.
class Receiver {
public:
    explicit Receiver(Counter hits) : _hits{std::move(hits)} {}

    void attachTo(morph::async::Completion<int>& completion) {
        completion.then(_callbacks, [this](int val) { record(val); });
    }

    void attachErrorTo(morph::async::Completion<int>& completion) {
        completion.onError(_callbacks, [this](const std::exception_ptr&) { record(-1); });
    }

    void stop() { _callbacks.requestStop(); }
    void supersede() { _callbacks.reset(); }
    [[nodiscard]] int lastValue() const { return _lastValue; }

private:
    void record(int val) {
        _lastValue = val;
        _hits->fetch_add(1);
    }

    Counter _hits;
    int _lastValue = 0;
    // Declared last: first member destroyed, so gated callbacks are refused
    // before anything above is torn down.
    CallbackScope _callbacks;
};

}  // namespace

// ── The three states, on the token itself ───────────────────────────────────

TEST_CASE("CallbackToken: a live scope reports Active", "[callback-scope][issue-138]") {
    CallbackScope scope;
    auto const token = scope.token();
    REQUIRE(token.status() == CallbackStatus::Active);
    REQUIRE(token.active());
    REQUIRE_FALSE(token.expired());
    REQUIRE_FALSE(scope.stopRequested());
}

TEST_CASE("CallbackToken: liveness and stop are distinguishable", "[callback-scope][issue-138]") {
    // The whole point of not reusing a bare weak_ptr: "the owner is gone" and
    // "the owner is here but cancelled" must not collapse into one answer.
    CallbackToken stoppedToken;
    {
        CallbackScope scope;
        stoppedToken = scope.token();
        scope.requestStop();

        REQUIRE(scope.stopRequested());
        REQUIRE(stoppedToken.status() == CallbackStatus::Stopped);
        REQUIRE_FALSE(stoppedToken.active());
        // Stopped is *not* expired: the owner still exists.
        REQUIRE_FALSE(stoppedToken.expired());
    }
    // Same token, after the owner is destroyed.
    REQUIRE(stoppedToken.status() == CallbackStatus::Expired);
    REQUIRE(stoppedToken.expired());
}

TEST_CASE("CallbackToken: a default-constructed token is permanently expired", "[callback-scope][issue-138]") {
    CallbackToken const unbound;
    REQUIRE(unbound.status() == CallbackStatus::Expired);
    REQUIRE_FALSE(unbound.active());

    auto hits = makeCounter();
    auto guarded = unbound.guard([hits] { hits->fetch_add(1); });
    guarded();
    REQUIRE(hits->load() == 0);
}

// ── guard(): the general-purpose form, for callbacks that are not Completions ─

TEST_CASE("CallbackScope::guard: forwards arguments while active", "[callback-scope][issue-138]") {
    CallbackScope scope;
    auto seen = std::make_shared<std::string>();
    auto guarded = scope.guard([seen](const std::string& text, int times) {
        for (int i = 0; i < times; ++i) {
            *seen += text;
        }
    });

    guarded(std::string{"ab"}, 2);
    REQUIRE(*seen == "abab");

    scope.requestStop();
    guarded(std::string{"cd"}, 1);
    REQUIRE(*seen == "abab");
}

TEST_CASE("CallbackScope::guard: a wrapper does not keep its scope alive", "[callback-scope][issue-138]") {
    auto hits = makeCounter();
    std::function<void()> guarded;
    {
        CallbackScope scope;
        guarded = scope.guard([hits] { hits->fetch_add(1); });
    }
    guarded();
    REQUIRE(hits->load() == 0);
}

// ── Completion gating: live / stopped / destroyed-owner ─────────────────────

TEST_CASE("Completion::then(scope, fn): the live receiver is delivered to",
          "[callback-scope][completion][issue-138]") {
    StepExecutor exec;
    auto [completion, promise] = morph::async::Completion<int>::makeSettleable(&exec);

    auto hits = makeCounter();
    Receiver receiver{hits};
    receiver.attachTo(completion);

    promise.resolve(11);
    REQUIRE(exec.runAll() == 1U);

    REQUIRE(hits->load() == 1);
    REQUIRE(receiver.lastValue() == 11);
}

TEST_CASE("Completion::then(scope, fn): a stopped receiver is not delivered to",
          "[callback-scope][completion][issue-138]") {
    StepExecutor exec;
    auto [completion, promise] = morph::async::Completion<int>::makeSettleable(&exec);

    auto hits = makeCounter();
    Receiver receiver{hits};
    receiver.attachTo(completion);

    promise.resolve(11);
    // The receiver is perfectly alive -- it simply stopped caring between the
    // dispatch and the delivery. This is the case a bare liveness token cannot
    // express at all.
    receiver.stop();
    REQUIRE(exec.runAll() == 1U);

    REQUIRE(hits->load() == 0);
    REQUIRE(receiver.lastValue() == 0);
}

TEST_CASE("Completion::then(scope, fn): a destroyed receiver is not delivered to",
          "[callback-scope][completion][issue-138]") {
    StepExecutor exec;
    auto [completion, promise] = morph::async::Completion<int>::makeSettleable(&exec);

    // `hits` outlives the receiver on purpose: the assertion below is about the
    // callback body not running, not about a sanitizer noticing a freed `this`.
    auto hits = makeCounter();
    {
        Receiver receiver{hits};
        receiver.attachTo(completion);
        promise.resolve(11);
    }
    REQUIRE(exec.runAll() == 1U);

    REQUIRE(hits->load() == 0);
}

TEST_CASE("Completion::onError(scope, fn): gating applies to the error path too",
          "[callback-scope][completion][issue-138]") {
    StepExecutor exec;
    auto [completion, promise] = morph::async::Completion<int>::makeSettleable(&exec);

    auto hits = makeCounter();
    {
        Receiver receiver{hits};
        receiver.attachErrorTo(completion);
        promise.reject(std::make_exception_ptr(std::runtime_error{"gone"}));
    }
    REQUIRE(exec.runAll() == 1U);
    REQUIRE(hits->load() == 0);
}

TEST_CASE("Completion::onError(scope, fn): a suppressed error still counts as handled",
          "[callback-scope][completion][issue-138]") {
    // Suppression is a deliberate act by the receiver, so it must not re-arm
    // the orphan logger: attaching a gated handler discharges the error exactly
    // as the ungated form does.
    StepExecutor exec;
    auto state = std::make_shared<morph::async::detail::CompletionState<int>>();
    {
        morph::async::Completion<int> completion{state, &exec};
        CallbackScope scope;
        completion.onError(scope, [](const std::exception_ptr&) {});
        scope.requestStop();
    }
    state->setException(std::make_exception_ptr(std::runtime_error{"handled"}));
    REQUIRE(state->onErrAttached);
    exec.runAll();
}

TEST_CASE("Completion::thenDetached: the ungated spelling still delivers", "[callback-scope][completion][issue-138]") {
    StepExecutor exec;
    auto [completion, promise] = morph::async::Completion<int>::makeSettleable(&exec);

    auto hits = makeCounter();
    completion.thenDetached([hits](int) { hits->fetch_add(1); });
    promise.resolve(3);
    REQUIRE(exec.runAll() == 1U);
    REQUIRE(hits->load() == 1);
}

TEST_CASE("Completion::then(fn) without a scope is unchanged", "[callback-scope][completion][issue-138]") {
    // The composition-over-inheritance requirement in the flesh: code that does
    // not opt in keeps working exactly as before.
    StepExecutor exec;
    auto [completion, promise] = morph::async::Completion<int>::makeSettleable(&exec);

    int seen = 0;
    completion.then([&seen](int val) { seen = val; });
    promise.resolve(5);
    REQUIRE(exec.runAll() == 1U);
    REQUIRE(seen == 5);
}

// ── reset(): the supersede verb ─────────────────────────────────────────────

TEST_CASE("CallbackScope::reset: retires old tokens and revives the scope", "[callback-scope][issue-138]") {
    CallbackScope scope;
    auto const stale = scope.token();
    scope.requestStop();
    REQUIRE(stale.status() == CallbackStatus::Stopped);

    scope.reset();

    REQUIRE(stale.status() == CallbackStatus::Expired);
    REQUIRE_FALSE(scope.stopRequested());
    REQUIRE(scope.token().status() == CallbackStatus::Active);
}

TEST_CASE("CallbackScope::reset: an old reply is dropped and the new one delivered",
          "[callback-scope][completion][issue-138]") {
    // The stale-search-result case: a second query supersedes the first, and
    // the first must not overwrite the second when it finally arrives.
    StepExecutor exec;
    auto [oldCompletion, oldPromise] = morph::async::Completion<int>::makeSettleable(&exec);
    auto [newCompletion, newPromise] = morph::async::Completion<int>::makeSettleable(&exec);

    auto hits = makeCounter();
    Receiver receiver{hits};
    receiver.attachTo(oldCompletion);

    receiver.supersede();
    receiver.attachTo(newCompletion);

    // The superseded reply lands first, then the current one.
    oldPromise.resolve(1);
    newPromise.resolve(2);
    REQUIRE(exec.runAll() == 2U);

    REQUIRE(hits->load() == 1);
    REQUIRE(receiver.lastValue() == 2);
}

// ── Subscriptions ───────────────────────────────────────────────────────────

TEST_CASE("BridgeHandler::subscribe(scope, cb): delivery stops when the scope does",
          "[callback-scope][bridge][subscription][issue-138]") {
    morph::testing::InlineExecutor exec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(exec)};
    morph::bridge::BridgeHandler<CbsCounterModel, morph::bridge::AllowShared> handler{bridge, &exec};

    auto hits = makeCounter();
    CallbackScope scope;
    handler.subscribe<CbsCounterState>(scope, [hits](CbsCounterState) { hits->fetch_add(1); });

    auto drain = [&](morph::async::Completion<CbsCounterState> comp) {
        auto done = std::make_shared<std::atomic<bool>>(false);
        std::move(comp)
            .then([done](const CbsCounterState&) { done->store(true); })
            .onError([done](const std::exception_ptr&) { done->store(true); });
        REQUIRE(morph::testing::waitUntil([&] { return done->load(); }));
    };

    drain(handler.execute(CbsBump{.id = 1, .by = 1}));
    REQUIRE(hits->load() == 1);

    scope.requestStop();
    drain(handler.execute(CbsBump{.id = 1, .by = 1}));
    REQUIRE(hits->load() == 1);

    // reset() starts a *new* generation, so it does not revive the sink that
    // was installed under the old one: an already-installed subscription holds
    // a token for the generation current when it was subscribed. The sink entry
    // is still there (nothing prunes it) -- it is simply never delivered to
    // again. Re-arming means re-subscribing under the new generation.
    scope.reset();
    drain(handler.execute(CbsBump{.id = 1, .by = 1}));
    REQUIRE(hits->load() == 1);

    handler.subscribe<CbsCounterState>(scope, [hits](CbsCounterState) { hits->fetch_add(1); });
    drain(handler.execute(CbsBump{.id = 1, .by = 1}));
    REQUIRE(hits->load() == 2);
}

// ── A stop racing a dispatch: deterministically forced ─────────────────────
//
// "The stop landed while a dispatch was in flight" is the case that matters,
// and it does not need threads to reproduce. A `StepExecutor` holds a posted
// task in its queue until the test says otherwise, so the stop provably lands
// *after* the dispatch was scheduled and *before* it is delivered. That is the
// whole window, forced, on every schedule and every platform.

TEST_CASE("CallbackScope: a stop landing between dispatch and delivery refuses the delivery",
          "[callback-scope][issue-138]") {
    StepExecutor exec;
    CallbackScope scope;
    auto hits = makeCounter();

    for (int i = 0; i < 3; ++i) {
        exec.post(scope.guard([hits] { hits->fetch_add(1); }));
    }
    REQUIRE(exec.pending() == 3U);

    // In flight: posted, queued, not yet delivered.
    scope.requestStop();

    REQUIRE(exec.runAll() == 3U);
    REQUIRE(hits->load() == 0);
}

TEST_CASE("CallbackScope: a stop issued from inside one delivery refuses the deliveries behind it",
          "[callback-scope][issue-138]") {
    // The tightest interleaving there is: the stop happens *during* dispatch,
    // between two queued callbacks, with no thread involved.
    StepExecutor exec;
    CallbackScope scope;
    auto hits = makeCounter();

    exec.post(scope.guard([hits, &scope] {
        hits->fetch_add(1);
        scope.requestStop();
    }));
    exec.post(scope.guard([hits] { hits->fetch_add(1); }));
    exec.post(scope.guard([hits] { hits->fetch_add(1); }));

    REQUIRE(exec.runAll() == 3U);
    REQUIRE(hits->load() == 1);
}

TEST_CASE("CallbackScope: destroying the scope between dispatch and delivery refuses the delivery",
          "[callback-scope][issue-138]") {
    StepExecutor exec;
    auto hits = makeCounter();
    {
        CallbackScope scope;
        for (int i = 0; i < 3; ++i) {
            exec.post(scope.guard([hits] { hits->fetch_add(1); }));
        }
        REQUIRE(exec.pending() == 3U);
    }
    // The owner is gone; the tasks are still queued. `hits` outlives it, so the
    // question "did the body run" is still answerable.
    REQUIRE(exec.runAll() == 3U);
    REQUIRE(hits->load() == 0);
}

TEST_CASE("CallbackScope: reset() between dispatch and delivery refuses only the old generation",
          "[callback-scope][issue-138]") {
    StepExecutor exec;
    CallbackScope scope;
    auto hits = makeCounter();

    exec.post(scope.guard([hits] { hits->fetch_add(1); }));  // old generation
    scope.reset();
    exec.post(scope.guard([hits] { hits->fetch_add(1); }));  // new generation

    REQUIRE(exec.runAll() == 2U);
    REQUIRE(hits->load() == 1);
}

// ── The same, under real thread contention ─────────────────────────────────
//
// These are stress drivers, not schedule assertions. Nothing here REQUIREs
// that the race interleaved a particular way -- an instrumented or loaded
// runner (Valgrind serialises threads outright) is free to run the whole
// dispatch loop before the stop lands, or the stop before the loop starts, and
// both are legal schedules. What *is* asserted holds on every one of them:
//
//   - monotonicity: no delivery after this thread observed a refusal;
//   - the deterministic post-condition, checked on the main thread after the
//     join: the gate is shut and stays shut.
//
// Whether the interleaving actually happened is recorded as information.

namespace {

/// What the dispatching thread observed while hammering a guarded callback.
struct RaceOutcome {
    bool refusalObserved = false;     ///< The gate refused at least once (informational).
    int deliveriesAfterRefusal = 0;   ///< Deliveries seen *after* the first refusal.
    int deliveriesBeforeRefusal = 0;  ///< Deliveries seen before it.
};

/// Invokes @p guarded @p iterations times, yielding between calls, and records
/// how the gate behaved. Bounded by a call count, never by wall-clock time, and
/// cooperative so a serialising scheduler can still run the other thread.
template <typename G>
RaceOutcome hammer(G guarded, const std::shared_ptr<std::atomic<bool>>& ran, int iterations) {
    RaceOutcome outcome;
    for (int i = 0; i < iterations; ++i) {
        ran->store(false, std::memory_order_release);
        guarded();
        if (ran->load(std::memory_order_acquire)) {
            if (outcome.refusalObserved) {
                ++outcome.deliveriesAfterRefusal;
            } else {
                ++outcome.deliveriesBeforeRefusal;
            }
        } else {
            outcome.refusalObserved = true;
        }
        std::this_thread::yield();
    }
    return outcome;
}

}  // namespace

TEST_CASE("CallbackScope: requestStop() from another thread shuts the gate and never un-shuts it",
          "[callback-scope][issue-138][thread]") {
    constexpr int kRounds = 20;
    constexpr int kIterations = 200;

    for (int round = 0; round < kRounds; ++round) {
        CallbackScope scope;
        auto ran = std::make_shared<std::atomic<bool>>(false);
        auto hits = makeCounter();
        auto guarded = scope.guard([ran, hits] {
            hits->fetch_add(1);
            ran->store(true, std::memory_order_release);
        });

        auto outcome = std::make_shared<RaceOutcome>();
        std::thread dispatcher{[guarded, ran, outcome]() mutable { *outcome = hammer(guarded, ran, kIterations); }};

        std::this_thread::yield();
        scope.requestStop();

        dispatcher.join();

        INFO("round " << round << ": refusalObserved=" << outcome->refusalObserved
                      << " before=" << outcome->deliveriesBeforeRefusal);
        // Holds on every schedule, including the ones where the stop landed
        // entirely before or entirely after the loop.
        REQUIRE(outcome->deliveriesAfterRefusal == 0);

        // Deterministic post-condition: the stop has definitely happened and
        // the dispatcher has definitely finished, so the gate must be shut --
        // and one more delivery attempt must change nothing.
        REQUIRE(scope.stopRequested());
        REQUIRE_FALSE(scope.token().active());
        auto const settled = hits->load();
        guarded();
        REQUIRE(hits->load() == settled);
    }
}

TEST_CASE("CallbackScope: destroying the scope under a concurrent dispatch loop shuts the gate",
          "[callback-scope][issue-138][thread]") {
    // The destroyed-owner half. `hits` and `ran` are held by the wrapper and by
    // the test, so they outlive the scope: "did the body run" stays answerable
    // after the owner is gone, instead of depending on a sanitizer noticing a
    // freed `this`.
    constexpr int kRounds = 20;
    constexpr int kIterations = 200;

    for (int round = 0; round < kRounds; ++round) {
        auto scope = std::make_unique<CallbackScope>();
        auto ran = std::make_shared<std::atomic<bool>>(false);
        auto hits = makeCounter();
        auto guarded = scope->guard([ran, hits] {
            hits->fetch_add(1);
            ran->store(true, std::memory_order_release);
        });

        auto outcome = std::make_shared<RaceOutcome>();
        std::thread dispatcher{[guarded, ran, outcome]() mutable { *outcome = hammer(guarded, ran, kIterations); }};

        std::this_thread::yield();
        scope.reset();

        dispatcher.join();

        INFO("round " << round << ": refusalObserved=" << outcome->refusalObserved
                      << " before=" << outcome->deliveriesBeforeRefusal);
        REQUIRE(outcome->deliveriesAfterRefusal == 0);

        // The owner is gone and the dispatcher has finished: another attempt
        // delivers nothing.
        auto const settled = hits->load();
        guarded();
        REQUIRE(hits->load() == settled);
    }
}
