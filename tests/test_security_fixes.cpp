// SPDX-License-Identifier: Apache-2.0
//
// Regression tests for a batch of memory-safety / security fixes:
//   FIX 1 — RemoteServer::dispatchExecute captures shared_from_this so the
//           server outlives the strand task (no UAF, reply never lost).
//   FIX 5 — a failed authenticate() clears the client-asserted principal so an
//           unauthenticated identity is never presented to the model.
// (FIX 2 / FIX 4 live in test_bridge_lifetime.cpp; FIX 3 is Qt-only.)

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <memory>
#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/core/wire.hpp>
#include <morph/session/session.hpp>
#include <morph/session/session_auth.hpp>
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
    [[nodiscard]] bool authorize(const morph::session::Context&, std::string_view, std::string_view) const override {
        return true;
    }
    // authenticate() inherits the default nullopt.
};

}  // namespace

// ── FIX 1 — server stays alive until the async strand reply fires ────────────

TEST_CASE(
    "RemoteServer::dispatchExecute: reply is delivered after the last external "
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

// ── One token verification per execute ───────────────────────────────────────

namespace {

/// A clock that counts its reads: `SigningAuthorizer` reads it once per token
/// verification. Reads up to `validReads` see a time before the tokens'
/// expiry at 2000, every later read one after it. Shared state, because the
/// authorizer keeps a copy.
struct CountingClock {
    std::shared_ptr<std::atomic<int>> reads = std::make_shared<std::atomic<int>>(0);
    std::shared_ptr<std::atomic<int>> validReads = std::make_shared<std::atomic<int>>(1'000'000);

    int64_t operator()() const {
        int const read = reads->fetch_add(1) + 1;
        return read <= validReads->load() ? int64_t{1'000} : int64_t{3'000};
    }
};

morph::wire::Envelope echoExecute(std::uint64_t modelId, std::string token) {
    morph::wire::Envelope req;
    req.kind = "execute";
    req.modelId = modelId;
    req.modelType = "SEC_EchoModel";
    req.actionType = "SEC_Echo";
    req.body = "{}";
    req.session.principal = "spoofed";
    req.session.token = std::move(token);
    return req;
}

}  // namespace

TEST_CASE("RemoteServer::dispatchExecute verifies the session token once", "[remote][auth][security]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto registry = makeEchoRegistry();
    auto dispatcher = makeEchoDispatcher();
    const std::string secret = "sec-once-key";
    CountingClock const clock;
    auto const authz = std::make_shared<morph::session::SigningAuthorizer>(secret, morph::session::hmacSha256, clock);
    auto const server = std::make_shared<morph::backend::RemoteServer>(pool, authz, dispatcher, registry);

    morph::testing::WaitReply reg;
    server->handle(morph::wire::encode(morph::wire::makeRegister("SEC_EchoModel")), std::ref(reg));
    REQUIRE(reg.await());
    REQUIRE(reg.env.kind == "ok");

    auto const token = morph::session::TokenIssuer{secret}.issue(
        morph::session::SessionToken{.principal = "verified-user", .expiresAtMs = 2'000, .roles = {}});
    int const before = clock.reads->load();
    morph::testing::WaitReply waiter;
    server->handle(morph::wire::encode(echoExecute(reg.env.modelId, token)), std::ref(waiter));
    REQUIRE(waiter.await());
    REQUIRE(waiter.env.kind == "ok");
    CHECK(waiter.env.body == "\"verified-user\"");
    CHECK(clock.reads->load() - before == 1);
}

TEST_CASE("RemoteServer::dispatchExecute: a token that expires during the call is judged once",
          "[remote][auth][security]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto registry = makeEchoRegistry();
    auto dispatcher = makeEchoDispatcher();
    const std::string secret = "sec-expiry-key";
    CountingClock const clock;
    auto const authz = std::make_shared<morph::session::SigningAuthorizer>(secret, morph::session::hmacSha256, clock);
    auto const server = std::make_shared<morph::backend::RemoteServer>(pool, authz, dispatcher, registry);

    morph::testing::WaitReply reg;
    server->handle(morph::wire::encode(morph::wire::makeRegister("SEC_EchoModel")), std::ref(reg));
    REQUIRE(reg.await());
    REQUIRE(reg.env.kind == "ok");

    // The token is valid at the execute's first clock read and expired at any
    // later one: the call is admitted and runs under the verified principal,
    // both decisions taken from the one verification.
    clock.validReads->store(clock.reads->load() + 1);
    auto const token = morph::session::TokenIssuer{secret}.issue(
        morph::session::SessionToken{.principal = "verified-user", .expiresAtMs = 2'000, .roles = {}});
    morph::testing::WaitReply waiter;
    server->handle(morph::wire::encode(echoExecute(reg.env.modelId, token)), std::ref(waiter));
    REQUIRE(waiter.await());
    REQUIRE(waiter.env.kind == "ok");
    CHECK(waiter.env.body == "\"verified-user\"");
}

namespace {

/// A `SigningAuthorizer` with its own rules over the base one: `SEC_Open`
/// needs no token, and `SEC_Closed` is refused whatever the token. Everything
/// else is the base rule.
struct RuledAuthorizer final : morph::session::SigningAuthorizer {
    using SigningAuthorizer::SigningAuthorizer;

    [[nodiscard]] bool authorize(const morph::session::Context& ctx,
                                 // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
                                 std::string_view modelType, std::string_view actionType) const override {
        if (actionType == "SEC_Open") {
            return true;
        }
        if (actionType == "SEC_Closed") {
            return false;
        }
        return SigningAuthorizer::authorize(ctx, modelType, actionType);
    }
};

}  // namespace

TEST_CASE("SigningAuthorizer::admit keeps a subclass's own authorize decisions", "[auth][security]") {
    const std::string secret = "sec-ruled-key";
    CountingClock const clock;
    RuledAuthorizer const authz{secret, morph::session::hmacSha256, clock};
    morph::session::Context withToken;
    withToken.token = morph::session::TokenIssuer{secret}.issue(
        morph::session::SessionToken{.principal = "verified-user", .expiresAtMs = 2'000, .roles = {}});
    morph::session::Context const withoutToken;

    // The carve-out admits a caller with no token, who has no principal.
    auto const open = authz.admit(withoutToken, "SEC_EchoModel", "SEC_Open");
    CHECK(open.allowed);
    CHECK_FALSE(open.principal.has_value());

    // The stricter rule refuses a valid token the base rule would admit.
    auto const closed = authz.admit(withToken, "SEC_EchoModel", "SEC_Closed");
    CHECK_FALSE(closed.allowed);
    CHECK(closed.principal == "verified-user");

    // Delegating to the base rule still verifies the token once.
    int const before = clock.reads->load();
    auto const delegated = authz.admit(withToken, "SEC_EchoModel", "SEC_Echo");
    CHECK(delegated.allowed);
    CHECK(delegated.principal == "verified-user");
    CHECK(clock.reads->load() - before == 1);

    // Outside admit, each call verifies on its own.
    int const outside = clock.reads->load();
    CHECK(authz.authorize(withToken, "SEC_EchoModel", "SEC_Echo"));
    CHECK(authz.authenticate(withToken) == "verified-user");
    CHECK(clock.reads->load() - outside == 2);
}
