// SPDX-License-Identifier: Apache-2.0
//
// Tests for the SECURITY/POLICY hardening fixes:
//   1. Token expiry-0 is treated as expired (never eternal).
//   2. Not-before / issued-at check.
//   3. base64url canonical-encoding rejection (signature malleability).
//   4. Per-instance ownership authorization (cross-tenant modelId targeting).
//   5. Offline-queue idempotency key.
//   6. UnitRelation positivity guard (compile-time; witnessed here at runtime).
//   7. Log-injection sanitisation in the default sink.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/logger.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/core/wire.hpp>
#include <morph/offline/offline_queue.hpp>
#include <morph/session/session.hpp>
#include <morph/session/session_auth.hpp>
#include <morph/util/quantity.hpp>
#include <morph/util/rational.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "test_support.hpp"

using morph::session::AuthError;
using morph::session::hmacSha256;
using morph::session::SessionToken;
using morph::session::TokenIssuer;
using morph::session::TokenVerifier;

namespace {

constexpr int64_t kNow = 1'700'000'000'000;  // ms since epoch

SessionToken validClaims(std::string principal) {
    return SessionToken{
        .principal = std::move(principal), .issuedAtMs = kNow, .expiresAtMs = kNow + 60'000, .roles = {}};
}

}  // namespace

// ── Item 1: expiry-0 is expired, not eternal ─────────────────────────────────

TEST_CASE("a token with expiresAtMs == 0 is rejected as expired (not eternal)", "[policy][session_auth]") {
    const std::string secret = "s";
    // Mint a token whose expiry is left at the default 0.
    SessionToken claims;
    claims.principal = "eve";
    claims.issuedAtMs = 0;
    claims.expiresAtMs = 0;  // the dangerous "never expires" value
    const std::string token = TokenIssuer{secret}.issue(claims);

    const auto res = TokenVerifier{secret}.verify(token, kNow);
    REQUIRE_FALSE(res.has_value());
    REQUIRE(res.error() == AuthError::Expired);
}

TEST_CASE("a token with a negative expiresAtMs is rejected as expired", "[policy][session_auth]") {
    const std::string secret = "s";
    SessionToken claims;
    claims.principal = "eve";
    claims.expiresAtMs = -1;
    const std::string token = TokenIssuer{secret}.issue(claims);

    const auto res = TokenVerifier{secret}.verify(token, kNow);
    REQUIRE_FALSE(res.has_value());
    REQUIRE(res.error() == AuthError::Expired);
}

TEST_CASE("a token with a real positive expiry still verifies", "[policy][session_auth]") {
    const std::string secret = "s";
    const std::string token = TokenIssuer{secret}.issue(validClaims("alice"));
    const auto res = TokenVerifier{secret}.verify(token, kNow);
    REQUIRE(res.has_value());
    REQUIRE(res.value().principal == "alice");
}

// ── Item 2: not-before / issued-at ───────────────────────────────────────────

TEST_CASE("a token issued far in the future is rejected as not-yet-valid", "[policy][session_auth]") {
    const std::string secret = "s";
    SessionToken claims;
    claims.principal = "alice";
    claims.issuedAtMs = kNow + 10 * 60'000;   // 10 min in the future
    claims.expiresAtMs = kNow + 20 * 60'000;  // still unexpired
    const std::string token = TokenIssuer{secret}.issue(claims);

    const auto res = TokenVerifier{secret}.verify(token, kNow);
    REQUIRE_FALSE(res.has_value());
    REQUIRE(res.error() == AuthError::NotYetValid);
}

TEST_CASE("a token issued within the clock-skew tolerance is accepted", "[policy][session_auth]") {
    const std::string secret = "s";
    SessionToken claims;
    claims.principal = "alice";
    claims.issuedAtMs = kNow + 5'000;  // 5s ahead, within kClockSkewMs (60s)
    claims.expiresAtMs = kNow + 60'000;
    const std::string token = TokenIssuer{secret}.issue(claims);

    const auto res = TokenVerifier{secret}.verify(token, kNow);
    REQUIRE(res.has_value());
}

TEST_CASE("an unset issuedAtMs (0) disables the not-before check", "[policy][session_auth]") {
    const std::string secret = "s";
    SessionToken claims;
    claims.principal = "alice";
    claims.issuedAtMs = 0;  // unset
    claims.expiresAtMs = kNow + 60'000;
    const std::string token = TokenIssuer{secret}.issue(claims);

    const auto res = TokenVerifier{secret}.verify(token, kNow);
    REQUIRE(res.has_value());
}

// ── Item 3: base64url canonical / malleability ───────────────────────────────

TEST_CASE("base64UrlDecode rejects a length that is impossible (% 4 == 1)", "[policy][session_auth]") {
    // 5 symbols → 4 (one group) + 1 leftover: not producible by the encoder.
    REQUIRE_FALSE(morph::session::detail::base64UrlDecode("AAAAA").has_value());
}

TEST_CASE("base64UrlDecode rejects non-canonical trailing bits", "[policy][session_auth]") {
    // Two symbols encode one byte; the low 4 bits of the 2nd symbol are padding
    // and must be zero. "AA" (0,0) decodes to a single 0x00 byte canonically.
    REQUIRE(morph::session::detail::base64UrlDecode("AA").has_value());
    // "AB" has value (0, 1): the leftover 4 bits are 0b0001 != 0 → non-canonical.
    REQUIRE_FALSE(morph::session::detail::base64UrlDecode("AB").has_value());
}

TEST_CASE("a token whose signature trailing char is mutated (non-canonical) is rejected", "[policy][session_auth]") {
    const std::string secret = "top-secret";
    std::string token = TokenIssuer{secret}.issue(validClaims("bob"));
    // Mutate the final signature character to a symbol that keeps the same
    // decoded prefix bytes but flips only the discarded low bits. We search for
    // a replacement that produces a *different* base64url symbol; a canonical
    // decoder rejects it as non-canonical (leftover bits nonzero) or the MAC
    // simply mismatches — either way verification must fail.
    const char last = token.back();
    token.back() = (last == 'A') ? 'B' : 'A';
    const auto res = TokenVerifier{secret}.verify(token, kNow);
    REQUIRE_FALSE(res.has_value());
}

// ── Item 4: per-instance ownership authorization ─────────────────────────────

// A minimal authorizer that treats ctx.principal as the authenticated identity
// (as if a token were already verified) and enforces per-instance ownership:
// the recorded owner must match the caller's principal.
struct OwnershipAuthorizer : morph::session::IAuthorizer {
    [[nodiscard]] bool authorize(const morph::session::Context&, std::string_view, std::string_view) const override {
        return true;  // type-level: allow; ownership is enforced per instance
    }
    [[nodiscard]] std::optional<std::string> authenticate(const morph::session::Context& ctx) const override {
        if (ctx.principal.empty()) {
            return std::nullopt;
        }
        return ctx.principal;  // stand-in for a verified principal
    }
    [[nodiscard]] bool authorizeInstance(const morph::session::Context& ctx, std::string_view, std::string_view,
                                         std::uint64_t, std::string_view ownerPrincipal) const override {
        // Unowned instances are open; owned instances only to their owner.
        return ownerPrincipal.empty() || ownerPrincipal == ctx.principal;
    }
};

// Model/action types must have external linkage for glaze reflection, so they
// live at namespace scope (not in an anonymous namespace).
struct OwnedAction {
    int x = 0;
};
struct OwnedModel {
    int execute(const OwnedAction& act) { return act.x + 1; }
};

namespace {

morph::wire::Envelope registerAs(std::string principal) {
    auto env = morph::wire::makeRegister("POL_OwnedModel");
    env.session.principal = std::move(principal);
    return env;
}

morph::wire::Envelope executeAs(std::uint64_t modelId, std::string principal) {
    morph::wire::Envelope env;
    env.kind = "execute";
    env.modelId = modelId;
    env.modelType = "POL_OwnedModel";
    env.actionType = "POL_OwnedAction";
    env.body = R"({"x":10})";
    env.session.principal = std::move(principal);
    return env;
}

struct PolEnv {
    morph::model::detail::ActionDispatcher dispatcher;
    morph::model::detail::ModelRegistryFactory registry;
    PolEnv() {
        registry.registerModel<OwnedModel>("POL_OwnedModel");
        dispatcher.registerAction<OwnedModel, OwnedAction>("POL_OwnedModel", "POL_OwnedAction");
    }
};

}  // namespace

template <>
struct morph::model::ModelTraits<OwnedModel> {
    static constexpr std::string_view typeId() { return "POL_OwnedModel"; }
};
template <>
struct morph::model::ActionTraits<OwnedAction> {
    using Result = int;
    static constexpr std::string_view typeId() { return "POL_OwnedAction"; }
    static std::string toJson(const OwnedAction& act) {
        std::string out;
        (void)glz::write_json(act, out);
        return out;
    }
    static OwnedAction fromJson(std::string_view json) {
        OwnedAction action{};
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

TEST_CASE("with an ownership authorizer, principal B cannot execute principal A's instance", "[policy][remote]") {
    morph::testing::InlineExecutor pool;
    PolEnv env;
    auto authz = std::make_shared<OwnershipAuthorizer>();
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, authz, env.dispatcher, env.registry);

    // Alice registers an instance.
    morph::testing::WaitReply reg;
    server->handle(morph::wire::encode(registerAs("alice")), std::ref(reg));
    REQUIRE(reg.await());
    REQUIRE(reg.env.kind == "ok");
    const auto mid = reg.env.modelId;

    // Alice (the owner) can execute.
    morph::testing::WaitReply okRun;
    server->handle(morph::wire::encode(executeAs(mid, "alice")), std::ref(okRun));
    REQUIRE(okRun.await());
    REQUIRE(okRun.env.kind == "ok");

    // Bob cannot execute Alice's instance.
    morph::testing::WaitReply denied;
    server->handle(morph::wire::encode(executeAs(mid, "bob")), std::ref(denied));
    REQUIRE(denied.await());
    REQUIRE(denied.env.kind == "err");
    REQUIRE(denied.env.message == "unauthorized");
}

TEST_CASE("with an ownership authorizer, principal B cannot deregister principal A's instance", "[policy][remote]") {
    morph::testing::InlineExecutor pool;
    PolEnv env;
    auto authz = std::make_shared<OwnershipAuthorizer>();
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, authz, env.dispatcher, env.registry);

