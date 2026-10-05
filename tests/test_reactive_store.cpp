// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <exception>
#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/callback_scope.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/signal.hpp>
#include <morph/reactive/store.hpp>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>

#include "owner_probe_recorder.hpp"
#include "test_support.hpp"

namespace {

using morph::reactive::Effect;
using morph::reactive::ExhaustiveUpdate;
using morph::reactive::Runtime;
using morph::reactive::Signal;
using morph::reactive::Store;
using Owner = morph::testing::StepExecutor;
using Probe = morph::testing::OwnerProbeRecorder;
namespace site = morph::reactive::detail::site;

// The overload-set idiom: one call operator per base lambda.
template <typename... Fs>
struct Overloaded : Fs... {  // NOLINT(misc-multiple-inheritance)
    using Fs::operator()...;
};

struct Counter {
    Signal<int> count;
    Signal<std::string> label;
    Signal<bool> armed;
};

struct Inc {};
struct Rename {
    std::string to;
};
struct Arm {};
using CounterMsg = std::variant<Inc, Rename, Arm>;

Counter initCounter(Runtime& runtime) {
    return Counter{.count{runtime, 0}, .label{runtime, "idle"}, .armed{runtime, false}};
}

constexpr auto updateCounter = Overloaded{
    [](Counter& state, Inc const&) { state.count.set(state.count.peek() + 1); },
    [](Counter& state, Rename const& msg) { state.label.set(msg.to); },
    [](Counter& state, Arm const&) {
        state.count.set(10);
        state.label.set("armed");
        state.armed.set(true);
    },
};

[[maybe_unused]] constexpr auto partialUpdate = Overloaded{
    [](Counter& state, Inc const&) { state.count.set(state.count.peek() + 1); },
    [](Counter& state, Rename const& msg) { state.label.set(msg.to); },
};

using InitFn = Counter (*)(Runtime&);

}  // namespace

static_assert(ExhaustiveUpdate<decltype(updateCounter), Counter, CounterMsg>);
static_assert(!ExhaustiveUpdate<decltype(partialUpdate), Counter, CounterMsg>);
static_assert(std::is_constructible_v<Store<Counter, CounterMsg>, Runtime&, InitFn, decltype(updateCounter)>);
static_assert(!std::is_constructible_v<Store<Counter, CounterMsg>, Runtime&, InitFn, decltype(partialUpdate)>);

TEST_CASE("reactive::Store: send applies the update for that alternative", "[reactive][store]") {
    Owner owner;
    Runtime runtime{owner};
    Store<Counter, CounterMsg> store{runtime, initCounter, updateCounter};
    store.send(Inc{});
    store.send(Rename{"two"});
    CHECK(store.state().count.peek() == 1);
    CHECK(store.state().label.peek() == "two");
}

TEST_CASE("reactive::Store: a three-field update is one flush", "[reactive][store]") {
    Owner owner;
    Runtime runtime{owner};
    Store<Counter, CounterMsg> store{runtime, initCounter, updateCounter};
    int runs = 0;
    Effect const watch{runtime, [&] {
                           static_cast<void>(store.state().count.get());
                           static_cast<void>(store.state().label.get());
                           static_cast<void>(store.state().armed.get());
                           ++runs;
                       }};
    store.send(Arm{});
    CHECK(owner.pending() == 1);
    owner.runAll();
    CHECK(runs == 2);
}

TEST_CASE("reactive::Store: an update runs in one batch and subscribes nothing", "[reactive][store]") {
    Owner owner;
    Runtime runtime{owner};
    std::size_t depthInUpdate = 0;
    auto const reading = Overloaded{
        [&](Counter& state, Inc const&) {
            depthInUpdate = runtime.core()->batchDepth();
            state.count.set(state.count.get() + 1);
        },
        [](Counter&, Rename const&) {},
        [](Counter&, Arm const&) {},
    };
    Store<Counter, CounterMsg> store{runtime, initCounter, reading};
    store.send(Inc{});
    CHECK(depthInUpdate == 1);
    // An Effect that sends: the update's get() inside it must not subscribe the Effect to count.
    int runs = 0;
    Effect const sender{runtime, [&] {
                            ++runs;
                            store.send(Inc{});
                        }};
    store.send(Inc{});
    CHECK(owner.pending() == 0);
    owner.runAll();
    CHECK(runs == 1);
    CHECK(store.state().count.peek() == 3);
}

TEST_CASE("reactive::Store: reading one field tracks exactly that field", "[reactive][store]") {
    Owner owner;
    Runtime runtime{owner};
    Store<Counter, CounterMsg> store{runtime, initCounter, updateCounter};
    Effect const watch{runtime, [&] { static_cast<void>(store.state().label.get()); }};
    store.send(Inc{});
    CHECK(owner.pending() == 0);
}

TEST_CASE("reactive::Store: action() sends its message each time it is called", "[reactive][store]") {
    Owner owner;
    Runtime runtime{owner};
    Store<Counter, CounterMsg> store{runtime, initCounter, updateCounter};
    auto const inc = store.action(Inc{});
    inc();
    inc();
    CHECK(store.state().count.peek() == 2);
}

