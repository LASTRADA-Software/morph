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

// A model whose registered factory sleeps once (exchange-guarded, same idiom
// as test_remote_execute_ordering.cpp's SlowFirstAuthorizer) before returning
// a plain ModelHolder. Used to force the exact interleaving
// acquireSharedInstance's second attachExistingLocked check (remote.hpp) is
// for: two attaches to the same not-yet-existing key racing each other,
// where only one of the two _registry.create() calls can win the insert.
struct CsRaceModel {
    static inline std::atomic<bool> slowFactoryTaken{false};

    int execute(const CsSquareAction& act) { return act.x * act.x; }
};

template <>
struct morph::model::ModelTraits<CsRaceModel> {
    static constexpr std::string_view typeId() { return "CS_RaceModel"; }
};

static CsEnv& csEnv() {
    static CsEnv env = [] {
        CsEnv env2;
        env2.registry.registerModel<CsSquareModel>("CS_SquareModel");
        env2.dispatcher.registerAction<CsSquareModel, CsSquareAction>("CS_SquareModel", "CS_SquareAction");
        env2.dispatcher.registerAction<CsSquareModel, CsSquareFail>("CS_SquareModel", "CS_SquareFail");
        env2.registry.registerModel<CsSlowModel>("CS_SlowModel");
        env2.dispatcher.registerAction<CsSlowModel, CsSlowAction>("CS_SlowModel", "CS_SlowAction");
        env2.registry.registerModel<CsRaceModel>(
            "CS_RaceModel", []() -> std::unique_ptr<::morph::model::detail::IModelHolder> {
                if (!CsRaceModel::slowFactoryTaken.exchange(true)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds{200});
                }
                return std::make_unique<morph::model::detail::ModelHolder<CsRaceModel> >();
            });
        env2.dispatcher.registerAction<CsRaceModel, CsSquareAction>("CS_RaceModel", "CS_SquareAction");
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

TEST_CASE(
    "morph::backend::RemoteServer: attach on an already-closed connection scope replies "
    "\"connection closed\" instead of recording a bogus attachment",
    "[remote][connection-scope]") {
    // Race window remote.hpp's attachExistingLocked() exists to handle: a
    // client sends attach, but its connection is gone by the time the server
    // gets to noteScopeAttachLocked() -- e.g. the socket dropped between the
    // client sending the request and the server processing it. There is
    // nothing timing-dependent to reproduce here: closeConnection() and
    // handle() are both synchronous under _regMtx, so calling closeConnection
    // on a cid and then handle()-ing an attach carrying that same (now-closed)
    // cid deterministically presents the exact precondition
    // noteScopeAttachLocked() checks for, on every run.
    morph::exec::ThreadPoolExecutor pool{2};
    auto& env = csEnv();
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, env.dispatcher, env.registry);

    auto cidA = server->openConnection();
    auto cidB = server->openConnection();

    // A creates the shared instance and keeps a live reference to it, so the
    // directory entry B is about to attach to still exists.
    WaitReply regA;
    server->handle(morph::wire::encode(morph::wire::makeRegisterShared("CS_SquareModel", "42")), std::ref(regA),
                   cidA);
    REQUIRE(regA.await());
    REQUIRE(regA.env.kind == "ok");
    auto modelId = regA.env.modelId;

    // B's connection drops before its attach is processed.
    server->closeConnection(cidB);

    WaitReply attachB;
    server->handle(morph::wire::encode(morph::wire::makeAttach("CS_SquareModel", "42")), std::ref(attachB), cidB);
    REQUIRE(attachB.await());
    REQUIRE(attachB.env.kind == "err");
    REQUIRE(attachB.env.message == "connection closed");

    // The instance itself is untouched -- A's own reference survives B's
    // failed, already-dead attach attempt.
    morph::wire::Envelope execReq;
    execReq.kind = "execute";
    execReq.modelId = modelId;
    execReq.modelType = "CS_SquareModel";
    execReq.actionType = "CS_SquareAction";
    execReq.body = R"({"x":6})";
    WaitReply stillAlive;
    server->handle(morph::wire::encode(execReq), std::ref(stillAlive));
    REQUIRE(stillAlive.await());
    REQUIRE(stillAlive.env.kind == "ok");
    REQUIRE(stillAlive.env.body == "36");
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

TEST_CASE(
    "morph::backend::RemoteServer: attach re-points away from a key another connection still holds -- the "
    "old instance survives",
    "[remote][connection-scope][shared-instances]") {
    // Sibling of "attach re-points and releases the old instance" above, but
    // for the *other* branch of releaseScopedLocked's refcount-hits-zero
    // check (docs/spec/core/shared_instances.md, "Re-pointing, not
    // re-keying": "The instance for 42 is untouched ... and survives if any
    // other handler is still attached"). That test's sole connection is the
    // only holder of the key it moves away from, so the count always hits
    // zero; this one adds a second connection sharing the same key first, so
    // the re-pointing connection's own release decrements without reaching
    // zero.
    morph::exec::ThreadPoolExecutor pool{2};
    auto& env = csEnv();
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, env.dispatcher, env.registry);
    auto cidA = server->openConnection();
    auto cidB = server->openConnection();

    WaitReply regA;
    server->handle(morph::wire::encode(morph::wire::makeRegisterShared("CS_SquareModel", "1")), std::ref(regA), cidA);
    REQUIRE(regA.await());
    REQUIRE(regA.env.kind == "ok");

    WaitReply regB;
    server->handle(morph::wire::encode(morph::wire::makeRegisterShared("CS_SquareModel", "1")), std::ref(regB), cidB);
    REQUIRE(regB.await());
    REQUIRE(regB.env.kind == "ok");
    REQUIRE(regB.env.modelId == regA.env.modelId);  // both connections share the one instance for key 1
    REQUIRE(server->health().liveModels == 1U);

    // cidA re-points from key 1 to key 2. cidB still holds key 1, so the old
    // instance's refcount drops from 2 to 1 -- not to 0 -- and must survive.
    WaitReply moved;
    server->handle(morph::wire::encode(morph::wire::makeAttach("CS_SquareModel", "2", regA.env.modelId)),
                   std::ref(moved), cidA);
    REQUIRE(moved.await());
    REQUIRE(moved.env.kind == "ok");
    REQUIRE(moved.env.modelId != regA.env.modelId);
    // Two live instances now: the new one for key 2, and the old one for key
    // 1 -- still alive because cidB is still attached to it.
    REQUIRE(server->health().liveModels == 2U);

    // Key 1 is still reachable and still the same instance cidB originally
    // attached to -- proving it was kept alive, not silently recreated.
    WaitReply reattachB;
    server->handle(morph::wire::encode(morph::wire::makeRegisterShared("CS_SquareModel", "1")), std::ref(reattachB),
                   cidB);
    REQUIRE(reattachB.await());
    REQUIRE(reattachB.env.kind == "ok");
    REQUIRE(reattachB.env.modelId == regA.env.modelId);
    REQUIRE(server->health().liveModels == 2U);  // no new instance was created

    // Finally, cidB releases its own reference to key 1 -- now the refcount
    // does hit zero, and the old instance is reclaimed.
    server->closeConnection(cidB);
    REQUIRE(server->health().liveModels == 1U);  // only key 2's instance remains
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

TEST_CASE("morph::backend::RemoteServer: assign with an empty primary or an unregistered modelId is a silent "
          "no-op, still reporting \"ok\"",
          "[remote][connection-scope][shared-instances]") {
    // applyAssignLocked's own early-return guard (env.primary.empty() ||
    // !_models.contains(mid)) never actually fires in any of this file's
    // other assign tests -- every one of them assigns a real, just-created
    // mid onto a non-empty primary. The wire handler replies "ok"
    // unconditionally after applyAssignLocked() returns, whether it filed
    // anything or silently declined to, so this is the one place that
    // distinction is externally observable: check what a subsequent
    // register-shared onto the same key actually reaches.
    morph::exec::ThreadPoolExecutor pool{2};
    auto& env = csEnv();
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, env.dispatcher, env.registry);
    auto cid = server->openConnection();

    WaitReply anon;
    server->handle(morph::wire::encode(morph::wire::makeRegister("CS_SquareModel")), std::ref(anon), cid);
    REQUIRE(anon.await());
    REQUIRE(anon.env.kind == "ok");

    // Empty primary: the guard's first disjunct.
    WaitReply emptyPrimary;
    server->handle(morph::wire::encode(morph::wire::makeAssign("CS_SquareModel", "", anon.env.modelId)),
                   std::ref(emptyPrimary), cid);
    REQUIRE(emptyPrimary.await());
    REQUIRE(emptyPrimary.env.kind == "ok");

    // An unregistered modelId: the guard's second disjunct. 0 is never a real
    // id (nextOpaqueId() never hands it out), so _models never contains it.
    WaitReply unknownMid;
    server->handle(morph::wire::encode(morph::wire::makeAssign("CS_SquareModel", "300", 0)), std::ref(unknownMid),
                   cid);
    REQUIRE(unknownMid.await());
    REQUIRE(unknownMid.env.kind == "ok");

    // Neither call filed anything: a fresh register-shared under "300" gets
    // its own new instance, not anon's -- had the guard been bypassed, this
    // would instead reach anon.env.modelId.
    WaitReply attached;
    server->handle(morph::wire::encode(morph::wire::makeRegisterShared("CS_SquareModel", "300")), std::ref(attached),
                   cid);
    REQUIRE(attached.await());
    REQUIRE(attached.env.modelId != anon.env.modelId);
}

