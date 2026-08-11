// SPDX-License-Identifier: Apache-2.0

#include <morph/util/datetime.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <compare>
#include <format>
#include <optional>
#include <string>

using morph::time::DateTime;
using morph::time::Timestamp;

namespace {

[[nodiscard]] DateTime sample() {
    return DateTime{std::chrono::year{2026},   std::chrono::month{7},    std::chrono::day{5},
                    std::chrono::hours{14},    std::chrono::minutes{30}, std::chrono::seconds{15},
                    std::chrono::milliseconds{250}};
}

}  // namespace

// Global scope on purpose: glaze reflection needs a type with linkage.
struct DtProbe {
    Timestamp at;
};

TEST_CASE("DateTime::Iso8601::RoundTrip", "[datetime]") {
    auto const instant = sample();
    CHECK(instant.toIso8601() == "2026-07-05T14:30:15.250Z");

    auto const parsed = DateTime::fromIso8601("2026-07-05T14:30:15.250Z");
    REQUIRE(parsed.has_value());
    CHECK(parsed.value_or(DateTime{}) == instant);

    // The fraction and the trailing Z are optional on input.
    CHECK(DateTime::fromIso8601("2026-07-05T14:30:15") ==
          std::optional<DateTime>{DateTime{std::chrono::year{2026}, std::chrono::month{7}, std::chrono::day{5},
                                           std::chrono::hours{14}, std::chrono::minutes{30},
                                           std::chrono::seconds{15}}});
    CHECK(DateTime::fromIso8601("2026-07-05T14:30:15Z") == DateTime::fromIso8601("2026-07-05T14:30:15"));

    // Short fractions are padded: .5 == 500 ms, .25 == 250 ms.
    auto const half = DateTime::fromIso8601("2026-07-05T14:30:15.5Z");
    REQUIRE(half.has_value());
    CHECK(half.value_or(DateTime{}).toIso8601() == "2026-07-05T14:30:15.500Z");
    auto const quarter = DateTime::fromIso8601("2026-07-05T14:30:15.25");
    REQUIRE(quarter.has_value());
    CHECK(quarter.value_or(DateTime{}).toIso8601() == "2026-07-05T14:30:15.250Z");
}

TEST_CASE("DateTime::Iso8601::RejectsMalformedInput", "[datetime]") {
    // Too short.
    CHECK_FALSE(DateTime::fromIso8601("").has_value());
    CHECK_FALSE(DateTime::fromIso8601("2026-07-05").has_value());
    // Every separator position, individually wrong.
    CHECK_FALSE(DateTime::fromIso8601("2026x07-05T14:30:15").has_value());
    CHECK_FALSE(DateTime::fromIso8601("2026-07x05T14:30:15").has_value());
    CHECK_FALSE(DateTime::fromIso8601("2026-07-05 14:30:15").has_value());  // space, not T
    CHECK_FALSE(DateTime::fromIso8601("2026-07-05T14x30:15").has_value());
    CHECK_FALSE(DateTime::fromIso8601("2026-07-05T14:30x15").has_value());
    // Non-digit components: trailing junk within a field ("20x6") and a
    // from_chars hard failure at the field start, for every field.
    CHECK_FALSE(DateTime::fromIso8601("20x6-07-05T14:30:15").has_value());
    CHECK_FALSE(DateTime::fromIso8601("2026-xa-05T14:30:15").has_value());
    CHECK_FALSE(DateTime::fromIso8601("2026-07-xaT14:30:15").has_value());
    CHECK_FALSE(DateTime::fromIso8601("2026-07-05Txa:30:15").has_value());
    CHECK_FALSE(DateTime::fromIso8601("2026-07-05T14:xa:15").has_value());
    CHECK_FALSE(DateTime::fromIso8601("2026-07-05T14:30:xa").has_value());
    // A dot must be followed by at least one digit; non-digits end the
    // fraction and then count as trailing junk.
    CHECK_FALSE(DateTime::fromIso8601("2026-07-05T14:30:15.Z").has_value());
    CHECK_FALSE(DateTime::fromIso8601("2026-07-05T14:30:15.5x").has_value());
    CHECK_FALSE(DateTime::fromIso8601("2026-07-05T14:30:15.5-").has_value());  // below '0' ends the fraction
    // Trailing junk (including unsupported zone offsets).
    CHECK_FALSE(DateTime::fromIso8601("2026-07-05T14:30:15+02:00").has_value());
    CHECK_FALSE(DateTime::fromIso8601("2026-07-05T14:30:15.2504Z").has_value());
    // Impossible calendar dates and clock values, each limb of the check.
    CHECK_FALSE(DateTime::fromIso8601("2026-02-30T10:00:00Z").has_value());
    CHECK_FALSE(DateTime::fromIso8601("2026-07-05T24:00:00Z").has_value());
    CHECK_FALSE(DateTime::fromIso8601("2026-07-05T14:60:00Z").has_value());
    CHECK_FALSE(DateTime::fromIso8601("2026-07-05T14:30:61Z").has_value());
}

