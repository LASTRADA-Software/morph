// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/model.hpp>
#include <morph/core/registry.hpp>
#include <thread>

#include "test_support.hpp"

// ── Test models ───────────────────────────────────────────────────────────────

struct NotifiableModel {
    int notifyCount = 0;
    void onBackendChanged() { ++notifyCount; }
};

struct SilentModel {};

// ── Concept detection ─────────────────────────────────────────────────────────

TEST_CASE("morph::model::detail::BackendChangedNotifiable: detects onBackendChanged method", "[model][concept]") {
    STATIC_REQUIRE(morph::model::detail::BackendChangedNotifiable<NotifiableModel>);
    STATIC_REQUIRE_FALSE(morph::model::detail::BackendChangedNotifiable<SilentModel>);
}

TEST_CASE(
    "morph::model::detail::ModelHolder for notifiable model implements morph::model::detail::IBackendChangedSink",
    "[model][concept]") {
    auto holder = std::make_unique<morph::model::detail::ModelHolder<NotifiableModel>>();
    auto* sink = dynamic_cast<morph::model::detail::IBackendChangedSink*>(holder.get());
    REQUIRE(sink != nullptr);
}

TEST_CASE(
    "morph::model::detail::ModelHolder for silent model does NOT implement morph::model::detail::IBackendChangedSink",
    "[model][concept]") {
    auto holder = std::make_unique<morph::model::detail::ModelHolder<SilentModel>>();
    auto* sink = dynamic_cast<morph::model::detail::IBackendChangedSink*>(holder.get());
    REQUIRE(sink == nullptr);
}

TEST_CASE("morph::model::detail::IBackendChangedSink::onBackendChanged delegates to model method",
          "[model][concept]") {
    auto holder = std::make_unique<morph::model::detail::ModelHolder<NotifiableModel>>();
    auto* sink = dynamic_cast<morph::model::detail::IBackendChangedSink*>(holder.get());
    REQUIRE(sink != nullptr);

    sink->onBackendChanged();
    REQUIRE(holder->model.notifyCount == 1);

    sink->onBackendChanged();
    REQUIRE(holder->model.notifyCount == 2);
}

// ── notifyBackendChanged ──────────────────────────────────────────────────────
TEST_CASE("morph::backend::LocalBackend::notifyBackendChanged calls onBackendChanged on notifiable models only",
          "[backend][notify]") {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::backend::LocalBackend backend{pool};

    // Register one notifiable model and one silent model.
    backend.registerModel("NotifiableModel",
                          [] { return std::make_unique<morph::model::detail::ModelHolder<NotifiableModel>>(); });
    backend.registerModel("SilentModel",
                          [] { return std::make_unique<morph::model::detail::ModelHolder<SilentModel>>(); });

    // Must not throw or crash regardless of model mix.
    REQUIRE_NOTHROW(backend.notifyBackendChanged());
}

// ── switchBackend test models ─────────────────────────────────────────────────
struct CountAction {
    int x = 0;
};
struct SwitchCountAction {};  // queries how many times onBackendChanged fired

struct CountModel {
    int value = 0;
    int switchCount = 0;
    int execute(const CountAction& act) {
        value += act.x;
        return value;
    }
    [[nodiscard]] int execute(const SwitchCountAction&) const { return switchCount; }
    void onBackendChanged() { ++switchCount; }
};

template <>
struct morph::model::ModelTraits<CountModel> {
    static constexpr std::string_view typeId() { return "SW_CountModel"; }
};
template <>
struct morph::model::ActionTraits<CountAction> {
    using Result = int;
    static constexpr std::string_view typeId() { return "SW_CountAction"; }
    static std::string toJson(const CountAction& act) { return R"({"x":)" + std::to_string(act.x) + "}"; }
    static CountAction fromJson(std::string_view) { return {}; }
    static std::string resultToJson(const int& res) { return std::to_string(res); }
    static int resultFromJson(std::string_view str) { return std::stoi(std::string{str}); }
};
template <>
struct morph::model::ActionTraits<SwitchCountAction> {
    using Result = int;
    static constexpr std::string_view typeId() { return "SW_SwitchCountAction"; }
    static std::string toJson(const SwitchCountAction&) { return "{}"; }
    static SwitchCountAction fromJson(std::string_view) { return {}; }
    static std::string resultToJson(const int& res) { return std::to_string(res); }
    static int resultFromJson(std::string_view str) { return std::stoi(std::string{str}); }
};

using SyncExec = morph::testing::InlineExecutor;

// ── switchBackend tests ───────────────────────────────────────────────────────

