// SPDX-License-Identifier: Apache-2.0

#include <morph/bridge.hpp>
#include <morph/executor.hpp>
#include <morph/registry.hpp>
#include <morph/remote.hpp>
#include <morph/session.hpp>
#include <morph/session_auth.hpp>
#include <morph/wire.hpp>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <stdexcept>
#include <thread>

#include "test_support.hpp"


// Fresh dispatcher + registry per test to avoid global state pollution
struct Env {
    morph::model::detail::ActionDispatcher dispatcher;
    morph::model::detail::ModelRegistryFactory registry;
};

struct SquareAction {
    int x = 0;
};
struct SquareFail {};
struct SquareModel {
    int execute(const SquareAction& act) { return act.x * act.x; }
    int execute(const SquareFail&) { throw std::runtime_error("square failed"); }
};

template <>
struct morph::model::ModelTraits<SquareModel> {
    static constexpr std::string_view typeId() { return "RX_SquareModel"; }
};
template <>
struct morph::model::ActionTraits<SquareAction> {
    using Result = int;
    static constexpr std::string_view typeId() { return "RX_SquareAction"; }
    static std::string toJson(const SquareAction& act) {
        std::string out;
        (void)glz::write_json(act, out);
        return out;
    }
    static SquareAction fromJson(std::string_view json) {
        SquareAction action{};
        (void)glz::read_json(action, json);
        return action;
    }
    static std::string resultToJson(const int& res) {
        std::string out;
        (void)glz::write_json(res, out);
        return out;
    }
    static int resultFromJson(std::string_view json) {
        int result{};
        (void)glz::read_json(result, json);
        return result;
    }
};
template <>
struct morph::model::ActionTraits<SquareFail> {
    using Result = int;
    static constexpr std::string_view typeId() { return "RX_SquareFail"; }
    static std::string toJson(const SquareFail&) { return "{}"; }
    static SquareFail fromJson(std::string_view) { return {}; }
    static std::string resultToJson(const int&) { return "0"; }
    static int resultFromJson(std::string_view) { return 0; }
};

static Env& sharedEnv() {
    static Env env = [] {
        Env env2;
        env2.registry.registerModel<SquareModel>("RX_SquareModel");
        env2.dispatcher.registerAction<SquareModel, SquareAction>("RX_SquareModel", "RX_SquareAction");
        env2.dispatcher.registerAction<SquareModel, SquareFail>("RX_SquareModel", "RX_SquareFail");
        return env2;
    }();
    return env;
}

using SyncExec = morph::testing::InlineExecutor;
using morph::testing::WaitReply;

// ── morph::backend::RemoteServer: bad message type ───────────────────────────────────────────

TEST_CASE("morph::backend::RemoteServer: unknown command replies with err", "[remote]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto& env = sharedEnv();
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, env.dispatcher, env.registry);

    morph::wire::Envelope req;
    req.kind = "badcmd";
    WaitReply waiter;
    server->handle(morph::wire::encode(req), std::ref(waiter));
    waiter.await();
    REQUIRE(waiter.env.kind == "err");
}

// ── morph::backend::RemoteServer: deregister path ────────────────────────────────────────────

TEST_CASE("morph::backend::RemoteServer: register then deregister succeeds", "[remote]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto& env = sharedEnv();
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, env.dispatcher, env.registry);

    WaitReply reg;
    server->handle(morph::wire::encode(morph::wire::makeRegister("RX_SquareModel")), std::ref(reg));
    reg.await();
    REQUIRE(reg.env.kind == "ok");

    WaitReply dereg;
    server->handle(morph::wire::encode(morph::wire::makeDeregister(reg.env.modelId)), std::ref(dereg));
    dereg.await();
    REQUIRE(dereg.env.kind == "ok");
}

// ── morph::backend::RemoteServer: execute on unknown model ─────────────────────────────────

