// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <exception>
#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
#include <morph/forms/engine/handler_submitter.hpp>
#include <optional>
#include <stdexcept>
#include <string>

// File-scope, not anonymous-namespaced: glaze's reflection needs linkage.
// NOLINTBEGIN(misc-use-internal-linkage)
struct HsBump {
    int by = 0;
};
struct HsRead {};
struct HsCount {
    int value = 0;
};
struct HsEcho {
    std::string text;
};

// In-memory state: each BridgeHandler<HsCounterModel> is its own instance under LocalBackend.
struct HsCounterModel {
    int count = 0;
    HsCount execute(HsBump action) {
        count += action.by;
        return HsCount{count};
    }
    HsCount execute(HsRead) const { return HsCount{count}; }
};

struct HsEchoModel {
    HsEcho execute(HsEcho action) { return action; }
};
// NOLINTEND(misc-use-internal-linkage)

BRIDGE_REGISTER_MODEL(HsCounterModel, "Test_HsCounterModel")
BRIDGE_REGISTER_ACTION(HsCounterModel, HsBump, "Test_HsBump")
BRIDGE_REGISTER_ACTION(HsCounterModel, HsRead, "Test_HsRead")
BRIDGE_REGISTER_MODEL(HsEchoModel, "Test_HsEchoModel")
BRIDGE_REGISTER_ACTION(HsEchoModel, HsEcho, "Test_HsEcho")

namespace {

struct Wiring {
    morph::exec::ThreadPoolExecutor pool{1};
    morph::exec::MainThreadExecutor owner;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool), owner};
    morph::bridge::BridgeHandler<HsCounterModel> counter{bridge, &owner};
    morph::bridge::BridgeHandler<HsEchoModel> echo{bridge, &owner};
};

struct Outcome {
    std::optional<std::string> reply;
    std::exception_ptr error;
};

Outcome settle(morph::exec::MainThreadExecutor& owner, morph::async::Completion<std::string> completion) {
    auto outcome = std::make_shared<Outcome>();
    auto done = std::make_shared<std::atomic<bool>>(false);
    completion
        .then([outcome, done](std::string const& reply) {
            outcome->reply = reply;
            done->store(true);
        })
        .onError([outcome, done](std::exception_ptr const& error) {
            outcome->error = error;
            done->store(true);
        });
    for (int step = 0; step < 400 && !done->load(); ++step) {
        owner.runFor(std::chrono::milliseconds{5});
    }
    REQUIRE(done->load());
    return *outcome;
}

}  // namespace

TEST_CASE("forms::handlerSubmitter: submissions land in the given handler's instance", "[forms-engine][handler]") {
    Wiring wiring;
    auto const submit = morph::forms::handlerSubmitter(wiring.owner, wiring.counter, wiring.echo);
    CHECK(settle(wiring.owner, submit("Test_HsBump", R"({"by":2})")).reply ==
          std::optional<std::string>{R"({"value":2})"});
    CHECK(settle(wiring.owner, submit("Test_HsBump", R"({"by":3})")).reply ==
          std::optional<std::string>{R"({"value":5})"});
    // The typed path on the same handler reads the same instance.
    CHECK(settle(wiring.owner, wiring.counter.executeJson("Test_HsRead", "{}")).reply ==
          std::optional<std::string>{R"({"value":5})"});
}

TEST_CASE("forms::handlerSubmitter: each action routes to the handler that serves it", "[forms-engine][handler]") {
    Wiring wiring;
    auto const submit = morph::forms::handlerSubmitter(wiring.owner, wiring.counter, wiring.echo);
    CHECK(settle(wiring.owner, submit("Test_HsEcho", R"({"text":"hi"})")).reply ==
          std::optional<std::string>{R"({"text":"hi"})"});
}

TEST_CASE("forms::handlerSubmitter: an action no handler serves is a failed completion", "[forms-engine][handler]") {
    Wiring wiring;
    auto const submit = morph::forms::handlerSubmitter(wiring.owner, wiring.echo);
    auto const outcome = settle(wiring.owner, submit("Test_HsBump", R"({"by":1})"));
    REQUIRE(outcome.error != nullptr);
    CHECK_THROWS_AS(std::rethrow_exception(outcome.error), std::invalid_argument);
}

TEST_CASE("forms::handlerChoiceFetcher: fetches options through the given handler", "[forms-engine][handler]") {
    Wiring wiring;
    auto const fetch = morph::forms::handlerChoiceFetcher(wiring.owner, wiring.counter);
    CHECK(settle(wiring.owner, fetch("Test_HsRead", "{}")).reply == std::optional<std::string>{R"({"value":0})"});
}
