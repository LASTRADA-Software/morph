// SPDX-License-Identifier: Apache-2.0
#include <Lightweight/DataMapper/DataMapper.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <memory>
#include <morph/core/bridge.hpp>
#include <string>

#include "ledger/auth/ledger_authorizer.hpp"
#include "ledger/core/errors.hpp"
#include "ledger/db/ledger_entity.hpp"
#include "ledger/models/auth_model.hpp"
#include "ledger/models/ledger_model.hpp"
#include "testkit/backend_rig.hpp"
#include "testkit/db_fixture.hpp"
#include "testkit/pump.hpp"

// morph#242: this rung had no authentication/authorization story, so every
// mutating action failed in the shipped desktop client, in both deployment
// modes -- Local (no login was ever installed) and Remote (RemoteServer
// clears an unverified principal, and this rung shipped no authorizer that
// could verify one, and no server binary). This file exercises the Remote-
// mode half end to end, the same shape `test_bookmarks_authorizer.cpp` uses
// for its own rung's identical fix: a tokenless client is refused, `Login`
// mints a real signed token, and that token unlocks the rest.

using ledger::auth::isReservedPrincipal;
using ledger::auth::isValidPrincipal;
using ledger::auth::LedgerAuthorizer;
using morph::ladder::testkit::awaitQt;
using morph::ladder::testkit::BackendRig;
using morph::ladder::testkit::DbFixture;
using morph::ladder::testkit::Mode;
using morph::session::Context;
using morph::session::SessionToken;
using morph::session::TokenIssuer;

namespace {
constexpr std::string_view kSecret = "test-only-shared-secret";

/// @brief Installs a process-global `TokenIssuer` for a scope and clears it
///        again on the way out, whether the scope exits normally or through a
///        failing Catch2 assertion. Mirrors bookmarks' identical
///        `ScopedTokenIssuer`.
class ScopedTokenIssuer {
public:
    explicit ScopedTokenIssuer(std::shared_ptr<TokenIssuer> issuer) {
        ledger::auth::setTokenIssuer(std::move(issuer));
    }
    ~ScopedTokenIssuer() { ledger::auth::setTokenIssuer(nullptr); }
    ScopedTokenIssuer(const ScopedTokenIssuer&) = delete;
    ScopedTokenIssuer& operator=(const ScopedTokenIssuer&) = delete;
    ScopedTokenIssuer(ScopedTokenIssuer&&) = delete;
    ScopedTokenIssuer& operator=(ScopedTokenIssuer&&) = delete;
};
}  // namespace

TEST_CASE("isValidPrincipal accepts ordinary usernames and the report-runner principal", "[ledger][auth]") {
    CHECK(isValidPrincipal("alice"));
    CHECK(isValidPrincipal("alice_2"));
    CHECK(isValidPrincipal("alice.smith-99"));
    CHECK(isValidPrincipal(ledger::kReportRunnerPrincipal));
}

TEST_CASE("isValidPrincipal rejects the empty string and overlong input", "[ledger][auth]") {
    CHECK_FALSE(isValidPrincipal(""));
    const std::string tooLong(65, 'a');
    CHECK_FALSE(isValidPrincipal(tooLong));
    const std::string atLimit(64, 'a');
    CHECK(isValidPrincipal(atLimit));
}

TEST_CASE("isReservedPrincipal flags the system: namespace and nothing else", "[ledger][auth]") {
    CHECK(isReservedPrincipal(ledger::kReportRunnerPrincipal));
    CHECK(isReservedPrincipal("system:anything"));
    CHECK_FALSE(isReservedPrincipal("alice"));
    CHECK_FALSE(isReservedPrincipal(""));
}

TEST_CASE("LedgerAuthorizer authenticates and authorizes a validly signed token", "[ledger][auth]") {
    const LedgerAuthorizer authz{std::string{kSecret}, morph::session::hmacSha256};
    const TokenIssuer issuer{std::string{kSecret}, morph::session::hmacSha256};

    const std::string token = issuer.issue(SessionToken{
        .principal = "alice",
        .issuedAtMs = 0,
        .expiresAtMs = 4102444800000,  // year 2100, far future
        .roles = {},
    });

    Context ctx;
    ctx.token = token;

    CHECK(authz.authorize(ctx, "LedgerModel", "OpenAccount"));
    const auto principal = authz.authenticate(ctx);
    REQUIRE(principal.has_value());
    CHECK(*principal == "alice");
}