TEST_CASE("DateTime::Iso8601::RejectsSignInjection", "[datetime]") {
    // A leading '-' in a fixed-width clock field must NOT parse as a negative
    // component (from_chars accepts a leading sign). Without the guard,
    // "T-5:30:15" would read hour = -5 and silently shift to a *different*
    // valid instant instead of failing. Every clock field is checked.
    CHECK_FALSE(DateTime::fromIso8601("2026-07-05T-5:30:15").has_value());
    CHECK_FALSE(DateTime::fromIso8601("2026-07-05T14:-5:15").has_value());
    CHECK_FALSE(DateTime::fromIso8601("2026-07-05T14:30:-5").has_value());
    // Likewise for the fixed-width date fields month and day.
    CHECK_FALSE(DateTime::fromIso8601("2026--7-05T14:30:15").has_value());
    CHECK_FALSE(DateTime::fromIso8601("2026-07--5T14:30:15").has_value());
    // And a leading '+' is equally rejected in those fields.
    CHECK_FALSE(DateTime::fromIso8601("2026-07-05T+5:30:15").has_value());
    CHECK_FALSE(DateTime::fromIso8601("2026-+7-05T14:30:15").has_value());
    // A signed fraction is rejected too (the sign is trailing junk after the
    // dot, so digits == 0).
    CHECK_FALSE(DateTime::fromIso8601("2026-07-05T14:30:15.-5Z").has_value());

    // Regression: the specific instant from the bug report must not shift to
    // 2026-07-04T19:30:15Z.
    CHECK_FALSE(DateTime::fromIso8601("2026-07-05T-5:30:15").has_value());
}

TEST_CASE("DateTime::Iso8601::NegativeYearRoundTrip", "[datetime]") {
    // Negative years round-trip as long as toIso8601 emits exactly four
    // characters for the year field ('-' + three digits), i.e. years -001..-999,
    // and fromIso8601 reads exactly four characters back. A leading '-' is
    // legitimate ONLY in the whole-string year field.
    auto const roundTrip = [](std::chrono::year y) {
        DateTime const dt{y,
                          std::chrono::month{7},
                          std::chrono::day{5},
                          std::chrono::hours{14},
                          std::chrono::minutes{30},
                          std::chrono::seconds{15}};
        auto const text = dt.toIso8601();
        auto const parsed = DateTime::fromIso8601(text);
        return parsed.has_value() && *parsed == dt;
    };

    // Boundary: -001 and -999 round-trip (four-character year field).
    CHECK(roundTrip(std::chrono::year{-1}));
    CHECK(roundTrip(std::chrono::year{-999}));
    CHECK(roundTrip(std::chrono::year{-123}));
    CHECK(DateTime::fromIso8601("-123-07-05T14:30:15") ==
          std::optional<DateTime>{DateTime{std::chrono::year{-123}, std::chrono::month{7}, std::chrono::day{5},
                                           std::chrono::hours{14}, std::chrono::minutes{30},
                                           std::chrono::seconds{15}}});

    // Just past the boundary: year -1000 emits a FIVE-character field
    // ("-1000-…"), which shifts the '-' separator past offset 4, so it does not
    // round-trip. This pins the true lower bound at -999.
    CHECK(DateTime{std::chrono::year{-1000}, std::chrono::month{7}, std::chrono::day{5}, std::chrono::hours{14},
                   std::chrono::minutes{30}, std::chrono::seconds{15}}
              .toIso8601()
              .starts_with("-1000-"));
    CHECK_FALSE(DateTime::fromIso8601("-1000-07-05T14:30:15").has_value());

    // Year 0000 round-trips too (four digits, no sign).
    CHECK(roundTrip(std::chrono::year{0}));
}

TEST_CASE("DateTime::ComparisonAndArithmetic", "[datetime]") {
    auto const earlier = sample();
    auto later = earlier + std::chrono::minutes{5};
    CHECK(earlier < later);
    CHECK((earlier <=> earlier) == std::strong_ordering::equal);
    CHECK(later - earlier == std::chrono::minutes{5});

    later -= std::chrono::minutes{5};
    CHECK(later == earlier);
    later += std::chrono::hours{1};
    CHECK((later - std::chrono::hours{1}) == earlier);

    CHECK(DateTime::now().value.time_since_epoch() > std::chrono::years{50});  // sanity: past 2020
}

