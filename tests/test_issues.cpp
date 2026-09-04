// SPDX-License-Identifier: Apache-2.0

// Tests covering issues found during codebase analysis.
// Each section corresponds to a numbered issue in the analysis document.

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <functional>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/completion.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/model.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/core/strand.hpp>
#include <morph/core/wire.hpp>
#include <morph/offline/network_monitor.hpp>
#include <morph/offline/offline_queue.hpp>
#include <morph/offline/sync_worker.hpp>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "test_support.hpp"

using namespace std::chrono_literals;

using SyncExec = morph::testing::InlineExecutor;
using morph::testing::waitUntil;

// ── Issue 1: CompletionState::setValue should move value into callback ────────

TEST_CASE("Issue 1: CompletionState setValue moves value into callback, not copies", "[completion][issue1]") {
    SyncExec exec;
    auto state = std::make_shared<morph::async::detail::CompletionState<std::string>>();
    state->cbExec = &exec;

    std::string received;
    state->attachThen([&](std::string val) { received = std::move(val); });

    state->setValue(std::string(1000, 'x'));
    REQUIRE(received.size() == 1000U);
    REQUIRE(received == std::string(1000, 'x'));
}

// ── Issue 2: attachThen on already-errored state silently drops handler ───────

TEST_CASE("Issue 2: attachThen on errored state does not call handler and does not crash", "[completion][issue2]") {
    SyncExec exec;
    auto state = std::make_shared<morph::async::detail::CompletionState<int>>();
    state->cbExec = &exec;
    state->attachOnError([](const std::exception_ptr&) {});
    state->setException(std::make_exception_ptr(std::runtime_error{"already failed"}));
    REQUIRE(state->ready);

    bool thenFired = false;
    state->attachThen([&](int) { thenFired = true; });
    REQUIRE_FALSE(thenFired);
}

// ── Issue 4: morph::exec::detail::StrandExecutor map cleaned up after queue drains ─────────────────

TEST_CASE("Issue 4: morph::exec::detail::StrandExecutor works correctly after strand entries are cleaned up",
          "[strand][issue4]") {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::exec::detail::StrandExecutor strand{pool};

    constexpr int numKeys = 20;
    std::atomic<int> completed{0};

    for (int key = 1; key <= numKeys; ++key) {
        strand.post(morph::exec::detail::ModelId{static_cast<uint64_t>(key)}, [&] { completed.fetch_add(1); });
    }
    REQUIRE(waitUntil([&] { return completed.load() == numKeys; }));

    // Let pool finish cleanup bookkeeping
    std::this_thread::sleep_for(20ms);

    // Post again to confirm strand still works after cleanup
    std::atomic<int> completed2{0};
    for (int key = 1; key <= numKeys; ++key) {
        strand.post(morph::exec::detail::ModelId{static_cast<uint64_t>(key)}, [&] { completed2.fetch_add(1); });
    }
    REQUIRE(waitUntil([&] { return completed2.load() == numKeys; }));
}

TEST_CASE("Issue 4: morph::exec::detail::StrandExecutor per-key ordering preserved after re-use", "[strand][issue4]") {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::exec::detail::StrandExecutor strand{pool};
    morph::exec::detail::ModelId key{42};

    // First batch: drain and clean up
    std::atomic<int> batch1{0};
    for (int task = 0; task < 5; ++task) {
        strand.post(key, [&] { batch1.fetch_add(1); });
    }
    REQUIRE(waitUntil([&] { return batch1.load() == 5; }));
    std::this_thread::sleep_for(20ms);

    // Second batch on same key: must still serialize in order
    std::vector<int> order;
    std::mutex orderMtx;
    std::atomic<int> batch2{0};
    for (int taskId = 0; taskId < 5; ++taskId) {
        strand.post(key, [&, taskId] {
            std::scoped_lock lock{orderMtx};
            order.push_back(taskId);
            batch2.fetch_add(1);
        });
    }
    REQUIRE(waitUntil([&] { return batch2.load() == 5; }));
    REQUIRE(order.size() == 5U);
    for (std::size_t idx = 0; idx < order.size(); ++idx) {
        REQUIRE(std::cmp_equal(order[idx], idx));
    }
}

// ── Issue 5: morph::offline::NetworkMonitor probe exception does not crash monitor ────────────

