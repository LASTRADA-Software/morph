// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <expected>
#include <limits>
#include <morph/render/locale_format.hpp>
#include <morph/ui/message.hpp>
#include <morph/util/rational.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ui = morph::ui;
namespace math = morph::math;

namespace {

using Arguments = std::vector<std::pair<std::string, ui::MessageArgument>>;

[[nodiscard]] ui::MessageArgument count(std::int64_t value, std::uint32_t places = 0) {
    return ui::MessageArgument{
        .number = math::Rational{value, math::DecimalPlaces{places}}, .places = places, .text = {}};
}

[[nodiscard]] ui::MessageArgument decimal(std::int64_t num, std::int64_t den, std::uint32_t places) {
    return ui::MessageArgument{
        .number = math::Rational{math::Numerator{num}, math::Denominator{den}, math::DecimalPlaces{places}},
        .places = places,
        .text = {}};
}

[[nodiscard]] ui::MessageArgument text(std::string value) {
    return ui::MessageArgument{.number = std::nullopt, .places = 0, .text = std::move(value)};
}

[[nodiscard]] std::string formatted(std::string_view pattern, Arguments const& arguments, std::string_view locale,
                                    morph::render::NumericLocale const& numeric = {}) {
    std::expected<ui::Message, std::string> const message = ui::parseMessage(pattern);
    INFO(pattern);
    if (!message) {
        INFO(message.error());
    }
    REQUIRE(message.has_value());
    std::expected<std::string, std::string> out = ui::formatMessage(*message, arguments, locale, numeric);
    if (!out) {
        INFO(out.error());
    }
    REQUIRE(out.has_value());
    return *out;
}

[[nodiscard]] std::string category(std::string_view locale, std::string_view shown) {
    return std::string{ui::pluralKeyword(ui::pluralCategory(locale, ui::pluralOperands(shown)))};
}

constexpr std::string_view kEnglishSamples = ".input {$n :number} .match $n one {{{$n} sample}} * {{{$n} samples}}";
constexpr std::string_view kPolishSamples =
    ".input {$n :number} .match $n one {{{$n} próbka}} few {{{$n} próbki}} many {{{$n} próbek}} * {{{$n} próbki}}";

}  // namespace

// Mutation: round half away from zero in decimalText (5/2 at 0 places then reads 3).
TEST_CASE("ui::decimalText: exact digits at the places, rounded half to even", "[ui][message]") {
    CHECK(ui::decimalText(math::Rational{math::Numerator{-3}, math::Denominator{2}, math::DecimalPlaces{2}}, 2) ==
          "-1.50");
    CHECK(ui::decimalText(math::Rational{math::Numerator{1}, math::Denominator{3}, math::DecimalPlaces{3}}, 3) ==
          "0.333");
    CHECK(ui::decimalText(math::Rational{math::Numerator{5}, math::Denominator{2}, math::DecimalPlaces{1}}, 0) == "2");
    CHECK(ui::decimalText(math::Rational{math::Numerator{7}, math::Denominator{2}, math::DecimalPlaces{1}}, 0) == "4");
    CHECK(ui::decimalText(math::Rational{std::numeric_limits<std::int64_t>::min(), math::DecimalPlaces{0}}, 0) ==
          "-9223372036854775808");
}

// Mutations: drop Polish's 12-14 exclusion from `few`; read Arabic's `few` and `many` from `n` instead of
// `n % 100`; let a shown fraction digit count as an integer in English.
TEST_CASE("ui::pluralCategory: CLDR's cardinal rules for en, de, fr, pl and ar", "[ui][message]") {
    struct Row {
        std::string_view locale;
        std::string_view shown;
        std::string_view category;
    };
    for (Row const& row : {
             Row{"en", "1", "one"},        Row{"en-GB", "1.0", "other"},    Row{"en", "0", "other"},
             Row{"en", "2", "other"},      Row{"de-DE", "1", "one"},        Row{"de", "21", "other"},
             Row{"fr", "0", "one"},        Row{"fr-FR", "1.5", "one"},      Row{"fr", "2", "other"},
             Row{"fr", "1000000", "many"}, Row{"fr", "1000000.0", "other"}, Row{"pl", "1", "one"},
             Row{"pl", "2", "few"},        Row{"pl", "4", "few"},           Row{"pl", "5", "many"},
             Row{"pl", "12", "many"},      Row{"pl", "14", "many"},         Row{"pl", "22", "few"},
             Row{"pl", "112", "many"},     Row{"pl", "0", "many"},          Row{"pl", "1.5", "other"},
             Row{"ar", "0", "zero"},       Row{"ar", "1", "one"},           Row{"ar", "2", "two"},
             Row{"ar", "3", "few"},        Row{"ar", "103", "few"},         Row{"ar", "11", "many"},
             Row{"ar", "199", "many"},     Row{"ar", "100", "other"},       Row{"ar", "2.5", "other"},
             Row{"ar", "3.0", "few"},      Row{"ja", "1", "other"},
         }) {
        INFO(row.locale << " " << row.shown);
        CHECK(category(row.locale, row.shown) == row.category);
    }
}