    morph::testing::WaitReply reg;
    server->handle(morph::wire::encode(registerAs("alice")), std::ref(reg));
    REQUIRE(reg.await());
    const auto mid = reg.env.modelId;

    // Bob's deregister is rejected.
    auto bobDereg = morph::wire::makeDeregister(mid);
    bobDereg.session.principal = "bob";
    morph::testing::WaitReply denied;
    server->handle(morph::wire::encode(bobDereg), std::ref(denied));
    REQUIRE(denied.await());
    REQUIRE(denied.env.kind == "err");
    REQUIRE(denied.env.message == "unauthorized");

    // The instance is still alive: Alice can still execute it.
    morph::testing::WaitReply stillThere;
    server->handle(morph::wire::encode(executeAs(mid, "alice")), std::ref(stillThere));
    REQUIRE(stillThere.await());
    REQUIRE(stillThere.env.kind == "ok");

    // Alice (the owner) can deregister.
    auto aliceDereg = morph::wire::makeDeregister(mid);
    aliceDereg.session.principal = "alice";
    morph::testing::WaitReply okDereg;
    server->handle(morph::wire::encode(aliceDereg), std::ref(okDereg));
    REQUIRE(okDereg.await());
    REQUIRE(okDereg.env.kind == "ok");
}

namespace {

// Derives the principal from the TOKEN alone and never reads ctx.principal, so
// a claimed principal that disagrees with the token is visible: the verified
// identity is whatever the token says, and only the server's stamp can make the
// ownership check see it. Type-level `authorize` admits exactly the callers it
// can authenticate.
struct TokenOwnershipAuthorizer : morph::session::IAuthorizer {
    [[nodiscard]] static std::optional<std::string> principalFor(std::string_view token) {
        if (token == "tok-alice") {
            return std::string{"alice"};
        }
        if (token == "tok-bob") {
            return std::string{"bob"};
        }
        return std::nullopt;
    }
    [[nodiscard]] bool authorize(const morph::session::Context& ctx, std::string_view,
                                 std::string_view) const override {
        return principalFor(ctx.token).has_value();
    }
    [[nodiscard]] std::optional<std::string> authenticate(const morph::session::Context& ctx) const override {
        return principalFor(ctx.token);
    }
    [[nodiscard]] bool authorizeInstance(const morph::session::Context& ctx, std::string_view, std::string_view,
                                         std::uint64_t, std::string_view ownerPrincipal) const override {
        return ownerPrincipal.empty() || ownerPrincipal == ctx.principal;
    }
};

morph::wire::Envelope withSession(morph::wire::Envelope env, std::string token, std::string claimedPrincipal) {
    env.session.token = std::move(token);
    env.session.principal = std::move(claimedPrincipal);
    return env;
}

morph::wire::Envelope roundTrip(morph::backend::RemoteServer& server, const morph::wire::Envelope& env) {
    morph::testing::WaitReply reply;
    server.handle(morph::wire::encode(env), std::ref(reply));
    REQUIRE(reply.await());
    return reply.env;
}

}  // namespace

TEST_CASE("deregister keys ownership on the verified principal, not the claimed one", "[policy][remote]") {
    morph::testing::InlineExecutor pool;
    PolEnv env;
    auto authz = std::make_shared<TokenOwnershipAuthorizer>();
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, authz, env.dispatcher, env.registry);

