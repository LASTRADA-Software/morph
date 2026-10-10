// SPDX-License-Identifier: Apache-2.0

// RemoteServer's one owner: every envelope, every public verb and every
// `execute` admission runs on the server strand, whichever thread asked.
//
// Each case calls from a thread that is not the server strand (a plain
// std::thread, or a task of the pool the strand runs on) and reads, inside the
// body the call reached, whether the server strand's scope was in force there
// — through the owner probe (`owner_probe_recorder.hpp`), which reads the
// scope chain itself rather than asking the server.

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <memory>
#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/core/wire.hpp>
#include <morph/journal/action_log.hpp>
#include <morph/testing/owner_probe_recorder.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include "test_support.hpp"

struct RssAddAction {
    int by = 0;
};

/// Blocks its model's strand until `release` is set.
struct RssHoldAction {};

/// Set while an `RssHoldAction` is running; cleared by the test.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): a flag shared between the test thread and the running action.
inline std::atomic<bool> gRssHoldStarted{false};
/// Lets a running `RssHoldAction` return.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): a flag shared between the test thread and the running action.
inline std::atomic<bool> gRssHoldRelease{false};

struct RssCounterModel {
    int value = 0;
    int execute(RssAddAction action) {
        value += action.by;
        return value;
    }
    [[nodiscard]] int execute(RssHoldAction /*action*/) const {
        gRssHoldStarted.store(true);
        (void)morph::testing::waitUntil([] { return gRssHoldRelease.load(); },
                                        morph::testing::WaitBudget{std::chrono::milliseconds{10000}});
        return value;
    }
};

template <>
struct morph::model::ModelTraits<RssCounterModel> {
    static constexpr std::string_view typeId() { return "RSS_CounterModel"; }
};
template <>
struct morph::model::ActionTraits<RssAddAction> {
    using Result = int;
    static constexpr std::string_view typeId() { return "RSS_AddAction"; }
    static std::string toJson(const RssAddAction& action) { return "{\"by\":" + std::to_string(action.by) + "}"; }
    static RssAddAction fromJson(std::string_view json) {
        RssAddAction action;
        if (auto pos = json.find(':'); pos != std::string_view::npos) {
            action.by = std::stoi(std::string{json.substr(pos + 1, json.find('}') - pos - 1)});
        }
        return action;
    }
    static std::string resultToJson(const int& result) { return std::to_string(result); }
    static int resultFromJson(std::string_view json) { return std::stoi(std::string{json}); }
};
template <>
struct morph::model::ActionTraits<RssHoldAction> {
    using Result = int;
    static constexpr std::string_view typeId() { return "RSS_HoldAction"; }
    static std::string toJson(const RssHoldAction& /*action*/) { return "{}"; }
    static RssHoldAction fromJson(std::string_view /*json*/) { return {}; }
    static std::string resultToJson(const int& result) { return std::to_string(result); }
    static int resultFromJson(std::string_view json) { return std::stoi(std::string{json}); }
};

namespace {

using morph::testing::OwnerProbeRecorder;
using morph::testing::WaitReply;

morph::model::detail::ActionDispatcher& rssDispatcher() {
    static morph::model::detail::ActionDispatcher dispatcher = [] {
        morph::model::detail::ActionDispatcher built;
        built.registerAction<RssCounterModel, RssAddAction>("RSS_CounterModel", "RSS_AddAction");
        built.registerAction<RssCounterModel, RssHoldAction>("RSS_CounterModel", "RSS_HoldAction");
        return built;
    }();
    return dispatcher;
}

morph::model::detail::ModelRegistryFactory& rssRegistry() {
    static morph::model::detail::ModelRegistryFactory registry = [] {
        morph::model::detail::ModelRegistryFactory built;
        built.registerModel<RssCounterModel>("RSS_CounterModel");
        return built;
    }();
    return registry;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the three are a model id, a call id and an increment.
morph::wire::Envelope executeEnvelope(std::uint64_t modelId, std::uint64_t callId, int by) {
    morph::wire::Envelope env;
    env.kind = "execute";
    env.callId = callId;
    env.modelId = modelId;
    env.modelType = "RSS_CounterModel";
    env.actionType = "RSS_AddAction";
    env.body = R"({"by":)" + std::to_string(by) + "}";
    return env;
}

/// @brief Runs @p body on a thread of its own, which is on no executor.
template <typename Body>
void onAnotherThread(Body&& body) {
    std::thread caller{std::forward<Body>(body)};
    caller.join();
}

}  // namespace

TEST_CASE(
    "RemoteServer: envelopes, connection scopes, the log provider and execute admission run on the server strand",
    "[remote][owner]") {
    morph::exec::ThreadPoolExecutor pool{2};
    std::atomic<int> providerCalls{0};
    morph::backend::ServerConfig config;
    config.limits.maxLiveModels = 8;
    config.limits.maxInFlightExecutes = 8;
    config.logProvider = [&providerCalls](std::string_view, std::string_view) {
        ++providerCalls;
        return std::shared_ptr<morph::journal::IActionLog>{};
    };
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, config, rssDispatcher(), rssRegistry());
    OwnerProbeRecorder const recorder{server->strand().coreExecutor()};
    REQUIRE_FALSE(morph::exec::runningOn(server->strand()));

