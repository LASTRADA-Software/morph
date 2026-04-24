// SPDX-License-Identifier: Apache-2.0

#include <morph/bridge.hpp>
#include <morph/executor.hpp>
#include <morph/logger.hpp>
#include <morph/registry.hpp>
#include <morph/remote.hpp>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>


// ── Test fixture: a model with two action types ─────────────────────────────
//
// FormAction takes five doubles; the validator requires all three of {a, b, c}
// to be non-zero before the action may fire. d and e are optional inputs.
struct FormAction {
    double a = 0.0;
    double b = 0.0;
    double c = 0.0;
    double d = 0.0;
    double e = 0.0;
};

// SimpleAction has no validator override — default morph::model::ActionValidator returns true,
// so it should fire on the first set<>.
struct SimpleAction {
    int x = 0;
};

struct ThrowAction {
    int trigger = 0;
};

// FlakyAction throws only when `mode == 0`, otherwise returns mode * 2. Used to test
// re-fire after error: first fire throws, second fire (after setting mode=1) succeeds.
struct FlakyAction {
    int mode = -1;
};

// A bundle of non-numeric field types to verify set<> with strings and nested structs.
struct Inner {
    int n = 0;
};
struct MixedAction {
    std::string name;
    Inner inner;
    int count = 0;
};

// A slow action whose `execute` sleeps long enough that bursting set<>() during
// the in-flight call exercises the coalescing path explicitly.
struct SlowAction {
    int seq = 0;
};

struct FormModel {
    double execute(FormAction action) { return action.a + action.b + action.c + action.d + action.e; }
    int execute(SimpleAction action) { return action.x * 10; }
    int execute(ThrowAction /*unused*/) { throw std::runtime_error("boom"); }
    int execute(FlakyAction action) {
        if (action.mode == 0) {
            throw std::runtime_error("flaky");
        }
        return action.mode * 2;
    }
    std::string execute(const MixedAction& action) {
        return action.name + ":" + std::to_string(action.inner.n) + ":" + std::to_string(action.count);
    }
    int execute(SlowAction action) {
        std::this_thread::sleep_for(std::chrono::milliseconds{40});
        return action.seq;
    }
};

BRIDGE_REGISTER_MODEL(FormModel, "Test_FormModel")
BRIDGE_REGISTER_ACTION(FormModel, FormAction, "Test_FormAction")
BRIDGE_REGISTER_ACTION(FormModel, SimpleAction, "Test_SimpleAction")
BRIDGE_REGISTER_ACTION(FormModel, ThrowAction, "Test_ThrowAction")
BRIDGE_REGISTER_ACTION(FormModel, FlakyAction, "Test_FlakyAction")
BRIDGE_REGISTER_ACTION(FormModel, MixedAction, "Test_MixedAction")
BRIDGE_REGISTER_ACTION(FormModel, SlowAction, "Test_SlowAction")

BRIDGE_REGISTER_VALIDATOR(FormAction, [](const FormAction& action) {
    return action.a != 0.0 && action.b != 0.0 && action.c != 0.0;
})
BRIDGE_REGISTER_VALIDATOR(ThrowAction, [](const ThrowAction& action) { return action.trigger != 0; })
BRIDGE_REGISTER_VALIDATOR(FlakyAction, [](const FlakyAction& action) { return action.mode >= 0; })
BRIDGE_REGISTER_VALIDATOR(MixedAction, [](const MixedAction& action) {
    return !action.name.empty() && action.inner.n != 0 && action.count != 0;
})

// Runs the callback executor inline (simulates a GUI thread pump)
struct SyncExecutor : morph::exec::IExecutor {
    void post(std::function<void()> task) override { task(); }
};

