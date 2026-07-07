// SPDX-License-Identifier: Apache-2.0

#include <morph/bridge.hpp>
#include <morph/executor.hpp>
#include <morph/registry.hpp>
#include <morph/remote.hpp>

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <optional>
#include <string>

#include "test_support.hpp"

// Test-local model — only used in this translation unit
struct AddNumbers {
    int a = 0;
    int b = 0;
};

struct AddResult {
    int sum = 0;
};

struct MathModel {
    AddResult execute(const AddNumbers& action) { return AddResult{.sum = action.a + action.b}; }
};

BRIDGE_REGISTER_MODEL(MathModel, "Test_ExecJson_MathModel")
BRIDGE_REGISTER_ACTION(MathModel, AddNumbers, "Test_ExecJson_AddNumbers")

using SyncExecutor = morph::testing::InlineExecutor;

TEST_CASE("ActionExecuteRegistry: executeJson deserialises, executes, and re-serialises", "[bridge][execute-json]") {
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<MathModel> handler{bridge, &cbExec};

    std::optional<std::string> resultJson;
    std::atomic<bool> done{false};
    handler.executeJson("Test_ExecJson_AddNumbers", R"({"a":3,"b":4})")
        .then([&](std::string json) {
            resultJson = std::move(json);
            done.store(true);
        })
        .onError([&](const std::exception_ptr&) { done.store(true); });

    REQUIRE(morph::testing::waitUntil([&] { return done.load(); }));
    REQUIRE(resultJson.has_value());
    REQUIRE(*resultJson == R"({"sum":7})");
}

TEST_CASE("ActionExecuteRegistry: executeJson reports parse errors via onError", "[bridge][execute-json]") {
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<MathModel> handler{bridge, &cbExec};

    bool sawError = false;
    std::atomic<bool> done{false};
    handler.executeJson("Test_ExecJson_AddNumbers", R"({"a":"not a number"})")
        .then([&](std::string) { done.store(true); })
        .onError([&](const std::exception_ptr&) {
            sawError = true;
            done.store(true);
        });

    REQUIRE(morph::testing::waitUntil([&] { return done.load(); }));
    REQUIRE(sawError);
}

TEST_CASE("ActionExecuteRegistry: unknown action type throws", "[bridge][execute-json]") {
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<MathModel> handler{bridge, &cbExec};

    REQUIRE_THROWS_AS(handler.executeJson("NoSuchAction", "{}"), std::runtime_error);
}
