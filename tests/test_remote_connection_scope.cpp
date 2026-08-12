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

TEST_CASE(
    "morph::backend::RemoteServer: deregister releases the requesting connection's own reference, not "
    "whichever connection attached last",
    "[remote][connection-scope]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto& env = csEnv();
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, env.dispatcher, env.registry);

    auto cidA = server->openConnection();
    auto cidB = server->openConnection();

    WaitReply regA;
    server->handle(morph::wire::encode(morph::wire::makeRegisterShared("CS_SquareModel", "42")), std::ref(regA),
                   cidA);
    REQUIRE(regA.await());
    REQUIRE(regA.env.kind == "ok");
    auto modelId = regA.env.modelId;

    // B attaches the same shared key -- the connection that "last touched"
    // the instance, exactly the case a single-owner ModelId->ConnectionId map
    // would misattribute the instance to.
    WaitReply regB;
    server->handle(morph::wire::encode(morph::wire::makeRegisterShared("CS_SquareModel", "42")), std::ref(regB),
                   cidB);
    REQUIRE(regB.await());
    REQUIRE(regB.env.modelId == modelId);

    // A releases its own reference explicitly.
    WaitReply dereg;
    server->handle(morph::wire::encode(morph::wire::makeDeregister(modelId)), std::ref(dereg), cidA);
    REQUIRE(dereg.await());
    REQUIRE(dereg.env.kind == "ok");

    // The instance must still be reachable -- B's reference is still live.
    morph::wire::Envelope execReq;
    execReq.kind = "execute";
    execReq.modelId = modelId;
    execReq.modelType = "CS_SquareModel";
    execReq.actionType = "CS_SquareAction";
    execReq.body = R"({"x":3})";
    WaitReply stillAlive;
    server->handle(morph::wire::encode(execReq), std::ref(stillAlive));
    REQUIRE(stillAlive.await());
    REQUIRE(stillAlive.env.kind == "ok");

    // Closing B's connection must release B's own reference and destroy the
    // instance -- it must not find its scope entry already (wrongly) cleared
    // by A's earlier deregister.
    server->closeConnection(cidB);

    WaitReply gone;
    server->handle(morph::wire::encode(execReq), std::ref(gone));
    REQUIRE(gone.await());
    REQUIRE(gone.env.kind == "err");
    REQUIRE(gone.env.message == "model not found");
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

// ── A register racing its own connection's close must not resurrect the scope ─

TEST_CASE("morph::backend::RemoteServer: a register arriving after closeConnection is refused, not stranded",
          "[remote][connection-scope]") {
    // handle() *posts*, while closeConnection() runs synchronously on the
    // transport's disconnect callback, so a client that registers and
    // immediately drops its socket genuinely lands in this order. Recording the
    // model with `_connectionScopes[cid]` would default-construct the scope
    // that closeConnection just erased, and nothing ever closes a scope twice:
    // the instance would survive with no connection able to reclaim it.
    morph::exec::ThreadPoolExecutor pool{2};
    auto& env = csEnv();
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, env.dispatcher, env.registry);

    auto cid = server->openConnection();
    server->closeConnection(cid);

    WaitReply reg;
    server->handle(morph::wire::encode(morph::wire::makeRegister("CS_SquareModel")), std::ref(reg), cid);
    REQUIRE(reg.await());
    REQUIRE(reg.env.kind == "err");
    REQUIRE(reg.env.message == "connection closed");

    // The decisive assertion: no instance was retained. A resurrected scope
    // would leave liveModels at 1 with no way to ever reclaim it, which is what
    // exhausts LimitPolicy::maxLiveModels and wedges the server.
    REQUIRE(server->health().liveModels == 0U);

    // And closing again stays a no-op rather than finding a recreated scope.
    server->closeConnection(cid);
    REQUIRE(server->health().liveModels == 0U);
}

TEST_CASE("morph::backend::RemoteServer: repeated registers on a closed scope never accumulate models",
          "[remote][connection-scope]") {
    // The leak is unbounded, not one-shot: every late register on a dead cid
    // used to add another unreclaimable instance.
    morph::exec::ThreadPoolExecutor pool{2};
    auto& env = csEnv();
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, env.dispatcher, env.registry);

    auto cid = server->openConnection();
    server->closeConnection(cid);

    for (int attempt = 0; attempt < 5; ++attempt) {
        WaitReply reg;
        server->handle(morph::wire::encode(morph::wire::makeRegister("CS_SquareModel")), std::ref(reg), cid);
        REQUIRE(reg.await());
        REQUIRE(reg.env.kind == "err");
    }
    REQUIRE(server->health().liveModels == 0U);
}