namespace {

template <typename Pred>
void waitFor(Pred pred, std::chrono::milliseconds budget = std::chrono::milliseconds{2000}) {
    auto deadline = std::chrono::steady_clock::now() + budget;
    while (!pred() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
}

}  // namespace

TEST_CASE("Subscription: default validator fires on first set", "[bridge][subscription]") {
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<FormModel> handler{bridge, &cbExec};

    std::atomic<int> seen{-1};
    handler.subscribe<SimpleAction>([&](int result) { seen.store(result); });

    handler.set<&SimpleAction::x>(7);

    waitFor([&] { return seen.load() != -1; });
    REQUIRE(seen.load() == 70);
}

TEST_CASE("Subscription: custom validator gates fire until ready", "[bridge][subscription]") {
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<FormModel> handler{bridge, &cbExec};

    std::atomic<int> calls{0};
    std::atomic<double> last{0.0};
    handler.subscribe<FormAction>([&](double sum) {
        last.store(sum);
        calls.fetch_add(1);
    });

    // a alone is not ready → no fire
    handler.set<&FormAction::a>(1.0);
    std::this_thread::sleep_for(std::chrono::milliseconds{30});
    REQUIRE(calls.load() == 0);

    // a + b still not enough → no fire
    handler.set<&FormAction::b>(2.0);
    std::this_thread::sleep_for(std::chrono::milliseconds{30});
    REQUIRE(calls.load() == 0);

    // a + b + c → validator passes, action fires
    handler.set<&FormAction::c>(4.0);

    waitFor([&] { return calls.load() >= 1; });
    REQUIRE(calls.load() == 1);
    REQUIRE(last.load() == 7.0);
}

TEST_CASE("Subscription: re-fires on subsequent sets after ready", "[bridge][subscription]") {
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<FormModel> handler{bridge, &cbExec};

    std::mutex resultsMtx;
    std::vector<double> results;
    handler.subscribe<FormAction>([&](double sum) {
        std::scoped_lock lock{resultsMtx};
        results.push_back(sum);
    });

    // Cross readiness on the third set
    handler.set<&FormAction::a>(1.0);
    handler.set<&FormAction::b>(1.0);
    handler.set<&FormAction::c>(1.0);  // first fire = 3.0
    waitFor([&] {
        std::scoped_lock lock{resultsMtx};
        return !results.empty();
    });

    // Now the draft already passes validation; every subsequent set re-fires
    // once the in-flight call settles. Push more values and observe results
    // converge to the latest snapshot.
    handler.set<&FormAction::d>(10.0);
    handler.set<&FormAction::e>(100.0);

    waitFor([&] {
        std::scoped_lock lock{resultsMtx};
        return !results.empty() && results.back() == 113.0;
    });

    std::scoped_lock lock{resultsMtx};
    REQUIRE(!results.empty());
    REQUIRE(results.front() == 3.0);
    REQUIRE(results.back() == 113.0);
    // Coalescing means we expect fewer fires than sets: at most one per
    // "round", not one per set.
    REQUIRE(results.size() <= 3);
}

TEST_CASE("Subscription: error sink receives model exceptions", "[bridge][subscription]") {
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<FormModel> handler{bridge, &cbExec};

    std::atomic<bool> errFired{false};
    handler.subscribe<ThrowAction>(
        [](int /*unused*/) {},
        [&](const std::exception_ptr& err) {
            try {
                std::rethrow_exception(err);
            } catch (const std::runtime_error&) {
                errFired.store(true);
            }
        });

    handler.set<&ThrowAction::trigger>(1);

    waitFor([&] { return errFired.load(); });
    REQUIRE(errFired.load());
}

TEST_CASE("Subscription: unsubscribe stops further results", "[bridge][subscription]") {
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<FormModel> handler{bridge, &cbExec};

    std::atomic<int> calls{0};
    handler.subscribe<SimpleAction>([&](int /*unused*/) { calls.fetch_add(1); });

    handler.set<&SimpleAction::x>(1);
    waitFor([&] { return calls.load() >= 1; });
    REQUIRE(calls.load() == 1);

    handler.unsubscribe<SimpleAction>();

    handler.set<&SimpleAction::x>(2);
    // give the worker a chance to fire (action still executes; the result is
    // dropped because no sink). The count should stay at 1.
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    REQUIRE(calls.load() == 1);
}

TEST_CASE("Subscription: distinct action types do not interfere", "[bridge][subscription]") {
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<FormModel> handler{bridge, &cbExec};

    std::atomic<int> simple{-1};
    std::atomic<double> form{-1.0};
    handler.subscribe<SimpleAction>([&](int result) { simple.store(result); });
    handler.subscribe<FormAction>([&](double sum) { form.store(sum); });

    handler.set<&SimpleAction::x>(3);
    handler.set<&FormAction::a>(1.0);
    handler.set<&FormAction::b>(2.0);
    handler.set<&FormAction::c>(3.0);  // FormAction now ready

    waitFor([&] { return simple.load() != -1 && form.load() != -1.0; });
    REQUIRE(simple.load() == 30);
    REQUIRE(form.load() == 6.0);
}

TEST_CASE("Subscription: reset clears the draft", "[bridge][subscription]") {
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<FormModel> handler{bridge, &cbExec};

    std::atomic<double> last{-1.0};
    handler.subscribe<FormAction>([&](double sum) { last.store(sum); });

    handler.set<&FormAction::a>(1.0);
    handler.set<&FormAction::b>(2.0);
    handler.set<&FormAction::c>(3.0);
    waitFor([&] { return last.load() == 6.0; });

    handler.reset<FormAction>();

    // After reset, just setting `a` alone shouldn't fire — validator needs all of a/b/c.
    last.store(-1.0);
    handler.set<&FormAction::a>(5.0);
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    REQUIRE(last.load() == -1.0);

    // Re-fill and confirm the draft genuinely restarted from defaults.
    handler.set<&FormAction::b>(5.0);
    handler.set<&FormAction::c>(5.0);
    waitFor([&] { return last.load() != -1.0; });
    REQUIRE(last.load() == 15.0);
}

TEST_CASE("Subscription: result is dropped silently with no sink installed", "[bridge][subscription]") {
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<FormModel> handler{bridge, &cbExec};

    // No subscribe<SimpleAction>() — set<> still triggers execute, but result drops.
    handler.set<&SimpleAction::x>(42);
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    // No assertion required; the test passes if nothing crashes and no orphan
    // error is logged (because there is no exception, just a discarded result).
    SUCCEED("no-sink fire completed without UAF or hang");
}

TEST_CASE("Subscription: callbacks no-op after handler destruction", "[bridge][subscription]") {
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};

