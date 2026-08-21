// SPDX-License-Identifier: Apache-2.0
#include "kanban/auth/kanban_authorizer.hpp"

#include <morph/session/session_auth.hpp>

#include <catch2/catch_test_macros.hpp>

namespace {
constexpr std::string_view kSecret = "test-secret-at-least-32-bytes-long!!";
}

TEST_CASE("KanbanAuthorizer authenticates a validly-signed token and rejects a forged one", "[kanban][auth]") {
    auto issuer = std::make_shared<morph::session::TokenIssuer>(std::string{kSecret}, morph::session::hmacSha256);
    kanban::auth::setTokenIssuer(issuer);
    kanban::auth::KanbanAuthorizer authorizer{std::string{kSecret}, morph::session::hmacSha256};

    auto token = issuer->issue(morph::session::SessionToken{
        .principal = "alice", .issuedAtMs = 0, .expiresAtMs = 4102444800000, .roles = {}});

    morph::session::Context ctx;
    ctx.token = token;
    auto principal = authorizer.authenticate(ctx);
    REQUIRE(principal.has_value());
    CHECK(*principal == "alice");

    morph::session::Context forged;
    forged.token = "not-a-real-token";
    CHECK_FALSE(authorizer.authenticate(forged).has_value());

    kanban::auth::setTokenIssuer(nullptr);
}

TEST_CASE("KanbanAuthorizer::authorizeRegister and authorizeInstance stay permissive", "[kanban][auth]") {
    // Mirrors bookmarks::auth::BookmarksAuthorizer's own carve-out shape:
    // identity is authenticated, but instance/register-level admission is
    // not additionally restricted -- BoardModel's own requireRole() is the
    // enforcement layer (design spec §3).
    kanban::auth::KanbanAuthorizer authorizer{std::string{kSecret}, morph::session::hmacSha256};
    morph::session::Context ctx;
    CHECK(authorizer.authorizeRegister(ctx, "BoardModel"));
    CHECK(authorizer.authorizeInstance(ctx, "BoardModel", "MoveTaskPosition", 1, ""));
}

TEST_CASE("setTokenIssuer/tokenIssuer share one mutex-guarded process-global slot", "[kanban][auth]") {
    // Mirrors bookmarks::auth's identical coverage (test_bookmarks_authorizer.cpp)
    // for the mutex-guarded slot -- see kanban_authorizer.cpp's detail::
    // tokenIssuerMutex()/tokenIssuerSlot().
    CHECK(kanban::auth::tokenIssuer() == nullptr);
    auto issuer = std::make_shared<morph::session::TokenIssuer>(std::string{kSecret}, morph::session::hmacSha256);
    kanban::auth::setTokenIssuer(issuer);
    CHECK(kanban::auth::tokenIssuer() == issuer);
    kanban::auth::setTokenIssuer(nullptr);
    CHECK(kanban::auth::tokenIssuer() == nullptr);
}