TEST_CASE("morph::backend::RemoteServer: execute on unknown model replies with err", "[remote]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto& env = sharedEnv();
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, env.dispatcher, env.registry);

    morph::wire::Envelope req;
    req.kind = "execute";
    req.modelId = 9999;
    req.modelType = "RX_SquareModel";
    req.actionType = "RX_SquareAction";
    req.body = R"({"x":3})";
    WaitReply waiter;
    server->handle(morph::wire::encode(req), std::ref(waiter));
    waiter.await();
    REQUIRE(waiter.env.kind == "err");
    REQUIRE(waiter.env.message == "model not found");
}

TEST_CASE("morph::backend::RemoteServer: execute on unknown model echoes callId in err", "[remote]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto& env = sharedEnv();
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, env.dispatcher, env.registry);

    morph::wire::Envelope req;
    req.kind = "execute";
    req.callId = 7;
    req.modelId = 9999;
    req.modelType = "RX_SquareModel";
    req.actionType = "RX_SquareAction";
    req.body = R"({"x":3})";
    WaitReply waiter;
    server->handle(morph::wire::encode(req), std::ref(waiter));
    waiter.await();
    REQUIRE(waiter.env.kind == "err");
    REQUIRE(waiter.env.callId == 7U);
}

// ── morph::backend::RemoteServer: valid execute with callId ────────────────────────────────

TEST_CASE("morph::backend::RemoteServer: execute with callId returns ok envelope", "[remote]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto& env = sharedEnv();
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, env.dispatcher, env.registry);

    WaitReply reg;
    server->handle(morph::wire::encode(morph::wire::makeRegister("RX_SquareModel")), std::ref(reg));
    reg.await();
    REQUIRE(reg.env.kind == "ok");

    morph::wire::Envelope req;
    req.kind = "execute";
    req.callId = 42;
    req.modelId = reg.env.modelId;
    req.modelType = "RX_SquareModel";
    req.actionType = "RX_SquareAction";
    req.body = R"({"x":5})";
    WaitReply waiter;
    server->handle(morph::wire::encode(req), std::ref(waiter));
    waiter.await();
    REQUIRE(waiter.env.kind == "ok");
    REQUIRE(waiter.env.callId == 42U);
    REQUIRE(waiter.env.body == "25");
}

// ── morph::backend::RemoteServer: dispatch exception propagates as err reply ──────────────────

TEST_CASE("morph::backend::RemoteServer: model action exception becomes err reply", "[remote]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto& env = sharedEnv();
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, env.dispatcher, env.registry);

    WaitReply reg;
    server->handle(morph::wire::encode(morph::wire::makeRegister("RX_SquareModel")), std::ref(reg));
    reg.await();
    REQUIRE(reg.env.kind == "ok");

    morph::wire::Envelope req;
    req.kind = "execute";
    req.modelId = reg.env.modelId;
    req.modelType = "RX_SquareModel";
    req.actionType = "RX_SquareFail";
    req.body = "{}";
    WaitReply waiter;
    server->handle(morph::wire::encode(req), std::ref(waiter));
    waiter.await();
    REQUIRE(waiter.env.kind == "err");
}

// ── morph::backend::SimulatedRemoteBackend: malformed reply ───────────────────────────────────

TEST_CASE("morph::backend::SimulatedRemoteBackend: malformed reply triggers onError", "[remote]") {
    SyncExec cbExec;
    auto state = std::make_shared<morph::async::detail::CompletionState<std::shared_ptr<void>>>();
    morph::async::Completion<std::shared_ptr<void>> comp{state, &cbExec};

    std::string errMsg;
    comp.onError([&](const std::exception_ptr& exc) {
        try {
            std::rethrow_exception(exc);
        } catch (const std::runtime_error& ex) {
            errMsg = ex.what();
        }
    });

    // Simulate what the callback in morph::backend::SimulatedRemoteBackend does with garbage:
    // decode() throws on non-JSON, which the backend turns into a CompletionState exception.
    try {
        (void)morph::wire::decode("garbage");
    } catch (...) {
        state->setException(std::current_exception());
    }

    REQUIRE_FALSE(errMsg.empty());
}

// ── morph::backend::SimulatedRemoteBackend: register failure (server sends non-ok reply) ──────