TEST_CASE("morph::backend::RemoteServer: assign stamps the caller's authenticated principal, not just the "
          "unauthenticated-empty case",
          "[remote][connection-scope][shared-instances][auth]") {
    // "assign"'s authenticate() call has two branches: the caller is
    // unauthenticated (env.session.principal cleared -- already covered by
    // every other assign test in this file, none of which configure an
    // authenticating IAuthorizer), and the caller *is* authenticated (the
    // verified principal is stamped onto env.session.principal instead).
    // OwnershipAuthorizer::authenticate returns ctx.principal verbatim
    // whenever it's non-empty, so a session carrying one exercises the
    // second branch deterministically.
    struct EchoAuthorizer : morph::session::IAuthorizer {
        [[nodiscard]] bool authorize(const morph::session::Context&, std::string_view,
                                     std::string_view) const override {
            return true;
        }
        [[nodiscard]] std::optional<std::string> authenticate(const morph::session::Context& ctx) const override {
            return ctx.principal.empty() ? std::nullopt : std::make_optional(ctx.principal);
        }
    };

    morph::exec::ThreadPoolExecutor pool{2};
    auto& env = csEnv();
    auto authz = std::make_shared<EchoAuthorizer>();
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, authz, env.dispatcher, env.registry);
    auto cid = server->openConnection();

    WaitReply anon;
    server->handle(morph::wire::encode(morph::wire::makeRegister("CS_SquareModel")), std::ref(anon), cid);
    REQUIRE(anon.await());
    REQUIRE(anon.env.kind == "ok");

    morph::wire::Envelope assignReq = morph::wire::makeAssign("CS_SquareModel", "300", anon.env.modelId);
    assignReq.session.principal = "alice";
    WaitReply promoted;
    server->handle(morph::wire::encode(assignReq), std::ref(promoted), cid);
    REQUIRE(promoted.await());
    REQUIRE(promoted.env.kind == "ok");

    // The instance is filed under the key regardless of which authenticate()
    // branch stamped the principal -- assign's own authorization gate is
    // authorizeRegister, not ownership, so this is the same observable
    // outcome as the unauthenticated case; the point of this test is that the
    // authenticated branch runs at all, not a different result.
    WaitReply attached;
    server->handle(morph::wire::encode(morph::wire::makeRegisterShared("CS_SquareModel", "300")), std::ref(attached),
                   cid);
    REQUIRE(attached.await());
    REQUIRE(attached.env.modelId == anon.env.modelId);
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

