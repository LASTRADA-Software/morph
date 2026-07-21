// SPDX-License-Identifier: Apache-2.0

// Spec <-> code drift guard: mechanically pins the facts docs/spec/*.md files
// state in prose (enum cardinalities, key constants, canonical error/reply
// strings, glaze parsing behavior) against the real code, so a future edit to
// one without the other fails this build. See docs/spec/pinned_facts.toml
// (the single source of truth for expected values) and
// scripts/check_spec_citations.sh (the complementary prose-vs-manifest lint).
//
// Two extraction mechanisms, matching what each fact class allows:
//  - static_assert / an exhaustive switch, for anything visible to the type
//    system (constants, enum cardinality) -- the strongest guard, since a
//    drift fails to *compile*.
//  - Catch2 runtime assertions, for facts only observable through behavior
//    (an exception's what(), a RemoteServer reply string, a JSON-parsing
//    option that is a private function-local constant with no reachable
//    symbol to static_assert against).

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/logger.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/core/wire.hpp>
#include <morph/offline/reconnect_coordinator.hpp>
#include <morph/session/session.hpp>
#include <morph/session/session_auth.hpp>
#include <morph/util/rational.hpp>
#include <string_view>

#include "pinned_facts_generated.hpp"
#include "test_support.hpp"

// ── Key constants ────────────────────────────────────────────────────────────

static_assert(morph::wire::kMaxEnvelopeBytes ==
                  static_cast<std::size_t>(morph::pinned_facts::kExpected_MAX_ENVELOPE_BYTES),
              "morph::wire::kMaxEnvelopeBytes drifted from docs/spec/pinned_facts.toml "
              "(see docs/spec/core/wire.md)");

static_assert(morph::math::kMaxDecimalPlaces ==
                  static_cast<std::uint32_t>(morph::pinned_facts::kExpected_MAX_DECIMAL_PLACES),
              "morph::math::kMaxDecimalPlaces drifted from docs/spec/pinned_facts.toml "
              "(see docs/spec/util/rational.md)");

static_assert(morph::session::kClockSkewMs == static_cast<std::int64_t>(morph::pinned_facts::kExpected_CLOCK_SKEW_MS),
              "morph::session::kClockSkewMs drifted from docs/spec/pinned_facts.toml "
              "(see docs/spec/security.md)");

TEST_CASE("pinned-facts: key constants match docs/spec/pinned_facts.toml", "[pinned-facts]") {
    // The static_asserts above already gate the build; this TEST_CASE gives
    // the checks a visible, run-time-confirmed entry in `ctest` output too.
    STATIC_REQUIRE(morph::wire::kMaxEnvelopeBytes ==
                   static_cast<std::size_t>(morph::pinned_facts::kExpected_MAX_ENVELOPE_BYTES));
    STATIC_REQUIRE(morph::math::kMaxDecimalPlaces ==
                   static_cast<std::uint32_t>(morph::pinned_facts::kExpected_MAX_DECIMAL_PLACES));
    STATIC_REQUIRE(morph::session::kClockSkewMs ==
                   static_cast<std::int64_t>(morph::pinned_facts::kExpected_CLOCK_SKEW_MS));
}

// ── Enum cardinalities ───────────────────────────────────────────────────────
//
// Each `pin*Switch` below lists a `case` for every enumerator declared today.
// Under MORPH_ENABLE_STRICT_COMPILATION (-Werror/-WX, the CI default),
// -Wswitch-enum and -Wswitch-default (GCC explicitly; Clang via -Weverything;
// MSVC via /w14061, scoped to this file in tests/CMakeLists.txt) turn a
// future appended, removed, or renamed enumerator into a hard compile error:
// -Wswitch-enum fires on a missing case *even with* a `default` label present
// (unlike plain -Wswitch), which is exactly why a `default` here does not
// weaken the guard. This is a stronger, cross-compiler-consistent version of
// the "last member's ordinal" check docs/planned/drift_guard.md sketches;
// the STATIC_REQUIRE below adds that check too, as a second, independent
// signal tied to the manifest's numeric claim.