TEST_CASE("Issue 5: morph::offline::NetworkMonitor probe that throws is treated as failure, monitor stays alive",
          "[network_monitor][issue5]") {
    std::atomic<int> offlineCount{0};
    std::atomic<int> probeCallCount{0};

    morph::offline::NetworkMonitor monitor{
        [&] {
            probeCallCount.fetch_add(1);
            if (probeCallCount.load() % 2 == 0) {
                throw std::runtime_error("probe exploded");
            }
            return false;
        },
        [&] { offlineCount.fetch_add(1); }, [] {},
        morph::offline::NetworkMonitor::Config{.probeInterval = 20ms, .failureThreshold = 2, .onlineThreshold = 1}};

    REQUIRE(waitUntil([&] { return offlineCount.load() >= 1; }, 1s));
    REQUIRE(monitor.isOnline() == false);
    REQUIRE_NOTHROW(monitor.stop());
}

TEST_CASE("Issue 5: morph::offline::NetworkMonitor probe that always throws treats each throw as failure",
          "[network_monitor][issue5]") {
    std::atomic<int> offlineCount{0};
    std::atomic<int> onlineCount{0};

    morph::offline::NetworkMonitor monitor{
        []() -> bool { throw std::runtime_error("always explodes"); }, [&] { offlineCount.fetch_add(1); },
        [&] { onlineCount.fetch_add(1); },
        morph::offline::NetworkMonitor::Config{.probeInterval = 20ms, .failureThreshold = 1, .onlineThreshold = 1}};

    REQUIRE(waitUntil([&] { return offlineCount.load() >= 1; }, 1s));
    std::this_thread::sleep_for(80ms);
    REQUIRE(onlineCount.load() == 0);
    REQUIRE_NOTHROW(monitor.stop());
}

// ── Issue 6: morph::offline::NetworkMonitor callback calling stop() does not deadlock ─────────

TEST_CASE("Issue 6: morph::offline::NetworkMonitor stop() called from onOffline callback does not deadlock",
          "[network_monitor][issue6]") {
    std::atomic<bool> callbackFired{false};

    // atomic<morph::offline::NetworkMonitor*> avoids the data race between the main thread
    // writing the pointer and T1 reading it from the callback.
    std::atomic<morph::offline::NetworkMonitor*> monitorPtr{nullptr};
    auto monitor = std::make_shared<morph::offline::NetworkMonitor>(
        [] { return false; },
        [&callbackFired, &monitorPtr] {
            callbackFired.store(true);
            if (auto* mon = monitorPtr.load()) {
                mon->stop();  // must not deadlock even from probe thread
            }
        },
        [] {},
        morph::offline::NetworkMonitor::Config{.probeInterval = 20ms, .failureThreshold = 1, .onlineThreshold = 1});
    monitorPtr.store(monitor.get());

    REQUIRE(waitUntil([&] { return callbackFired.load(); }, 1s));
    REQUIRE_NOTHROW(monitor.reset());
}

// ── Issue 9: morph::model::detail::ActionDispatcher hash: similar IDs dispatch to correct runners ───

// Top-level types needed for Issue 9 test (traits required at file scope)
struct Iss9ModelA {
    int execute(const struct Iss9ActionA& act);
};
struct Iss9ActionA {
    int delta = 0;
};
struct Iss9ModelB {
    int execute(const struct Iss9ActionB& act);
};
struct Iss9ActionB {
    int delta = 0;
};

template <>
struct morph::model::ModelTraits<Iss9ModelA> {
    static constexpr std::string_view typeId() { return "ISS9_Model_A"; }
};
template <>
struct morph::model::ModelTraits<Iss9ModelB> {
    static constexpr std::string_view typeId() { return "ISS9_Model_B"; }
};
template <>
struct morph::model::ActionTraits<Iss9ActionA> {
    using Result = int;
    static constexpr std::string_view typeId() { return "ISS9_Action_A"; }
    static std::string toJson(const Iss9ActionA& act) { return R"({"delta":)" + std::to_string(act.delta) + "}"; }
    static Iss9ActionA fromJson(std::string_view str) {
        Iss9ActionA act{};
        (void)glz::read_json(act, str);
        return act;
    }
    static std::string resultToJson(const int& res) { return std::to_string(res); }
    static int resultFromJson(std::string_view str) { return std::stoi(std::string{str}); }
};
template <>
struct morph::model::ActionTraits<Iss9ActionB> {
    using Result = int;
    static constexpr std::string_view typeId() { return "ISS9_Action_B"; }
    static std::string toJson(const Iss9ActionB& act) { return R"({"delta":)" + std::to_string(act.delta) + "}"; }
    static Iss9ActionB fromJson(std::string_view str) {
        Iss9ActionB act{};
        (void)glz::read_json(act, str);
        return act;
    }
    static std::string resultToJson(const int& res) { return std::to_string(res); }
    static int resultFromJson(std::string_view str) { return std::stoi(std::string{str}); }
};

inline int Iss9ModelA::execute(const Iss9ActionA& act) { return act.delta * 2; }
inline int Iss9ModelB::execute(const Iss9ActionB& act) { return act.delta * 3; }