    std::atomic<int> calls{0};
    {
        morph::bridge::BridgeHandler<FormModel> handler{bridge, &cbExec};
        handler.subscribe<SimpleAction>([&](int /*unused*/) { calls.fetch_add(1); });
        handler.set<&SimpleAction::x>(1);
        waitFor([&] { return calls.load() >= 1; });
    }
    // Handler has been destroyed. Give any racing completion callbacks a window
    // to fire — they should now see a null weak_ptr and no-op.
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    REQUIRE(calls.load() == 1);
}

TEST_CASE("Subscription: second subscribe replaces the first", "[bridge][subscription]") {
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<FormModel> handler{bridge, &cbExec};

    std::atomic<int> firstCalls{0};
    std::atomic<int> secondCalls{0};
    handler.subscribe<SimpleAction>([&](int /*unused*/) { firstCalls.fetch_add(1); });
    handler.subscribe<SimpleAction>([&](int /*unused*/) { secondCalls.fetch_add(1); });

    handler.set<&SimpleAction::x>(1);
    waitFor([&] { return secondCalls.load() >= 1; });
    // Only the latest subscriber sees results.
    REQUIRE(secondCalls.load() == 1);
    REQUIRE(firstCalls.load() == 0);
}

TEST_CASE("Subscription: set before subscribe drops the result", "[bridge][subscription]") {
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<FormModel> handler{bridge, &cbExec};

    // Fire before any subscriber is installed — result is computed and dropped.
    handler.set<&SimpleAction::x>(1);
    std::this_thread::sleep_for(std::chrono::milliseconds{30});

    // Now install a subscriber. It must not see the prior dropped result.
    std::atomic<int> calls{0};
    std::atomic<int> last{-1};
    handler.subscribe<SimpleAction>([&](int val) {
        last.store(val);
        calls.fetch_add(1);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds{30});
    REQUIRE(calls.load() == 0);

    // The next set still fires from the preserved draft (x already 1 from earlier),
    // but at this point the value is whatever the next set lands.
    handler.set<&SimpleAction::x>(5);
    waitFor([&] { return calls.load() >= 1; });
    REQUIRE(calls.load() == 1);
    REQUIRE(last.load() == 50);
}

TEST_CASE("Subscription: works with string and nested-struct fields", "[bridge][subscription]") {
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<FormModel> handler{bridge, &cbExec};

    std::atomic<bool> fired{false};
    std::string seen;
    std::mutex seenMtx;
    handler.subscribe<MixedAction>([&](std::string result) {
        std::scoped_lock lock{seenMtx};
        seen = std::move(result);
        fired.store(true);
    });

    handler.set<&MixedAction::name>(std::string{"alpha"});
    handler.set<&MixedAction::inner>(Inner{42});
    handler.set<&MixedAction::count>(7);

    waitFor([&] { return fired.load(); });
    std::scoped_lock lock{seenMtx};
    REQUIRE(seen == "alpha:42:7");
}

TEST_CASE("Subscription: unsubscribe preserves the draft", "[bridge][subscription]") {
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<FormModel> handler{bridge, &cbExec};

    std::atomic<double> last{-1.0};
    handler.subscribe<FormAction>([&](double sum) { last.store(sum); });

    handler.set<&FormAction::a>(1.0);
    handler.set<&FormAction::b>(2.0);
    handler.set<&FormAction::c>(3.0);
    waitFor([&] { return last.load() == 6.0; });

    handler.unsubscribe<FormAction>();
    // The draft is still intact. Mutate one more field; the action will fire
    // again (no subscriber → result drops) but the draft state persists.
    handler.set<&FormAction::d>(10.0);
    std::this_thread::sleep_for(std::chrono::milliseconds{30});

    // Re-subscribe; the next set must fire against the *preserved* draft state
    // (a/b/c/d already populated), producing a + b + c + d + e = 16 + e.
    last.store(-1.0);
    handler.subscribe<FormAction>([&](double sum) { last.store(sum); });
    handler.set<&FormAction::e>(100.0);
    waitFor([&] { return last.load() != -1.0; });
    REQUIRE(last.load() == 116.0);
}

TEST_CASE("Subscription: works under morph::backend::SimulatedRemoteBackend", "[bridge][subscription][remote]") {
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    SyncExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::SimulatedRemoteBackend>(*server)};
    morph::bridge::BridgeHandler<FormModel> handler{bridge, &cbExec};

    std::atomic<double> last{-1.0};
    handler.subscribe<FormAction>([&](double sum) { last.store(sum); });

    handler.set<&FormAction::a>(2.0);
    handler.set<&FormAction::b>(3.0);
    handler.set<&FormAction::c>(5.0);

    waitFor([&] { return last.load() == 10.0; }, std::chrono::milliseconds{4000});
    REQUIRE(last.load() == 10.0);
}

