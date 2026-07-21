// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <exception>
#include <glaze/glaze.hpp>
#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
#include <morph/forms/flows.hpp>
#include <stdexcept>
#include <string>
#include <thread>

#include "test_support.hpp"

// ---------------------------------------------------------------------------
// Fixture: a two-step flow. Step one registers a sample and returns its id
// (with a small delay so the backend-switch test below can reliably land
// mid-flight, mirroring test_subscription.cpp's SlowAction pattern); step
// two records a note against that id. The wizard's Bind prefills step two's
// refId from step one's returned id.
// ---------------------------------------------------------------------------

struct FlowStepOne {
    std::string label;
    [[nodiscard]] bool validate() const { return !label.empty(); }
};
struct FlowStepOneResult {
    std::int64_t id = 0;
    std::string label;
};

struct FlowStepTwo {
    std::int64_t refId = 0;
    std::string note;
    [[nodiscard]] bool validate() const { return refId != 0 && !note.empty(); }
};
struct FlowStepTwoResult {
    std::string summary;
};

struct FlowTestModel {
    std::int64_t nextId = 1;

    FlowStepOneResult execute(const FlowStepOne& action) {
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
        return FlowStepOneResult{.id = nextId++, .label = action.label};
    }
    FlowStepTwoResult execute(const FlowStepTwo& action) {
        return FlowStepTwoResult{.summary = std::to_string(action.refId) + ":" + action.note};
    }
};

BRIDGE_REGISTER_MODEL(FlowTestModel, "FlowsTest_FlowTestModel")
BRIDGE_REGISTER_ACTION(FlowTestModel, FlowStepOne, "FlowsTest_FlowStepOne")
BRIDGE_REGISTER_ACTION(FlowTestModel, FlowStepTwo, "FlowsTest_FlowStepTwo")

using DemoWizard = morph::flows::Wizard<
    "Demo flow", morph::flows::WizardStep<FlowStepOne, "Step one">,
    morph::flows::WizardStep<FlowStepTwo, "Step two", morph::flows::Bind<"refId", "FlowsTest_FlowStepOne.id">>>;

BRIDGE_REGISTER_WIZARD(DemoWizard, "FlowsTest_DemoWizard")

TEST_CASE("Flows::WizardSchemaJson emits title, steps, and prefill", "[flows]") {
    auto const schema = morph::flows::wizardSchemaJson<DemoWizard>();
    REQUIRE_FALSE(schema.empty());

    glz::generic_u64 dom{};
    REQUIRE_FALSE(glz::read_json(dom, schema));

    CHECK(schema.contains(R"("w-title":"Demo flow")"));
    CHECK(schema.contains(R"("action":"FlowsTest_FlowStepOne")"));
    CHECK(schema.contains(R"("title":"Step one")"));
    CHECK(schema.contains(R"("action":"FlowsTest_FlowStepTwo")"));
    CHECK(schema.contains(R"("prefill":{"refId":"FlowsTest_FlowStepOne.id"})"));
}

TEST_CASE("Flows::WizardSchemaJson omits prefill for steps with no Bind", "[flows]") {
    auto const schema = morph::flows::wizardSchemaJson<DemoWizard>();
    auto const stepOnePos = schema.find(R"("action":"FlowsTest_FlowStepOne")");
    auto const stepTwoPos = schema.find(R"("action":"FlowsTest_FlowStepTwo")");
    REQUIRE(stepOnePos != std::string::npos);
    REQUIRE(stepTwoPos != std::string::npos);
    auto const stepOneChunk = schema.substr(stepOnePos, stepTwoPos - stepOnePos);
    CHECK_FALSE(stepOneChunk.contains("prefill"));
}
