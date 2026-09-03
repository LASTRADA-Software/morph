// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
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
#include <morph/core/logger.hpp>
#include <morph/core/registry.hpp>
#include <morph/forms/app.hpp>
#include <morph/forms/flows.hpp>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

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

// A step whose execute() always throws, so a FlowSession's onError path
// (including the no-onError-given default logging path) can be exercised
// without needing a backend switch mid-flight, mirroring
// test_subscription.cpp's SubExplode pattern.
struct FlowStepExplodes {
    std::string label;
    [[nodiscard]] bool validate() const { return !label.empty(); }
};
struct FlowStepExplodesResult {
    std::int64_t id = 0;
};

// A step whose execute() throws a type that does not derive from
// std::exception, so logUnhandledError's `catch (...)` arm (as opposed to
// its `catch (const std::exception&)` sibling, already exercised by
// FlowStepExplodes above) gets driven -- mirrors test_executor_extra.cpp's
// and test_timeout_scheduler.cpp's `throw 42;` non-std::exception pattern.
struct FlowStepExplodesNonStd {
    std::string label;
    [[nodiscard]] bool validate() const { return !label.empty(); }
};
struct FlowStepExplodesNonStdResult {
    std::int64_t id = 0;
};

struct FlowTestModel {
    std::int64_t nextId = 1;

    // How many times FlowStepTwo has actually been dispatched, across every
    // instance in this translation unit. FlowSession publishes only the *last*
    // result of a step, so the number of dispatches behind it is not otherwise
    // observable -- and it is exactly what the no-coalescing contract is about.
    static inline std::atomic<int> stepTwoExecutions{0};

    FlowStepOneResult execute(const FlowStepOne& action) {
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
        // A sentinel label to let a test deterministically fail one specific
        // fire of FlowStepOne (e.g. a stale, already-superseded one) without
        // needing a real backend-switch race.
        if (action.label == "explode") {
            throw std::runtime_error{"boom"};
        }
        return FlowStepOneResult{.id = nextId++, .label = action.label};
    }
    FlowStepTwoResult execute(const FlowStepTwo& action) {
        stepTwoExecutions.fetch_add(1, std::memory_order_relaxed);
        return FlowStepTwoResult{.summary = std::to_string(action.refId) + ":" + action.note};
    }
    static FlowStepExplodesResult execute(const FlowStepExplodes& /*action*/) { throw std::runtime_error{"boom"}; }
    static FlowStepExplodesNonStdResult execute(const FlowStepExplodesNonStd& /*action*/) {
        throw 42;  // NOLINT(hicpp-exception-baseclass) — exercises logUnhandledError's catch(...) arm
    }
};

BRIDGE_REGISTER_MODEL(FlowTestModel, "FlowsTest_FlowTestModel")
BRIDGE_REGISTER_ACTION(FlowTestModel, FlowStepOne, "FlowsTest_FlowStepOne")
BRIDGE_REGISTER_ACTION(FlowTestModel, FlowStepTwo, "FlowsTest_FlowStepTwo")
BRIDGE_REGISTER_ACTION(FlowTestModel, FlowStepExplodes, "FlowsTest_FlowStepExplodes")
BRIDGE_REGISTER_ACTION(FlowTestModel, FlowStepExplodesNonStd, "FlowsTest_FlowStepExplodesNonStd")

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

