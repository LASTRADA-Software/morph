// SPDX-License-Identifier: Apache-2.0

// Regression tests for two audited defects:
//
//   Part A  onBackendChanged threading — notifyBackendChanged must dispatch each
//           model's onBackendChanged() onto that model's strand (serialised
//           against execute, off Bridge::_mtx), so:
//             (a) a model that reacts to a drained item and mutates state is not
//                 racing switchBackend/execute (strand serialisation holds), and
//             (b) a model whose onBackendChanged re-enters the bridge
//                 (switchBackend/registerHandler/deregisterHandler) does NOT
//                 self-deadlock — it completes.
//
//   Part B  executeJson dispatch must enforce the action's validator before
//           invoking the handler (a failing action is rejected with an error,
//           not silently executed), and must retag incoming Quantity fields to
//           their declared precision so the stored value matches the schema's
//           advertised x-decimalPlaces.

#include <morph/backend.hpp>
#include <morph/bridge.hpp>
#include <morph/executor.hpp>
#include <morph/forms.hpp>
#include <morph/quantity.hpp>
#include <morph/rational.hpp>
#include <morph/registry.hpp>

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "test_support.hpp"

using morph::math::DecimalPlaces;
using morph::math::Denominator;
using morph::math::Numerator;
using morph::math::Rational;
using SyncExec = morph::testing::InlineExecutor;

// ── Part A: strand-serialised onBackendChanged ─────────────────────────────────

namespace bfa {
struct BFQueryAction {};  // read-only probe of the model's own counters
class ReactModel;
class ReentrantModel;
}  // namespace bfa

// Traits must be visible where ReentrantModel::onBackendChanged uses
// ModelTraits<ReactModel>, so they precede the class definitions.
template <>
struct morph::model::ModelTraits<bfa::ReactModel> {
    static constexpr std::string_view typeId() { return "BF_ReactModel"; }
};
template <>
struct morph::model::ModelTraits<bfa::ReentrantModel> {
    static constexpr std::string_view typeId() { return "BF_ReentrantModel"; }
};
template <>
struct morph::model::ActionTraits<bfa::BFQueryAction> {
    using Result = int;
    static constexpr std::string_view typeId() { return "BF_QueryAction"; }
    static std::string toJson(const bfa::BFQueryAction&) { return "{}"; }
    static bfa::BFQueryAction fromJson(std::string_view) { return {}; }
    static std::string resultToJson(const int& res) { return std::to_string(res); }
    static int resultFromJson(std::string_view str) { return std::stoi(std::string{str}); }
};

namespace bfa {

// A model whose onBackendChanged mutates plain (unlocked) state. If the
// framework ran onBackendChanged on the model's strand, it can never overlap an
// execute() on the same model, so the plain int is safe.
class ReactModel {
public:
    [[nodiscard]] int execute(const BFQueryAction&) const { return notifyCount; }

    void onBackendChanged() { ++notifyCount; }

    int notifyCount = 0;
};

// A model whose onBackendChanged RE-ENTERS the bridge by calling
// registerHandler + deregisterHandler on the SAME bridge — the conflict-
// resolution reentrancy the audit flagged (#14). Under the old design,
// notifyBackendChanged ran inline while Bridge::_mtx was held, so any of these
// re-entrant calls (which also take _mtx) self-deadlocked. With strand dispatch
// the callback runs on a pool thread with _mtx free, so the re-entrant calls
// acquire the lock normally and complete.
//
// (Re-entering switchBackend specifically is NOT exercised: switchBackend from
// inside onBackendChanged would destroy the very strand the callback is running
// on — an inherent precondition documented in bridge.md, independent of the
// _mtx fix.)
class ReentrantModel {
public:
    [[nodiscard]] int execute(const BFQueryAction&) const { return completed.load(); }

    void onBackendChanged() {
        if (bridge == nullptr) {
            return;
        }
        // Re-enter the bridge from inside onBackendChanged. Both calls take
        // Bridge::_mtx; under the old inline-notify design this hung.
        auto sub = std::make_shared<morph::bridge::detail::HandlerBinding>();
        sub->typeId = std::string{morph::model::ModelTraits<ReactModel>::typeId()};
        sub->modelFactory = [] -> std::unique_ptr<morph::model::detail::IModelHolder> {
            return std::make_unique<morph::model::detail::ModelHolder<ReactModel>>();
        };
        bridge->registerHandler(sub);
        bridge->deregisterHandler(sub);
        completed.store(1);
    }

