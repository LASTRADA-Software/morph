// SPDX-License-Identifier: Apache-2.0

// Conflict-resolution integration tests.
//
// Core invariant tested: the framework fires onBackendChanged() on the model
// instance living in the NEW backend. All replay, conflict detection, and merge
// logic belongs to the model. The framework is untouched.
//
// Test model: OrderModel
//   Tracks results of onBackendChanged() in observable public counters.
//   The conflict checker and resolver are injected at construction so each
//   test can control the scenario without subclassing.

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <functional>
#include <morph/attributes.hpp>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
#include <morph/offline/offline_queue.hpp>
#include <string>
#include <thread>
#include <vector>

#include "test_support.hpp"

using namespace std::chrono_literals;

using SyncExec = morph::testing::InlineExecutor;

// ── Domain types ──────────────────────────────────────────────────────────────

struct OrderQueryAction {};  // read-only  -  returns notifyCount from the model instance

// ── OrderModel ────────────────────────────────────────────────────────────────
//
// The model holds a reference to a shared morph::offline::IOfflineQueue.
// onBackendChanged() drains the queue and processes each item:
//   - ConflictChecker(payload) → true  : conflict path
//   - ConflictResolver(payload) → ""   : discard (markDone, don't apply)
//   - ConflictResolver(payload) → text : merge (markDone, record merge)
//   - ConflictChecker(payload) → false : clean replay (markDone)
//
// All counters are plain ints  -  onBackendChanged runs on the backend strand
// (single-threaded per model), so no locking is needed here.

class OrderModel {
public:
    using ConflictChecker = std::function<bool(const std::string&)>;
    using ConflictResolver = std::function<std::string(const std::string&)>;

    OrderModel(morph::offline::IOfflineQueue& queue MORPH_LIFETIMEBOUND, ConflictChecker check MORPH_LIFETIMEBOUND,
               ConflictResolver resolve MORPH_LIFETIMEBOUND)
        : _queue{queue}, _check{std::move(check)}, _resolve{std::move(resolve)} {}

    [[nodiscard]] int execute(const OrderQueryAction&) const { return notifyCount; }

    void onBackendChanged() {
        ++notifyCount;
        for (auto& item : _queue.drain()) {
            if (_check(item.payload)) {
                std::string merged = _resolve(item.payload);
                if (merged.empty()) {
                    ++discarded;
                } else {
                    ++mergedCount;
                }
            } else {
                ++replayed;
            }
            _queue.markDone(item.id);  // always remove from queue after handling
        }
    }

    int notifyCount = 0;
    int replayed = 0;
    int mergedCount = 0;
    int discarded = 0;

private:
    morph::offline::IOfflineQueue& _queue;
    ConflictChecker _check;
    ConflictResolver _resolve;
};

// ── Traits ────────────────────────────────────────────────────────────────────

template <>
struct morph::model::ModelTraits<OrderModel> {
    static constexpr std::string_view typeId() { return "CR_OrderModel"; }
};

template <>
struct morph::model::ActionTraits<OrderQueryAction> {
    using Result = int;
    static constexpr std::string_view typeId() { return "CR_OrderQueryAction"; }
    static std::string toJson(const OrderQueryAction&) { return "{}"; }
    static OrderQueryAction fromJson(std::string_view) { return {}; }
    static std::string resultToJson(const int& res) { return std::to_string(res); }
    static int resultFromJson(std::string_view str) { return std::stoi(std::string{str}); }
};

// ── Factory helper ────────────────────────────────────────────────────────────
//
// Builds a morph::bridge::detail::HandlerBinding whose modelFactory captures the queue and resolution
// callables. morph::bridge::Bridge uses this factory to create the model instance for each
// new backend  -  allowing the same queue and resolvers to be shared.

// queue must outlive the returned binding and all model instances created from it
// (captured by reference in the factory lambda).
static std::shared_ptr<morph::bridge::detail::HandlerBinding> makeOrderBinding(morph::offline::IOfflineQueue& queue,
                                                                               OrderModel::ConflictChecker check,
                                                                               OrderModel::ConflictResolver resolve) {
    auto binding = std::make_shared<morph::bridge::detail::HandlerBinding>();
    binding->typeId = std::string{morph::model::ModelTraits<OrderModel>::typeId()};
    binding->modelFactory = [&queue, check = std::move(check),
                             resolve =
                                 std::move(resolve)]() mutable -> std::unique_ptr<morph::model::detail::IModelHolder> {
        // The model is built here and moved into the holder, rather than
        // forwarding (queue, check, resolve) through `ModelHolder`'s variadic
        // constructor. Forwarding makes the holder's constructor template the
        // place where `queue`'s reference escapes into a member, and Clang then
        // asks for `[[clang::lifetimebound]]` on that *shared* parameter pack —
        // an annotation that is right for this instantiation and wrong for the
        // many that move a self-contained model in, where it turns every
        // `ModelHolder<M>(prvalue...)` into a false dangling report. Binding the
        // reference here keeps the contract stated where it actually holds:
        // OrderModel's own constructor, just above.
        return std::make_unique<morph::model::detail::ModelHolder<OrderModel>>(OrderModel{queue, check, resolve});
    };
    return binding;
}