TEST_CASE(
    "morph::backend::RemoteServer: two attaches racing the creation of the same not-yet-existing "
    "shared key still resolve to one instance",
    "[remote][connection-scope][shared-instances]") {
    // acquireSharedInstance's create path (remote.hpp) builds a holder
    // *outside* _regMtx, then re-checks the directory under the lock before
    // inserting -- because a concurrent request for the same key may have
    // already won that insert while this one's holder was under
    // construction. CsRaceModel's factory sleeps once (exchange-guarded), so
    // of two attaches fired back-to-back for the same brand-new key, the
    // first one dispatched is reliably the one still sleeping in
    // _registry.create() when the second (unslowed) one's own create/insert
    // completes -- forcing the first to find the directory already
    // populated on its own re-check, rather than hoping real thread
    // scheduling happens to interleave that way.
    CsRaceModel::slowFactoryTaken.store(false);
    morph::exec::ThreadPoolExecutor pool{2};
    auto& env = csEnv();
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, env.dispatcher, env.registry);

    WaitReply first;
    server->handle(morph::wire::encode(morph::wire::makeAttach("CS_RaceModel", "race-key")), std::ref(first));

    WaitReply second;
    server->handle(morph::wire::encode(morph::wire::makeAttach("CS_RaceModel", "race-key")), std::ref(second));

    REQUIRE(first.await(std::chrono::milliseconds{2000}));
    REQUIRE(second.await(std::chrono::milliseconds{2000}));
    REQUIRE(first.env.kind == "ok");
    REQUIRE(second.env.kind == "ok");

    // One instance, not two, regardless of which call's create() actually won
    // the race -- the load-bearing assertion this test exists for.
    REQUIRE(first.env.modelId == second.env.modelId);
    REQUIRE(server->health().liveModels == 1U);
}

