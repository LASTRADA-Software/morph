// SPDX-License-Identifier: Apache-2.0
#include "ledger/core/time_util.hpp"

#include <catch2/catch_test_macros.hpp>

#include <morph/util/datetime.hpp>

#include <string>

namespace {

/// @brief Parses an ISO-8601 UTC literal, failing the test rather than
///        returning an empty optional -- every literal in this file is a
///        constant the test itself wrote, so a parse failure is a bug in the
///        test, not a case worth branching on.
[[nodiscard]] morph::time::DateTime utc(std::string_view iso) {
    const auto parsed = morph::time::DateTime::fromIso8601(iso);
    REQUIRE(parsed.has_value());
    return *parsed;
}

}  // namespace

TEST_CASE("A transaction at 23:30 local time lands in its local month across a UTC boundary",
          "[ledger][time]") {
    // UTC-5. 2026-01-31T23:30 local is 2026-02-01T04:30 UTC -- still January
    // where the client lives, already February in the column it is stored in.
    const auto [start, end] = ledger::localMonthToUtcRange(2026, /*month=*/1, /*timezoneOffsetMinutes=*/-300);

    CHECK(start.toIso8601() == "2026-01-01T05:00:00.000Z");
    CHECK(end.toIso8601() == "2026-02-01T05:00:00.000Z");

    const auto lateJanuaryLocal = utc("2026-02-01T04:30:00Z");
    CHECK(lateJanuaryLocal >= start);
    CHECK(lateJanuaryLocal < end);

    // Just past local midnight on Feb 1 -- must fall outside January.
    const auto earlyFebruaryLocal = utc("2026-02-01T05:01:00Z");
    CHECK_FALSE(earlyFebruaryLocal < end);
}

TEST_CASE("The month range is half-open, so consecutive months tile exactly", "[ledger][time]") {
    // The boundary instant belongs to February and to nothing else: were the
    // range closed at both ends, a transaction at exactly 05:00Z on Feb 1
    // would be reported in both months and double-counted.
    const auto [janStart, janEnd] = ledger::localMonthToUtcRange(2026, 1, -300);
    const auto [febStart, febEnd] = ledger::localMonthToUtcRange(2026, 2, -300);

    CHECK(janEnd == febStart);
    CHECK_FALSE(janEnd < janEnd);  // the boundary is not inside January
    CHECK(febStart >= febStart);   // but it is inside February
    CHECK(janStart < janEnd);
    CHECK(febStart < febEnd);
}

TEST_CASE("Month ranges handle year rollover, leap February, and a half-hour zone", "[ledger][time]") {
    // December rolls into the next year rather than month 13.
    const auto [decStart, decEnd] = ledger::localMonthToUtcRange(2026, 12, /*UTC*/ 0);
    CHECK(decStart.toIso8601() == "2026-12-01T00:00:00.000Z");
    CHECK(decEnd.toIso8601() == "2027-01-01T00:00:00.000Z");

    // 2028 is a leap year: February must end on the 29th->March 1 boundary.
    const auto [febStart, febEnd] = ledger::localMonthToUtcRange(2028, 2, 0);
    CHECK(febStart.toIso8601() == "2028-02-01T00:00:00.000Z");
    CHECK(febEnd.toIso8601() == "2028-03-01T00:00:00.000Z");

    // A non-whole-hour offset (UTC+5:30) must shift by the exact minutes, not
    // a rounded hour -- the case an hours-only implementation gets wrong.
    const auto [mayStart, mayEnd] = ledger::localMonthToUtcRange(2026, 5, /*+05:30*/ 330);
    CHECK(mayStart.toIso8601() == "2026-04-30T18:30:00.000Z");
    CHECK(mayEnd.toIso8601() == "2026-05-31T18:30:00.000Z");
}

TEST_CASE("A positive offset shifts the range earlier and a negative one later", "[ledger][time]") {
    // Direction matters and is easy to invert: at UTC+X local midnight has
    // already happened in UTC terms, so the UTC range starts *before* the
    // nominal date; at UTC-X it starts after.
    const auto [utcStart, utcEnd] = ledger::localMonthToUtcRange(2026, 6, 0);
    const auto [eastStart, eastEnd] = ledger::localMonthToUtcRange(2026, 6, 120);   // UTC+2
    const auto [westStart, westEnd] = ledger::localMonthToUtcRange(2026, 6, -120);  // UTC-2

    CHECK(eastStart < utcStart);
    CHECK(westStart > utcStart);
    CHECK(eastEnd < utcEnd);
    CHECK(westEnd > utcEnd);
}