// ── Shared instance directory: the server side of keyed instances ────────────
//
// The directory keys on the `(typeId, primary)` strings the envelope carries,
// so it needs no keyed model type to exercise — that is a client-side concept.
// These drive RemoteServer directly, which is the only way to reach the
// refcount-and-scope interactions that make cross-client sharing safe.

TEST_CASE("morph::backend::RemoteServer: two connections sharing a key reach one instance",
          "[remote][connection-scope][shared-instances]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto& env = csEnv();
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, env.dispatcher, env.registry);

    auto cidA = server->openConnection();
    auto cidB = server->openConnection();

    WaitReply regA;
    server->handle(morph::wire::encode(morph::wire::makeRegisterShared("CS_SquareModel", "42")), std::ref(regA), cidA);
    REQUIRE(regA.await());
    REQUIRE(regA.env.kind == "ok");

    WaitReply regB;
    server->handle(morph::wire::encode(morph::wire::makeRegisterShared("CS_SquareModel", "42")), std::ref(regB), cidB);
    REQUIRE(regB.await());
    REQUIRE(regB.env.kind == "ok");

    // One instance, not two: the second register attached to the first's.
    REQUIRE(regB.env.modelId == regA.env.modelId);
    REQUIRE(server->health().liveModels == 1U);

    // Closing one connection releases only *its* reference — the instance must
    // survive for the connection still attached. This is the A7 change.
    server->closeConnection(cidA);
    REQUIRE(server->health().liveModels == 1U);

    morph::wire::Envelope execReq;
    execReq.kind = "execute";
    execReq.modelId = regA.env.modelId;
    execReq.modelType = "CS_SquareModel";
    execReq.actionType = "CS_SquareAction";
    execReq.body = R"({"x":5})";
    WaitReply stillThere;
    server->handle(morph::wire::encode(execReq), std::ref(stillThere));
    REQUIRE(stillThere.await());
    REQUIRE(stillThere.env.kind == "ok");
    REQUIRE(stillThere.env.body == "25");

    // The last reference goes, and so does the instance.
    server->closeConnection(cidB);
    REQUIRE(server->health().liveModels == 0U);
}

TEST_CASE("morph::backend::RemoteServer: instances lists live shared keys",
          "[remote][connection-scope][shared-instances]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto& env = csEnv();
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, env.dispatcher, env.registry);
    auto cid = server->openConnection();

    // Unrolled rather than looped: each Catch2 REQUIRE expands to branches, and
    // a loop around them trips the cognitive-complexity gate for no benefit.
    WaitReply regSeven;
    server->handle(morph::wire::encode(morph::wire::makeRegisterShared("CS_SquareModel", "7")), std::ref(regSeven),
                   cid);
    REQUIRE(regSeven.await());
    REQUIRE(regSeven.env.kind == "ok");

    WaitReply regNine;
    server->handle(morph::wire::encode(morph::wire::makeRegisterShared("CS_SquareModel", "9")), std::ref(regNine), cid);
    REQUIRE(regNine.await());
    REQUIRE(regNine.env.kind == "ok");

    // A private register is invisible to the directory by construction.
    WaitReply priv;
    server->handle(morph::wire::encode(morph::wire::makeRegister("CS_SquareModel")), std::ref(priv), cid);
    REQUIRE(priv.await());

    WaitReply listed;
    server->handle(morph::wire::encode(morph::wire::makeInstances("CS_SquareModel")), std::ref(listed), cid);
    REQUIRE(listed.await());
    REQUIRE(listed.env.kind == "ok");
    std::vector<std::string> keys;
    REQUIRE_FALSE(glz::read_json(keys, listed.env.body));
    std::ranges::sort(keys);
    REQUIRE(keys == std::vector<std::string>{"7", "9"});
}