TEST_CASE("Subscription: draft survives switchBackend", "[bridge][subscription][switch]") {
    morph::exec::ThreadPoolExecutor pool1{2};
    morph::exec::ThreadPoolExecutor pool2{2};
    SyncExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool1)};
    morph::bridge::BridgeHandler<FormModel> handler{bridge, &cbExec};

    std::atomic<double> last{-1.0};
    handler.subscribe<FormAction>([&](double sum) { last.store(sum); });

    // Build a partial draft — validator does not pass yet, so nothing fires.
    handler.set<&FormAction::a>(1.0);
    handler.set<&FormAction::b>(2.0);
    std::this_thread::sleep_for(std::chrono::milliseconds{30});
    REQUIRE(last.load() == -1.0);

    // Switch backends mid-edit. The draft lives in the handler, so it survives.
    bridge.switchBackend(std::make_unique<morph::backend::LocalBackend>(pool2));

    // Complete the draft. The fire must reach the new backend.
    handler.set<&FormAction::c>(3.0);
    waitFor([&] { return last.load() != -1.0; });
    REQUIRE(last.load() == 6.0);
}

TEST_CASE("Subscription: re-fires after a failed execute", "[bridge][subscription]") {
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<FormModel> handler{bridge, &cbExec};

    std::atomic<int> okFired{0};
    std::atomic<int> errFired{0};
    std::atomic<int> lastOk{-1};
    handler.subscribe<FlakyAction>(
        [&](int val) {
            lastOk.store(val);
            okFired.fetch_add(1);
        },
        [&](const std::exception_ptr& /*unused*/) { errFired.fetch_add(1); });

    // mode=0 → execute throws → errFired increments, running flag must reset.
    handler.set<&FlakyAction::mode>(0);
    waitFor([&] { return errFired.load() >= 1; });
    REQUIRE(errFired.load() == 1);
    REQUIRE(okFired.load() == 0);

    // mode=5 → execute succeeds → okFired increments. If the previous error
    // path failed to clear `running`, this set would silently be queued as
    // `pending` and never actually run.
    handler.set<&FlakyAction::mode>(5);
    waitFor([&] { return okFired.load() >= 1; });
    REQUIRE(okFired.load() == 1);
    REQUIRE(lastOk.load() == 10);
    REQUIRE(errFired.load() == 1);
}