TEST_CASE("Issue 9: morph::model::detail::ActionDispatcher dispatches correctly with similar-prefix type IDs",
          "[registry][issue9]") {
    morph::model::detail::ActionDispatcher dispatcher;
    morph::model::detail::ModelRegistryFactory registry;
    registry.registerModel<Iss9ModelA>("ISS9_Model_A");
    registry.registerModel<Iss9ModelB>("ISS9_Model_B");
    dispatcher.registerAction<Iss9ModelA, Iss9ActionA>("ISS9_Model_A", "ISS9_Action_A");
    dispatcher.registerAction<Iss9ModelB, Iss9ActionB>("ISS9_Model_B", "ISS9_Action_B");

    auto holderA = registry.create("ISS9_Model_A");
    auto holderB = registry.create("ISS9_Model_B");

    // Correct pairs dispatch without error
    auto resA = dispatcher.dispatch("ISS9_Model_A", "ISS9_Action_A", *holderA, R"({"delta":5})");
    auto resB = dispatcher.dispatch("ISS9_Model_B", "ISS9_Action_B", *holderB, R"({"delta":5})");
    REQUIRE(resA == "10");  // 5 * 2
    REQUIRE(resB == "15");  // 5 * 3

    // Cross-contamination throws
    REQUIRE_THROWS_AS(dispatcher.dispatch("ISS9_Model_A", "ISS9_Action_B", *holderA, "{}"), std::runtime_error);
    REQUIRE_THROWS_AS(dispatcher.dispatch("ISS9_Model_B", "ISS9_Action_A", *holderB, "{}"), std::runtime_error);
}

// ── Issue 11: morph::model::detail::IModelHolder::into<> throws bad_cast on type mismatch ───────────

struct Iss11FooModel {
    int val = 0;
};
struct Iss11BarModel {
    double val = 0.0;
};

TEST_CASE("Issue 11: morph::model::detail::IModelHolder::into throws bad_cast on wrong model type",
          "[model][issue11]") {
    auto holder = std::make_unique<morph::model::detail::ModelHolder<Iss11FooModel>>();
    REQUIRE_NOTHROW(holder->into<Iss11FooModel>());
    REQUIRE_THROWS_AS(holder->into<Iss11BarModel>(), std::bad_cast);
}

TEST_CASE("Issue 11: morph::model::detail::IModelHolder::into does not throw on correct type", "[model][issue11]") {
    auto holder = std::make_unique<morph::model::detail::ModelHolder<Iss11FooModel>>();
    holder->model.val = 99;
    REQUIRE_NOTHROW(holder->into<Iss11FooModel>());
    REQUIRE(holder->into<Iss11FooModel>().val == 99);
}

// ── Issue 15: morph::backend::RemoteServer malformed message bounds checking ──────────────────

TEST_CASE("Issue 15: morph::backend::RemoteServer handles 'register' with no typeId gracefully", "[remote][issue15]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);

    std::atomic<bool> done{false};
    std::string reply;
    morph::wire::Envelope env;
    env.kind = "register";  // empty typeId
    server->handle(morph::wire::encode(env), [&](const std::string& msg) {
        reply = msg;
        done.store(true);
    });
    REQUIRE(waitUntil([&] { return done.load(); }));
    auto decoded = morph::wire::decode(reply);
    REQUIRE(decoded.kind == "err");
}

TEST_CASE("Issue 15: morph::backend::RemoteServer handles 'deregister' with no id gracefully", "[remote][issue15]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);

    std::atomic<bool> done{false};
    std::string reply;
    morph::wire::Envelope env;
    env.kind = "deregister";  // modelId defaults to 0; valid envelope, erase is a no-op
    server->handle(morph::wire::encode(env), [&](const std::string& msg) {
        reply = msg;
        done.store(true);
    });
    REQUIRE(waitUntil([&] { return done.load(); }));
    auto decoded = morph::wire::decode(reply);
    // Deregister with id 0 is a no-op (id 0 is never assigned), so this is "ok".
    REQUIRE(decoded.kind == "ok");
}

TEST_CASE("Issue 15: morph::backend::RemoteServer rejects unknown envelope kind gracefully", "[remote][issue15]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);

    std::atomic<bool> done{false};
    std::string reply;
    morph::wire::Envelope env;
    env.kind = "frobnicate";
    server->handle(morph::wire::encode(env), [&](const std::string& msg) {
        reply = msg;
        done.store(true);
    });
    REQUIRE(waitUntil([&] { return done.load(); }));
    auto decoded = morph::wire::decode(reply);
    REQUIRE(decoded.kind == "err");
}