TEST_CASE("LedgerAuthorizer rejects a tampered or expired token", "[ledger][auth]") {
    const LedgerAuthorizer authz{std::string{kSecret}, morph::session::hmacSha256};
    const TokenIssuer issuer{std::string{kSecret}, morph::session::hmacSha256};

    const std::string expired = issuer.issue(SessionToken{
        .principal = "alice",
        .issuedAtMs = 0,
        .expiresAtMs = 1,  // 1970-01-01T00:00:00.001Z -- long expired
        .roles = {},
    });
    Context expiredCtx;
    expiredCtx.token = expired;
    CHECK_FALSE(authz.authorize(expiredCtx, "LedgerModel", "OpenAccount"));

    const std::string valid = issuer.issue(SessionToken{
        .principal = "alice",
        .issuedAtMs = 0,
        .expiresAtMs = 4102444800000,
        .roles = {},
    });
    Context tamperedCtx;
    tamperedCtx.token = valid + "x";  // corrupt the signature
    CHECK_FALSE(authz.authorize(tamperedCtx, "LedgerModel", "OpenAccount"));

    Context noTokenCtx;  // empty token: malformed
    CHECK_FALSE(authz.authorize(noTokenCtx, "LedgerModel", "OpenAccount"));
}

TEST_CASE("LedgerAuthorizer::authorize admits Login without a token, and nothing else", "[ledger][auth]") {
    // The carve-out that makes login possible at all -- without it
    // SigningAuthorizer::authorize() rejects every tokenless execute,
    // including the one action whose whole purpose is handing out the first
    // token, and a fresh client can never get past `err "unauthorized"`. See
    // LedgerAuthorizer::authorize's own doc comment.
    const LedgerAuthorizer authz{std::string{kSecret}, morph::session::hmacSha256};
    const Context anonymous;  // no token at all, like a just-launched client

    CHECK(authz.authorize(anonymous, "AuthModel", "Login"));

    // Nothing else is reachable anonymously.
    CHECK_FALSE(authz.authorize(anonymous, "AuthModel", "SomethingElse"));
    CHECK_FALSE(authz.authorize(anonymous, "LedgerModel", "Login"));
    CHECK_FALSE(authz.authorize(anonymous, "LedgerModel", "OpenAccount"));
    CHECK_FALSE(authz.authorize(anonymous, "BudgetModel", "CreateCategory"));
    CHECK_FALSE(authz.authorize(anonymous, "RuleModel", "CreateRule"));
}

TEST_CASE("AuthModel::execute(Login) mints a token that verifies against the same secret", "[ledger][auth]") {
    const ScopedTokenIssuer issuer{std::make_shared<TokenIssuer>(std::string{kSecret}, morph::session::hmacSha256)};
    ledger::AuthModel authModel;
    const auto result = authModel.execute(ledger::Login{.username = "alice"});
    REQUIRE(result.token.hasValue());
    CHECK(result.principal == "alice");

    const LedgerAuthorizer authz{std::string{kSecret}, morph::session::hmacSha256};
    Context ctx;
    ctx.token = *result.token;
    const auto principal = authz.authenticate(ctx);
    REQUIRE(principal.has_value());
    CHECK(*principal == "alice");
    CHECK(authz.authorize(ctx, "LedgerModel", "OpenAccount"));

    // ...and does not verify against a different secret.
    const LedgerAuthorizer other{std::string{"a-different-secret"}, morph::session::hmacSha256};
    CHECK_FALSE(other.authenticate(ctx).has_value());
}

TEST_CASE("AuthModel::execute(Login) refuses to mint a token in the reserved system: namespace", "[ledger][auth]") {
    // Otherwise any client could log in as the report runner and complete
    // report jobs it does not own.
    const ScopedTokenIssuer issuer{std::make_shared<TokenIssuer>(std::string{kSecret}, morph::session::hmacSha256)};
    ledger::AuthModel authModel;
    REQUIRE_THROWS_AS(authModel.execute(ledger::Login{.username = std::string{ledger::kReportRunnerPrincipal}}),
                      ledger::ValidationError);
    REQUIRE_THROWS_AS(authModel.execute(ledger::Login{.username = "system:anything"}), ledger::ValidationError);
}

