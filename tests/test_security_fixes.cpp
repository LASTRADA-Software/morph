// SPDX-License-Identifier: Apache-2.0
//
// Regression tests for a batch of memory-safety / security fixes:
//   FIX 1 — RemoteServer::dispatchExecute captures shared_from_this so the
//           server outlives the strand task (no UAF, reply never lost).
//   FIX 5 — a failed authenticate() clears the client-asserted principal so an
//           unauthenticated identity is never presented to the model.
// (FIX 2 / FIX 4 live in test_bridge_lifetime.cpp; FIX 3 is Qt-only.)

#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/session/session.hpp>
#include <morph/session/session_auth.hpp>
#include <morph/core/wire.hpp>

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "test_support.hpp"

namespace {

struct SecEcho {};
struct SecEchoModel {
    std::string execute(const SecEcho&) const {
        const auto* ctx = morph::session::current();
        return (ctx != nullptr) ? ctx->principal : std::string{};
    }
};

}  // namespace

template <>
struct morph::model::ModelTraits<SecEchoModel> {
    static constexpr std::string_view typeId() { return "SEC_EchoModel"; }
};
template <>
struct morph::model::ActionTraits<SecEcho> {
    using Result = std::string;
    static constexpr std::string_view typeId() { return "SEC_Echo"; }
    [[maybe_unused]] static std::string toJson(const SecEcho&) { return "{}"; }
    static SecEcho fromJson(std::string_view) { return {}; }
    static std::string resultToJson(const std::string& res) { return "\"" + res + "\""; }
    [[maybe_unused]] static std::string resultFromJson(std::string_view json) {
        std::string out;
        (void)glz::read_json(out, json);
        return out;
    }
};

namespace {

morph::model::detail::ModelRegistryFactory makeEchoRegistry() {
    morph::model::detail::ModelRegistryFactory registry;
    registry.registerModel<SecEchoModel>("SEC_EchoModel");
    return registry;
}
morph::model::detail::ActionDispatcher makeEchoDispatcher() {
    morph::model::detail::ActionDispatcher dispatcher;
    dispatcher.registerAction<SecEchoModel, SecEcho>("SEC_EchoModel", "SEC_Echo");
    return dispatcher;
}

// An authorizer that ALLOWS the call but never authenticates (returns nullopt).
// Models the TOCTOU divergence and the authorize-only passthrough: authorize
// admits the request but authenticate cannot vouch for the principal.
struct AllowButNoAuthAuthorizer : morph::session::IAuthorizer {
    [[nodiscard]] bool authorize(const morph::session::Context&, std::string_view,
                                 std::string_view) const override {
        return true;
    }
    // authenticate() inherits the default nullopt.
};

}  // namespace

// ── FIX 1 — server stays alive until the async strand reply fires ────────────

TEST_CASE("RemoteServer::dispatchExecute: reply is delivered after the last external "
          "shared_ptr is dropped",
          "[remote][lifetime]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto registry = makeEchoRegistry();
    auto dispatcher = makeEchoDispatcher();
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, dispatcher, registry);
    std::weak_ptr<morph::backend::RemoteServer> weak = server;

    morph::testing::WaitReply reg;
    server->handle(morph::wire::encode(morph::wire::makeRegister("SEC_EchoModel")), std::ref(reg));
    REQUIRE(reg.await());
    REQUIRE(reg.env.kind == "ok");

    morph::wire::Envelope req;
    req.kind = "execute";
    req.callId = 11;
    req.modelId = reg.env.modelId;
    req.modelType = "SEC_EchoModel";
    req.actionType = "SEC_Echo";
    req.body = "{}";

    morph::testing::WaitReply waiter;
    server->handle(morph::wire::encode(req), std::ref(waiter));

    // Drop the only external strong reference right after handle() returns. The
    // execute task is (or is about to be) queued on the strand; if the strand
    // task did not co-own the server, this would free _dispatcher out from under
    // it (UAF) or drop the reply. With the self-capture fix the reply arrives.
    server.reset();

    // The reply must still be delivered (callId echoed, kind == ok) even though
    // the only external shared_ptr is gone — proving the strand task kept the
    // server alive. (The echoed principal is empty: allow-all authenticate()
    // returns nullopt, so FIX 5 clears it. This test cares only about delivery.)
    REQUIRE(waiter.await());
    REQUIRE(waiter.env.kind == "ok");
    REQUIRE(waiter.env.callId == 11U);

    // Once the reply has fired and the strand task has released its self-capture,
    // the server is destroyed (no leak, no lingering strong ref).
    REQUIRE(morph::testing::waitUntil([&] { return weak.expired(); }));
}

