// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <exception>
#include <functional>
#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/observability.hpp>
#include <morph/core/registry.hpp>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include "test_support.hpp"

using SyncExecutor = morph::testing::InlineExecutor;

struct CounterAction {
    int delta = 0;
};
struct CounterModel {
    int value = 0;
    int execute(const CounterAction& act) {
        value += act.delta;
        return value;
    }
};

template <>
struct morph::model::ModelTraits<CounterModel> {
    static constexpr std::string_view typeId() { return "BE_CounterModel"; }
};
template <>
struct morph::model::ActionTraits<CounterAction> {
    using Result = int;
    static constexpr std::string_view typeId() { return "BE_CounterAction"; }
    static std::string toJson(const CounterAction& action) {
        std::string out;
        if (auto err = glz::write_json(action, out)) {
            throw morph::model::detail::ParseError{glz::format_error(err, out)};
        }
        return out;
    }
    static CounterAction fromJson(std::string_view json) {
        CounterAction act{};
        if (auto err = glz::read_json(act, json)) {
            throw morph::model::detail::ParseError{glz::format_error(err, json)};
        }
        return act;
    }
    static std::string resultToJson(const int& result) {
        std::string out;
        if (auto err = glz::write_json(result, out)) {
            throw morph::model::detail::ParseError{glz::format_error(err, out)};
        }
        return out;
    }
    static int resultFromJson(std::string_view json) {
        int result{};
        if (auto err = glz::read_json(result, json)) {
            throw morph::model::detail::ParseError{glz::format_error(err, json)};
        }
        return result;
    }
};

// ── morph::backend::LocalBackend: model-not-found path ────────────────────────────────────────

TEST_CASE("morph::backend::LocalBackend: execute after deregisterModel delivers error", "[backend][local]") {
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExecutor cbExec;
    morph::backend::LocalBackend backend{pool};

    auto mid = backend.registerModel("BE_CounterModel", morph::model::detail::ModelFactory::create<CounterModel>);
    backend.deregisterModel(mid);

    // Build a minimal morph::backend::detail::ActionCall that performs a local op
    morph::backend::detail::ActionCall call;
    call.modelTypeId = "BE_CounterModel";
    call.actionTypeId = "BE_CounterAction";
    call.serializeAction = [](const void*) { return std::string{"{}"}; };
    call.deserializeResult = [](std::string_view) -> std::shared_ptr<void> { return {}; };
    call.localOp = [](morph::model::detail::IModelHolder&, void*) -> std::shared_ptr<void> { return {}; };

    bool errorFired = false;
    backend.execute(mid, std::move(call), &cbExec)
        .then([](const std::shared_ptr<void>&) {})
        .onError([&](const std::exception_ptr& exc) {
            try {
                std::rethrow_exception(exc);
            } catch (const std::runtime_error&) {
                errorFired = true;
            }
        });

    for (int i = 0; i < 50 && !errorFired; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(errorFired);
}

// ── morph::bridge::Bridge: deregisterHandler edge cases ─────────────────────────────────────

TEST_CASE("morph::bridge::Bridge::deregisterHandler with already-zero currentId is a no-op", "[bridge]") {
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};

    auto binding = std::make_shared<morph::bridge::detail::HandlerBinding>();
    binding->typeId = "BE_CounterModel";
    binding->modelFactory = morph::model::detail::ModelFactory::create<CounterModel>;
    binding->currentId.store(0);  // simulate unbound

    // Should not crash or call backend with id=0
    bridge.deregisterHandler(binding);
    REQUIRE(true);
}

TEST_CASE("morph::bridge::Bridge::executeVia when handler currentId is zero returns error", "[bridge]") {
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};

    // Manually create an unbound binding
    auto binding = std::make_shared<morph::bridge::detail::HandlerBinding>();
    binding->typeId = "BE_CounterModel";
    binding->modelFactory = morph::model::detail::ModelFactory::create<CounterModel>;
    binding->currentId.store(0);

    bool errorFired = false;
    bridge.executeVia<CounterModel, CounterAction>(binding, CounterAction{1}, &cbExec)
        .then([](int) {})
        .onError([&](const std::exception_ptr& exc) {
            try {
                std::rethrow_exception(exc);
            } catch (const std::runtime_error& ex) {
                errorFired = (std::string{ex.what()} == "handler not bound");
            }
        });

    REQUIRE(errorFired);
}