TEST_CASE(
    "morph::backend::RemoteServer: maxLiveModels' authoritative re-test under the insert lock rejects a "
    "register the advisory pre-check let through",
    "[remote][connection-scope][limits]") {
    // The private register path checks maxLiveModels twice: an early,
    // advisory load (before authorize()/authenticate()/_registry.create()
    // run, so it can reject a request cheaply without paying for any of
    // that) and an authoritative re-test in the same locked section as the
    // actual insert. The comment right above that second check explains why
    // the first one alone is not a real bound: every concurrent register
    // that passes the advisory check while the server is still under cap
    // proceeds to authenticate/create, so a burst can overshoot the cap by
    // up to the worker pool's width -- exactly what the authoritative
    // re-test exists to catch. CsRaceModel's factory sleeps once
    // (exchange-guarded), so the first of two back-to-back registers is
    // reliably the one still in _registry.create() when the second's
    // fast create()+insert completes, forcing the first to find the cap
    // already reached at its own authoritative re-test.
    CsRaceModel::slowFactoryTaken.store(false);
    morph::exec::ThreadPoolExecutor pool{2};
    auto& env = csEnv();
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, env.dispatcher, env.registry);
    morph::backend::LimitPolicy policy;
    policy.maxLiveModels = 1;
    server->setLimitPolicy(policy);

    WaitReply first;
    server->handle(morph::wire::encode(morph::wire::makeRegister("CS_RaceModel")), std::ref(first));

    WaitReply second;
    server->handle(morph::wire::encode(morph::wire::makeRegister("CS_RaceModel")), std::ref(second));

    REQUIRE(first.await(std::chrono::milliseconds{2000}));
    REQUIRE(second.await(std::chrono::milliseconds{2000}));

    // Exactly one of the two must have won the single slot; the other must
    // have been rejected by the authoritative re-test, not silently admitted
    // past the cap.
    const bool firstOk = first.env.kind == "ok";
    const bool secondOk = second.env.kind == "ok";
    REQUIRE(firstOk != secondOk);
    const auto& loser = firstOk ? second : first;
    REQUIRE(loser.env.kind == "err");
    REQUIRE(loser.env.message == "too many models");
    REQUIRE(server->health().liveModels == 1U);
}

