// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <memory>
#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/core/wire.hpp>
#include <morph/session/session.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include "test_support.hpp"

using morph::testing::WaitReply;
using morph::testing::waitUntil;

// ── morph::backend::RemoteServer::openConnection / closeConnection ──────────
// (Foundational bookkeeping only — no models are registered in this task; the
// scoped `register`/`deregister` wiring and its tests are Task 2.)

TEST_CASE("morph::backend::RemoteServer::openConnection: returns fresh non-zero ids", "[remote][connection-scope]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);

    auto cidA = server->openConnection();
    auto cidB = server->openConnection();
    REQUIRE(cidA != 0U);
    REQUIRE(cidB != 0U);
    REQUIRE(cidA != cidB);
}

TEST_CASE("morph::backend::RemoteServer::closeConnection: cid 0 is a no-op", "[remote][connection-scope]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);
    server->closeConnection(0);  // must not crash
}

TEST_CASE("morph::backend::RemoteServer::closeConnection: unknown cid is a no-op", "[remote][connection-scope]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);
    server->closeConnection(12345);  // never opened — must not crash
}

TEST_CASE("morph::backend::RemoteServer::closeConnection: closing a freshly opened, empty scope twice is idempotent",
          "[remote][connection-scope]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);

    auto cid = server->openConnection();
    server->closeConnection(cid);
    server->closeConnection(cid);  // second call: already closed, still a no-op
}

// ── Fixture models for connection-scope tests ───────────────────────────────

struct CsSquareAction {
    int x = 0;
};
struct CsSquareFail {};
struct CsSquareModel {
    int execute(const CsSquareAction& act) { return act.x * act.x; }
    int execute(const CsSquareFail&) { throw std::runtime_error("square failed"); }
};