TEST_CASE("Subscription: unhandled errors route to the framework logger", "[bridge][subscription]") {
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<FormModel> handler{bridge, &cbExec};

    // Save and restore the global logger around the test so we don't pollute
    // other tests with our spy.
    auto savedLevel = morph::log::getLogLevel();
    morph::log::setLogLevel(morph::log::LogLevel::debug);
    std::mutex captureMtx;
    std::string captured;
    morph::log::setLogger([&](morph::log::LogLevel /*lvl*/, std::string_view msg) {
        std::scoped_lock lock{captureMtx};
        captured.append(msg).append("\n");
    });

    // Subscribe with success-only (no errCb). The action throws — without our
    // fallback, the error would be silently swallowed because the framework's
    // internal .onError attachment suppresses CompletionState's orphan log.
    handler.subscribe<ThrowAction>([](int /*unused*/) {});
    handler.set<&ThrowAction::trigger>(1);

    waitFor([&] {
        std::scoped_lock lock{captureMtx};
        return captured.contains("[subscription:");
    });

    {
        std::scoped_lock lock{captureMtx};
        REQUIRE(captured.contains("[subscription:Test_ThrowAction]"));
        REQUIRE(captured.contains("boom"));
    }

    morph::log::setLogger(nullptr);
    morph::log::setLogLevel(savedLevel);
}

TEST_CASE("Subscription: bursts coalesce while a fire is in flight", "[bridge][subscription]") {
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<FormModel> handler{bridge, &cbExec};

    std::atomic<int> fires{0};
    std::atomic<int> lastSeen{-1};
    handler.subscribe<SlowAction>([&](int val) {
        lastSeen.store(val);
        fires.fetch_add(1);
    });

    // First set kicks off a fire that will sleep ~40ms inside execute.
    handler.set<&SlowAction::seq>(1);

    // Burst many sets while that fire is in flight. With one-running +
    // one-pending coalescing, the in-flight call completes, then a single
    // re-fire runs with the latest snapshot (seq=10).
    for (int idx = 2; idx <= 10; ++idx) {
        handler.set<&SlowAction::seq>(idx);
    }

    waitFor([&] { return lastSeen.load() == 10; }, std::chrono::milliseconds{4000});

    REQUIRE(lastSeen.load() == 10);
    // 10 set<>() calls; without coalescing we'd see up to 10 fires. The
    // invariant is "strictly fewer fires than sets, and the last fire used
    // the latest snapshot". In practice this is typically 2 (the first +
    // one coalesced re-fire), but we test the invariant, not the exact count.
    REQUIRE(fires.load() >= 1);
    REQUIRE(fires.load() < 10);
}