namespace {

/// @brief Allow-all authorizer whose `authorize()` sleeps once, for the first
///        call it sees -- every later call returns immediately. Same idiom as
///        test_remote_execute_ordering.cpp's SlowFirstAuthorizer, reused here
///        to force a different race: two executes reaching
///        dispatchExecute's maxInFlightExecutes compare-exchange loop
///        (remote.hpp) close enough together that the first one dispatched
///        is reliably still held up in authorize() when the second's own
///        pre-CAS work finishes and wins the increment.
class CsSlowFirstAuthorizer : public morph::session::IAuthorizer {
  public:
    [[nodiscard]] bool authorize(const morph::session::Context&, std::string_view, std::string_view) const override {
        if (!_slowCallTaken.exchange(true)) {
            std::this_thread::sleep_for(std::chrono::milliseconds{200});
        }
        return true;
    }

  private:
    mutable std::atomic<bool> _slowCallTaken{false};
};

}  // namespace

TEST_CASE(
    "morph::backend::RemoteServer: maxInFlightExecutes' compare-exchange loop rejects an execute that "
    "loses the race for the last slot",
    "[remote][connection-scope][limits]") {
    // Distinct from test_limit_policy.cpp's "rejects a second execute while
    // the first is in flight" case: that test deliberately waits for the
    // first execute to have already started (and therefore already
    // incremented _inFlightExecutes) before sending the second, so the
    // second is rejected by the plain load further up dispatchExecute, never
    // reaching the compare-exchange loop's own reject branch at all. This
    // test forces the two executes to race the increment itself.
    CsSlowModel::started.store(false);
    CsSlowModel::proceed.store(false);
    morph::exec::ThreadPoolExecutor pool{2};
    auto& env = csEnv();
    auto authorizer = std::make_shared<CsSlowFirstAuthorizer>();
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, authorizer, env.dispatcher, env.registry);
    morph::backend::LimitPolicy policy;
    policy.maxInFlightExecutes = 1;
    server->setLimitPolicy(policy);

    WaitReply reg;
    server->handle(morph::wire::encode(morph::wire::makeRegister("CS_SlowModel")), std::ref(reg));
    REQUIRE(reg.await());
    auto modelId = reg.env.modelId;

    // CS_SlowModel, not CS_SquareModel: the slot must still be held (i.e. the
    // winner's own decrement must not yet have run) by the time the loser's
    // CAS loop checks it -- an instantly-completing action can finish its
    // whole execute (including the decrement) before the other side ever
    // gets scheduled, which starves this race of the window it needs.
    morph::wire::Envelope reqA;
    reqA.kind = "execute";
    reqA.callId = 1;
    reqA.modelId = modelId;
    reqA.modelType = "CS_SlowModel";
    reqA.actionType = "CS_SlowAction";
    reqA.body = R"({})";
    WaitReply replyA;
    server->handle(morph::wire::encode(reqA), std::ref(replyA));

    morph::wire::Envelope reqB = reqA;
    reqB.callId = 2;
    WaitReply replyB;
    server->handle(morph::wire::encode(reqB), std::ref(replyB));

    // Whichever call wins the slot is now blocked inside CS_SlowModel::execute
    // until proceed is set, holding the slot open long enough for the loser's
    // CAS loop to observe it -- unlike the plain-CS_SquareModel version of
    // this test, which raced the decrement itself and was flaky (~40%
    // failure across repeated local runs) for exactly that reason.
    REQUIRE(morph::testing::waitUntil([] { return CsSlowModel::started.load(); }));

    // Exactly one of the two already has its reply: the CAS loop rejects
    // synchronously, before ever reaching the strand, so the loser's
    // WaitReply settles immediately -- well before the winner's, which is
    // still blocked in execute() until released below. Poll `.ready`, not
    // `.env` directly: `.env` is written by the reply callback on a pool
    // thread with no synchronization of its own beyond `.ready`'s
    // release-store/acquire-load pair (see WaitReply's own doc comment) --
    // reading `.env.kind` before observing `.ready == true` is a real data
    // race (caught by this file's own TSan CI leg the first time this test
    // was written this way).
    REQUIRE(morph::testing::waitUntil(
        [&] { return replyA.ready.load() || replyB.ready.load(); }, std::chrono::milliseconds{2000}));

    const bool aErr = replyA.ready.load() && replyA.env.kind == "err";
    const bool bErr = replyB.ready.load() && replyB.env.kind == "err";
    REQUIRE(aErr != bErr);
    const auto& loser = aErr ? replyA : replyB;
    REQUIRE(loser.env.message == "server busy");

    CsSlowModel::proceed.store(true);
    auto& winner = aErr ? replyB : replyA;
    REQUIRE(winner.await(std::chrono::milliseconds{2000}));
    REQUIRE(winner.env.kind == "ok");
}