TEST_CASE("FlowSession: every set<> that leaves the draft ready dispatches again", "[flows]") {
    // `set<>`'s own @brief states this as a property callers have to plan
    // around: there is no in-flight coalescing, so keystroke-rate edits on an
    // already-complete draft produce one request each and the last reply to
    // land wins rather than the last call made. An earlier handler-side draft
    // did collapse those, and nothing here would have failed if that collapsing
    // came back -- every other flow test in this file drains until `ready()`,
    // which is true after one dispatch or after five.
    morph::exec::ThreadPoolExecutor pool{2};
    // Hand-stepped rather than the inline executor the cases above use. A
    // step's continuation runs on whatever executor resolves the completion,
    // and this case deliberately has five dispatches outstanding at once; with
    // an inline executor those five `captureResult` calls run on pool threads
    // and race the end of this scope, because `~FlowSession` only *requests* a
    // stop and `CallbackScope` documents that request as advisory across
    // threads -- a callback that has already passed its token check goes on to
    // touch the session. Stepping the continuations keeps every one of them on
    // this thread, so the flow is destroyed with nothing left to deliver.
    morph::testing::StepExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<FlowTestModel> handler{bridge, &cbExec};

    morph::flows::FlowSession<FlowTestModel, FlowStepOne, FlowStepTwo> flow{handler};

    // Step two is the one counted: its execute() does no sleeping, so five
    // dispatches of it cost nothing.
    flow.set<&FlowStepOne::label>(std::string{"counting"});
    while (!flow.ready()) {
        REQUIRE(morph::testing::waitUntil([&] { return cbExec.pending() > 0; }));
        cbExec.runAll();
    }
    REQUIRE(flow.advance());

    auto const before = FlowTestModel::stepTwoExecutions.load();

    // `refId` alone leaves the draft incomplete, so this dispatches nothing --
    // the readiness gate, not the edit, is what fires a step.
    flow.set<&FlowStepTwo::refId>(std::int64_t{7});
    CHECK(FlowTestModel::stepTwoExecutions.load() == before);

    // Five edits that each leave the draft ready: five dispatches, one per
    // edit, not one dispatch carrying the final draft. The counter is bumped on
    // entry to `execute`, so this says five requests were *made* -- which is
    // the claim -- and says nothing yet about their replies.
    for (const auto* note : {"a", "b", "c", "d", "e"}) {
        flow.set<&FlowStepTwo::note>(std::string{note});
    }
    REQUIRE(morph::testing::waitUntil([&] { return FlowTestModel::stepTwoExecutions.load() >= before + 5; }));
    CHECK(FlowTestModel::stepTwoExecutions.load() == before + 5);

    // Now deliver all five replies, and only then leave the scope. The loop
    // ends once a full budget passes with the queue empty, which is what makes
    // "nothing is still in flight" an observation rather than an assumption.
    while (morph::testing::waitUntil([&] { return cbExec.pending() > 0; }, std::chrono::milliseconds{250})) {
        cbExec.runAll();
    }
    CHECK(flow.ready());  // every one of the five replies landed
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

TEST_CASE("FlowSession: backend switch mid-flight surfaces BackendChangedError on the flow's onError", "[flows]") {
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

TEST_CASE("FlowSession: resolved() returns nullopt for a path never captured", "[flows]") {
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<FlowTestModel> handler{bridge, &cbExec};

    morph::flows::FlowSession<FlowTestModel, FlowStepOne, FlowStepTwo> flow{handler};

    // Nothing has fired yet -- the path is well-formed but was never recorded.
    CHECK_FALSE(flow.resolved("FlowsTest_FlowStepOne.id").has_value());
    // A path naming a field that is never produced by either step.
    CHECK_FALSE(flow.resolved("FlowsTest_FlowStepOne.nonexistentField").has_value());
}

TEST_CASE("FlowSession: advance() on an already-finished flow returns false", "[flows]") {
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<FlowTestModel> handler{bridge, &cbExec};

    morph::flows::FlowSession<FlowTestModel, FlowStepOne, FlowStepTwo> flow{handler};

    flow.set<&FlowStepOne::label>(std::string{"sample C"});
    REQUIRE(morph::testing::waitUntil([&] { return flow.ready(); }));
    REQUIRE(flow.advance());

    flow.set<&FlowStepTwo::refId>(std::int64_t{7});
    flow.set<&FlowStepTwo::note>(std::string{"final step"});
    REQUIRE(morph::testing::waitUntil([&] { return flow.ready(); }));
    REQUIRE(flow.advance());
    REQUIRE(flow.finished());

    // The flow has already advanced past its last step: advance() must
    // report false rather than incrementing _index further (finished()'s
    // arm of the `!ready || finished()` guard, not just the not-ready arm
    // already covered above).
    CHECK_FALSE(flow.advance());
    CHECK(flow.finished());
    CHECK(flow.currentIndex() == flow.stepCount());
}

TEST_CASE("FlowSession: no onError callback logs the failure instead", "[flows]") {
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<FlowTestModel> handler{bridge, &cbExec};

    // No onError given -- FlowSession's default routes the failure through
    // morph::log::logError (logUnhandledError) instead of a callback.
    morph::flows::FlowSession<FlowTestModel, FlowStepExplodes> flow{handler};

    // logUnhandledError's call site runs wherever the dispatched action's
    // exception is actually caught -- a `pool` worker thread, not this test's
    // own thread -- so `logged` is written cross-thread while this test's
    // waitUntil polls it; a plain std::vector needs a mutex for that, same
    // idiom as test_concurrency_invariants.cpp's own logger-override tests.
    std::mutex loggedMtx;
    std::vector<std::string> logged;
    morph::log::ScopedLoggerOverride guard{
        [&](morph::log::LogLevel, std::string_view msg) {
            std::scoped_lock const lock{loggedMtx};
            logged.emplace_back(msg);
        },
        morph::log::LogLevel::error,
    };

    flow.set<&FlowStepExplodes::label>(std::string{"trigger"});

    REQUIRE(morph::testing::waitUntil([&] {
        std::scoped_lock const lock{loggedMtx};
        return std::ranges::any_of(
            logged, [](const std::string& line) { return line.contains("FlowsTest_FlowStepExplodes"); });
    }));
    CHECK_FALSE(flow.ready());
}

TEST_CASE("FlowSession: no onError callback logs a non-std::exception failure as unknown", "[flows]") {
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<FlowTestModel> handler{bridge, &cbExec};

    // No onError given, and the step throws a type that is not a
    // std::exception -- drives logUnhandledError's `catch (...)` arm, the
    // sibling of the `catch (const std::exception&)` arm the test above
    // already exercises via std::runtime_error.
    morph::flows::FlowSession<FlowTestModel, FlowStepExplodesNonStd> flow{handler};

    std::mutex loggedMtx;
    std::vector<std::string> logged;
    morph::log::ScopedLoggerOverride guard{
        [&](morph::log::LogLevel, std::string_view msg) {
            std::scoped_lock const lock{loggedMtx};
            logged.emplace_back(msg);
        },
        morph::log::LogLevel::error,
    };

    flow.set<&FlowStepExplodesNonStd::label>(std::string{"trigger"});

    REQUIRE(morph::testing::waitUntil([&] {
        std::scoped_lock const lock{loggedMtx};
        return std::ranges::any_of(logged, [](const std::string& line) {
            return line.contains("FlowsTest_FlowStepExplodesNonStd") && line.contains("unknown exception");
        });
    }));
    CHECK_FALSE(flow.ready());
}

TEST_CASE("FlowSession: a late reply for a step already left behind is dropped", "[flows]") {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::testing::DeterministicExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<FlowTestModel> handler{bridge, &cbExec};

    morph::flows::FlowSession<FlowTestModel, FlowStepOne, FlowStepTwo> flow{handler};

    // First fire of step one.
    flow.set<&FlowStepOne::label>(std::string{"first"});
    // Edit the same field again while step one is still current: FlowSession
    // has no in-flight coalescing (see captureResult's doc comment), so this
    // is a second, independent fire for the same step.
    flow.set<&FlowStepOne::label>(std::string{"second"});

    // Each fire's result reaches FlowSession's captureResult through two
    // hops on cbExec (Bridge::executeVia's own internal .then, which then
    // posts the typed Completion's callback onward -- see completion.hpp's
    // CompletionState::setValue), so draining exactly one task at a time and
    // waiting for the queue to be repopulated between steps is what lets
    // this test stop the instant the *first* fire's real captureResult runs,
    // instead of draining straight through both fires' replies.
    while (!flow.ready()) {
        REQUIRE(morph::testing::waitUntil([&] { return cbExec.pending() > 0; }));
        cbExec.step();
    }

    // The first fire to actually reach captureResult marks the flow ready;
    // advance while the second fire's reply is still queued behind it.
    REQUIRE(flow.advance());
    CHECK(flow.currentIndex() == 1);

    auto const capturedBeforeStaleReply = flow.resolved("FlowsTest_FlowStepOne.label");
    REQUIRE(capturedBeforeStaleReply.has_value());
    // advance() clears readiness for the new current step (step two); step
    // two has had no set<> call at all, so this is false until the stale
    // reply is (wrongly, if the guard is broken) applied to it below.
    CHECK_FALSE(flow.ready());

    // Drain the remaining queued task(s): the second fire's reply, whose
    // stepIndex (captured as step 0 at dispatch time) no longer matches
    // _activeStep (now 1). captureResult must return early for it -- not
    // overwrite _resolvedValues with a superseded reply, and not mark step
    // two ready when nothing was ever entered into it.
    REQUIRE(morph::testing::waitUntil([&] { return cbExec.pending() > 0; }));
    while (cbExec.pending() > 0) {
        cbExec.step();
    }

    CHECK(flow.currentIndex() == 1);  // unaffected by the stale reply
    CHECK_FALSE(flow.ready());        // step two still not (falsely) marked ready
    CHECK(flow.resolved("FlowsTest_FlowStepOne.label") == capturedBeforeStaleReply);  // not overwritten
}

TEST_CASE("FlowSession: a late error for a step already left behind does not un-ready the new step", "[flows]") {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::testing::DeterministicExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<FlowTestModel> handler{bridge, &cbExec};

    std::vector<std::string> errors;
    morph::flows::FlowSession<FlowTestModel, FlowStepOne, FlowStepTwo> flow{handler, [&](std::exception_ptr err) {
                                                                                try {
                                                                                    std::rethrow_exception(err);
                                                                                } catch (const std::exception& exc) {
                                                                                    errors.emplace_back(exc.what());
                                                                                } catch (...) {
                                                                                    errors.emplace_back("unknown");
                                                                                }
                                                                            }};

    // A fire of step one that will fail (see FlowTestModel::execute's
    // "explode" sentinel), followed by a second fire of the same
    // still-current step that will succeed. Both are independent,
    // in-flight dispatches (BridgeHandler::execute has no coalescing --
    // only BridgeHandler::subscribe does, per bridge.md).
    flow.set<&FlowStepOne::label>(std::string{"explode"});
    flow.set<&FlowStepOne::label>(std::string{"succeeds"});

    // Drain tasks one at a time (see the previous test's comment on the
    // two-hop cbExec delivery) until the flow becomes ready -- that is the
    // succeeding fire's captureResult, whichever order the two fires'
    // replies actually arrive in.
    while (!flow.ready()) {
        REQUIRE(morph::testing::waitUntil([&] { return cbExec.pending() > 0; }));
        cbExec.step();
    }
    REQUIRE(flow.advance());
    CHECK(flow.currentIndex() == 1);

    // Make step two ready too, so its readiness is something the stale
    // error below could (if the guard were broken) wrongly clear. Drive
    // cbExec by hand (nothing runs it on its own) until that fire's own
    // captureResult has actually landed.
    flow.set<&FlowStepTwo::refId>(std::int64_t{42});
    flow.set<&FlowStepTwo::note>(std::string{"note"});
    while (!flow.ready()) {
        REQUIRE(morph::testing::waitUntil([&] { return cbExec.pending() > 0; }));
        cbExec.step();
    }

    // Drain whatever is left: the failing fire's stale error, dispatched
    // back when step one (index 0) was still current. Its stepIndex no
    // longer matches _activeStep (now 1, step two's own in-flight fire), so
    // the guard at captureResult's error-path sibling must not clear
    // _currentReady for step two -- even though the error is still reported
    // through onError either way (see fireStep's doc comment).
    while (!std::ranges::any_of(errors, [](const std::string& msg) { return msg == "boom"; })) {
        REQUIRE(morph::testing::waitUntil([&] { return cbExec.pending() > 0; }));
        cbExec.step();
    }

    CHECK(flow.currentIndex() == 1);
    CHECK(flow.ready());  // step two's own readiness must survive the stale error
}

TEST_CASE("FlowSession: a completion arriving after the flow is destroyed is a no-op", "[flows]") {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::testing::DeterministicExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<FlowTestModel> handler{bridge, &cbExec};

    auto flow = std::make_unique<morph::flows::FlowSession<FlowTestModel, FlowStepOne, FlowStepTwo>>(handler);

    flow->set<&FlowStepOne::label>(std::string{"about to vanish"});
    REQUIRE(morph::testing::waitUntil([&] { return cbExec.pending() > 0; }));

    // Destroy the flow before its in-flight step's completion is delivered.
    // ~FlowSession flips _alive to false; FlowSession::fireStep's own
    // .then() continuation must check it and return without touching the
    // destroyed FlowSession.
    flow.reset();

    // The result reaches FlowSession::fireStep's .then() through two hops
    // on cbExec (Bridge::executeVia's own internal .then, which then posts
    // the typed Completion's callback onward -- see completion.hpp's
    // CompletionState::setValue) -- draining every task this produces (not
    // just the first) is what actually reaches FlowSession's own closure
    // with _alive now false. Running all of it must not crash or touch
    // freed memory -- that is the entire assertion here (see
    // FlowSession::fireStep's doc comment on why both closures capture
    // _alive by value).
    while (cbExec.pending() > 0) {
        CHECK_NOTHROW(cbExec.step());
    }
}

TEST_CASE("FlowSession: an error arriving after the flow is destroyed is a no-op", "[flows]") {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::testing::DeterministicExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<FlowTestModel> handler{bridge, &cbExec};

    std::atomic<bool> onErrorCalled{false};
    auto flow = std::make_unique<morph::flows::FlowSession<FlowTestModel, FlowStepExplodes>>(
        handler, [&](std::exception_ptr) { onErrorCalled.store(true); });

    flow->set<&FlowStepExplodes::label>(std::string{"about to vanish"});
    REQUIRE(morph::testing::waitUntil([&] { return cbExec.pending() > 0; }));

    flow.reset();

    // See the matching comment in the .then() variant above for why every
    // queued task (not just the first) must be drained to actually reach
    // FlowSession::fireStep's own .onError() closure. The onError closure's
    // _alive check must fire first and return before touching `this` (which
    // would call the now-dangling _onError member).
    while (cbExec.pending() > 0) {
        CHECK_NOTHROW(cbExec.step());
    }
    CHECK_FALSE(onErrorCalled.load());
}

// ---------------------------------------------------------------------------
// App shell
// ---------------------------------------------------------------------------

using DemoApp = morph::app::App<"Demo app", std::tuple<morph::app::MenuEntry<"Flow", "flow">>,
                                std::tuple<morph::app::WizardScreen<"flow", DemoWizard>>>;

BRIDGE_REGISTER_APP(DemoApp, "FlowsTest_DemoApp")

TEST_CASE("App::AppSchemaJson emits title, menu, and screens", "[app]") {
    auto const schema = morph::app::appSchemaJson<DemoApp>();
    REQUIRE_FALSE(schema.empty());

    glz::generic_u64 dom{};
    REQUIRE_FALSE(glz::read_json(dom, schema));

    CHECK(schema.contains(R"("app-title":"Demo app")"));
    CHECK(schema.contains(R"("label":"Flow")"));
    CHECK(schema.contains(R"("screen":"flow")"));
    CHECK(schema.contains(R"("kind":"wizard")"));
    CHECK(schema.contains(R"("ref":"FlowsTest_DemoWizard")"));
}

TEST_CASE("App::FormScreen::ref resolves to the registered action's type-id", "[app]") {
    using DensityScreen = morph::app::FormScreen<"density", FlowStepOne>;
    CHECK(DensityScreen::kind() == "form");
    CHECK(DensityScreen::ref() == "FlowsTest_FlowStepOne");
}
