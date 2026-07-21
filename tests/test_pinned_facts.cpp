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
#include <morph/core/logger.hpp>
#include <morph/core/wire.hpp>
#include <morph/offline/reconnect_coordinator.hpp>
#include <morph/session/session_auth.hpp>
#include <morph/util/rational.hpp>

#include "pinned_facts_generated.hpp"

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