TEST_CASE("morph::bridge::BridgeHandler destructor deregisters model cleanly", "[bridge]") {
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};

    std::atomic<int> result{-1};
    {
        morph::bridge::BridgeHandler<CounterModel> handler{bridge, &cbExec};
        handler.execute(CounterAction{10})
            .then([&](int val) { result.store(val); })
            .onError([](const std::exception_ptr&) {});

        for (int i = 0; i < 50 && result.load() == -1; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        // handler goes out of scope here — deregister must not crash
    }
    REQUIRE(result.load() == 10);
}

// ── morph::backend::LocalBackend: observability (metrics + tracing) ─────────

TEST_CASE("morph::backend::LocalBackend: execute emits executeLatencyMs and toggles executeInFlight",
          "[backend][local][observability]") {
    morph::observe::ScopedObserveOverride guard;
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExecutor cbExec;
    morph::backend::LocalBackend backend{pool};

    auto mid = backend.registerModel("BE_CounterModel", morph::model::detail::ModelFactory::create<CounterModel>);

    std::atomic<int> latencyEvents{0};
    std::mutex sampleMtx;
    std::vector<double> inFlightSamples;
    morph::observe::setMetricSink([&](const morph::observe::MetricEvent& evt) {
        if (evt.metric == morph::observe::Metric::executeLatencyMs) {
            latencyEvents.fetch_add(1, std::memory_order_relaxed);
        } else if (evt.metric == morph::observe::Metric::executeInFlight) {
            std::scoped_lock const lock{sampleMtx};
            inFlightSamples.push_back(evt.value);
        }
    });

    morph::backend::detail::ActionCall call;
    call.modelTypeId = "BE_CounterModel";
    call.actionTypeId = "BE_CounterAction";
    call.localOp = [](morph::model::detail::IModelHolder& holder, void*) -> std::shared_ptr<void> {
        auto& typed = static_cast<morph::model::detail::ModelHolder<CounterModel>&>(holder);
        return std::make_shared<int>(typed.model.execute(CounterAction{.delta = 3}));
    };

    std::atomic<bool> done{false};
    backend.execute(mid, std::move(call), &cbExec).then([&](const std::shared_ptr<void>&) { done = true; });

    REQUIRE(morph::testing::waitUntil([&] { return done.load(); }));
    REQUIRE(latencyEvents.load() == 1);
    std::scoped_lock const lock{sampleMtx};
    REQUIRE(inFlightSamples.size() == 2);
    REQUIRE(inFlightSamples[0] == 1.0);
    REQUIRE(inFlightSamples[1] == 0.0);
}

TEST_CASE("morph::backend::LocalBackend: an erroring action emits executeErrors", "[backend][local][observability]") {
    morph::observe::ScopedObserveOverride guard;
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExecutor cbExec;
    morph::backend::LocalBackend backend{pool};

    auto mid = backend.registerModel("BE_CounterModel", morph::model::detail::ModelFactory::create<CounterModel>);

    std::atomic<int> errorEvents{0};
    morph::observe::setMetricSink([&](const morph::observe::MetricEvent& evt) {
        if (evt.metric == morph::observe::Metric::executeErrors) {
            errorEvents.fetch_add(1, std::memory_order_relaxed);
        }
    });

    morph::backend::detail::ActionCall call;
    call.modelTypeId = "BE_CounterModel";
    call.actionTypeId = "BE_CounterAction";
    call.localOp = [](morph::model::detail::IModelHolder&, void*) -> std::shared_ptr<void> {
        throw std::runtime_error("boom");
    };

    std::atomic<bool> errored{false};
    backend.execute(mid, std::move(call), &cbExec)
        .then([](const std::shared_ptr<void>&) {})
        .onError([&](const std::exception_ptr&) { errored = true; });

    REQUIRE(morph::testing::waitUntil([&] { return errored.load(); }));
    REQUIRE(errorEvents.load() == 1);
}