namespace {

void pinAuthErrorSwitch(morph::session::AuthError value) {
    switch (value) {
        case morph::session::AuthError::Malformed:
        case morph::session::AuthError::BadSignature:
        case morph::session::AuthError::Expired:
        case morph::session::AuthError::NotYetValid:
            break;
        default:
            break;
    }
}

void pinLogLevelSwitch(morph::log::LogLevel value) {
    switch (value) {
        case morph::log::LogLevel::debug:
        case morph::log::LogLevel::info:
        case morph::log::LogLevel::warn:
        case morph::log::LogLevel::error:
        case morph::log::LogLevel::off:
            break;
        default:
            break;
    }
}

void pinReconnectOutcomeSwitch(morph::offline::ReconnectOutcome value) {
    switch (value) {
        case morph::offline::ReconnectOutcome::Reconnected:
        case morph::offline::ReconnectOutcome::GaveUp:
        case morph::offline::ReconnectOutcome::Aborted:
            break;
        default:
            break;
    }
}

}  // namespace

TEST_CASE("pinned-facts: AuthError has exactly 4 enumerators", "[pinned-facts]") {
    // Compiling this TU at all *is* the assertion: pinAuthErrorSwitch's switch
    // above must list every current AuthError enumerator by name, or
    // -Wswitch-enum/-Wswitch-default (-> -Werror) fails the build.
    pinAuthErrorSwitch(morph::session::AuthError::NotYetValid);
    STATIC_REQUIRE(static_cast<int>(morph::session::AuthError::NotYetValid) ==
                   static_cast<int>(morph::pinned_facts::kExpected_AUTH_ERROR_CARDINALITY) - 1);
}

TEST_CASE("pinned-facts: LogLevel has exactly 5 enumerators", "[pinned-facts]") {
    pinLogLevelSwitch(morph::log::LogLevel::off);
    STATIC_REQUIRE(static_cast<int>(morph::log::LogLevel::off) ==
                   static_cast<int>(morph::pinned_facts::kExpected_LOG_LEVEL_CARDINALITY) - 1);
}

TEST_CASE("pinned-facts: ReconnectOutcome has exactly 3 enumerators", "[pinned-facts]") {
    pinReconnectOutcomeSwitch(morph::offline::ReconnectOutcome::Aborted);
    STATIC_REQUIRE(static_cast<int>(morph::offline::ReconnectOutcome::Aborted) ==
                   static_cast<int>(morph::pinned_facts::kExpected_RECONNECT_OUTCOME_CARDINALITY) - 1);
}

// ── Canonical error `what()` strings ────────────────────────────────────────
//
// These are runtime, not compile-time, pins: std::runtime_error::what() is
// not constexpr, so a static_assert cannot compare it. This is the "genuine
// fork" the plan for this feature calls out explicitly: pick static_assert
// where the type system allows it (Tasks 1–2), REQUIRE where it does not.

TEST_CASE("pinned-facts: canonical Completion cancellation error strings", "[pinned-facts]") {
    REQUIRE(std::string_view{morph::backend::BackendChangedError{}.what()} ==
            morph::pinned_facts::kExpected_BACKEND_CHANGED_ERROR_WHAT);
    REQUIRE(std::string_view{morph::backend::BridgeDestroyedError{}.what()} ==
            morph::pinned_facts::kExpected_BRIDGE_DESTROYED_ERROR_WHAT);
    REQUIRE(std::string_view{morph::backend::DisconnectedError{}.what()} ==
            morph::pinned_facts::kExpected_DISCONNECTED_ERROR_WHAT);
}

// ── Canonical RemoteServer reply strings ────────────────────────────────────
//
// A fresh dispatcher + registry per test file (not the process-level
// singleton default) avoids type-id collisions with every other TU in the
// suite -- the same pattern test_remote_extra.cpp and test_policy_hardening.cpp
// already use. File-scope (not an anonymous namespace), matching the exact
// convention those two files use for their own probe model/action types.