TEST_CASE("reactive::Store: a move-only update is accepted", "[reactive][store]") {
    Owner owner;
    Runtime runtime{owner};
    auto step = std::make_unique<int>(3);
    auto moveOnly = Overloaded{
        [step = std::move(step)](Counter& state, Inc const&) { state.count.set(state.count.peek() + *step); },
        [](Counter& state, Rename const& msg) { state.label.set(msg.to); },
        [](Counter& state, Arm const&) { state.armed.set(true); },
    };
    static_assert(!std::is_copy_constructible_v<decltype(moveOnly)>);
    Store<Counter, CounterMsg> store{runtime, initCounter, std::move(moveOnly)};
    store.send(Inc{});
    CHECK(store.state().count.peek() == 3);
}

TEST_CASE("reactive::Store: send() inside an update is dropped and reported", "[reactive][store][misuse]") {
    Owner owner;
    Probe const probe{owner.coreExecutor()};
    Runtime runtime{owner};
    Store<Counter, CounterMsg>* self = nullptr;
    auto const reentrant = Overloaded{
        [&](Counter&, Inc const&) { self->send(Arm{}); },
        [](Counter&, Rename const&) {},
        [](Counter& state, Arm const&) { state.armed.set(true); },
    };
    Store<Counter, CounterMsg> store{runtime, initCounter, reentrant};
    self = &store;
    store.send(Inc{});
    CHECK_FALSE(store.state().armed.peek());
    CHECK(probe.count(site::kSendInUpdate) == 1);
    store.send(Arm{});  // the guard was released
    CHECK(store.state().armed.peek());
    CHECK(probe.count(site::kSendInUpdate) == 1);
}

TEST_CASE("reactive::Store: an update that throws releases the guard", "[reactive][store][misuse]") {
    Owner owner;
    Probe const probe{owner.coreExecutor()};
    Runtime runtime{owner};
    auto const throwing = Overloaded{
        [](Counter&, Inc const&) { throw std::runtime_error{"update failed"}; },
        [](Counter& state, Rename const& msg) { state.label.set(msg.to); },
        [](Counter& state, Arm const&) { state.armed.set(true); },
    };
    Store<Counter, CounterMsg> store{runtime, initCounter, throwing};
    CHECK_THROWS_AS(store.send(Inc{}), std::runtime_error);
    store.send(Arm{});
    CHECK(store.state().armed.peek());
    CHECK(probe.count(site::kSendInUpdate) == 0);
    CHECK(runtime.core()->batchDepth() == 0);
}

TEST_CASE("reactive::Store: send() off the owner is dropped and reported", "[reactive][store][misuse]") {
    Owner owner;
    Probe const probe{owner.coreExecutor()};
    Runtime runtime{owner};
    Store<Counter, CounterMsg> store{runtime, initCounter, updateCounter};
    Effect const watch{runtime, [&] { static_cast<void>(store.state().count.get()); }};
    std::thread{[&] { store.send(Inc{}); }}.join();
    CHECK(store.state().count.peek() == 0);
    CHECK(owner.pending() == 0);
    CHECK(probe.count(site::kOffOwner) == 1);
}

TEST_CASE("reactive::Store: an update may destroy its own Store", "[reactive][store]") {
    Owner owner;
    Runtime runtime{owner};
    std::unique_ptr<Store<Counter, CounterMsg>> owned;
    int seen = 0;
    // The update reads its own capture after the reset: the closure must outlive the Store.
    auto destroying = Overloaded{
        [&owned, &seen, step = std::make_shared<int>(5)](Counter&, Inc const&) {
            owned.reset();
            seen = *step;
        },
        [](Counter&, Rename const&) {},
        [](Counter&, Arm const&) {},
    };
    owned = std::make_unique<Store<Counter, CounterMsg>>(runtime, initCounter, std::move(destroying));
    owned->send(Inc{});
    CHECK(owned == nullptr);
    CHECK(seen == 5);
    CHECK(runtime.core()->batchDepth() == 0);
    CHECK(runtime.core()->tracking() == nullptr);
    CHECK(runtime.core()->liveNodes() == 0);
}

// ── request() over a real LocalBackend ─────────────────────────────────────

struct StorePingAction {
    int value = 0;
};
struct StorePingFail {};

struct StorePingModel {
    int execute(StorePingAction action) { return action.value * 2; }
    int execute(StorePingFail) { throw std::runtime_error("ping failed"); }
};

BRIDGE_REGISTER_MODEL(StorePingModel, "Test_StorePingModel")
BRIDGE_REGISTER_ACTION(StorePingModel, StorePingAction, "Test_StorePingAction")
BRIDGE_REGISTER_ACTION(StorePingModel, StorePingFail, "Test_StorePingFail")

