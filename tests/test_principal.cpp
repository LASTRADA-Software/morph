// SPDX-License-Identifier: Apache-2.0
//
// Coverage for issue #24: morph::session::Principal and Bridge::setPrincipal/
// currentPrincipal -- readable authorization state outside a dispatch, so UI
// code can gate itself (e.g. disable a button) instead of attempting an
// action and catching the refusal.

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/session/session.hpp>
#include <string>

using morph::session::Principal;

// ── morph::session::Principal ────────────────────────────────────────────────

TEST_CASE("morph::session::Principal: default-constructed has no id and no roles", "[session][principal]") {
    Principal principal;
    REQUIRE(principal.id.empty());
    REQUIRE(principal.roles.empty());
    REQUIRE_FALSE(principal.hasRole("editor"));
}

TEST_CASE("morph::session::Principal::hasRole: true for a present role, false for an absent one",
          "[session][principal]") {
    Principal principal{.id = "alice", .roles = {"viewer", "editor"}, .claims = {}};
    REQUIRE(principal.hasRole("viewer"));
    REQUIRE(principal.hasRole("editor"));
    REQUIRE_FALSE(principal.hasRole("admin"));
}

// ── morph::bridge::Bridge::setPrincipal / currentPrincipal ──────────────────

TEST_CASE("morph::bridge::Bridge::currentPrincipal: empty before any setPrincipal call", "[bridge][principal]") {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};

    auto principal = bridge.currentPrincipal();
    REQUIRE(principal.id.empty());
    REQUIRE(principal.roles.empty());
}

TEST_CASE("morph::bridge::Bridge::setPrincipal/currentPrincipal: round-trips id, roles, and claims",
          "[bridge][principal]") {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};

    bridge.setPrincipal(Principal{
        .id = "alice",
        .roles = {"editor", "admin"},
        .claims = {{"tenant", "acme"}},
    });

    auto principal = bridge.currentPrincipal();
    REQUIRE(principal.id == "alice");
    REQUIRE(principal.hasRole("editor"));
    REQUIRE(principal.hasRole("admin"));
    REQUIRE_FALSE(principal.hasRole("viewer"));
    REQUIRE(principal.claims.at("tenant") == "acme");
}

TEST_CASE("morph::bridge::Bridge::setPrincipal: readable without an active dispatch (UI-gating use case)",
          "[bridge][principal]") {
    // No BridgeHandler, no execute() call anywhere in this test -- proves the
    // Principal is readable purely from the Bridge, independent of any
    // in-flight or prior dispatch. This is exactly the gap issue #24 reports:
    // session::current() (Context) only exists during a dispatch; Principal
    // does not have that restriction.
    morph::exec::ThreadPoolExecutor pool{2};
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};

    bridge.setPrincipal(Principal{.id = "bob", .roles = {"viewer"}, .claims = {}});
    REQUIRE(bridge.currentPrincipal().hasRole("viewer"));
    REQUIRE_FALSE(bridge.currentPrincipal().hasRole("editor"));
}

TEST_CASE("morph::bridge::Bridge::setPrincipal: passing a default-constructed Principal clears it (sign-out)",
          "[bridge][principal]") {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};

    bridge.setPrincipal(Principal{.id = "alice", .roles = {"editor"}, .claims = {}});
    REQUIRE(bridge.currentPrincipal().id == "alice");

    bridge.setPrincipal(Principal{});  // sign-out
    REQUIRE(bridge.currentPrincipal().id.empty());
    REQUIRE(bridge.currentPrincipal().roles.empty());
}

TEST_CASE("morph::bridge::Bridge::setPrincipal/currentPrincipal: independent per Bridge instance",
          "[bridge][principal]") {
    morph::exec::ThreadPoolExecutor pool1{2};
    morph::exec::ThreadPoolExecutor pool2{2};
    morph::bridge::Bridge bridgeA{std::make_unique<morph::backend::LocalBackend>(pool1)};
    morph::bridge::Bridge bridgeB{std::make_unique<morph::backend::LocalBackend>(pool2)};

    bridgeA.setPrincipal(Principal{.id = "alice", .roles = {}, .claims = {}});
    bridgeB.setPrincipal(Principal{.id = "bob", .roles = {}, .claims = {}});

    REQUIRE(bridgeA.currentPrincipal().id == "alice");
    REQUIRE(bridgeB.currentPrincipal().id == "bob");
}