// ── Helper: wait for a morph::async::Completion<int> ───────────────────────────────────────

static int waitInt(auto completion) {
    std::atomic<int> result{-999};
    std::move(completion).then([&](int val) { result.store(val); }).onError([](const std::exception_ptr&) {});
    for (int i = 0; i < 100 && result.load() == -999; ++i) {
        std::this_thread::sleep_for(10ms);
    }
    return result.load();
}

// ── Helper: poll until the queue is actually drained ──────────────────────────
//
// switchBackend() dispatches the new model's construction and its
// onBackendChanged() call (which drains the offline queue) onto the new
// backend's own thread pool, asynchronously -- there is no signal the caller
// can block on directly. A fixed sleep_for() guessing "surely long enough" is
// exactly the failure mode this helper replaces.
//
// notifyCount is NOT the right signal to poll here: onBackendChanged()
// increments it *before* draining the queue (see OrderModel::onBackendChanged),
// so notifyCount reaching 1 only proves the call started, not that the drain
// finished -- polling on it raced the drain itself and intermittently observed
// a non-empty queue right after. drain() itself is the correct signal: it is
// a thread-safe, non-destructive snapshot read on InMemoryOfflineQueue (see its
// own doc comment), so polling it repeatedly is safe and it becomes empty at
// the exact moment every item has been handled and markDone'd. Bounded, not
// unbounded: returns false (rather than hanging) if it never empties.
static bool waitForQueueDrained(morph::offline::InMemoryOfflineQueue& queue) {
    for (int i = 0; i < 200; ++i) {
        if (queue.drain().empty()) {
            return true;
        }
        std::this_thread::sleep_for(10ms);
    }
    return false;
}

// ── Tests ─────────────────────────────────────────────────────────────────────

TEST_CASE("ConflictResolution: no conflicts  -  all items markDone on switchBackend", "[conflict]") {
    morph::exec::ThreadPoolExecutor pool1{2};
    morph::exec::ThreadPoolExecutor pool2{2};
    SyncExec cbExec;
    morph::offline::InMemoryOfflineQueue queue;

    queue.enqueue(R"({"amount":10})");
    queue.enqueue(R"({"amount":20})");
    queue.enqueue(R"({"amount":30})");

    auto binding = makeOrderBinding(
        queue, [](const std::string&) { return false; },      // no conflicts
        [](const std::string& payload) { return payload; });  // resolver never called

    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool1)};
    morph::bridge::BridgeHandler<OrderModel> handler{bridge, &cbExec, binding};

    bridge.switchBackend(std::make_unique<morph::backend::LocalBackend>(pool2));
    REQUIRE(waitForQueueDrained(queue));

    // All items removed from queue after clean replay.
    REQUIRE(queue.drain().empty());

    // Query the new model instance's counters via execute.
    REQUIRE(waitInt(handler.execute(OrderQueryAction{})) == 1);  // notifyCount == 1
}

TEST_CASE("ConflictResolution: conflicting items discarded  -  resolver returns empty", "[conflict]") {
    morph::exec::ThreadPoolExecutor pool1{2};
    morph::exec::ThreadPoolExecutor pool2{2};
    SyncExec cbExec;
    morph::offline::InMemoryOfflineQueue queue;

    queue.enqueue("clean");
    queue.enqueue("CONFLICT");
    queue.enqueue("clean2");

    auto binding = makeOrderBinding(
        queue, [](const std::string& payload) { return payload.contains("CONFLICT"); },
        [](const std::string&) -> std::string { return ""; });  // discard

    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool1)};
    morph::bridge::BridgeHandler<OrderModel> handler{bridge, &cbExec, binding};

    bridge.switchBackend(std::make_unique<morph::backend::LocalBackend>(pool2));
    REQUIRE(waitForQueueDrained(queue));

    // All three items removed regardless of outcome (discard also calls markDone).
    REQUIRE(queue.drain().empty());
}