TEST_CASE("morph::backend::LocalBackend: registerModel/deregisterModel emit their counters",
          "[backend][local][observability]") {
    morph::observe::ScopedObserveOverride guard;
    morph::exec::ThreadPoolExecutor pool{2};
    morph::backend::LocalBackend backend{pool};

    std::atomic<int> registerEvents{0};
    std::atomic<int> deregisterEvents{0};
    morph::observe::setMetricSink([&](const morph::observe::MetricEvent& evt) {
        if (evt.metric == morph::observe::Metric::registerCount) {
            registerEvents.fetch_add(1, std::memory_order_relaxed);
        } else if (evt.metric == morph::observe::Metric::deregisterCount) {
            deregisterEvents.fetch_add(1, std::memory_order_relaxed);
        }
    });

    auto mid = backend.registerModel("BE_CounterModel", morph::model::detail::ModelFactory::create<CounterModel>);
    backend.deregisterModel(mid);

    REQUIRE(registerEvents.load() == 1);
    REQUIRE(deregisterEvents.load() == 1);
}

TEST_CASE("morph::backend::LocalBackend: one execute produces exactly one beginSpan/endSpan pair",
          "[backend][local][observability]") {
    morph::observe::ScopedObserveOverride guard;
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExecutor cbExec;
    morph::backend::LocalBackend backend{pool};

    auto mid = backend.registerModel("BE_CounterModel", morph::model::detail::ModelFactory::create<CounterModel>);

    std::atomic<int> beginCalls{0};
    std::atomic<int> endCalls{0};
    morph::observe::setTraceSink(morph::observe::TraceSink{
        .beginSpan =
            [&](std::string_view, std::string_view, std::string_view) {
                beginCalls.fetch_add(1, std::memory_order_relaxed);
                return morph::observe::SpanId{5};
            },
        .endSpan =
            [&](morph::observe::SpanId id, bool ok) {
                endCalls.fetch_add(1, std::memory_order_relaxed);
                REQUIRE(id == 5);
                REQUIRE(ok);
            },
    });

    morph::backend::detail::ActionCall call;
    call.modelTypeId = "BE_CounterModel";
    call.actionTypeId = "BE_CounterAction";
    call.localOp = [](morph::model::detail::IModelHolder& holder, void*) -> std::shared_ptr<void> {
        auto& typed = static_cast<morph::model::detail::ModelHolder<CounterModel>&>(holder);
        return std::make_shared<int>(typed.model.execute(CounterAction{.delta = 1}));
    };

    std::atomic<bool> done{false};
    backend.execute(mid, std::move(call), &cbExec).then([&](const std::shared_ptr<void>&) { done = true; });

    REQUIRE(morph::testing::waitUntil([&] { return done.load(); }));
    REQUIRE(beginCalls.load() == 1);
    REQUIRE(endCalls.load() == 1);
}

// ── morph::backend::LocalBackend: amortised pending-list compaction ─────────

namespace {

/// Builds an `ActionCall` whose local op is @p op — the two compaction cases
/// below differ only in that op.
morph::backend::detail::ActionCall pendingCall(std::function<void()> op) {
    morph::backend::detail::ActionCall call;
    call.modelTypeId = "BE_CounterModel";
    call.actionTypeId = "BE_CounterAction";
    // The op is the "action" this call carries: `localOp` is a function
    // pointer with nowhere to capture it, and `ActionCall::action` is the slot
    // the production path puts the action object in for exactly this reason.
    call.action = std::make_shared<std::function<void()>>(std::move(op));
    call.localOp = [](morph::model::detail::IModelHolder&, void* action) -> std::shared_ptr<void> {
        (*static_cast<std::function<void()>*>(action))();
        return {};
    };
    return call;
}

}  // namespace