TEST_CASE("DateTime::Formatter", "[datetime]") {
    CHECK(std::format("{}", sample()) == "2026-07-05T14:30:15.250Z");
    auto const value = sample();
    // "{:}" reaches parse() with an empty spec range (begin == end).
    CHECK(std::vformat("{:}", std::make_format_args(value)) == "2026-07-05T14:30:15.250Z");
    CHECK_THROWS_AS(std::vformat("{:x}", std::make_format_args(value)), std::format_error);
}

TEST_CASE("DateTime::Wire::StrictCodec", "[datetime]") {
    // Write: the ISO string.
    auto const written = glz::write_json(sample());
    REQUIRE(written.has_value());
    CHECK(*written == R"("2026-07-05T14:30:15.250Z")");

    // Read: round-trips.
    DateTime restored{};
    REQUIRE_FALSE(glz::read_json(restored, *written));
    CHECK(restored == sample());

    // Malformed content is a read ERROR (not clamped): a mistyped timestamp
    // has no meaningful clamp.
    DateTime target{};
    CHECK(bool(glz::read_json(target, R"("yesterday")")));
    CHECK(bool(glz::read_json(target, "123")));  // not even a string
}

TEST_CASE("Timestamp::EmptyStateAndWire", "[datetime][forms]") {
    Timestamp blank;
    CHECK_FALSE(blank.hasValue());

    auto const engaged = Timestamp{sample()};
    CHECK(engaged.hasValue());
    CHECK(*engaged == sample());
    CHECK(blank < engaged);  // empty sorts before engaged
    CHECK(Timestamp{std::optional<DateTime>{}} == blank);
    CHECK(Timestamp::now().hasValue());

    DtProbe probe{.at = engaged};
    auto const json = glz::write_json(probe);
    REQUIRE(json.has_value());
    CHECK(*json == R"({"at":"2026-07-05T14:30:15.250Z"})");

    DtProbe restored{};
    REQUIRE_FALSE(glz::read_json(restored, *json));
    CHECK(restored.at == engaged);

    // Empty writes as an omitted key; explicit null clears.
    CHECK(glz::write_json(DtProbe{}).value_or("fail") == "{}");
    REQUIRE_FALSE(glz::read_json(restored, R"({"at":null})"));
    CHECK_FALSE(restored.at.hasValue());
}

TEST_CASE("DateTime::NowOverride::FixedInstant", "[datetime]") {
    auto const fixed = sample();
    CHECK(DateTime::now() != fixed);  // sanity: real "now" is not the fixed sample

    {
        morph::time::ScopedNowOverride const guard{fixed};
        CHECK(DateTime::now() == fixed);
        CHECK(DateTime::now() == fixed);  // stable across repeated calls within the scope
        CHECK(Timestamp::now().hasValue());
        CHECK(*Timestamp::now() == fixed);  // Timestamp::now() delegates to DateTime::now()
    }
    // Restored after the guard's scope ends.
    CHECK(DateTime::now() != fixed);
}

TEST_CASE("DateTime::NowOverride::CustomCallable", "[datetime]") {
    int calls = 0;
    auto const base = sample();
    morph::time::ScopedNowOverride const guard{[&calls, base]() -> DateTime {
        ++calls;
        return base + std::chrono::seconds{calls};
    }};
    CHECK(DateTime::now() == base + std::chrono::seconds{1});
    CHECK(DateTime::now() == base + std::chrono::seconds{2});
    CHECK(calls == 2);
}

TEST_CASE("DateTime::NowOverride::NestedScopesRestoreInOrder", "[datetime]") {
    auto const outer = sample();
    auto const inner = sample() + std::chrono::hours{1};

    morph::time::ScopedNowOverride const outerGuard{outer};
    CHECK(DateTime::now() == outer);
    {
        morph::time::ScopedNowOverride const innerGuard{inner};
        CHECK(DateTime::now() == inner);
    }
    // Inner guard's destruction restores the outer override, not real time.
    CHECK(DateTime::now() == outer);
}

TEST_CASE("DateTime::NowOverride::SetAndClearDirectly", "[datetime]") {
    auto const fixed = sample();
    morph::time::setNowOverride([fixed] { return fixed; });
    CHECK(DateTime::now() == fixed);

    morph::time::setNowOverride(nullptr);
    CHECK(DateTime::now() != fixed);
}

TEST_CASE("Timestamp::Difference", "[datetime][forms]") {
    auto const earlier = Timestamp{sample()};
    auto const later = Timestamp{sample() + std::chrono::minutes{5}};

    // Both engaged: the signed chrono duration between the instants.
    auto const delta = later - earlier;
    REQUIRE(delta.has_value());
    CHECK(*delta == std::chrono::minutes{5});
    CHECK((earlier - later).value() == -std::chrono::minutes{5});

    // Either operand empty collapses to nullopt.
    Timestamp const blank;
    CHECK_FALSE((later - blank).has_value());
    CHECK_FALSE((blank - later).has_value());
    CHECK_FALSE((blank - blank).has_value());
}