    WaitReply reg;
    WaitReply exec;
    WaitReply dereg;
    onAnotherThread([&] {
        REQUIRE_FALSE(morph::exec::runningOn(server->strand()));
        auto const cid = server->openConnection();
        server->handle(morph::wire::encode(morph::wire::makeRegister("RSS_CounterModel", "ctx-1")), std::ref(reg),
                       cid);
        REQUIRE(reg.await());
        server->handle(morph::wire::encode(executeEnvelope(reg.env.modelId, 1, 3)), std::ref(exec), cid);
        REQUIRE(exec.await());
        server->handle(morph::wire::encode(morph::wire::makeDeregister(reg.env.modelId)), std::ref(dereg), cid);
        REQUIRE(dereg.await());
        server->closeConnection(cid);
    });
    REQUIRE(reg.env.kind == "ok");
    REQUIRE(exec.env.kind == "ok");
    REQUIRE(exec.env.body == "3");
    REQUIRE(dereg.env.kind == "ok");
    REQUIRE(providerCalls.load() == 1);
    // Posted after closeConnection() by the same thread's successor, so answered after it.
    REQUIRE(morph::testing::awaitAnswer([&](auto& owner) { return server->health(owner); }).liveModels == 0U);

    CHECK(recorder.count("RemoteServer::dispatch") == 3U);
    CHECK(recorder.allPosted("RemoteServer::dispatch"));
    CHECK(recorder.allPosted("RemoteServer::openConnection"));
    CHECK(recorder.allPosted("RemoteServer::closeConnection"));
    CHECK(recorder.allPosted("RemoteServer::attachLog"));
    CHECK(recorder.count("RemoteServer::admitExecute") == 1U);
    CHECK(recorder.allPosted("RemoteServer::admitExecute"));
    CHECK(recorder.allPosted("RemoteServer::executeFinished"));
}

TEST_CASE("RemoteServer: handleInline from another thread runs on the server strand and still returns the reply",
          "[remote][owner]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, rssDispatcher(), rssRegistry());
    OwnerProbeRecorder const recorder{server->strand().coreExecutor()};

    std::string reply;
    onAnotherThread(
        [&] { reply = server->handleInline(morph::wire::encode(morph::wire::makeRegister("RSS_CounterModel"))); });

    REQUIRE(morph::wire::decode(reply).kind == "ok");
    CHECK(recorder.count("RemoteServer::dispatch") == 1U);
    CHECK(recorder.allPosted("RemoteServer::dispatch"));
}

TEST_CASE("RemoteServer: health() called from a pool thread is answered on the server strand", "[remote][owner]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, rssDispatcher(), rssRegistry());
    OwnerProbeRecorder const recorder{server->strand().coreExecutor()};

    WaitReply reg;
    server->handle(morph::wire::encode(morph::wire::makeRegister("RSS_CounterModel")), std::ref(reg));
    REQUIRE(reg.await());

    // Asked from inside a task of the very pool the server strand runs on, but
    // not from inside the strand: the answer is still the strand's. The asker
    // is its own strand over the pool, and the answer is delivered there.
    morph::exec::OwnerStrand asker{pool};
    struct Asked {
        std::atomic<bool> done{false};
        std::atomic<bool> callerOnPool{false};
        std::atomic<bool> callerOnStrand{true};
        std::optional<morph::backend::HealthStatus> status;
    };
    auto asked = std::make_shared<Asked>();
    asker.post([&pool, &asker, server, asked] {
        asked->callerOnPool.store(morph::exec::runningOn(pool));
        asked->callerOnStrand.store(morph::exec::runningOn(server->strand()));
        server->health(asker).then([asked](const morph::backend::HealthStatus& status) {
            asked->status = status;
            asked->done.store(true);
        });
    });
    REQUIRE(morph::testing::waitUntil([&] { return asked->done.load(); }));

    CHECK(asked->callerOnPool.load());
    CHECK_FALSE(asked->callerOnStrand.load());
    REQUIRE(asked->status.has_value());
    CHECK(asked->status->ready);
    CHECK(asked->status->liveModels == 1U);
    CHECK(asked->status->inFlight == 0U);
    CHECK(recorder.count("RemoteServer::health") == 1U);
    CHECK(recorder.allPosted("RemoteServer::health"));
}