TEST_CASE("ConflictResolution: conflicting items merged  -  resolver returns non-empty", "[conflict]") {
    morph::exec::ThreadPoolExecutor pool1{2};
    morph::exec::ThreadPoolExecutor pool2{2};
    SyncExec cbExec;
    morph::offline::InMemoryOfflineQueue queue;

    queue.enqueue("CONFLICT_A");
    queue.enqueue("CONFLICT_B");
    queue.enqueue("clean");

    auto binding = makeOrderBinding(
        queue, [](const std::string& payload) { return payload.contains("CONFLICT"); },
        [](const std::string&) -> std::string { return "merged_value"; });  // merge

    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool1)};
    morph::bridge::BridgeHandler<OrderModel> handler{bridge, &cbExec, binding};

    bridge.switchBackend(std::make_unique<morph::backend::LocalBackend>(pool2));
    REQUIRE(waitForQueueDrained(queue));

    // All three items processed and removed.
    REQUIRE(queue.drain().empty());
}

TEST_CASE("ConflictResolution: framework fires onBackendChanged exactly once per switchBackend", "[conflict]") {
    // Verifies the framework invariant: each switchBackend() call triggers
    // exactly one onBackendChanged() on the new model instance.
    // We switch three times and confirm notifyCount == 1 on each new instance
    // (each switch creates a fresh model  -  its own notifyCount starts at 0).

    morph::exec::ThreadPoolExecutor pool1{2};
    morph::exec::ThreadPoolExecutor pool2{2};
    morph::exec::ThreadPoolExecutor pool3{2};
    morph::exec::ThreadPoolExecutor pool4{2};
    SyncExec cbExec;
    morph::offline::InMemoryOfflineQueue queue;

    auto binding = makeOrderBinding(
        queue, [](const std::string&) { return false; }, [](const std::string& payload) { return payload; });

    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool1)};
    morph::bridge::BridgeHandler<OrderModel> handler{bridge, &cbExec, binding};

    // Switch once  -  new model instance created, notifyCount becomes 1.
    // waitInt's own poll loop is the wait here: no separate fixed sleep needed.
    bridge.switchBackend(std::make_unique<morph::backend::LocalBackend>(pool2));
    REQUIRE(waitInt(handler.execute(OrderQueryAction{})) == 1);

    // Switch again  -  another fresh instance, again notifyCount == 1.
    bridge.switchBackend(std::make_unique<morph::backend::LocalBackend>(pool3));
    REQUIRE(waitInt(handler.execute(OrderQueryAction{})) == 1);

    // Third switch.
    bridge.switchBackend(std::make_unique<morph::backend::LocalBackend>(pool4));
    REQUIRE(waitInt(handler.execute(OrderQueryAction{})) == 1);
}

TEST_CASE("ConflictResolution: full offline scenario  -  accumulate offline, sync on reconnect",
          "[conflict][integration]") {
    // Real-world scenario:
    //   1. App starts offline (local backend).
    //   2. Three orders are placed offline  -  payloads enqueued manually
    //      (representing what the app would have queued).
    //   3. One payload is "stale"  -  server says it conflicts with an update
    //      made by another user while we were offline.
    //   4. Network recovers  -  switchBackend fires onBackendChanged.
    //   5. Model replays two clean items, merges one conflict, removes all from queue.
    //   6. Queue is empty; model is live on the new backend; execute still works.

    morph::exec::ThreadPoolExecutor localPool{2};
    morph::exec::ThreadPoolExecutor remotePool{2};
    SyncExec cbExec;
    morph::offline::InMemoryOfflineQueue queue;

    // Simulate three offline writes.
    queue.enqueue(R"({"item":"order_A"})");        // clean
    queue.enqueue(R"({"item":"order_B_stale"})");  // stale  -  conflicts with server
    queue.enqueue(R"({"item":"order_C"})");        // clean

    auto binding = makeOrderBinding(
        queue,
        // Conflict checker: payloads containing "stale" were superseded.
        [](const std::string& payload) { return payload.contains("stale"); },
        // Resolver: take server version (non-empty → merge applied).
        [](const std::string&) -> std::string { return R"({"item":"order_B_server"})"; });

    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(localPool)};
    morph::bridge::BridgeHandler<OrderModel> handler{bridge, &cbExec, binding};

    // Simulate reconnection  -  switch to remote backend.
    bridge.switchBackend(std::make_unique<morph::backend::LocalBackend>(remotePool));
    REQUIRE(waitForQueueDrained(queue));

    // Queue fully drained: 2 clean replays + 1 merge = 3 markDone calls.
    REQUIRE(queue.drain().empty());

    // New model is live  -  execute works.
    REQUIRE(waitInt(handler.execute(OrderQueryAction{})) == 1);
}
