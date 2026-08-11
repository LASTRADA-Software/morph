// SPDX-License-Identifier: Apache-2.0
//
// Reproduces and verifies the fix for #63: register/attach/assign/deregister
// envelopes built by the wire-backed IBackend implementations (SimulatedRemoteBackend,
// SocketBackend) never carried Bridge::defaultSession(), so a RemoteServer whose
// authorizer requires authentication rejected every register — and, even under an
// allow-all authorizer, the recorded owner principal was always empty, degrading
// authorizeInstance's ownership check to allow-all. See docs/spec/session/session.md
// ("How a Context originates and flows") and docs/spec/core/backend.md ("IBackend").

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/session/session.hpp>
#include <morph/session/session_auth.hpp>
#include <stdexcept>
#include <string>
#include <thread>

#include "test_support.hpp"

using SyncExecutor = morph::testing::InlineExecutor;

struct RegSessAction {
    int value = 0;
};
struct RegSessModel {
    int execute(const RegSessAction& act) { return act.value + 1; }
};

BRIDGE_REGISTER_MODEL(RegSessModel, "RS_Model")
BRIDGE_REGISTER_ACTION(RegSessModel, RegSessAction, "RS_Action")

namespace {

// Requires authentication (a validly-signed token) before allowing register —
// exactly the RegisterGate example in docs/spec/session/session.md.
struct RegisterRequiresAuth : morph::session::SigningAuthorizer {
    using SigningAuthorizer::SigningAuthorizer;
    [[nodiscard]] bool authorizeRegister(const morph::session::Context& ctx, std::string_view) const override {
        return !ctx.principal.empty();
    }
};

}  // namespace

TEST_CASE("Bridge::setDefaultSession's token reaches the register envelope so authorizeRegister can allow it",
          "[bridge][remote][session]") {
    morph::exec::ThreadPoolExecutor serverPool{2};
    const std::string secret = "reg-session-secret";
    auto authz = std::make_shared<RegisterRequiresAuth>(secret);
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool, authz);

    SyncExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::SimulatedRemoteBackend>(*server)};

    morph::session::Context session;
    session.principal = "alice";
    session.token = morph::session::TokenIssuer{secret}.issue({.principal = "alice", .expiresAtMs = 9999999999999});
    bridge.setDefaultSession(session);

    // Before the fix, registerModelWithContext (called from BridgeHandler's
    // constructor) sent a register envelope with a default-constructed
    // (empty) session, so RegisterRequiresAuth::authorizeRegister denied it
    // and this constructor threw "register failed: unauthorized".
    morph::bridge::BridgeHandler<RegSessModel> handler{bridge, &cbExec};

    std::atomic<int> result{-1};
    handler.execute(RegSessAction{41}).then([&](int val) { result.store(val); }).onError([](const std::exception_ptr&) {
    });

    for (int idx = 0; idx < 50 && result.load() == -1; ++idx) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(result.load() == 42);
}

namespace {

// Ownership-enforcing authorizer, as in tests/test_policy_hardening.cpp: the
// principal recorded at register time must match the principal executing.
struct OwnershipAuthz : morph::session::SigningAuthorizer {
    using SigningAuthorizer::SigningAuthorizer;
    [[nodiscard]] bool authorizeInstance(const morph::session::Context& ctx, std::string_view, std::string_view,
                                         std::uint64_t, std::string_view ownerPrincipal) const override {
        return ownerPrincipal.empty() || ownerPrincipal == ctx.principal;
    }
};

}  // namespace

TEST_CASE("register's recorded owner principal is the Bridge's authenticated default session, not always empty",
          "[bridge][remote][session]") {
    morph::exec::ThreadPoolExecutor serverPool{2};
    const std::string secret = "owner-session-secret";
    auto authz = std::make_shared<OwnershipAuthz>(secret);
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool, authz);

    SyncExecutor cbExec;
    morph::bridge::Bridge aliceBridge{std::make_unique<morph::backend::SimulatedRemoteBackend>(*server)};
    morph::session::Context aliceSession;
    aliceSession.principal = "alice";
    aliceSession.token =
        morph::session::TokenIssuer{secret}.issue({.principal = "alice", .expiresAtMs = 9999999999999});
    aliceBridge.setDefaultSession(aliceSession);

    // Registers on behalf of "alice" — before the fix, the owner recorded on
    // the server was always empty (the envelope's session was never set),
    // which degraded authorizeInstance's ownership check to allow-all.
    morph::bridge::BridgeHandler<RegSessModel> aliceHandler{aliceBridge, &cbExec};

    std::atomic<int> aliceResult{-1};
    aliceHandler.execute(RegSessAction{1})
        .then([&](int val) { aliceResult.store(val); })
        .onError([](const std::exception_ptr&) {});
    for (int idx = 0; idx < 50 && aliceResult.load() == -1; ++idx) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(aliceResult.load() == 2);

    // Bob (a different, also-valid principal) attempts to execute against the
    // *same* model id alice's handler was bound to. Before the fix this would
    // succeed (owner was empty => allow-all); after the fix it is denied.
    morph::bridge::Bridge bobBridge{std::make_unique<morph::backend::SimulatedRemoteBackend>(*server)};
    morph::session::Context bobSession;
    bobSession.principal = "bob";
    bobSession.token = morph::session::TokenIssuer{secret}.issue({.principal = "bob", .expiresAtMs = 9999999999999});
    bobBridge.setDefaultSession(bobSession);

    // Build a raw envelope targeting alice's modelId directly (bypassing a
    // second registration) so we exercise authorizeInstance on the recorded owner.
    auto probe = morph::wire::Envelope{};
    probe.kind = "execute";
    probe.modelId = aliceHandler.binding()->currentId.load();
    probe.modelType = "RS_Model";
    probe.actionType = "RS_Action";
    probe.body = R"({"value":1})";
    probe.session = bobSession;
    morph::testing::WaitReply bobReply;
    server->handle(morph::wire::encode(probe), std::ref(bobReply));
    REQUIRE(bobReply.await());
    REQUIRE(bobReply.env.kind == "err");
    REQUIRE(bobReply.env.message == "unauthorized");
}