namespace {

struct Reply {
    Signal<int> value;
    Signal<int> deliveries;
    Signal<std::string> error;
};

struct Doubled {
    int value;
};
struct Failed {
    std::exception_ptr error;
};
using ReplyMsg = std::variant<Doubled, Failed>;

Reply initReply(Runtime& runtime) { return Reply{.value{runtime, 0}, .deliveries{runtime, 0}, .error{runtime, ""}}; }

std::string describe(std::exception_ptr const& error) {
    try {
        std::rethrow_exception(error);
    } catch (std::exception const& exception) {
        return exception.what();
    } catch (...) {
        return "unknown";
    }
}

constexpr auto updateReply = Overloaded{
    [](Reply& state, Doubled const& msg) {
        state.value.set(msg.value);
        state.deliveries.set(state.deliveries.peek() + 1);
    },
    [](Reply& state, Failed const& msg) { state.error.set(describe(msg.error)); },
};

auto const toDoubled = [](int value) { return Doubled{value}; };
auto const toFailed = [](std::exception_ptr const& error) { return Failed{error}; };

struct Wiring {
    morph::exec::ThreadPoolExecutor pool{1};
    morph::exec::MainThreadExecutor owner;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool), owner};
    morph::bridge::BridgeHandler<StorePingModel> handler{bridge, &owner};
};

}  // namespace

TEST_CASE("reactive::request: a result arrives as its Msg", "[reactive][store]") {
    Wiring wiring;
    Runtime runtime{wiring.owner};
    Store<Reply, ReplyMsg> store{runtime, initReply, updateReply};
    morph::async::CallbackScope const scope;
    morph::reactive::request(store, wiring.handler, scope, StorePingAction{21}, toDoubled, toFailed);
    REQUIRE(morph::testing::pumpOwnerUntil(wiring.owner, [&] { return store.state().value.peek() == 42; }));
}

TEST_CASE("reactive::request: a failure arrives as a Msg carrying the exception", "[reactive][store]") {
    Wiring wiring;
    Runtime runtime{wiring.owner};
    Store<Reply, ReplyMsg> store{runtime, initReply, updateReply};
    morph::async::CallbackScope const scope;
    morph::reactive::request(store, wiring.handler, scope, StorePingFail{}, toDoubled, toFailed);
    REQUIRE(morph::testing::pumpOwnerUntil(wiring.owner, [&] { return store.state().error.peek() == "ping failed"; }));
}

TEST_CASE("reactive::request: a destroyed CallbackScope gates the reply", "[reactive][store]") {
    Wiring wiring;
    Runtime runtime{wiring.owner};
    Store<Reply, ReplyMsg> store{runtime, initReply, updateReply};
    auto gone = std::make_unique<morph::async::CallbackScope>();
    morph::async::CallbackScope const live;
    morph::reactive::request(store, wiring.handler, *gone, StorePingAction{21}, toDoubled, toFailed);
    gone.reset();
    // Same model instance, so the second reply is delivered after the first would have been.
    morph::reactive::request(store, wiring.handler, live, StorePingAction{5}, toDoubled, toFailed);
    REQUIRE(morph::testing::pumpOwnerUntil(wiring.owner, [&] { return store.state().value.peek() == 10; }));
    CHECK(store.state().deliveries.peek() == 1);
}

TEST_CASE("reactive::request: a destroyed CallbackScope gates a failure", "[reactive][store]") {
    Wiring wiring;
    Runtime runtime{wiring.owner};
    Store<Reply, ReplyMsg> store{runtime, initReply, updateReply};
    auto gone = std::make_unique<morph::async::CallbackScope>();
    morph::async::CallbackScope const live;
    morph::reactive::request(store, wiring.handler, *gone, StorePingFail{}, toDoubled, toFailed);
    gone.reset();
    // Same model instance, so the success is delivered after the failure would have been.
    morph::reactive::request(store, wiring.handler, live, StorePingAction{5}, toDoubled, toFailed);
    REQUIRE(morph::testing::pumpOwnerUntil(wiring.owner, [&] { return store.state().value.peek() == 10; }));
    CHECK(store.state().error.peek().empty());
}

TEST_CASE("reactive::request: a delivery whose update destroys the Store", "[reactive][store]") {
    Wiring wiring;
    Runtime runtime{wiring.owner};
    // What a controller looks like: it owns the Store and the scope its requests are gated by.
    struct Controller {
        std::unique_ptr<Store<Reply, ReplyMsg>> store;
        morph::async::CallbackScope scope;
    };
    auto controller = std::make_unique<Controller>();
    int seen = 0;
    auto closing = Overloaded{
        [&controller, &seen](Reply&, Doubled const& msg) {
            controller->store.reset();
            seen = msg.value;
        },
        [](Reply&, Failed const&) {},
    };
    controller->store = std::make_unique<Store<Reply, ReplyMsg>>(runtime, initReply, closing);
    morph::reactive::request(*controller->store, wiring.handler, controller->scope, StorePingAction{21}, toDoubled,
                             toFailed);
    REQUIRE(morph::testing::pumpOwnerUntil(wiring.owner, [&] { return controller->store == nullptr; }));
    CHECK(seen == 42);
    CHECK(runtime.core()->liveNodes() == 0);
}