    morph::bridge::Bridge* bridge = nullptr;
    std::atomic<int> completed{0};
};

}  // namespace bfa

using bfa::BFQueryAction;
using bfa::ReactModel;
using bfa::ReentrantModel;

namespace {

int waitInt(auto completion) {
    std::atomic<int> result{-999};
    std::move(completion).then([&](int val) { result.store(val); }).onError([](const std::exception_ptr&) {});
    morph::testing::waitUntil([&] { return result.load() != -999; });
    return result.load();
}

}  // namespace

TEST_CASE("Bridge::switchBackend fires onBackendChanged on the model strand - serialised, no race",
          "[bridge][backend-changed]") {
    morph::exec::ThreadPoolExecutor pool1{2};
    morph::exec::ThreadPoolExecutor pool2{2};
    SyncExec cbExec;

    auto binding = std::make_shared<morph::bridge::detail::HandlerBinding>();
    binding->typeId = std::string{morph::model::ModelTraits<ReactModel>::typeId()};
    binding->modelFactory = [] -> std::unique_ptr<morph::model::detail::IModelHolder> {
        return std::make_unique<morph::model::detail::ModelHolder<ReactModel>>();
    };

    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool1)};
    morph::bridge::BridgeHandler<ReactModel> handler{bridge, &cbExec, binding};

    bridge.switchBackend(std::make_unique<morph::backend::LocalBackend>(pool2));

    // onBackendChanged runs on the strand asynchronously; the query races behind
    // it on the SAME strand, so once the query returns 1 we know the mutation
    // was serialised ahead of it (no torn state, no lock).
    REQUIRE(waitInt(handler.execute(BFQueryAction{})) == 1);
}

TEST_CASE("onBackendChanged that re-enters registerHandler/deregisterHandler does not deadlock",
          "[bridge][backend-changed][reentrant]") {
    morph::exec::ThreadPoolExecutor pool1{2};
    morph::exec::ThreadPoolExecutor pool2{2};
    SyncExec cbExec;

    auto binding = std::make_shared<morph::bridge::detail::HandlerBinding>();
    binding->typeId = std::string{morph::model::ModelTraits<ReentrantModel>::typeId()};
    morph::bridge::Bridge* bridgePtr = nullptr;
    // The factory wires each fresh model instance to the bridge so its
    // onBackendChanged can re-enter register/deregisterHandler.
    binding->modelFactory = [&bridgePtr] -> std::unique_ptr<morph::model::detail::IModelHolder> {
        auto holder = std::make_unique<morph::model::detail::ModelHolder<ReentrantModel>>();
        holder->model.bridge = bridgePtr;
        return holder;
    };

    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool1)};
    bridgePtr = &bridge;
    morph::bridge::BridgeHandler<ReentrantModel> handler{bridge, &cbExec, binding};

    // This switch fires onBackendChanged on the new model, which itself calls
    // registerHandler + deregisterHandler. If notifyBackendChanged ran inline
    // under _mtx (the old design) this would self-deadlock; with strand dispatch
    // it completes.
    bridge.switchBackend(std::make_unique<morph::backend::LocalBackend>(pool2));

    // completed == 1 proves the re-entrant calls ran to completion. If they
    // deadlocked, this waits out its budget and returns 0.
    REQUIRE(waitInt(handler.execute(BFQueryAction{})) == 1);
}

// ── Part B: executeJson enforces validation + precision reconciliation ──────────

enum class BFUnit : std::uint8_t { kg };

template <>
struct morph::units::UnitTraits<BFUnit> {
    static constexpr morph::units::UnitMeta meta(BFUnit /*unit*/) noexcept {
        return {.id = "kg", .display = "kg", .defaultDecimals = 3};
    }
    static constexpr std::array<morph::units::UnitRelation<BFUnit>, 0> relations{};
};

