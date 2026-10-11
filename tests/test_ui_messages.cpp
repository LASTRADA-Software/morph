// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <fstream>
#include <morph/ui/message.hpp>
#include <sstream>
#include <string>

#include "ui_corpus.hpp"

using morph::ui::categoryName;
using morph::ui::formatMessage;
using morph::ui::Json;
using morph::ui::MessageLocale;
using morph::ui::MessagePattern;
using morph::ui::MessageVariant;
using morph::ui::parseMessage;
using morph::ui::PluralCategory;
using morph::ui::pluralCategory;
using morph::ui::Value;
using morph::ui::ValueRecord;

namespace {

Json const& messagesCorpus() {
    static Json const corpus = morph::ui::testing::loadCorpus(MORPH_UI_MESSAGES_CORPUS);
    return corpus;
}

}  // namespace

// Mutation: make Polish `few` ignore the 12..14 exclusion -> the `pl` 12 and 14 cases fail.
TEST_CASE("ui messages: CLDR plural categories per locale", "[ui][messages]") {
    using morph::ui::testing::member;
    std::size_t checked = 0;
    for (Json const& group : std::get<Json::array_t>(member(messagesCorpus(), "plurals").data)) {
        std::string const locale = std::get<std::string>(member(group, "locale").data);
        for (Json const& entry : std::get<Json::array_t>(member(group, "cases").data)) {
            auto const& pair = std::get<Json::array_t>(entry.data);
            auto const value = morph::ui::valueFromJson(pair.at(0));
            REQUIRE(value.has_value());
            auto const number = morph::ui::detail::numberOf(*value);
            REQUIRE(number.has_value());
            CAPTURE(locale, glz::write_json(pair.at(0)).value_or("?"));
            CHECK(categoryName(pluralCategory(locale, number->first, number->second)) ==
                  std::get<std::string>(pair.at(1).data));
            ++checked;
        }
    }
    CHECK(checked > 60);
}

// Mutation: prefer a category over an exact key -> "an exact key beats a category" fails.
TEST_CASE("ui messages: the MessageFormat 2 corpus", "[ui][messages]") {
    using morph::ui::testing::member;
    using morph::ui::testing::optionalMember;
    for (Json const& entry : std::get<Json::array_t>(member(messagesCorpus(), "messages").data)) {
        std::string const name = std::get<std::string>(member(entry, "name").data);
        std::string const pattern = std::get<std::string>(member(entry, "pattern").data);
        CAPTURE(name, pattern);
        auto parsed = parseMessage(pattern);
        if (Json const* const refused = optionalMember(entry, "refused")) {
            REQUIRE_FALSE(parsed.has_value());
            CAPTURE(parsed.error());
            CHECK(parsed.error().find(std::get<std::string>(refused->data)) != std::string::npos);
            continue;
        }
        if (!parsed.has_value()) {
            FAIL("refused: " << parsed.error());
        }
        MessageLocale locale;
        if (Json const* const named = optionalMember(entry, "locale")) {
            locale.locale = std::get<std::string>(named->data);
        }
        ValueRecord args;
        if (Json const* const given = optionalMember(entry, "args")) {
            auto value = morph::ui::valueFromJson(*given);
            REQUIRE(value.has_value());
            args = *value->asRecord();
        }
        auto const formatted = formatMessage(*parsed, args, locale);
        CHECK(formatted.text == std::get<std::string>(member(entry, "expect").data));
        std::size_t expectedErrors = 0;
        if (Json const* const errors = optionalMember(entry, "errors")) {
            expectedErrors = static_cast<std::size_t>(std::get<std::uint64_t>(errors->data));
        }
        CHECK(formatted.errors.size() == expectedErrors);
    }
}

// Mutation: format numbers without the locale -> the grouped text check fails.
TEST_CASE("ui messages: numbers take the locale's separators and digits", "[ui][messages]") {
    MessageLocale locale;
    locale.locale = "de";
    locale.numeric = morph::render::NumericLocale{.decimalSeparator = ",", .groupSeparator = "."};
    auto const parsed = parseMessage("{$n :number}");
    REQUIRE(parsed.has_value());
    CHECK(formatMessage(*parsed, ValueRecord{{"n", 1234567}}, locale).text == "1.234.567");
    CHECK(morph::ui::formatNumber(Value{"x"}, locale.numeric) == std::nullopt);
    CHECK(morph::ui::detail::decimalText(
              morph::math::Rational{morph::math::Numerator{-1}, morph::math::Denominator{1000},
                                    morph::math::DecimalPlaces{3}},
              1) == "0.0");
    CHECK(
        morph::ui::detail::decimalText(morph::math::Rational{morph::math::Numerator{-15}, morph::math::Denominator{10},
                                                             morph::math::DecimalPlaces{1}},
                                       0) == "-2");
}

// Mutation: drop the catch-all guard in formatMessage -> a hand-built pattern without one crashes.
TEST_CASE("ui messages: a hand-built match without a catch-all formats nothing", "[ui][messages]") {
    MessagePattern pattern;
    pattern.inputs.emplace_back("n", morph::ui::MessageFunction::Number);
    pattern.selectors.emplace_back("n");
    pattern.variants.push_back(MessageVariant{.keys = {"one"}, .pattern = {}});
    CHECK(formatMessage(pattern, ValueRecord{{"n", 5}}, MessageLocale{}).text.empty());
    for (auto const category : {PluralCategory::Zero, PluralCategory::One, PluralCategory::Two, PluralCategory::Few,
                                PluralCategory::Many, PluralCategory::Other}) {
        CHECK_FALSE(categoryName(category).empty());
    }
}