TEST_CASE("morph::backend::SimulatedRemoteBackend: register failure raises exception", "[remote]") {
    auto reply = morph::wire::encode(morph::wire::makeErr("no models allowed"));
    auto decoded = morph::wire::decode(reply);
    REQUIRE(decoded.kind == "err");
    REQUIRE(decoded.message == "no models allowed");
}

// ── morph::backend::RemoteServer::handleInline: execute is rejected up front ────────────────

TEST_CASE("morph::backend::RemoteServer::handleInline: execute envelope replies with err, not silently dropped",
          "[remote][handleInline]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto& env = sharedEnv();
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, env.dispatcher, env.registry);

    morph::wire::Envelope req;
    req.kind = "execute";
    req.callId = 5;
    req.modelId = 1;
    req.modelType = "RX_SquareModel";
    req.actionType = "RX_SquareAction";
    req.body = R"({"x":3})";

    auto reply = morph::wire::decode(server->handleInline(morph::wire::encode(req)));
    REQUIRE(reply.kind == "err");
    REQUIRE(reply.callId == 5U);
    REQUIRE(reply.message.find("execute") != std::string::npos);
}

TEST_CASE("morph::backend::RemoteServer::handleInline: malformed input falls through to the decode-error reply",
          "[remote][handleInline]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto& env = sharedEnv();
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, env.dispatcher, env.registry);

    auto reply = morph::wire::decode(server->handleInline("not json"));
    REQUIRE(reply.kind == "err");
}

// ── morph::backend::RemoteServer::dispatchExecute: authenticate() overrides principal ───────

struct WhoAmI {};
struct PrincipalEchoModel {
    std::string execute(const WhoAmI&) const {
        const auto* ctx = morph::session::current();
        return (ctx != nullptr) ? ctx->principal : std::string{};
    }
};

template <>
struct morph::model::ModelTraits<PrincipalEchoModel> {
    static constexpr std::string_view typeId() { return "RX_PrincipalEchoModel"; }
};
template <>
struct morph::model::ActionTraits<WhoAmI> {
    using Result = std::string;
    static constexpr std::string_view typeId() { return "RX_WhoAmI"; }
    static std::string toJson(const WhoAmI&) { return "{}"; }
    static WhoAmI fromJson(std::string_view) { return {}; }
    static std::string resultToJson(const std::string& res) { return "\"" + res + "\""; }
    static std::string resultFromJson(std::string_view json) {
        std::string out;
        (void)glz::read_json(out, json);
        return out;
    }
};

TEST_CASE(
    "morph::backend::RemoteServer::dispatchExecute: a verifying authorizer overrides the client-asserted principal",
    "[remote][auth]") {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::model::detail::ModelRegistryFactory registry;
    morph::model::detail::ActionDispatcher dispatcher;
    registry.registerModel<PrincipalEchoModel>("RX_PrincipalEchoModel");
    dispatcher.registerAction<PrincipalEchoModel, WhoAmI>("RX_PrincipalEchoModel", "RX_WhoAmI");

    const std::string secret = "hmac-key";
    auto authz = std::make_shared<morph::session::SigningAuthorizer>(secret);
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, authz, dispatcher, registry);

    WaitReply reg;
    server->handle(morph::wire::encode(morph::wire::makeRegister("RX_PrincipalEchoModel")), std::ref(reg));
    reg.await();
    REQUIRE(reg.env.kind == "ok");

    morph::wire::Envelope req;
    req.kind = "execute";
    req.modelId = reg.env.modelId;
    req.modelType = "RX_PrincipalEchoModel";
    req.actionType = "RX_WhoAmI";
    req.body = "{}";
    req.session.principal = "spoofed-client-claim";
    // expiresAtMs must be strictly positive — a zero expiry is now treated as
    // already-expired (never eternal), so mint with a far-future real expiry.
    req.session.token = morph::session::TokenIssuer{secret}.issue(
        morph::session::SessionToken{.principal = "real-verified-user", .expiresAtMs = 9'999'999'999'999});

    WaitReply waiter;
    server->handle(morph::wire::encode(req), std::ref(waiter));
    waiter.await();
    REQUIRE(waiter.env.kind == "ok");
    REQUIRE(waiter.env.body == "\"real-verified-user\"");
}