// Named namespace (not anonymous): glaze reflection requires the reflected
// type to have linkage.
namespace bf {

// Declared precision 3 (unit default). A client can submit at any dp; the
// dispatch path must retag to 3.
using BFMass = morph::units::Quantity<BFUnit::kg, 3>;

// Validated action: requires its Quantity engaged. `mass` empty → invalid.
struct SubmitMass {
    BFMass mass{};
    [[nodiscard]] bool validate() const { return morph::forms::allRequiredEngaged(*this); }
};

struct MassAck {
    // Echoes back the runtime dp the model actually received, so the test can
    // assert precision reconciliation happened before dispatch.
    std::int64_t receivedDp = 0;
    bool engaged = false;
};

class MassModel {
public:
    MassAck execute(const SubmitMass& action) {
        MassAck ack;
        ack.engaged = action.mass.hasValue();
        if (action.mass.hasValue()) {
            ack.receivedDp = static_cast<std::int64_t>(action.mass.value()->decimalPlaces.value);
        }
        return ack;
    }
};

}  // namespace bf

using bf::MassAck;
using bf::MassModel;
using bf::SubmitMass;

// glaze-reflected action codecs (SubmitMass is a plain aggregate over a Quantity).
template <>
struct morph::model::ActionTraits<SubmitMass> {
    using Result = MassAck;
    static constexpr std::string_view typeId() { return "BF_SubmitMass"; }
    static std::string toJson(const SubmitMass& act) {
        return glz::write_json(act).value_or(std::string{});
    }
    static SubmitMass fromJson(std::string_view json) {
        SubmitMass act{};
        if (auto err = glz::read_json(act, json)) {
            throw morph::model::detail::ParseError{"SubmitMass parse failed"};
        }
        return act;
    }
    static std::string resultToJson(const MassAck& res) {
        return R"({"receivedDp":)" + std::to_string(res.receivedDp) +
               R"(,"engaged":)" + (res.engaged ? "true" : "false") + "}";
    }
    static MassAck resultFromJson(std::string_view) { return {}; }
};

BRIDGE_REGISTER_MODEL(MassModel, "BF_MassModel")

namespace {
const bool kRegSubmitMass =
    morph::model::detail::registerActionExecutorOnce<MassModel, SubmitMass>("BF_MassModel", "BF_SubmitMass");
}  // namespace

TEST_CASE("executeJson rejects an action that fails its validator (not silently executed)",
          "[bridge][execute-json][validation]") {
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExec cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<MassModel> handler{bridge, &cbExec};

    std::atomic<bool> done{false};
    std::atomic<bool> sawError{false};
    std::atomic<bool> sawResult{false};
    // Empty mass → validate() == false → must be rejected before the handler.
    handler.executeJson("BF_SubmitMass", R"({"mass":null})")
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

TEST_CASE("executeJson dispatches a valid action", "[bridge][execute-json][validation]") {
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExec cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<MassModel> handler{bridge, &cbExec};

    std::optional<std::string> result;
    std::atomic<bool> done{false};
    handler.executeJson("BF_SubmitMass", R"({"mass":{"num":5,"den":1,"dp":2}})")
        .then([&](std::string json) {
            result = std::move(json);
            done.store(true);
        })
        .onError([&](const std::exception_ptr&) { done.store(true); });

    REQUIRE(morph::testing::waitUntil([&] { return done.load(); }));
    REQUIRE(result.has_value());
    REQUIRE(result->find(R"("engaged":true)") != std::string::npos);
}

TEST_CASE("executeJson retags a submitted Quantity to its declared precision", "[bridge][execute-json][precision]") {
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExec cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<MassModel> handler{bridge, &cbExec};

    std::optional<std::string> result;
    std::atomic<bool> done{false};
    // Client submits at dp=2, but BFMass declares dp=3. The stored value must be
    // retagged to 3 before the handler sees it.
    handler.executeJson("BF_SubmitMass", R"({"mass":{"num":5,"den":1,"dp":2}})")
        .then([&](std::string json) {
            result = std::move(json);
            done.store(true);
        })
        .onError([&](const std::exception_ptr&) { done.store(true); });

    REQUIRE(morph::testing::waitUntil([&] { return done.load(); }));
    REQUIRE(result.has_value());
    // receivedDp is the declared precision (3), not the client's submitted dp (2).
    REQUIRE(result->find(R"("receivedDp":3)") != std::string::npos);
}