TEST_CASE("morph::backend::RemoteServer: an action that throws still cancels its own executeTimeout, not just "
          "one that returns normally",
          "[remote][connection-scope][limits]") {
    // dispatchExecute's strand task cancels timeoutHandle in two places: the
    // try block's normal-completion path, and the catch block's
    // exception path. Every existing executeTimeout test in
    // test_limit_policy.cpp either lets the timeout actually fire (a slow
    // action outliving its budget) or lets a normal action complete well
    // inside it -- none combine a *throwing* action with executeTimeout
    // configured, so the catch block's own cancel() call had never run.
    // CsSquareFail (already registered in csEnv()) throws synchronously,
    // well inside any reasonable timeout.
    morph::exec::ThreadPoolExecutor pool{2};
    auto& env = csEnv();
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, env.dispatcher, env.registry);
    morph::backend::LimitPolicy policy;
    policy.executeTimeout = std::chrono::milliseconds{500};
    server->setLimitPolicy(policy);

    WaitReply reg;
    server->handle(morph::wire::encode(morph::wire::makeRegister("CS_SquareModel")), std::ref(reg));
    REQUIRE(reg.await());

    morph::wire::Envelope req;
    req.kind = "execute";
    req.callId = 1;
    req.modelId = reg.env.modelId;
    req.modelType = "CS_SquareModel";
    req.actionType = "CS_SquareFail";
    req.body = "{}";
    WaitReply reply;
    server->handle(morph::wire::encode(req), std::ref(reply));

    // The action throws immediately, well before the 500ms budget -- if this
    // reply is "err" with the action's own message (not "timeout"), the
    // catch block's cancel() ran and prevented the timeout from firing a
    // second, stale reply later.
    REQUIRE(reply.await(std::chrono::milliseconds{2000}));
    REQUIRE(reply.env.kind == "err");
    REQUIRE(reply.env.message == "square failed");

    // Waiting past the configured timeout confirms it was actually
    // cancelled, not merely that this reply beat it to the punch: a second,
    // stale "timeout" reply landing here (which WaitReply has no way to
    // observe, since it only keeps the first) would indicate the cancel
    // didn't take -- there is nothing further to assert beyond "the server
    // is still fine," which the next call demonstrates.
    std::this_thread::sleep_for(std::chrono::milliseconds{600});

    WaitReply again;
    server->handle(morph::wire::encode(morph::wire::makeRegister("CS_SquareModel")), std::ref(again));
    REQUIRE(again.await());
    REQUIRE(again.env.kind == "ok");
}