// ── FIX 5 — authenticate()==nullopt clears the client-asserted principal ─────

TEST_CASE("RemoteServer::dispatchExecute: authorize-only authorizer clears the client principal",
          "[remote][auth][security]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto registry = makeEchoRegistry();
    auto dispatcher = makeEchoDispatcher();
    auto authz = std::make_shared<AllowButNoAuthAuthorizer>();
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, authz, dispatcher, registry);

    morph::testing::WaitReply reg;
    server->handle(morph::wire::encode(morph::wire::makeRegister("SEC_EchoModel")), std::ref(reg));
    REQUIRE(reg.await());
    REQUIRE(reg.env.kind == "ok");

    morph::wire::Envelope req;
    req.kind = "execute";
    req.modelId = reg.env.modelId;
    req.modelType = "SEC_EchoModel";
    req.actionType = "SEC_Echo";
    req.body = "{}";
    req.session.principal = "attacker-chosen";  // unverified client claim

    morph::testing::WaitReply waiter;
    server->handle(morph::wire::encode(req), std::ref(waiter));
    REQUIRE(waiter.await());
    REQUIRE(waiter.env.kind == "ok");
    // The model must NOT see the attacker's claim; the principal is cleared
    // because authenticate() could not vouch for it.
    REQUIRE(waiter.env.body == "\"\"");
}

TEST_CASE("RemoteServer::dispatchExecute: allow-all authorizer clears the client principal",
          "[remote][auth][security]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto registry = makeEchoRegistry();
    auto dispatcher = makeEchoDispatcher();
    // Default constructor installs allowAllAuthorizer().
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, dispatcher, registry);

    morph::testing::WaitReply reg;
    server->handle(morph::wire::encode(morph::wire::makeRegister("SEC_EchoModel")), std::ref(reg));
    REQUIRE(reg.await());
    REQUIRE(reg.env.kind == "ok");

    morph::wire::Envelope req;
    req.kind = "execute";
    req.modelId = reg.env.modelId;
    req.modelType = "SEC_EchoModel";
    req.actionType = "SEC_Echo";
    req.body = "{}";
    req.session.principal = "unverified";

    morph::testing::WaitReply waiter;
    server->handle(morph::wire::encode(req), std::ref(waiter));
    REQUIRE(waiter.await());
    REQUIRE(waiter.env.kind == "ok");
    REQUIRE(waiter.env.body == "\"\"");
}

TEST_CASE("RemoteServer::dispatchExecute: verifying authorizer still stamps the verified principal",
          "[remote][auth][security]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto registry = makeEchoRegistry();
    auto dispatcher = makeEchoDispatcher();
    const std::string secret = "sec-fix-key";
    auto authz = std::make_shared<morph::session::SigningAuthorizer>(secret);
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, authz, dispatcher, registry);

    morph::testing::WaitReply reg;
    server->handle(morph::wire::encode(morph::wire::makeRegister("SEC_EchoModel")), std::ref(reg));
    REQUIRE(reg.await());
    REQUIRE(reg.env.kind == "ok");

    morph::wire::Envelope req;
    req.kind = "execute";
    req.modelId = reg.env.modelId;
    req.modelType = "SEC_EchoModel";
    req.actionType = "SEC_Echo";
    req.body = "{}";
    req.session.principal = "spoofed";
    // expiresAtMs must be strictly positive — a zero expiry is now treated as
    // already-expired (never eternal), so mint with a far-future real expiry.
    req.session.token = morph::session::TokenIssuer{secret}.issue(
        morph::session::SessionToken{.principal = "verified-user", .expiresAtMs = 9'999'999'999'999, .roles = {}});

    morph::testing::WaitReply waiter;
    server->handle(morph::wire::encode(req), std::ref(waiter));
    REQUIRE(waiter.await());
    REQUIRE(waiter.env.kind == "ok");
    REQUIRE(waiter.env.body == "\"verified-user\"");
}