    const auto reg = roundTrip(*server, withSession(registerAs("alice"), "tok-alice", "alice"));
    REQUIRE(reg.kind == "ok");
    const auto mid = reg.modelId;

    SECTION("bob's token claiming to be alice is refused, and alice's instance survives") {
        const auto forged = roundTrip(*server, withSession(morph::wire::makeDeregister(mid), "tok-bob", "alice"));
        CHECK(forged.kind == "err");
        CHECK(forged.message == "unauthorized");

        const auto aliceRuns = roundTrip(*server, withSession(executeAs(mid, "alice"), "tok-alice", "alice"));
        REQUIRE(aliceRuns.kind == "ok");
    }

    SECTION("control: bob's token with an honest claim is refused") {
        const auto honest = roundTrip(*server, withSession(morph::wire::makeDeregister(mid), "tok-bob", "bob"));
        CHECK(honest.kind == "err");
        CHECK(honest.message == "unauthorized");

        const auto aliceRuns = roundTrip(*server, withSession(executeAs(mid, "alice"), "tok-alice", "alice"));
        REQUIRE(aliceRuns.kind == "ok");
    }

    SECTION("control: alice's own token releases it, whatever she claims") {
        const auto own = roundTrip(*server, withSession(morph::wire::makeDeregister(mid), "tok-alice", "mallory"));
        REQUIRE(own.kind == "ok");

        const auto gone = roundTrip(*server, withSession(executeAs(mid, "alice"), "tok-alice", "alice"));
        CHECK(gone.kind == "err");
    }
}