TEST_CASE("morph::backend::RemoteServer: attach re-points and releases the old instance",
          "[remote][connection-scope][shared-instances]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto& env = csEnv();
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, env.dispatcher, env.registry);
    auto cid = server->openConnection();

    WaitReply first;
    server->handle(morph::wire::encode(morph::wire::makeRegisterShared("CS_SquareModel", "1")), std::ref(first), cid);
    REQUIRE(first.await());
    REQUIRE(first.env.kind == "ok");
    REQUIRE(server->health().liveModels == 1U);

    WaitReply moved;
    server->handle(morph::wire::encode(morph::wire::makeAttach("CS_SquareModel", "2", first.env.modelId)),
                   std::ref(moved), cid);
    REQUIRE(moved.await());
    REQUIRE(moved.env.kind == "ok");
    REQUIRE(moved.env.modelId != first.env.modelId);
    // Nobody else held key 1, so re-pointing destroyed it rather than leaking.
    REQUIRE(server->health().liveModels == 1U);
}

TEST_CASE("morph::backend::RemoteServer: assign files a live instance under a key",
          "[remote][connection-scope][shared-instances]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto& env = csEnv();
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, env.dispatcher, env.registry);
    auto cid = server->openConnection();

    // A private instance, as a create-style action would run on.
    WaitReply anon;
    server->handle(morph::wire::encode(morph::wire::makeRegister("CS_SquareModel")), std::ref(anon), cid);
    REQUIRE(anon.await());
    REQUIRE(anon.env.kind == "ok");

    WaitReply promoted;
    server->handle(morph::wire::encode(morph::wire::makeAssign("CS_SquareModel", "100", anon.env.modelId)),
                   std::ref(promoted), cid);
    REQUIRE(promoted.await());
    REQUIRE(promoted.env.kind == "ok");

    // It is the *same* instance, now reachable by key — nothing was recreated.
    WaitReply attached;
    server->handle(morph::wire::encode(morph::wire::makeRegisterShared("CS_SquareModel", "100")), std::ref(attached),
                   cid);
    REQUIRE(attached.await());
    REQUIRE(attached.env.modelId == anon.env.modelId);

}

TEST_CASE("morph::backend::RemoteServer: assign never displaces the incumbent holder of a key",
          "[remote][connection-scope][shared-instances]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto& env = csEnv();
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, env.dispatcher, env.registry);
    auto cid = server->openConnection();

    WaitReply incumbent;
    server->handle(morph::wire::encode(morph::wire::makeRegisterShared("CS_SquareModel", "200")), std::ref(incumbent),
                   cid);
    REQUIRE(incumbent.await());
    REQUIRE(incumbent.env.kind == "ok");

    // A second instance promoting onto the taken key is a silent no-op: the
    // incumbent always wins, so no already-attached client is redirected.
    WaitReply other;
    server->handle(morph::wire::encode(morph::wire::makeRegister("CS_SquareModel")), std::ref(other), cid);
    REQUIRE(other.await());
    WaitReply clash;
    server->handle(morph::wire::encode(morph::wire::makeAssign("CS_SquareModel", "200", other.env.modelId)),
                   std::ref(clash), cid);
    REQUIRE(clash.await());
    REQUIRE(clash.env.kind == "ok");

    WaitReply again;
    server->handle(morph::wire::encode(morph::wire::makeRegisterShared("CS_SquareModel", "200")), std::ref(again), cid);
    REQUIRE(again.await());
    REQUIRE(again.env.modelId == incumbent.env.modelId);
}

TEST_CASE("morph::backend::RemoteServer: the new kinds reject an empty typeId",
          "[remote][connection-scope][shared-instances]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto& env = csEnv();
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, env.dispatcher, env.registry);

    for (const auto& request : {morph::wire::makeAttach("", "1"), morph::wire::makeAssign("", "1", 7),
                                morph::wire::makeInstances("")}) {
        WaitReply reply;
        server->handle(morph::wire::encode(request), std::ref(reply));
        REQUIRE(reply.await());
        REQUIRE(reply.env.kind == "err");
    }
}

TEST_CASE("morph::backend::RemoteServer: a shared register on a closed scope is refused",
          "[remote][connection-scope][shared-instances]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto& env = csEnv();
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, env.dispatcher, env.registry);

    auto cid = server->openConnection();
    server->closeConnection(cid);

    WaitReply reg;
    server->handle(morph::wire::encode(morph::wire::makeRegisterShared("CS_SquareModel", "5")), std::ref(reg), cid);
    REQUIRE(reg.await());
    REQUIRE(reg.env.kind == "err");
    REQUIRE(reg.env.message == "connection closed");
    REQUIRE(server->health().liveModels == 0U);
}