TEST_CASE("RemoteServer: beginShutdown and drainedWithin called off the strand run their bodies on it",
          "[remote][owner]") {
    morph::exec::ThreadPoolExecutor pool{2};
    std::atomic<int> handlerCalls{0};
    std::atomic<bool> handlerOnStrand{false};
    morph::backend::ServerConfig config;
    std::shared_ptr<morph::backend::RemoteServer> server;
    config.healthHandler = [&](const morph::backend::HealthStatus&) {
        if (server) {
            handlerOnStrand.store(morph::exec::runningOn(server->strand()));
        }
        ++handlerCalls;
    };
    server = std::make_shared<morph::backend::RemoteServer>(pool, config, rssDispatcher(), rssRegistry());
    REQUIRE(handlerCalls.load() == 1);  // from the constructor
    OwnerProbeRecorder const recorder{server->strand().coreExecutor()};

    std::optional<bool> drained;
    onAnotherThread([&] {
        server->beginShutdown();
        drained = morph::testing::awaitAnswer(
            [&](auto& owner) { return server->drainedWithin(std::chrono::milliseconds{2000}, owner); });
    });

    REQUIRE(drained.has_value());
    CHECK(*drained);
    CHECK(handlerCalls.load() == 2);
    CHECK(handlerOnStrand.load());
    CHECK(recorder.count("RemoteServer::beginShutdown") == 1U);
    CHECK(recorder.allPosted("RemoteServer::beginShutdown"));
    CHECK(recorder.count("RemoteServer::drainedWithin") == 1U);
    CHECK(recorder.allPosted("RemoteServer::drainedWithin"));
}

TEST_CASE("RemoteServer: a drainedWithin deadline is answered false on the server strand", "[remote][owner]") {
    gRssHoldStarted.store(false);
    gRssHoldRelease.store(false);
    morph::exec::ThreadPoolExecutor pool{3};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, rssDispatcher(), rssRegistry());
    OwnerProbeRecorder const recorder{server->strand().coreExecutor()};

    WaitReply reg;
    server->handle(morph::wire::encode(morph::wire::makeRegister("RSS_CounterModel")), std::ref(reg));
    REQUIRE(reg.await());

    auto hold = executeEnvelope(reg.env.modelId, 7, 0);
    hold.actionType = "RSS_HoldAction";
    hold.body = "{}";
    WaitReply exec;
    server->handle(morph::wire::encode(hold), std::ref(exec));
    REQUIRE(morph::testing::waitUntil([] { return gRssHoldStarted.load(); }));

    // The execute is in flight on its model's strand; the deadline elapses
    // first, and its expiry is posted back to the server strand.
    std::optional<bool> timedOut;
    onAnotherThread([&] {
        timedOut = morph::testing::awaitAnswer(
            [&](auto& owner) { return server->drainedWithin(std::chrono::milliseconds{20}, owner); });
    });
    REQUIRE(timedOut.has_value());
    CHECK_FALSE(*timedOut);
    CHECK(morph::testing::awaitAnswer([&](auto& owner) { return server->health(owner); }).inFlight == 1U);

    gRssHoldRelease.store(true);
    REQUIRE(exec.await());
    CHECK(exec.env.kind == "ok");
    CHECK(morph::testing::awaitAnswer(
        [&](auto& owner) { return server->drainedWithin(std::chrono::milliseconds{2000}, owner); }));
    CHECK(recorder.count("RemoteServer::drainedWithin") == 3U);
    CHECK(recorder.allPosted("RemoteServer::drainedWithin"));
    CHECK(recorder.allPosted("RemoteServer::executeFinished"));
}