TEST_CASE("deregister is gated by type-level authorize before the ownership check", "[policy][remote]") {
    morph::testing::InlineExecutor pool;
    PolEnv env;
    auto authz = std::make_shared<TokenOwnershipAuthorizer>();
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, authz, env.dispatcher, env.registry);

    // Registered without a token, so it is unowned: authorizeInstance admits
    // everyone, and only `authorize` stands between a tokenless caller and it.
    const auto reg = roundTrip(*server, withSession(registerAs({}), {}, {}));
    REQUIRE(reg.kind == "ok");
    const auto mid = reg.modelId;

    const auto tokenless = roundTrip(*server, withSession(morph::wire::makeDeregister(mid), {}, "alice"));
    CHECK(tokenless.kind == "err");
    CHECK(tokenless.message == "unauthorized");

    const auto stillThere = roundTrip(*server, withSession(executeAs(mid, {}), "tok-bob", "bob"));
    REQUIRE(stillThere.kind == "ok");

    const auto withToken = roundTrip(*server, withSession(morph::wire::makeDeregister(mid), "tok-bob", "bob"));
    REQUIRE(withToken.kind == "ok");
}

TEST_CASE("without an ownership authorizer the default allows any principal (backward compatible)",
          "[policy][remote]") {
    morph::testing::InlineExecutor pool;
    PolEnv env;
    // Default allow-all authorizer (no authorizeInstance override).
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, env.dispatcher, env.registry);

    morph::testing::WaitReply reg;
    server->handle(morph::wire::encode(registerAs("alice")), std::ref(reg));
    REQUIRE(reg.await());
    const auto mid = reg.env.modelId;

    // A different principal can execute — behaviour is unchanged from before.
    morph::testing::WaitReply run;
    server->handle(morph::wire::encode(executeAs(mid, "bob")), std::ref(run));
    REQUIRE(run.await());
    REQUIRE(run.env.kind == "ok");

    // And can deregister.
    auto dereg = morph::wire::makeDeregister(mid);
    dereg.session.principal = "bob";
    morph::testing::WaitReply okDereg;
    server->handle(morph::wire::encode(dereg), std::ref(okDereg));
    REQUIRE(okDereg.await());
    REQUIRE(okDereg.env.kind == "ok");
}

// ── Item 5: offline-queue idempotency key ────────────────────────────────────

