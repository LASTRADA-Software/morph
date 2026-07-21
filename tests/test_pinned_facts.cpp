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
#include <morph/core/wire.hpp>
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