struct DriftGuardEnv {
    morph::model::detail::ActionDispatcher dispatcher;
    morph::model::detail::ModelRegistryFactory registry;
};

struct DriftGuardProbeAction {
    int x = 0;
};

struct DriftGuardProbeModel {
    int execute(const DriftGuardProbeAction& act) { return act.x; }
};

struct DenyAllAuthorizer : morph::session::IAuthorizer {
    [[nodiscard]] bool authorize(const morph::session::Context& /*ctx*/, std::string_view /*modelType*/,
                                 std::string_view /*actionType*/) const override {
        return false;
    }
};

template <>
struct morph::model::ModelTraits<DriftGuardProbeModel> {
    static constexpr std::string_view typeId() { return "DG_ProbeModel"; }
};

template <>
struct morph::model::ActionTraits<DriftGuardProbeAction> {
    using Result = int;
    static constexpr std::string_view typeId() { return "DG_ProbeAction"; }
    static std::string toJson(const DriftGuardProbeAction& act) {
        std::string out;
        (void)glz::write_json(act, out);
        return out;
    }
    static DriftGuardProbeAction fromJson(std::string_view json) {
        DriftGuardProbeAction action{};
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

static DriftGuardEnv& driftGuardEnv() {
    static DriftGuardEnv env = [] {
        DriftGuardEnv env2;
        env2.registry.registerModel<DriftGuardProbeModel>("DG_ProbeModel");
        env2.dispatcher.registerAction<DriftGuardProbeModel, DriftGuardProbeAction>("DG_ProbeModel", "DG_ProbeAction");
        return env2;
    }();
    return env;
}

TEST_CASE("pinned-facts: RemoteServer denies with the canonical \"unauthorized\" reply", "[pinned-facts]") {
    morph::testing::InlineExecutor pool;
    auto& env = driftGuardEnv();
    auto authz = std::make_shared<DenyAllAuthorizer>();
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, authz, env.dispatcher, env.registry);

    morph::wire::Envelope req;
    req.kind = "execute";
    req.modelId = 1;
    req.modelType = "DG_ProbeModel";
    req.actionType = "DG_ProbeAction";
    req.body = "{}";
    morph::testing::WaitReply waiter;
    server->handle(morph::wire::encode(req), std::ref(waiter));
    REQUIRE(waiter.await());
    REQUIRE(waiter.env.kind == "err");
    REQUIRE(waiter.env.message == morph::pinned_facts::kExpected_UNAUTHORIZED_REPLY);
}

TEST_CASE("pinned-facts: RemoteServer replies \"model not found\" for an unknown model id", "[pinned-facts]") {
    morph::testing::InlineExecutor pool;
    auto& env = driftGuardEnv();
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, env.dispatcher, env.registry);

    morph::wire::Envelope req;
    req.kind = "execute";
    req.modelId = 999999;  // never registered
    req.modelType = "DG_ProbeModel";
    req.actionType = "DG_ProbeAction";
    req.body = "{}";
    morph::testing::WaitReply waiter;
    server->handle(morph::wire::encode(req), std::ref(waiter));
    REQUIRE(waiter.await());
    REQUIRE(waiter.env.kind == "err");
    REQUIRE(waiter.env.message == morph::pinned_facts::kExpected_MODEL_NOT_FOUND_REPLY);
}

TEST_CASE("pinned-facts: RemoteServer replies \"register requires a typeId\" for an empty typeId", "[pinned-facts]") {
    morph::testing::InlineExecutor pool;
    auto& env = driftGuardEnv();
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, env.dispatcher, env.registry);

    morph::wire::Envelope req;
    req.kind = "register";
    req.typeId = "";  // empty on purpose
    morph::testing::WaitReply waiter;
    server->handle(morph::wire::encode(req), std::ref(waiter));
    REQUIRE(waiter.await());
    REQUIRE(waiter.env.kind == "err");
    REQUIRE(waiter.env.message == morph::pinned_facts::kExpected_REGISTER_REQUIRES_TYPEID_REPLY);
}
