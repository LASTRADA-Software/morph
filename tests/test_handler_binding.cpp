// SPDX-License-Identifier: Apache-2.0

#include <morph/backend.hpp>
#include <morph/bridge.hpp>
#include <morph/executor.hpp>
#include <morph/registry.hpp>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <thread>

#include "test_support.hpp"

using SyncExec = morph::testing::InlineExecutor;


struct HBAction {
    int x = 0;
};
struct HBModel {
    int execute(const HBAction& act) { return act.x + 1; }
};

template <>
struct morph::model::ModelTraits<HBModel> {
    static constexpr std::string_view typeId() { return "HB_Model"; }
};
template <>
struct morph::model::ActionTraits<HBAction> {
    using Result = int;
    static constexpr std::string_view typeId() { return "HB_Action"; }
    static std::string toJson(const HBAction& action) { return R"({"x":)" + std::to_string(action.x) + "}"; }
    static HBAction fromJson(std::string_view) { return {}; }
    static std::string resultToJson(const int& res) { return std::to_string(res); }
    static int resultFromJson(std::string_view json) { return std::stoi(std::string{json}); }
};

// ── RAII: morph::bridge::BridgeHandler destructor deregisters model ─────────────────────────
//
// After the handler goes out of scope the model is deregistered. morph::bridge::Bridge's
// _handlers list should contain only expired weak_ptrs for that slot.
// A fresh handler registered afterwards must work correctly, proving the
// slot was not corrupted.

TEST_CASE("morph::bridge::detail::HandlerBinding: RAII  -  model deregistered when morph::bridge::BridgeHandler destroyed", "[binding]") {
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExec cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};

    std::atomic<int> result{-1};
    {
        morph::bridge::BridgeHandler<HBModel> handler{bridge, &cbExec};
        handler.execute(HBAction{10}).then([&](int val) { result.store(val); }).onError([](const std::exception_ptr&) {
        });

        for (int i = 0; i < 50 && result.load() == -1; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        REQUIRE(result.load() == 11);
        // handler destroyed here  -  deregisterModel called on backend
    }

    // A new handler registered after the old one is destroyed must work,
    // demonstrating the backend slot was cleanly released.
    std::atomic<int> result2{-1};
    {
        morph::bridge::BridgeHandler<HBModel> handler2{bridge, &cbExec};
        handler2.execute(HBAction{20})
            .then([&](int val) { result2.store(val); })
            .onError([](const std::exception_ptr&) {});

        for (int i = 0; i < 50 && result2.load() == -1; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        REQUIRE(result2.load() == 21);
    }
}

// ── currentId == 0 → executeVia returns immediate error ───────────────────────
//
// morph::bridge::detail::HandlerBinding::currentId = 0 is the sentinel for "not bound to any backend
// slot". morph::bridge::Bridge::executeVia must return a failed morph::async::Completion immediately rather
// than forwarding to the backend with an invalid id.

TEST_CASE("morph::bridge::detail::HandlerBinding: executeVia with currentId=0 delivers error immediately", "[binding]") {
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExec cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};

    auto binding = std::make_shared<morph::bridge::detail::HandlerBinding>();
    binding->typeId = "HB_Model";
    binding->modelFactory = morph::model::detail::ModelFactory::create<HBModel>;
    binding->currentId.store(0);  // simulate unbound

    bool errorFired = false;
    bridge.executeVia<HBModel, HBAction>(binding, HBAction{1}, &cbExec)
        .then([](int) {})
        .onError([&](const std::exception_ptr& exc) {
            try {
                std::rethrow_exception(exc);
            } catch (const std::runtime_error& ex) {
                errorFired = (std::string{ex.what()} == "handler not bound");
            }
        });

    // SyncExec fires callbacks inline, so no polling needed.
    REQUIRE(errorFired);
}

// ── weak_ptr expiry: morph::bridge::Bridge skips dead handlers silently ──────────────────────
//
// morph::bridge::Bridge::_handlers holds weak_ptrs. When a morph::bridge::BridgeHandler is destroyed, its
// weak_ptr goes stale. A subsequent registration of a new handler must succeed,
// proving morph::bridge::Bridge does not retain the destroyed binding or crash on the stale
// weak_ptr.

TEST_CASE("morph::bridge::detail::HandlerBinding: morph::bridge::Bridge holds weak_ptr  -  destroyed handler does not block new registration", "[binding]") {
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExec cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};

    // Register and immediately destroy several handlers.
    for (int i = 0; i < 5; ++i) {
        morph::bridge::BridgeHandler<HBModel> tmp{bridge, &cbExec};
        // tmp destroyed here  -  weak_ptr in bridge goes stale
    }

    // morph::bridge::Bridge's internal list now has up to 5 stale weak_ptrs.
    // Registering a fresh handler and using it must still work.
    std::atomic<int> result{-1};
    morph::bridge::BridgeHandler<HBModel> live{bridge, &cbExec};
    live.execute(HBAction{99}).then([&](int val) { result.store(val); }).onError([](const std::exception_ptr&) {});

    for (int i = 0; i < 50 && result.load() == -1; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(result.load() == 100);
}

// ── shared_ptr ownership: binding outlives morph::bridge::BridgeHandler ─────────────────────
//
// morph::bridge::Bridge::executeVia captures a shared_ptr<morph::bridge::detail::HandlerBinding> transiently for the
// duration of each call, keeping the binding alive even while the morph::bridge::BridgeHandler
// destructor is running concurrently. This test uses the binding() accessor to
// grab a second strong reference and verifies the object is still intact after
// the handler is destroyed.

TEST_CASE("morph::bridge::detail::HandlerBinding: shared_ptr keeps binding alive past morph::bridge::BridgeHandler destructor", "[binding]") {
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExec cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};

    std::shared_ptr<morph::bridge::detail::HandlerBinding> captured;

    {
        morph::bridge::BridgeHandler<HBModel> handler{bridge, &cbExec};
        // Grab a second strong reference  -  same as executeVia does transiently.
        captured = handler.binding();
        REQUIRE(captured != nullptr);
        REQUIRE(captured->currentId.load() != 0);
        // handler destroyed here; deregisterModel is called on the backend,
        // but the morph::bridge::detail::HandlerBinding object itself survives because we hold captured.
    }

    // Object is still alive and readable  -  no use-after-free.
    // currentId retains the value it had when the model was registered;
    // deregisterHandler does not reset it to 0.
    REQUIRE(captured != nullptr);
    REQUIRE(captured->currentId.load() != 0);
}