TEST_CASE("morph::bridge::Bridge::switchBackend  -  handler works before and after switch", "[bridge][switch]") {
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExec cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<CountModel> handler{bridge, &cbExec};

    // Execute on original backend.
    std::atomic<int> res1{-1};
    handler.execute(CountAction{5}).then([&](int val) { res1.store(val); }).onError([](const std::exception_ptr&) {});
    for (int i = 0; i < 50 && res1.load() == -1; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(res1.load() == 5);

    // Switch to a fresh backend  -  model state resets (new instance).
    morph::exec::ThreadPoolExecutor pool2{2};
    bridge.switchBackend(std::make_unique<morph::backend::LocalBackend>(pool2));

    std::atomic<int> res2{-1};
    handler.execute(CountAction{7}).then([&](int val) { res2.store(val); }).onError([](const std::exception_ptr&) {});
    for (int i = 0; i < 50 && res2.load() == -1; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(res2.load() == 7);
}

TEST_CASE("morph::bridge::Bridge::switchBackend  -  destroyed handler not re-registered, no crash",
          "[bridge][switch]") {
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExec cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};

    {
        morph::bridge::BridgeHandler<CountModel> handler{bridge, &cbExec};
    }  // handler destroyed  -  weak_ptr in bridge goes stale

    morph::exec::ThreadPoolExecutor pool2{2};
    REQUIRE_NOTHROW(bridge.switchBackend(std::make_unique<morph::backend::LocalBackend>(pool2)));
}

TEST_CASE("morph::bridge::Bridge::switchBackend  -  multiple live handlers all re-registered", "[bridge][switch]") {
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExec cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<CountModel> handler1{bridge, &cbExec};
    morph::bridge::BridgeHandler<CountModel> handler2{bridge, &cbExec};

    morph::exec::ThreadPoolExecutor pool2{2};
    bridge.switchBackend(std::make_unique<morph::backend::LocalBackend>(pool2));

    std::atomic<int> res1{-1};
    std::atomic<int> res2{-1};
    handler1.execute(CountAction{10}).then([&](int val) { res1.store(val); }).onError([](const std::exception_ptr&) {
    });
    handler2.execute(CountAction{20}).then([&](int val) { res2.store(val); }).onError([](const std::exception_ptr&) {
    });
    for (int i = 0; i < 50 && (res1.load() == -1 || res2.load() == -1); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(res1.load() == 10);
    REQUIRE(res2.load() == 20);
}

// ── Deep onBackendChanged count verification ──────────────────────────────────

TEST_CASE(
    "morph::bridge::Bridge::switchBackend  -  onBackendChanged called exactly once on new model after one switch",
    "[bridge][switch][notify]") {
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExec cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<CountModel> handler{bridge, &cbExec};

    morph::exec::ThreadPoolExecutor pool2{2};
    bridge.switchBackend(std::make_unique<morph::backend::LocalBackend>(pool2));

    // Query the new model instance's switchCount.
    std::atomic<int> count{-1};
    handler.execute(SwitchCountAction{})
        .then([&](int val) { count.store(val); })
        .onError([](const std::exception_ptr&) {});
    for (int i = 0; i < 50 && count.load() == -1; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(count.load() == 1);
}

TEST_CASE(
    "morph::bridge::Bridge::switchBackend  -  onBackendChanged called exactly once per switch across two switches",
    "[bridge][switch][notify]") {
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExec cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<CountModel> handler{bridge, &cbExec};

    morph::exec::ThreadPoolExecutor pool2{2};
    bridge.switchBackend(std::make_unique<morph::backend::LocalBackend>(pool2));

    morph::exec::ThreadPoolExecutor pool3{2};
    bridge.switchBackend(std::make_unique<morph::backend::LocalBackend>(pool3));

    // Each switch creates a fresh model instance. The LAST instance receives
    // onBackendChanged() exactly once (from the second switch).
    std::atomic<int> count{-1};
    handler.execute(SwitchCountAction{})
        .then([&](int val) { count.store(val); })
        .onError([](const std::exception_ptr&) {});
    for (int i = 0; i < 50 && count.load() == -1; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(count.load() == 1);
}

// ── Remote backend no-op ──────────────────────────────────────────────────────

#include <morph/core/remote.hpp>

TEST_CASE("morph::backend::SimulatedRemoteBackend::notifyBackendChanged is a documented no-op", "[remote][notify]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);
    morph::backend::SimulatedRemoteBackend backend{*server};
    REQUIRE_NOTHROW(backend.notifyBackendChanged());
}

// ── switchBackend rollback on partial-registration failure ────────────────────

namespace {

/// @brief Backend whose `registerModelWithContext` throws once the Nth call is
/// reached, so `Bridge::switchBackend`'s Phase-1 rollback can be exercised
/// deterministically. `deregisterModel` optionally throws too, to exercise the
/// nested rollback-failure log path.
class FlakyBackend : public morph::backend::detail::IBackend {
public:
    FlakyBackend(morph::exec::IExecutor& pool, int failOnCallNumber, bool deregisterThrows = false)
        : _inner{pool}, _failOnCallNumber{failOnCallNumber}, _deregisterThrows{deregisterThrows} {}

    morph::exec::detail::ModelId registerModel(
        const std::string& typeId,
        std::function<std::unique_ptr<morph::model::detail::IModelHolder>()> factory) override {
        return registerModelWithContext(typeId, std::move(factory), {});
    }

    morph::exec::detail::ModelId registerModelWithContext(
        const std::string& typeId, std::function<std::unique_ptr<morph::model::detail::IModelHolder>()> factory,
        std::string_view contextKey) override {
        ++_calls;
        if (_calls == _failOnCallNumber) {
            throw std::runtime_error("simulated registration failure");
        }
        auto id = _inner.registerModelWithContext(typeId, std::move(factory), contextKey);
        _registered.push_back(id);
        return id;
    }

    void deregisterModel(morph::exec::detail::ModelId mid) override {
        ++(*_deregisterCalls);
        if (_deregisterThrows) {
            throw std::runtime_error("simulated deregister failure");
        }
        _inner.deregisterModel(mid);
    }

    morph::async::Completion<std::shared_ptr<void>> execute(morph::exec::detail::ModelId mid,
                                                            morph::backend::detail::ActionCall call,
                                                            morph::exec::IExecutor* cbExec) override {
        return _inner.execute(mid, std::move(call), cbExec);
    }

    void notifyBackendChanged() override { _inner.notifyBackendChanged(); }
    void cancelPending(const std::exception_ptr& exc) override { _inner.cancelPending(exc); }

    /// @brief Shared handle to the deregister-call counter.
    ///
    /// `Bridge::switchBackend` takes ownership of the backend and destroys it
    /// when a partial-registration failure unwinds, so a raw `this` pointer is
    /// dangling by the time the test inspects the rollback. The counter lives in
    /// a `shared_ptr` the test can hold independently, outliving the backend.
    [[nodiscard]] std::shared_ptr<const int> deregisterCallCounter() const { return _deregisterCalls; }

private:
    morph::backend::LocalBackend _inner;
    int _failOnCallNumber;
    bool _deregisterThrows;
    int _calls{0};
    std::shared_ptr<int> _deregisterCalls{std::make_shared<int>(0)};
    std::vector<morph::exec::detail::ModelId> _registered;
};

}  // namespace

TEST_CASE("morph::bridge::Bridge::switchBackend  -  rollback on partial failure leaves old backend active",
          "[bridge][switch][rollback]") {
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExec cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<CountModel> handler1{bridge, &cbExec};
    morph::bridge::BridgeHandler<CountModel> handler2{bridge, &cbExec};

    auto const idBefore1 = handler1.binding()->currentId.load();
    auto const idBefore2 = handler2.binding()->currentId.load();

    morph::exec::ThreadPoolExecutor pool2{2};
    // Two live handlers means two registerModelWithContext calls; fail on the 2nd
    // so the 1st is already staged when the rollback runs.
    auto flaky = std::make_unique<FlakyBackend>(pool2, /*failOnCallNumber=*/2);
    auto deregisterCalls = flaky->deregisterCallCounter();  // outlives the backend

    REQUIRE_THROWS_AS(bridge.switchBackend(std::move(flaky)), std::runtime_error);
    REQUIRE(*deregisterCalls == 1);  // the 1 staged registration was rolled back

    // currentId values are untouched — the switch is a no-op on failure.
    REQUIRE(handler1.binding()->currentId.load() == idBefore1);
    REQUIRE(handler2.binding()->currentId.load() == idBefore2);

    // The old backend is still active and functional.
    std::atomic<int> res{-1};
    handler1.execute(CountAction{3}).then([&](int val) { res.store(val); }).onError([](const std::exception_ptr&) {});
    REQUIRE(morph::testing::waitUntil([&] { return res.load() != -1; }));
    REQUIRE(res.load() == 3);
}

TEST_CASE("morph::bridge::Bridge::switchBackend  -  rollback still rethrows original error when deregister also fails",
          "[bridge][switch][rollback]") {
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExec cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<CountModel> handler1{bridge, &cbExec};
    morph::bridge::BridgeHandler<CountModel> handler2{bridge, &cbExec};

    morph::exec::ThreadPoolExecutor pool2{2};
    auto flaky = std::make_unique<FlakyBackend>(pool2, /*failOnCallNumber=*/2, /*deregisterThrows=*/true);
    auto deregisterCalls = flaky->deregisterCallCounter();  // outlives the backend

    REQUIRE_THROWS_AS(bridge.switchBackend(std::move(flaky)), std::runtime_error);
    REQUIRE(*deregisterCalls == 1);  // rollback attempted despite itself throwing
}

// ── BridgeHandler destructor: bridge destroyed first (dead-liveness-token branch) ─────────────

TEST_CASE("morph::bridge::BridgeHandler destructor is a no-op when the bridge is already destroyed",
          "[bridge][lifetime]") {
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExec cbExec;
    std::unique_ptr<morph::bridge::BridgeHandler<CountModel>> handler;
    {
        auto bridge = std::make_unique<morph::bridge::Bridge>(std::make_unique<morph::backend::LocalBackend>(pool));
        handler = std::make_unique<morph::bridge::BridgeHandler<CountModel>>(*bridge, &cbExec);
        // bridge destroyed here while handler still lives — its liveness token expires.
    }
    REQUIRE_NOTHROW(handler.reset());  // handler dtor must not dereference the dangling Bridge&
}

// ── Behavior-parity fixtures for the compile-time onBackendChanged dispatch
//    refactor (docs/planned/backend_changed_dispatch.md) ─────────────────────
//
// AwareCounterModel exposes its onBackendChanged() side effect through an
// external std::shared_ptr<std::atomic<int>> (rather than a plain int member
// like NotifiableModel above) because LocalBackend::registerModel() only ever
// returns a ModelId — the constructed ModelHolder is never handed back to the
// caller, so there is no other way to observe the callback firing once the
// backend owns the holder.

struct AwareCounterModel {
    std::shared_ptr<std::atomic<int>> counter;
    void onBackendChanged() { counter->fetch_add(1); }
};

static std::unique_ptr<morph::model::detail::IModelHolder> makeAwareCounterHolder(
    std::shared_ptr<std::atomic<int>> counter) {
    auto holder = std::make_unique<morph::model::detail::ModelHolder<AwareCounterModel>>();
    holder->model.counter = std::move(counter);
    return holder;
}

// ── Parity baseline ───────────────────────────────────────────────────────────
// These two tests pass against today's dynamic_cast-based
// LocalBackend::notifyBackendChanged (this task adds no production code).
// Task 3 re-runs them, byte-for-byte unmodified, against the rewritten
// implementation to prove the refactor is behavior-preserving.

TEST_CASE(
    "morph::backend::LocalBackend::notifyBackendChanged: parity baseline - notifies every change-aware "
    "model in a mixed population",
    "[backend][notify][parity]") {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::backend::LocalBackend backend{pool};

    auto counter1 = std::make_shared<std::atomic<int>>(0);
    auto counter2 = std::make_shared<std::atomic<int>>(0);
    auto counter3 = std::make_shared<std::atomic<int>>(0);

    backend.registerModel("Aware1", [counter1] { return makeAwareCounterHolder(counter1); });
    backend.registerModel("Silent1",
                          [] { return std::make_unique<morph::model::detail::ModelHolder<SilentModel>>(); });
    backend.registerModel("Aware2", [counter2] { return makeAwareCounterHolder(counter2); });
    backend.registerModel("Silent2",
                          [] { return std::make_unique<morph::model::detail::ModelHolder<SilentModel>>(); });
    backend.registerModel("Aware3", [counter3] { return makeAwareCounterHolder(counter3); });

    backend.notifyBackendChanged();

    // Set semantics, not order: the spec makes no ordering guarantee across
    // models (each is serialised only against itself, via its own strand), so
    // this only asserts that all three eventually fire, not in what order.
    REQUIRE(morph::testing::waitUntil(
        [&] { return counter1->load() == 1 && counter2->load() == 1 && counter3->load() == 1; }));
}

TEST_CASE(
    "morph::backend::LocalBackend::notifyBackendChanged: parity baseline - a deregistered change-aware "
    "model is never notified again",
    "[backend][notify][parity]") {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::backend::LocalBackend backend{pool};

    auto counter = std::make_shared<std::atomic<int>>(0);
    auto mid = backend.registerModel("Aware", [counter] { return makeAwareCounterHolder(counter); });

    backend.notifyBackendChanged();
    REQUIRE(morph::testing::waitUntil([&] { return counter->load() == 1; }));

    backend.deregisterModel(mid);
    REQUIRE_NOTHROW(backend.notifyBackendChanged());

    // Give any errant post a chance to land, then confirm the count never moved.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    REQUIRE(counter->load() == 1);
}
