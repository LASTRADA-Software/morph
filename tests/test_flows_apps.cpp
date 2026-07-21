// SPDX-License-Identifier: Apache-2.0

#include <atomic>
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

// ---------------------------------------------------------------------------
// FlowSession
// ---------------------------------------------------------------------------

namespace {
using SyncExecutor = morph::testing::InlineExecutor;
}

TEST_CASE("FlowSession: fires step one, advances, and captures step two's prefill source", "[flows]") {
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<FlowTestModel> handler{bridge, &cbExec};

    morph::flows::FlowSession<FlowTestModel, FlowStepOne, FlowStepTwo> flow{handler};

    CHECK(flow.currentIndex() == 0);
    CHECK_FALSE(flow.finished());
    CHECK(flow.currentActionType() == morph::model::ActionTraits<FlowStepOne>::typeId());

    // Not ready before any set<> — advance() is gated exactly as a standalone
    // form is gated by ActionValidator::ready.
    CHECK_FALSE(flow.advance());

    flow.set<&FlowStepOne::label>(std::string{"sample A"});
    REQUIRE(morph::testing::waitUntil([&] { return flow.ready(); }));

    REQUIRE(flow.advance());
    CHECK(flow.currentIndex() == 1);
    CHECK(flow.currentActionType() == morph::model::ActionTraits<FlowStepTwo>::typeId());

    // Step one's result is captured under "<typeId>.id" (the DemoWizard's
    // declared Bind path).
    auto const resolvedId = flow.resolved("FlowsTest_FlowStepOne.id");
    REQUIRE(resolvedId.has_value());
    CHECK(*resolvedId == "1");

    // Apply the prefill exactly as a renderer would: parse the resolved id
    // and set<> it on step two's bound field.
    std::int64_t refId{};
    REQUIRE_FALSE(glz::read_json(refId, *resolvedId));
    flow.set<&FlowStepTwo::refId>(refId);
    CHECK_FALSE(flow.ready());  // note is still empty
    flow.set<&FlowStepTwo::note>(std::string{"looks fine"});

    REQUIRE(morph::testing::waitUntil([&] { return flow.ready(); }));
    REQUIRE(flow.advance());
    CHECK(flow.finished());

    auto const summary = flow.resolved("FlowsTest_FlowStepTwo.summary");
    REQUIRE(summary.has_value());
    CHECK(*summary == R"("1:looks fine")");
}

TEST_CASE("FlowSession: back() returns to step one with its draft intact", "[flows]") {
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<FlowTestModel> handler{bridge, &cbExec};

    morph::flows::FlowSession<FlowTestModel, FlowStepOne, FlowStepTwo> flow{handler};

    flow.set<&FlowStepOne::label>(std::string{"sample B"});
    REQUIRE(morph::testing::waitUntil([&] { return flow.ready(); }));
    REQUIRE(flow.advance());

    REQUIRE(flow.back());
    CHECK(flow.currentIndex() == 0);
    CHECK(flow.ready());  // step one already produced a result once

    // Re-editing step one's draft still re-fires: the handler's own draft for
    // FlowStepOne was never reset<>()'d.
    flow.set<&FlowStepOne::label>(std::string{"sample B revised"});
    REQUIRE(morph::testing::waitUntil(
        [&] { return flow.resolved("FlowsTest_FlowStepOne.label") == R"("sample B revised")"; }));

    CHECK_FALSE(flow.back());  // already at step 0
}

TEST_CASE("FlowSession: set<> on an action that is not the current step throws", "[flows]") {
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<FlowTestModel> handler{bridge, &cbExec};

    morph::flows::FlowSession<FlowTestModel, FlowStepOne, FlowStepTwo> flow{handler};

    CHECK_THROWS_AS(flow.set<&FlowStepTwo::note>(std::string{"too early"}), std::logic_error);
}

TEST_CASE("FlowSession: backend switch mid-flight surfaces BackendChangedError on the step's errSink", "[flows]") {
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<FlowTestModel> handler{bridge, &cbExec};

    std::atomic<bool> sawBackendChanged{false};
    morph::flows::FlowSession<FlowTestModel, FlowStepOne, FlowStepTwo> flow{
        handler, [&](std::exception_ptr err) {
            try {
                std::rethrow_exception(err);
            } catch (const morph::backend::BackendChangedError&) {
                sawBackendChanged.store(true);
            } catch (...) {
            }
        }};

    flow.set<&FlowStepOne::label>(std::string{"racing"});  // starts a 50ms fire (see FlowTestModel)
    bridge.switchBackend(std::make_unique<morph::backend::LocalBackend>(pool));

    REQUIRE(morph::testing::waitUntil([&] { return sawBackendChanged.load(); }));

    // The draft survives the switch: setting the same field again re-fires
    // cleanly against the new backend.
    flow.set<&FlowStepOne::label>(std::string{"racing again"});
    REQUIRE(morph::testing::waitUntil([&] { return flow.ready(); }));
}
