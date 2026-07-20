// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/forms/forms.hpp>
#include <morph/util/quantity.hpp>
#include <morph/util/rational.hpp>
#include <stdexcept>
#include <string>

#include "test_support.hpp"

using SyncExecutor = morph::testing::InlineExecutor;

// ─────────────────────────────────────────────────────────────────────────
// morph::model::ValidationError
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE("morph::model::ValidationError carries model/action type ids in its message", "[registry][validation]") {
    morph::model::ValidationError const err{"MyModel", "MyAction"};
    REQUIRE(std::string{err.what()} == "action failed validation: MyModel/MyAction");
    REQUIRE(dynamic_cast<const std::runtime_error*>(&err) != nullptr);
}

// ─────────────────────────────────────────────────────────────────────────
// ActionDispatcher::registerAction's runner: validation + precision reconciliation
// ─────────────────────────────────────────────────────────────────────────

namespace {
std::atomic<int> gGatedExecuteCount{0};
}  // namespace

struct GatedAction {
    int payload = 0;
    bool ready = false;

    [[nodiscard]] bool validate() const { return ready; }
};

struct GatedResult {
    int payload = 0;
};

struct GatedModel {
    GatedResult execute(const GatedAction& action) {
        gGatedExecuteCount.fetch_add(1);
        return GatedResult{.payload = action.payload};
    }
};

BRIDGE_REGISTER_MODEL(GatedModel, "Test_Validation_GatedModel")
BRIDGE_REGISTER_ACTION(GatedModel, GatedAction, "Test_Validation_GatedAction")

struct UngatedAction {
    int payload = 0;
};

struct UngatedModel {
    int execute(const UngatedAction& action) { return action.payload * 2; }
};

BRIDGE_REGISTER_MODEL(UngatedModel, "Test_Validation_UngatedModel")
BRIDGE_REGISTER_ACTION(UngatedModel, UngatedAction, "Test_Validation_UngatedAction")

enum class ValUnit : std::uint8_t { scalar };

template <>
struct morph::units::UnitTraits<ValUnit> {
    static constexpr morph::units::UnitMeta meta(ValUnit /*unit*/) noexcept {
        return {.id = "scalar", .display = "", .defaultDecimals = 2};
    }
};

struct PrecisionAction {
    morph::units::Quantity<ValUnit::scalar> amount;
};

struct PrecisionResult {
    std::uint32_t observedDecimalPlaces = 0;
};

struct PrecisionModel {
    PrecisionResult execute(const PrecisionAction& action) {
        return PrecisionResult{.observedDecimalPlaces = (*action.amount).getDecimalPlaces().value};
    }
};

BRIDGE_REGISTER_MODEL(PrecisionModel, "Test_Validation_PrecisionModel")
BRIDGE_REGISTER_ACTION(PrecisionModel, PrecisionAction, "Test_Validation_PrecisionAction")

TEST_CASE("ActionDispatcher::registerAction runner rejects an invalid action with ValidationError",
          "[registry][validation]") {
    gGatedExecuteCount.store(0);
    auto holder = morph::model::detail::ModelFactory::create<GatedModel>();
    REQUIRE_THROWS_AS(
        morph::model::detail::ActionDispatcher::instance().dispatch(
            "Test_Validation_GatedModel", "Test_Validation_GatedAction", *holder, R"({"payload":7,"ready":false})"),
        morph::model::ValidationError);
    REQUIRE(gGatedExecuteCount.load() == 0);
}

TEST_CASE("ActionDispatcher::registerAction runner dispatches a valid action normally", "[registry][validation]") {
    gGatedExecuteCount.store(0);
    auto holder = morph::model::detail::ModelFactory::create<GatedModel>();
    auto const resultJson = morph::model::detail::ActionDispatcher::instance().dispatch(
        "Test_Validation_GatedModel", "Test_Validation_GatedAction", *holder, R"({"payload":7,"ready":true})");
    auto const result = morph::model::ActionTraits<GatedAction>::resultFromJson(resultJson);
    REQUIRE(result.payload == 7);
    REQUIRE(gGatedExecuteCount.load() == 1);
}

TEST_CASE("ActionDispatcher::registerAction runner dispatches an action with no validator unchanged",
          "[registry][validation]") {
    auto holder = morph::model::detail::ModelFactory::create<UngatedModel>();
    auto const resultJson = morph::model::detail::ActionDispatcher::instance().dispatch(
        "Test_Validation_UngatedModel", "Test_Validation_UngatedAction", *holder, R"({"payload":9})");
    auto const result = morph::model::ActionTraits<UngatedAction>::resultFromJson(resultJson);
    REQUIRE(result == 18);
}

TEST_CASE("ActionDispatcher::registerAction runner reconciles declared Quantity precision before dispatch",
          "[registry][validation]") {
    auto holder = morph::model::detail::ModelFactory::create<PrecisionModel>();
    // Declared precision for ValUnit::scalar is 2 (UnitMeta::defaultDecimals); the
    // wire payload below carries dp=5, simulating a hand-built envelope that
    // ignores the schema's advertised x-decimalPlaces.
    auto const resultJson = morph::model::detail::ActionDispatcher::instance().dispatch(
        "Test_Validation_PrecisionModel", "Test_Validation_PrecisionAction", *holder,
        R"({"amount":{"num":314,"den":100,"dp":5}})");
    auto const result = morph::model::ActionTraits<PrecisionAction>::resultFromJson(resultJson);
    REQUIRE(result.observedDecimalPlaces == 2);
}