template <>
struct morph::model::ModelTraits<CsSquareModel> {
    static constexpr std::string_view typeId() { return "CS_SquareModel"; }
};
template <>
struct morph::model::ActionTraits<CsSquareAction> {
    using Result = int;
    static constexpr std::string_view typeId() { return "CS_SquareAction"; }
    static std::string toJson(const CsSquareAction& act) {
        std::string out;
        (void)glz::write_json(act, out);
        return out;
    }
    static CsSquareAction fromJson(std::string_view json) {
        CsSquareAction action{};
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
struct morph::model::ActionTraits<CsSquareFail> {
    using Result = int;
    static constexpr std::string_view typeId() { return "CS_SquareFail"; }
    static std::string toJson(const CsSquareFail&) { return "{}"; }
    static CsSquareFail fromJson(std::string_view) { return {}; }
    static std::string resultToJson(const int&) { return "0"; }
    static int resultFromJson(std::string_view) { return 0; }
};

// Blocking model used to prove closeConnection never races a running execute.
struct CsSlowAction {};
struct CsSlowModel {
    static inline std::atomic<bool> started{false};
    static inline std::atomic<bool> proceed{false};

    int execute(const CsSlowAction&) {
        started.store(true);
        while (!proceed.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return 7;
    }
};

template <>
struct morph::model::ModelTraits<CsSlowModel> {
    static constexpr std::string_view typeId() { return "CS_SlowModel"; }
};
template <>
struct morph::model::ActionTraits<CsSlowAction> {
    using Result = int;
    static constexpr std::string_view typeId() { return "CS_SlowAction"; }
    static std::string toJson(const CsSlowAction&) { return "{}"; }
    static CsSlowAction fromJson(std::string_view) { return {}; }
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

struct CsEnv {
    morph::model::detail::ActionDispatcher dispatcher;
    morph::model::detail::ModelRegistryFactory registry;
};

static CsEnv& csEnv() {
    static CsEnv env = [] {
        CsEnv env2;
        env2.registry.registerModel<CsSquareModel>("CS_SquareModel");
        env2.dispatcher.registerAction<CsSquareModel, CsSquareAction>("CS_SquareModel", "CS_SquareAction");
        env2.dispatcher.registerAction<CsSquareModel, CsSquareFail>("CS_SquareModel", "CS_SquareFail");
        env2.registry.registerModel<CsSlowModel>("CS_SlowModel");
        env2.dispatcher.registerAction<CsSlowModel, CsSlowAction>("CS_SlowModel", "CS_SlowAction");
        return env2;
    }();
    return env;
}

// ── morph::backend::RemoteServer: scoped register / closeConnection reclaim ──

TEST_CASE("morph::backend::RemoteServer: closeConnection reclaims a model registered under its scope",
          "[remote][connection-scope]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto& env = csEnv();
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, env.dispatcher, env.registry);

    auto cid = server->openConnection();

    WaitReply reg;
    server->handle(morph::wire::encode(morph::wire::makeRegister("CS_SquareModel")), std::ref(reg), cid);
    REQUIRE(reg.await());
    REQUIRE(reg.env.kind == "ok");
    auto modelId = reg.env.modelId;

    // The instance works while the connection is "open".
    morph::wire::Envelope execReq;
    execReq.kind = "execute";
    execReq.modelId = modelId;
    execReq.modelType = "CS_SquareModel";
    execReq.actionType = "CS_SquareAction";
    execReq.body = R"({"x":4})";
    WaitReply before;
    server->handle(morph::wire::encode(execReq), std::ref(before));
    REQUIRE(before.await());
    REQUIRE(before.env.kind == "ok");
    REQUIRE(before.env.body == "16");

    server->closeConnection(cid);

    WaitReply after;
    server->handle(morph::wire::encode(execReq), std::ref(after));
    REQUIRE(after.await());
    REQUIRE(after.env.kind == "err");
    REQUIRE(after.env.message == "model not found");
}

TEST_CASE("morph::backend::RemoteServer: explicit deregister then closeConnection is idempotent",
          "[remote][connection-scope]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto& env = csEnv();
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, env.dispatcher, env.registry);

    auto cid = server->openConnection();

    WaitReply reg;
    server->handle(morph::wire::encode(morph::wire::makeRegister("CS_SquareModel")), std::ref(reg), cid);
    REQUIRE(reg.await());
    auto modelId = reg.env.modelId;

    WaitReply dereg;
    server->handle(morph::wire::encode(morph::wire::makeDeregister(modelId)), std::ref(dereg));
    REQUIRE(dereg.await());
    REQUIRE(dereg.env.kind == "ok");

    // The connection scope no longer references the (already-deregistered)
    // model, so closing it must not double-erase or crash.
    server->closeConnection(cid);
    server->closeConnection(cid);  // and closing twice stays a no-op

    morph::wire::Envelope execReq;
    execReq.kind = "execute";
    execReq.modelId = modelId;
    execReq.modelType = "CS_SquareModel";
    execReq.actionType = "CS_SquareAction";
    execReq.body = R"({"x":1})";
    WaitReply waiter;
    server->handle(morph::wire::encode(execReq), std::ref(waiter));
    REQUIRE(waiter.await());
    REQUIRE(waiter.env.kind == "err");
    REQUIRE(waiter.env.message == "model not found");
}

TEST_CASE("morph::backend::RemoteServer: connection scopes are isolated from one another",
          "[remote][connection-scope]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto& env = csEnv();
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, env.dispatcher, env.registry);

    auto cidA = server->openConnection();
    auto cidB = server->openConnection();

    WaitReply regA;
    server->handle(morph::wire::encode(morph::wire::makeRegister("CS_SquareModel")), std::ref(regA), cidA);
    REQUIRE(regA.await());
    auto modelA = regA.env.modelId;

    WaitReply regB;
    server->handle(morph::wire::encode(morph::wire::makeRegister("CS_SquareModel")), std::ref(regB), cidB);
    REQUIRE(regB.await());
    auto modelB = regB.env.modelId;

    server->closeConnection(cidA);

    morph::wire::Envelope execA;
    execA.kind = "execute";
    execA.modelId = modelA;
    execA.modelType = "CS_SquareModel";
    execA.actionType = "CS_SquareAction";
    execA.body = R"({"x":2})";
    WaitReply waiterA;
    server->handle(morph::wire::encode(execA), std::ref(waiterA));
    REQUIRE(waiterA.await());
    REQUIRE(waiterA.env.kind == "err");
    REQUIRE(waiterA.env.message == "model not found");

    // B's instance is untouched by A's disconnect.
    morph::wire::Envelope execB;
    execB.kind = "execute";
    execB.modelId = modelB;
    execB.modelType = "CS_SquareModel";
    execB.actionType = "CS_SquareAction";
    execB.body = R"({"x":3})";
    WaitReply waiterB;
    server->handle(morph::wire::encode(execB), std::ref(waiterB));
    REQUIRE(waiterB.await());
    REQUIRE(waiterB.env.kind == "ok");
    REQUIRE(waiterB.env.body == "9");
}

TEST_CASE("morph::backend::RemoteServer: the unscoped two-argument handle() never populates any connection scope",
          "[remote][connection-scope][regression]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto& env = csEnv();
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, env.dispatcher, env.registry);

    // Registered via the plain, unscoped handle() — exactly today's call shape.
    WaitReply reg;
    server->handle(morph::wire::encode(morph::wire::makeRegister("CS_SquareModel")), std::ref(reg));
    REQUIRE(reg.await());
    auto modelId = reg.env.modelId;

    // Closing an arbitrary, never-opened connection id must not affect it.
    server->closeConnection(999999);

    morph::wire::Envelope execReq;
    execReq.kind = "execute";
    execReq.modelId = modelId;
    execReq.modelType = "CS_SquareModel";
    execReq.actionType = "CS_SquareAction";
    execReq.body = R"({"x":5})";
    WaitReply waiter;
    server->handle(morph::wire::encode(execReq), std::ref(waiter));
    REQUIRE(waiter.await());
    REQUIRE(waiter.env.kind == "ok");
    REQUIRE(waiter.env.body == "25");
}