TEST_CASE("AuthModel::execute(Login) throws when no App has installed a TokenIssuer", "[ledger][auth]") {
    REQUIRE(ledger::auth::tokenIssuer() == nullptr);
    ledger::AuthModel authModel;
    REQUIRE_THROWS_AS(authModel.execute(ledger::Login{.username = "alice"}), ledger::ValidationError);
}

TEST_CASE("Login rejects an invalid username via the shared principal charset", "[ledger][auth]") {
    ledger::AuthModel authModel;
    REQUIRE_THROWS_AS(authModel.execute(ledger::Login{.username = ""}), ledger::ValidationError);
    REQUIRE_THROWS_AS(authModel.execute(ledger::Login{.username = "alice bob"}), ledger::ValidationError);
    CHECK_FALSE(ledger::Login{.username = std::string(65, 'a')}.validate());
    CHECK(ledger::Login{.username = "alice"}.validate());
}

TEST_CASE("setTokenIssuer/tokenIssuer share one process-global slot", "[ledger][auth]") {
    CHECK(ledger::auth::tokenIssuer() == nullptr);
    auto issuer = std::make_shared<TokenIssuer>(std::string{kSecret}, morph::session::hmacSha256);
    ledger::auth::setTokenIssuer(issuer);
    CHECK(ledger::auth::tokenIssuer() == issuer);
    ledger::auth::setTokenIssuer(nullptr);
    CHECK(ledger::auth::tokenIssuer() == nullptr);
}

TEST_CASE("A tokenless client logs in over a real RemoteServer and its token unlocks the rest", "[ledger][auth]") {
    // The end-to-end shape of morph#242's fix, at the wire level: this is the
    // exact sequence a freshly launched Remote-mode desktop client performs.
    DbFixture fixture;
    const auto authorizer = std::make_shared<LedgerAuthorizer>(std::string{kSecret}, morph::session::hmacSha256);
    // RAII, not a trailing reset: a failing REQUIRE below throws, and a
    // leaked process-global issuer would then break a sibling case asserting
    // none is installed, under any run order.
    const ScopedTokenIssuer issuer{std::make_shared<TokenIssuer>(std::string{kSecret}, morph::session::hmacSha256)};
    BackendRig rig{Mode::Socket, 1, authorizer};

    // Deliberately no setDefaultSession: this bridge carries no credential.
    morph::bridge::BridgeHandler<ledger::AuthModel> auth{rig.bridge(0), rig.executor()};

    ledger::db::LedgerRecord ledgerRow;
    ledgerRow.name = "Personal";
    Lightweight::DataMapper mapper;
    mapper.Create(ledgerRow);
    const auto ledgerId = ledger::LedgerId{static_cast<std::int64_t>(ledgerRow.id.Value())};

    morph::bridge::BridgeHandler<ledger::LedgerModel, morph::bridge::AllowShared> ledgerHandler{rig.bridge(0),
                                                                                                rig.executor()};

    // Without a token, a domain action is refused by the server.
    CHECK_THROWS(awaitQt(ledgerHandler.execute(ledger::OpenAccount{.ledgerId = ledgerId,
                                                                   .name = "Checking",
                                                                   .kind = ledger::AccountKind::Asset,
                                                                   .currency = ledger::Currency::USD})));

    const auto result = awaitQt(auth.execute(ledger::Login{.username = "alice"}));
    REQUIRE(result.token.hasValue());
    CHECK(result.principal == "alice");

    // Exactly what a Remote-mode login installs on the shared bridge.
    morph::session::Context session;
    session.principal = result.principal;
    session.token = *result.token;
    rig.bridge(0).setDefaultSession(session);

    const auto account = awaitQt(ledgerHandler.execute(ledger::OpenAccount{.ledgerId = ledgerId,
                                                                           .name = "Checking",
                                                                           .kind = ledger::AccountKind::Asset,
                                                                           .currency = ledger::Currency::USD}));
    REQUIRE(account.id.hasValue());

    const auto state = awaitQt(ledgerHandler.execute(ledger::GetLedger{.ledgerId = ledgerId}));
    REQUIRE(state.accounts.size() == 1);
    CHECK(state.accounts.front().name == "Checking");
}