TEST_CASE("offline queue stores an idempotency key when enqueued with one", "[policy][queue]") {
    morph::offline::InMemoryOfflineQueue queue{morph::testing::storageOwner()};
    const auto id1 = queue.enqueue("payload-1", "op-key-1");
    const auto id2 = queue.enqueue("payload-2");  // no key
    REQUIRE(id1 != id2);

    const auto items = queue.drain();
    REQUIRE(items.size() == 2);
    REQUIRE(items[0].payload == "payload-1");
    REQUIRE(items[0].idempotencyKey == "op-key-1");
    REQUIRE(items[1].payload == "payload-2");
    REQUIRE(items[1].idempotencyKey.empty());
}

TEST_CASE("an idempotency key survives drain and can dedup a replay", "[policy][queue]") {
    morph::offline::InMemoryOfflineQueue queue{morph::testing::storageOwner()};
    (void)queue.enqueue("a", "k1");
    (void)queue.enqueue("b", "k1");  // same logical op re-enqueued (same key)
    (void)queue.enqueue("c", "k2");

    // A replay consumer deduping by idempotencyKey applies each key once.
    std::vector<std::string> applied;
    std::vector<std::string> seenKeys;
    for (const auto& item : queue.drain()) {
        if (!item.idempotencyKey.empty() &&
            std::find(seenKeys.begin(), seenKeys.end(), item.idempotencyKey) != seenKeys.end()) {
            continue;  // already applied this logical op
        }
        applied.push_back(item.payload);
        if (!item.idempotencyKey.empty()) {
            seenKeys.push_back(item.idempotencyKey);
        }
    }
    // "b" is skipped because it shares key "k1" with the already-applied "a".
    REQUIRE(applied == std::vector<std::string>{"a", "c"});
}

// ── Item 6: UnitRelation positivity guard (compile-time) ─────────────────────

TEST_CASE("requirePositiveRatio accepts a strictly-positive ratio at compile time", "[policy][quantity]") {
    using morph::math::DecimalPlaces;
    using morph::math::Denominator;
    using morph::math::Numerator;
    using morph::math::Rational;
    // Consteval evaluation: a strictly-positive ratio compiles and returns true.
    // This is the "green" witness that the guard admits valid relations.
    constexpr bool ok =
        morph::units::detail::requirePositiveRatio(Rational{Numerator{1}, Denominator{1000}, DecimalPlaces{3}});
    STATIC_REQUIRE(ok);

    // The "red" side is a COMPILE error, not a runtime one, so it cannot be
    // expressed as a normal assertion. Uncommenting either line below fails the
    // build (the consteval `throw` in `requirePositiveRatio` is ill-formed in a
    // constant-evaluation context) — this is the intended protection:
    //
    //   constexpr auto bad0 = morph::units::detail::requirePositiveRatio(
    //       Rational{Numerator{0}, Denominator{1}, DecimalPlaces{3}});   // zero ratio
    //   constexpr auto badN = morph::units::detail::requirePositiveRatio(
    //       Rational{Numerator{-1}, Denominator{2}, DecimalPlaces{3}});  // negative ratio
    //
    // Every `UnitTraits::relations` array is consumed by the consteval
    // `conversionRatio` search, which calls `requirePositiveRatio` on each edge,
    // so a bad ratio declared by an application is rejected at build time.
}

// ── Item 7: log-injection sanitisation ───────────────────────────────────────

TEST_CASE("the default sink sanitises newlines so a message cannot forge a log line", "[policy][logger]") {
    // sanitizeLogLine is what the default sink applies; test it directly (the
    // default sink writes to stderr, which a unit test cannot capture portably).
    const std::string forged = "user logged in\n[ERROR] fake error injected";
    const std::string clean = morph::log::detail::sanitizeLogLine(forged);

    // No raw newline survives — the whole message is one physical line.
    REQUIRE(clean.find('\n') == std::string::npos);
    REQUIRE(clean.find('\r') == std::string::npos);
    // The newline is escaped rather than dropped.
    REQUIRE(clean.find("\\n[ERROR] fake error injected") != std::string::npos);
}

TEST_CASE("sanitizeLogLine escapes carriage returns, tabs, and other control bytes", "[policy][logger]") {
    const std::string msg = std::string("a\r\tb") + '\x01' + "c" + '\x7f';
    const std::string clean = morph::log::detail::sanitizeLogLine(msg);
    REQUIRE(clean == "a\\r\\tb\\x01c\\x7f");
}

TEST_CASE("sanitizeLogLine leaves clean printable text (including UTF-8) untouched", "[policy][logger]") {
    const std::string clean = "normal message with UTF-8: \xc3\xa9 \xe2\x9c\x93";
    REQUIRE(morph::log::detail::sanitizeLogLine(clean) == clean);
}
