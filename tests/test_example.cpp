#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
#include <stdexcept>
#include <thread>

#include "test_support.hpp"
using SyncExec = morph::testing::InlineExecutor;

struct ActionInput {
    double a;
    double b;
    double c;

    [[nodiscard]] bool validate() const { return a != 0.0 && b != 0.0 && c != 0.0; }
};

struct ActionOutput {
    double result;
};

struct Model {
    ActionOutput execute(const ActionInput& input) { return ActionOutput{input.a + input.b + input.c}; }
};

BRIDGE_REGISTER_MODEL(Model, "Test_Model")
BRIDGE_REGISTER_ACTION(Model, ActionInput, "Test_ActionInput")

TEST_CASE("Example Model", "[model]") {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};

    SyncExec cbExec;
    morph::bridge::BridgeHandler<Model> handler{bridge, &cbExec};

    handler.execute(ActionInput{1.0, 2.0, 3.0})
        .then([&](ActionOutput output) { REQUIRE(output.result == 6.0); })
        .onError([](const std::exception_ptr&) { FAIL("Action execution should not have thrown an exception"); });

    // Subscribing names the *result* type, so the observer never has to know
    // which action produced it.
    std::atomic<bool> fired{false};
    handler.subscribe<ActionOutput>([&](ActionOutput output) {
        REQUIRE(output.result == 6.0);
        fired.store(true);
    });
    handler.execute(ActionInput{1.0, 2.0, 3.0});

    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    REQUIRE(fired.load() == true);
}
