// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
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

// ── Coverage: registerAction guarded forwarding (bridge.hpp L845-847, L849) ──
// These target the two uncovered regions in ActionExecuteRegistry::registerAction's
// executor lambda:
//   - L845-847  the `catch (...)` inside the `.then` forwarding lambda: when
//               `ActionTraits<Action>::resultToJson(result)` throws, the exception
//               must land on the error sink (setException), not escape the callback
//               executor (where it would hang the completion or std::terminate).
//   - L849      the `.onError` forwarding lambda: when the handler's execution
//               itself fails, the exception_ptr must be forwarded to resultState.

// Action whose execute succeeds but whose resultToJson throws at run time —
// exercises the catch-all inside the .then forwarding lambda (L845-847). Uses a
// manual ActionTraits (registered without BRIDGE_REGISTER_ACTION, which would
// generate a conflicting traits specialization). File scope (not an anonymous
// namespace) so the registration template machinery resolves.
struct ThrowOnSerialise {
    int v = 0;
};

struct ThrowOnSerialiseModel {
    int execute(const ThrowOnSerialise& act) { return act.v; }
};

// Action whose execute throws — exercises the .onError forwarding lambda (L849).
struct ThrowOnExecute {
    int v = 0;
};

struct ThrowOnExecuteModel {
    int execute(const ThrowOnExecute&) { throw std::runtime_error{"execute failed"}; }
};

template <>
struct morph::model::ModelTraits<ThrowOnSerialiseModel> {
    static constexpr std::string_view typeId() { return "Test_ExecJson_ThrowSerialiseModel"; }
};
template <>
struct morph::model::ActionTraits<ThrowOnSerialise> {
    using Result = int;
    static constexpr std::string_view typeId() { return "Test_ExecJson_ThrowOnSerialise"; }
    static std::string toJson(const ThrowOnSerialise& act) { return R"({"v":)" + std::to_string(act.v) + "}"; }
    static ThrowOnSerialise fromJson(std::string_view) { return {}; }
    // Deliberately throws at run time: the dispatch path must route this to the
    // error sink (setException), not let it escape the callback executor.
    static std::string resultToJson(const int&) { throw std::runtime_error{"serialise failed"}; }
    static int resultFromJson(std::string_view) { return 0; }
};

template <>
struct morph::model::ModelTraits<ThrowOnExecuteModel> {
    static constexpr std::string_view typeId() { return "Test_ExecJson_ThrowExecuteModel"; }
};
template <>
struct morph::model::ActionTraits<ThrowOnExecute> {
    using Result = int;
    static constexpr std::string_view typeId() { return "Test_ExecJson_ThrowOnExecute"; }
    static std::string toJson(const ThrowOnExecute& act) { return R"({"v":)" + std::to_string(act.v) + "}"; }
    static ThrowOnExecute fromJson(std::string_view) { return {}; }
    static std::string resultToJson(const int& res) { return std::to_string(res); }
    static int resultFromJson(std::string_view) { return 0; }
};

namespace {
// Register the custom-traits models/actions without BRIDGE_REGISTER_MODEL /
// BRIDGE_REGISTER_ACTION (those macros emit their own Traits specialisations and
// would clash with the manual ones above).
const bool kRegThrowOnSerialiseModel =
    morph::model::detail::registerModelOnce<ThrowOnSerialiseModel>("Test_ExecJson_ThrowSerialiseModel");
const bool kRegThrowOnSerialise =
    morph::model::detail::registerActionExecutorOnce<ThrowOnSerialiseModel, ThrowOnSerialise>(
        "Test_ExecJson_ThrowSerialiseModel", "Test_ExecJson_ThrowOnSerialise");
const bool kRegThrowOnExecuteModel =
    morph::model::detail::registerModelOnce<ThrowOnExecuteModel>("Test_ExecJson_ThrowExecuteModel");
const bool kRegThrowOnExecute = morph::model::detail::registerActionExecutorOnce<ThrowOnExecuteModel, ThrowOnExecute>(
    "Test_ExecJson_ThrowExecuteModel", "Test_ExecJson_ThrowOnExecute");
}  // namespace

TEST_CASE("ActionExecuteRegistry: executeJson routes a throwing resultToJson to onError (not a hang)",
          "[bridge][execute-json][coverage]") {
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<ThrowOnSerialiseModel> handler{bridge, &cbExec};

    std::atomic<bool> sawError{false};
    std::atomic<bool> sawResult{false};
    std::atomic<bool> done{false};
    // execute succeeds (returns 5) but resultToJson throws — the guarded .then
    // must catch and forward via setException, resolving through onError.
    handler.executeJson("Test_ExecJson_ThrowOnSerialise", R"({"v":5})")
        .then([&](std::string) {
            sawResult.store(true);
            done.store(true);
        })
        .onError([&](const std::exception_ptr&) {
            sawError.store(true);
            done.store(true);
        });

    REQUIRE(morph::testing::waitUntil([&] { return done.load(); }));
    REQUIRE(sawError.load());
    REQUIRE_FALSE(sawResult.load());
}

TEST_CASE("ActionExecuteRegistry: executeJson forwards a handler execution failure via onError",
          "[bridge][execute-json][coverage]") {
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<ThrowOnExecuteModel> handler{bridge, &cbExec};

    std::atomic<bool> sawError{false};
    std::atomic<bool> sawResult{false};
    std::atomic<bool> done{false};
    // execute() throws — the inner completion fails and the .onError forwarding
    // lambda (L849) must route the exception_ptr to resultState.
    handler.executeJson("Test_ExecJson_ThrowOnExecute", R"({"v":1})")
        .then([&](std::string) {
            sawResult.store(true);
            done.store(true);
        })
        .onError([&](const std::exception_ptr&) {
            sawError.store(true);
            done.store(true);
        });

    REQUIRE(morph::testing::waitUntil([&] { return done.load(); }));
    REQUIRE(sawError.load());
    REQUIRE_FALSE(sawResult.load());
}