// ── Issue #48: connection-scoped SimulatedRemoteBackend ─────────────────────
//
// SimulatedRemoteBackend used to send every register/deregister/attach/assign
// through the unscoped two-argument RemoteServer::handle/handleInline, so it
// had no way to open its own connection scope -- every simulated client
// looked identical to the server's registry, and closeConnection-style
// reclamation (rate limiting, connection-drop recovery, cross-connection
// shared-instance attach/detach) could not be exercised without a real
// socket. The ConnectionId constructor closes that gap.

TEST_CASE("morph::backend::SimulatedRemoteBackend: the unscoped constructor still uses ConnectionId{0}",
          "[remote][connection-scope][issue48]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto& env = csEnv();
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, env.dispatcher, env.registry);

    morph::backend::SimulatedRemoteBackend backend{*server};
    auto mid = backend.registerModel("CS_SquareModel", {});
    REQUIRE(mid.v != 0U);

    // Never attributed to any connection scope -- closing an arbitrary,
    // never-opened cid must not affect it (mirrors the existing "unscoped
    // handle() never populates any connection scope" regression test above).
    server->closeConnection(999999);
    REQUIRE(server->health().liveModels == 1U);
}

TEST_CASE("morph::backend::SimulatedRemoteBackend: a connection-scoped backend's registrations are reclaimed by "
          "closeConnection",
          "[remote][connection-scope][issue48]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto& env = csEnv();
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, env.dispatcher, env.registry);

    auto cid = server->openConnection();
    morph::backend::SimulatedRemoteBackend backend{*server, cid};

    auto mid = backend.registerModelWithContext("CS_SquareModel", {}, {});
    REQUIRE(mid.v != 0U);
    REQUIRE(server->health().liveModels == 1U);

    server->closeConnection(cid);
    REQUIRE(server->health().liveModels == 0U);
}

TEST_CASE(
    "morph::backend::SimulatedRemoteBackend: two connection-scoped backends sharing a key reach one instance, "
    "and each closeConnection releases only its own reference",
    "[remote][connection-scope][issue48][shared-instances]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto& env = csEnv();
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, env.dispatcher, env.registry);

    auto cidA = server->openConnection();
    auto cidB = server->openConnection();
    morph::backend::SimulatedRemoteBackend backendA{*server, cidA};
    morph::backend::SimulatedRemoteBackend backendB{*server, cidB};

    auto midA = backendA.registerModelShared("CS_SquareModel", {}, {.contextKey = "42", .primary = "42"});
    auto midB = backendB.registerModelShared("CS_SquareModel", {}, {.contextKey = "42", .primary = "42"});
    REQUIRE(midA.v == midB.v);  // one instance, not two
    REQUIRE(server->health().liveModels == 1U);

    // Closing A's connection releases only A's reference -- B still holds
    // the instance, exactly the cross-connection accounting a real
    // QtWebSocketServer/SocketServer gives, now reachable without a socket.
    server->closeConnection(cidA);
    REQUIRE(server->health().liveModels == 1U);

    server->closeConnection(cidB);
    REQUIRE(server->health().liveModels == 0U);
}

TEST_CASE("morph::backend::SimulatedRemoteBackend: deregisterModel releases this backend's own connection "
          "reference, not whichever connection attached last",
          "[remote][connection-scope][issue48][shared-instances]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto& env = csEnv();
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, env.dispatcher, env.registry);

    auto cidA = server->openConnection();
    auto cidB = server->openConnection();
    morph::backend::SimulatedRemoteBackend backendA{*server, cidA};
    morph::backend::SimulatedRemoteBackend backendB{*server, cidB};

    auto midA = backendA.registerModelShared("CS_SquareModel", {}, {.contextKey = "7", .primary = "7"});
    auto midB = backendB.registerModelShared("CS_SquareModel", {}, {.contextKey = "7", .primary = "7"});
    REQUIRE(midA.v == midB.v);

    // A releases its own reference explicitly; the instance must survive
    // because B's reference is still live.
    backendA.deregisterModel(midA);
    REQUIRE(server->health().liveModels == 1U);

    // Closing B's connection (which never explicitly deregistered) is what
    // finally releases the last reference.
    server->closeConnection(cidB);
    REQUIRE(server->health().liveModels == 0U);
}