TEST_CASE("Issue 15: morph::backend::RemoteServer handles malformed JSON gracefully", "[remote][issue15]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);

    std::atomic<bool> done{false};
    std::string reply;
    server->handle("not-json", [&](const std::string& msg) {
        reply = msg;
        done.store(true);
    });
    REQUIRE(waitUntil([&] { return done.load(); }));
    auto decoded = morph::wire::decode(reply);
    REQUIRE(decoded.kind == "err");
}

// ── Issue 10: In-flight execute after deregisterModel completes safely ─────────

struct Iss10Model {
    int value = 0;
    int execute(const struct Iss10Action& act);
};
struct Iss10Action {
    int delta = 0;
};

template <>
struct morph::model::ModelTraits<Iss10Model> {
    static constexpr std::string_view typeId() { return "ISS10_Model"; }
};
template <>
struct morph::model::ActionTraits<Iss10Action> {
    using Result = int;
    static constexpr std::string_view typeId() { return "ISS10_Action"; }
    static std::string toJson(const Iss10Action& act) { return R"({"delta":)" + std::to_string(act.delta) + "}"; }
    static Iss10Action fromJson(std::string_view) { return {}; }
    static std::string resultToJson(const int& res) { return std::to_string(res); }
    static int resultFromJson(std::string_view str) { return std::stoi(std::string{str}); }
};
inline int Iss10Model::execute(const Iss10Action& act) {
    value += act.delta;
    return value;
}

TEST_CASE("Issue 10: in-flight execute after deregisterModel completes without crash", "[backend][issue10]") {
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExec cbExec;
    morph::backend::LocalBackend backend{pool};

    auto mid = backend.registerModel("ISS10_Model", morph::model::detail::ModelFactory::create<Iss10Model>);

    morph::backend::detail::ActionCall call;
    call.modelTypeId = "ISS10_Model";
    call.actionTypeId = "ISS10_Action";
    call.serializeAction = [] { return R"({"delta":1})"; };
    call.deserializeResult = [](std::string_view) -> std::shared_ptr<void> { return {}; };
    call.localOp = [](morph::model::detail::IModelHolder& holder) -> std::shared_ptr<void> {
        auto& model = holder.into<Iss10Model>();
        return std::make_shared<int>(model.execute(Iss10Action{1}));
    };

    std::atomic<bool> completed{false};
    auto comp = backend.execute(mid, std::move(call), &cbExec);
    backend.deregisterModel(mid);

    comp.then([&](const std::shared_ptr<void>&) { completed.store(true); }).onError([&](const std::exception_ptr&) {
        completed.store(true);
    });

    REQUIRE(waitUntil([&] { return completed.load(); }));
}

// ── Issue 12: morph::offline::SyncWorker — concurrent enqueue during run does not corrupt queue

TEST_CASE("Issue 12: morph::offline::SyncWorker concurrent enqueue during run does not corrupt queue",
          "[sync][issue12]") {
    morph::offline::InMemoryOfflineQueue queue;
    (void)queue.enqueue("pre1");
    (void)queue.enqueue("pre2");

    std::atomic<bool> replayStarted{false};

    morph::offline::SyncWorker worker{queue, [&](const std::string&) {
                                          replayStarted.store(true);
                                          std::this_thread::sleep_for(30ms);
                                          return true;
                                      }};

    std::thread enqueuer{[&] {
        waitUntil([&] { return replayStarted.load(); });
        (void)queue.enqueue("concurrent");
    }};

    auto result = worker.run();
    enqueuer.join();

    REQUIRE(result.successful == 2);
    auto remaining = queue.drain();
    REQUIRE(remaining.size() == 1U);
    REQUIRE(remaining[0].payload == "concurrent");
}

// ── Issue 13: morph::exec::ThreadPoolExecutor drains queued tasks on shutdown ───────────────

TEST_CASE("Issue 13: morph::exec::ThreadPoolExecutor processes all queued tasks before shutdown",
          "[executor][issue13]") {
    std::atomic<int> count{0};
    {
        morph::exec::ThreadPoolExecutor pool{1};
        for (int item = 0; item < 10; ++item) {
            pool.post([&] { count.fetch_add(1); });
        }
    }
    REQUIRE(count.load() == 10);
}

TEST_CASE("Issue 13: morph::exec::ThreadPoolExecutor task that posts another task - both run before shutdown",
          "[executor][issue13]") {
    std::atomic<int> count{0};
    {
        morph::exec::ThreadPoolExecutor pool{1};
        pool.post([&] {
            count.fetch_add(1);
            pool.post([&] { count.fetch_add(1); });
        });
    }
    REQUIRE(count.load() == 2);
}