// Mutation: take the first variant that matches instead of the best (the category then wins over `=0`).
TEST_CASE("ui::formatMessage: .match picks the exact value, then the plural category, then *", "[ui][message]") {
    CHECK(formatted(kEnglishSamples, {{"n", count(1)}}, "en") == "1 sample");
    CHECK(formatted(kEnglishSamples, {{"n", count(3)}}, "en") == "3 samples");
    for (auto const& [n, expected] : std::vector<std::pair<std::int64_t, std::string>>{
             {1, "1 próbka"}, {2, "2 próbki"}, {5, "5 próbek"}, {12, "12 próbek"}, {22, "22 próbki"}}) {
        CHECK(formatted(kPolishSamples, {{"n", count(n)}}, "pl-PL") == expected);
    }
    std::string_view const zero = ".input {$n :number} .match $n * {{{$n} items}} one {{one item}} =0 {{no items}}";
    CHECK(formatted(zero, {{"n", count(0)}}, "en") == "no items");
    CHECK(formatted(zero, {{"n", count(1)}}, "en") == "one item");
    CHECK(formatted(zero, {{"n", count(7)}}, "en") == "7 items");
    std::string_view const bare = ".input {$n :number} .match $n 0 {{none}} * {{some}}";
    CHECK(formatted(bare, {{"n", count(0)}}, "en") == "none");
    std::string_view const select =
        ".input {$s :string} .match $s draft {{Draft}} |in review| {{Under review}} * {{Other}}";
    CHECK(formatted(select, {{"s", text("in review")}}, "en") == "Under review");
    CHECK(formatted(select, {{"s", text("draft")}}, "en") == "Draft");
    CHECK(formatted(select, {{"s", text("closed")}}, "en") == "Other");
    std::string_view const two =
        ".input {$a :number} .input {$b :string} .match $a $b one x {{one x}} * x {{many x}} * * {{other}}";
    CHECK(formatted(two, {{"a", count(1)}, {"b", text("x")}}, "en") == "one x");
    CHECK(formatted(two, {{"a", count(2)}, {"b", text("x")}}, "en") == "many x");
    CHECK(formatted(two, {{"a", count(1)}, {"b", text("y")}}, "en") == "other");
}

// Mutation: format a `:number` placeholder with decimalText alone (the locale's separators are then lost).
TEST_CASE("ui::formatMessage: placeholders, escapes and locale digits", "[ui][message]") {
    morph::render::NumericLocale const german{.decimalSeparator = ",", .groupSeparator = "."};
    CHECK(formatted("Total: {$t :number} kg", {{"t", decimal(24'691, 20, 1)}}, "de", german) == "Total: 1.234,6 kg");
    CHECK(formatted("Hello, {$name}!", {{"name", text("Zoë")}}, "en") == "Hello, Zoë!");
    CHECK(formatted("Code {$n :string}", {{"n", count(1'234)}}, "de", german) == "Code 1234");
    CHECK(formatted(R"(\{literal\} \| \\)", {}, "en") == R"({literal} | \)");
    CHECK(formatted("{{Quoted {$n}}}", {{"n", count(4)}}, "en") == "Quoted 4");
    CHECK(formatted("", {}, "en").empty());
}

// Mutations: accept a `.match` with no all-`*` variant; accept an annotation other than :number and :string.
TEST_CASE("ui::parseMessage: a pattern outside the subset is refused with the reason", "[ui][message]") {
    struct Row {
        std::string_view pattern;
        std::string_view reason;
    };
    for (Row const& row : {
             Row{"Hello }", "an unescaped `}` at offset 6"},
             Row{"Hi {$name :datetime}", "only `:number` and `:string` are supported at offset 10"},
             Row{"Hi {#b}", "a placeholder names an argument, `{$name}` at offset 4"},
             Row{"Hi {$name style=bold}",
                 "a placeholder ends with `}`; options and markup are not supported at offset 10"},
             Row{R"(Bad \n)", "`\\` escapes only `{`, `}`, `|` and `\\` at offset 5"},
             Row{".local $x = {1} {{x}}", "`.local` is not supported at offset 6"},
             Row{".input {$n :number} .match $n one {{one}}",
                 "`.match` needs a variant whose every key is `*` at offset 41"},
             Row{".input {$n :number} .match $m * {{x}}",
                 "`.match` selects on `$m`, which no `.input` declares at offset 29"},
             Row{".input {$n} .match $n * {{x}}",
                 "`.input` declares `{$name :number}` or `{$name :string}` at offset 11"},
             Row{".input {$n :number} .match * {{x}}", "`.match` names at least one `$selector` at offset 27"},
             Row{".input {$n :number} .match $n * {x}", "a variant's pattern is quoted, `{{...}}` at offset 32"},
             Row{"{{open", "a quoted pattern ends with `}}` at offset 6"},
         }) {
        INFO(row.pattern);
        std::expected<ui::Message, std::string> const message = ui::parseMessage(row.pattern);
        REQUIRE_FALSE(message.has_value());
        CHECK(message.error() == row.reason);
    }
}

// Mutation: write a missing argument as empty text instead of refusing.
TEST_CASE("ui::formatMessage: a missing argument and a :number given text are refused", "[ui][message]") {
    std::expected<ui::Message, std::string> const english = ui::parseMessage(kEnglishSamples);
    REQUIRE(english.has_value());
    CHECK(ui::formatMessage(*english, {}, "en", {}).error() == "no argument `n`");
    CHECK(ui::formatMessage(*english, {{"n", text("three")}}, "en", {}).error() ==
          "`n` selects `:number` but is not a number");
    std::expected<ui::Message, std::string> const simple = ui::parseMessage("{$x :number} and {$y}");
    REQUIRE(simple.has_value());
    CHECK(ui::formatMessage(*simple, {{"x", count(1)}}, "en", {}).error() == "no argument `y`");
    CHECK(ui::formatMessage(*simple, {{"x", text("one")}, {"y", text("two")}}, "en", {}).error() ==
          "`x` is formatted `:number` but is not a number");
}