// Two properties of `trackPending`'s amortised sweep, in one fixture because
// they are the two halves of the same trade: the sweep must run often enough to
// bound the list, and must never take a live entry with it.
//
//  1. **The list stays bounded.** Compaction reclaims dead entries, so a backend
//     that has admitted thousands of since-settled calls does not carry
//     thousands of dead `weak_ptr`s. Mutating `trackPending` to never sweep
//     (drop the `_pending.size() >= _compactAt` branch) leaves ~3k entries
//     against the bound asserted below, and fails here.
//  2. **`cancelPending` still reaches every live completion.** Dead entries
//     linger between sweeps, so this is the property the optimisation could
//     plausibly break. Mutating the sweep predicate to `true` (erase
//     everything, not just the expired) drops the parked completions and fails
//     here.
//
// What this does *not* assert is admission latency — the reason the sweep was
// made amortised in the first place. That is measured by a benchmark, not by a
// test; a wall-clock assertion on a shared CI runner would be a flake, not
// evidence. The measured numbers are in docs/spec/core/backend.md.
TEST_CASE("morph::backend::LocalBackend: amortised pending compaction bounds the list and keeps cancelPending whole",
          "[backend][local][pending]") {
    constexpr int kRounds = 48;
    constexpr int kChurnPerRound = 64;  // 3072 admissions that settle and are dropped

    // Declared *before* the pool and the backend, so they outlive them. Only the
    // one parked task that is actually running has left the strand queue when
    // this scope ends; the other 47 are still queued, and `~StrandExecutor` /
    // `~ThreadPoolExecutor` run them during teardown — after any state declared
    // below the pool has already been destroyed. Getting this backwards is a
    // stack-use-after-scope on `gate`, which is exactly what ASan reported the
    // first time round, not a theoretical one.
    std::atomic<bool> gate{false};
    std::atomic<int> churnSettled{0};
    std::atomic<int> cancelled{0};
    std::atomic<int> parkedRan{0};

    morph::exec::ThreadPoolExecutor pool{4};
    SyncExecutor cbExec;
    morph::backend::LocalBackend backend{pool};

    // Two instances, so the parked one's strand cannot hold up the churning one.
    auto parked = backend.registerModel("BE_CounterModel", morph::model::detail::ModelFactory::create<CounterModel>);
    auto churner = backend.registerModel("BE_CounterModel", morph::model::detail::ModelFactory::create<CounterModel>);

    std::vector<morph::async::Completion<std::shared_ptr<void>>> live;

    for (int round = 0; round < kRounds; ++round) {
        // One completion that parks on the gate and so stays live in `_pending`.
        live.push_back(backend.execute(parked, pendingCall([&gate, &parkedRan] {
                                           while (!gate.load(std::memory_order_acquire)) {
                                               std::this_thread::sleep_for(std::chrono::milliseconds(1));
                                           }
                                           parkedRan.fetch_add(1, std::memory_order_relaxed);
                                       }),
                                       &cbExec));
        live.back().onError([&cancelled](const std::exception_ptr& exc) {
            try {
                std::rethrow_exception(exc);
            } catch (const morph::backend::BackendChangedError&) {
                cancelled.fetch_add(1, std::memory_order_relaxed);
            } catch (...) {  // NOLINT(bugprone-empty-catch)
            }
        });

        // A batch that settles and is then dropped, leaving dead entries behind
        // for the next sweep to reclaim.
        std::vector<morph::async::Completion<std::shared_ptr<void>>> churn;
        churn.reserve(kChurnPerRound);
        int const target = churnSettled.load(std::memory_order_relaxed) + kChurnPerRound;
        for (int i = 0; i < kChurnPerRound; ++i) {
            churn.push_back(backend.execute(
                churner, pendingCall([&churnSettled] { churnSettled.fetch_add(1, std::memory_order_relaxed); }),
                nullptr));
        }
        REQUIRE(morph::testing::waitUntil([&] { return churnSettled.load(std::memory_order_relaxed) >= target; },
                                          morph::testing::WaitBudget{std::chrono::milliseconds{5000}},
                                          morph::testing::WaitStep{std::chrono::milliseconds{1}}));
        churn.clear();
    }

    // Property 1. The exact steady-state size depends on how promptly the strand
    // releases each settled task's captured state, so the bound is deliberately
    // loose — it only has to sit well below the 3120 entries an uncompacted list
    // would hold, and it does.
    auto const tracked = backend.trackedPendingCount();
    INFO("tracked=" << tracked << " admitted=" << (kRounds * (kChurnPerRound + 1)));
    CHECK(tracked < 1024);

    // Property 2. Every parked completion, admitted across every sweep, is still
    // reachable. `cbExec` is inline, so the handlers have all run by the time
    // `cancelPending` returns.
    backend.cancelPending(std::make_exception_ptr(morph::backend::BackendChangedError{}));
    auto const cancelledCount = cancelled.load(std::memory_order_relaxed);

    // Release the parked ops and drain them before any assertion can abandon the
    // fixture: `~StrandExecutor` blocks until the running task returns, and a
    // `CHECK` that fires mid-teardown should not leave that to chance.
    gate.store(true, std::memory_order_release);
    REQUIRE(morph::testing::waitUntil([&] { return parkedRan.load(std::memory_order_relaxed) == kRounds; },
                                      morph::testing::WaitBudget{std::chrono::milliseconds{10000}}));
    live.clear();

    CHECK(cancelledCount == kRounds);
}