// ── morph::backend::RemoteServer::closeConnection: safety & authorization ───

TEST_CASE("morph::backend::RemoteServer::closeConnection: an in-flight execute completes safely across a disconnect",
          "[remote][connection-scope]") {
    CsSlowModel::started.store(false);
    CsSlowModel::proceed.store(false);

    morph::exec::ThreadPoolExecutor pool{2};
    auto& env = csEnv();
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, env.dispatcher, env.registry);

    auto cid = server->openConnection();

    WaitReply reg;
    server->handle(morph::wire::encode(morph::wire::makeRegister("CS_SlowModel")), std::ref(reg), cid);
    REQUIRE(reg.await());
    auto modelId = reg.env.modelId;

    morph::wire::Envelope execReq;
    execReq.kind = "execute";
    execReq.callId = 11;
    execReq.modelId = modelId;
    execReq.modelType = "CS_SlowModel";
    execReq.actionType = "CS_SlowAction";
    execReq.body = "{}";
    WaitReply execWaiter;
    server->handle(morph::wire::encode(execReq), std::ref(execWaiter), cid);
    REQUIRE(waitUntil([] { return CsSlowModel::started.load(); }));

    // The connection drops mid-execute: the transport calls closeConnection
    // while the action is still running on the model's strand.
    server->closeConnection(cid);

    // A concurrent lookup against the now-reclaimed id sees it as gone...
    WaitReply lookupWaiter;
    server->handle(morph::wire::encode(execReq), std::ref(lookupWaiter));
    REQUIRE(lookupWaiter.await());
    REQUIRE(lookupWaiter.env.kind == "err");
    REQUIRE(lookupWaiter.env.message == "model not found");

    // ...but the in-flight execute still completes and delivers its reply.
    CsSlowModel::proceed.store(true);
    REQUIRE(execWaiter.await());
    REQUIRE(execWaiter.env.kind == "ok");
    REQUIRE(execWaiter.env.callId == 11U);
    REQUIRE(execWaiter.env.body == "7");
}

TEST_CASE(
    "morph::backend::RemoteServer::closeConnection: reclaims an instance an ownership-enforcing authorizer "
    "would reject a foreign deregister for",
    "[remote][connection-scope][auth]") {
    struct OwnershipAuthorizer : morph::session::IAuthorizer {
        [[nodiscard]] bool authorize(const morph::session::Context&, std::string_view,
                                     std::string_view) const override {
            return true;
        }
        [[nodiscard]] std::optional<std::string> authenticate(const morph::session::Context& ctx) const override {
            return ctx.principal.empty() ? std::nullopt : std::make_optional(ctx.principal);
        }
        [[nodiscard]] bool authorizeInstance(const morph::session::Context& ctx, std::string_view, std::string_view,
                                             std::uint64_t, std::string_view ownerPrincipal) const override {
            return ownerPrincipal.empty() || ownerPrincipal == ctx.principal;
        }
    };

    morph::exec::ThreadPoolExecutor pool{2};
    auto& env = csEnv();
    auto authz = std::make_shared<OwnershipAuthorizer>();
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, authz, env.dispatcher, env.registry);

    auto cid = server->openConnection();

    morph::wire::Envelope regReq = morph::wire::makeRegister("CS_SquareModel");
    regReq.session.principal = "alice";
    WaitReply reg;
    server->handle(morph::wire::encode(regReq), std::ref(reg), cid);
    REQUIRE(reg.await());
    auto modelId = reg.env.modelId;

    // A foreign caller's wire deregister is rejected — ownership is enforced.
    morph::wire::Envelope foreignDereg = morph::wire::makeDeregister(modelId);
    foreignDereg.session.principal = "mallory";
    WaitReply foreignWaiter;
    server->handle(morph::wire::encode(foreignDereg), std::ref(foreignWaiter));
    REQUIRE(foreignWaiter.await());
    REQUIRE(foreignWaiter.env.kind == "err");
    REQUIRE(foreignWaiter.env.message == "unauthorized");

    // closeConnection reclaims the same instance anyway: it is server
    // housekeeping, not an action attributed to any caller, so it bypasses
    // IAuthorizer by design.
    server->closeConnection(cid);

    morph::wire::Envelope execReq;
    execReq.kind = "execute";
    execReq.modelId = modelId;
    execReq.modelType = "CS_SquareModel";
    execReq.actionType = "CS_SquareAction";
    execReq.body = R"({"x":3})";
    execReq.session.principal = "alice";
    WaitReply execWaiter;
    server->handle(morph::wire::encode(execReq), std::ref(execWaiter));
    REQUIRE(execWaiter.await());
    REQUIRE(execWaiter.env.kind == "err");
    REQUIRE(execWaiter.env.message == "model not found");
}
