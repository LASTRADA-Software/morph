// SPDX-License-Identifier: Apache-2.0
//
// The locale table: which facts a locale tag finds, and what `number` and `money` write once `Session::setLocale`
// has installed them.

#include <catch2/catch_test_macros.hpp>
#include <morph/ui/catalogue.hpp>
#include <morph/ui/date_pattern.hpp>
#include <morph/ui/locale_table.hpp>
#include <string>
#include <utility>

#include "ui_interpreter_support.hpp"

namespace {

constexpr char const* kAmounts = R"({
  "vocab": "ui/1", "id": "amounts",
  "view": { "kind": "column", "children": [
    { "kind": "text", "testId": "eur",
      "text": { "fn": "money", "args": [{ "lit": { "num": 1319640, "den": 100, "dp": 2 } }, "EUR"] } },
    { "kind": "text", "testId": "jpy", "text": { "fn": "money", "args": [1234567, "JPY"] } },
    { "kind": "text", "testId": "chf",
      "text": { "fn": "money", "args": [{ "lit": { "num": 245035, "den": 100, "dp": 2 } }, "CHF"] } },
    { "kind": "text", "testId": "debt",
      "text": { "fn": "money", "args": [{ "lit": { "num": -1300, "den": 100, "dp": 2 } }, "EUR"] } },
    { "kind": "text", "testId": "unlisted", "text": { "fn": "money", "args": [5, "XTS"] } },
    { "kind": "text", "testId": "code",
      "text": { "fn": "money", "args": [{ "lit": { "num": 1319640, "den": 100, "dp": 2 } }, "EUR", "code"] } },
    { "kind": "text", "testId": "number",
      "text": { "fn": "number", "args": [{ "lit": { "num": 12345675, "den": 10, "dp": 1 } }] } },
    { "kind": "text", "testId": "year", "text": { "ref": "y" } },
    { "kind": "text", "testId": "yearFormat", "text": { "fn": "format", "args": ["Year {0}", 2026] } },
    { "kind": "text", "testId": "yearNumber", "text": { "fn": "number", "args": [2026] } },
    { "kind": "text", "testId": "plain",
      "text": { "fn": "format", "args": ["Cost {0}", { "lit": { "num": 123456, "den": 100, "dp": 2 } }] } } ] },
  "state": { "y": { "type": "int", "init": 2026 } } })";

}  // namespace

// Mutation: drop the shorter-tag step from findLocaleFacts (de-AT finds nothing and falls back to en).
TEST_CASE("locale table: a tag finds its facts, then its language's, else none", "[ui][locale]") {
    REQUIRE(morph::ui::findLocaleFacts("en") != nullptr);
    CHECK(morph::ui::findLocaleFacts("en")->tag == "en");
    REQUIRE(morph::ui::findLocaleFacts("de-AT") != nullptr);
    CHECK(morph::ui::findLocaleFacts("de-AT")->tag == "de");
    CHECK(morph::ui::findLocaleFacts("pl") == nullptr);
    CHECK(morph::ui::localeFacts("pl").tag == "en");
}

// Mutation: put a pattern the date rules refuse in a row (checkDatePattern fails).
TEST_CASE("locale table: every pattern in the table is a valid date pattern", "[ui][locale]") {
    for (auto const& row : morph::ui::localeTable()) {
        if (!row.datePattern.empty()) {
            CHECK(morph::ui::checkDatePattern(row.datePattern, false).has_value());
        }
        if (!row.dateTimePattern.empty()) {
            CHECK(morph::ui::checkDatePattern(row.dateTimePattern, true).has_value());
        }
    }
}

// Mutations: skip the table switch in Session::setLocale (en reads 13196.40 EUR); ignore the placement in
// placeCurrency (de reads €13.196,40).
TEST_CASE("locale table: setLocale changes how numbers and money are written", "[ui][locale]") {
    morph::ui::testing::Harness h;
    auto const screen = h.mount(kAmounts);
    // A session nobody set a locale on keeps the evaluator's own forms.
    CHECK(h.text("eur") == "13196.40 EUR");

    h.session.setLocale("en");
    h.settle();
    CHECK(h.text("eur") == "€13,196.40");
    CHECK(h.text("jpy") == "¥1,234,567");
    CHECK(h.text("chf") == "CHF 2,450.35");
    CHECK(h.text("debt") == "-€13.00");
    CHECK(h.text("unlisted") == "5.00 XTS");
    CHECK(h.text("code") == "13,196.40 EUR");
    CHECK(h.text("number") == "1,234,567.5");

    h.session.setLocale("de");
    h.settle();
    CHECK(h.text("eur") == "13.196,40 €");
    CHECK(h.text("jpy") == "1.234.567 ¥");
    CHECK(h.text("chf") == "2.450,35 CHF");
    CHECK(h.text("debt") == "-13,00 €");
    CHECK(h.text("code") == "13.196,40 EUR");
    CHECK(h.text("number") == "1.234.567,5");
}

// Mutation: group an int in plain interpolation (withoutGrouping dropped from textOf); the year reads 2,026.
TEST_CASE("locale table: the group separator reaches number and money, not plain text, format or interpolation",
          "[ui][locale]") {
    morph::ui::testing::Harness h;
    auto const screen = h.mount(kAmounts);
    h.session.setLocale("en");
    h.settle();
    CHECK(h.text("year") == "2026");
    CHECK(h.text("yearFormat") == "Year 2026");
    CHECK(h.text("yearNumber") == "2,026");
    // The decimal separator follows the locale wherever a number is shown; the grouping does not.
    CHECK(h.text("plain") == "Cost 1234.56");
    h.session.setLocale("de");
    h.settle();
    CHECK(h.text("year") == "2026");
    CHECK(h.text("yearFormat") == "Year 2026");
    CHECK(h.text("yearNumber") == "2.026");
    CHECK(h.text("plain") == "Cost 1234,56");
}

// Mutation: let setLocale install the locale's date pattern although the application declared one (the declared
// case reads 09.11.2026).
TEST_CASE("locale table: a locale's date pattern applies only where the application declared none", "[ui][locale]") {
    morph::ui::testing::Harness h;
    auto const screen = h.mount(R"({
      "vocab": "ui/1", "id": "day",
      "view": { "kind": "text", "testId": "day", "text": { "fn": "date", "args": ["2026-11-09T09:00:00Z"] } } })");
    h.session.setLocale("de");
    h.settle();
    CHECK(h.text("day") == "09.11.2026");
    h.session.setDatePatterns("dd-MMM-yyyy", "dd-MMM-yyyy HH:mm");
    h.session.setLocale("en");
    h.session.setLocale("de");
    h.settle();
    CHECK(h.text("day") == "09-Nov-2026");
}

// Mutation: let a declared date pattern also hold the date-and-time pattern (the second reads 09-Nov-2026 09:00).
TEST_CASE("locale table: an application that declares one pattern gets the locale's for the other", "[ui][locale]") {
    morph::ui::testing::Harness h;
    auto const screen = h.mount(R"({
      "vocab": "ui/1", "id": "day",
      "view": { "kind": "column", "children": [
        { "kind": "text", "testId": "day", "text": { "fn": "date", "args": ["2026-11-09T09:00:00Z"] } },
        { "kind": "text", "testId": "time", "text": { "fn": "dateTime", "args": ["2026-11-09T09:00:00Z"] } } ] } })");
    h.session.setDatePatterns("dd-MMM-yyyy", "");
    h.session.setLocale("de");
    h.settle();
    CHECK(h.text("day") == "09-Nov-2026");
    CHECK(h.text("time") == "09.11.2026 09:00");
}

// Mutation: read the input's pattern untracked (the input keeps ISO after setLocale while the text reads 09.11.2026).
TEST_CASE("locale table: a date input follows the locale's date pattern with the texts beside it", "[ui][locale]") {
    morph::ui::testing::Harness h;
    auto const screen = h.mount(R"({
      "vocab": "ui/1", "id": "day",
      "state": { "when": { "init": "2026-11-09T09:00:00Z" } },
      "view": { "kind": "column", "children": [
        { "kind": "dateTimeInput", "testId": "input", "label": "When", "mode": "date", "value": { "ref": "when" } },
        { "kind": "text", "testId": "day", "text": { "fn": "date", "args": [{ "ref": "when" }] } } ] } })");
    auto const inputPattern = [&] { return h.backend.prop(h.byTestId("DateTimeInput", "input"), "pattern"); };
    CHECK(inputPattern().empty());
    CHECK(h.text("day") == "2026-11-09");
    h.session.setLocale("de");
    h.settle();
    CHECK(inputPattern() == "dd.MM.yyyy");
    CHECK(h.text("day") == "09.11.2026");
    h.session.setLocale("en");
    h.settle();
    CHECK(inputPattern().empty());
    CHECK(h.text("day") == "2026-11-09");
}

// Mutation: answer the text as written when the active locale has no catalogue (the pl case reads the raw key).
TEST_CASE("catalogue: a label falls back from the locale to en to any locale to the text", "[ui][locale][catalogue]") {
    morph::ui::MessageCatalogues catalogues;
    catalogues.add("en", std::move(*morph::ui::parseCatalogue(R"({"group.a": "Main work", "group.b": "Both"})")));
    catalogues.add("de", std::move(*morph::ui::parseCatalogue(R"({"group.b": "Beide", "group.c": "Nur deutsch"})")));
    CHECK(morph::ui::catalogLabel(catalogues, "group.a", "en") == "Main work");
    CHECK(morph::ui::catalogLabel(catalogues, "group.b", "de") == "Beide");
    CHECK(morph::ui::catalogLabel(catalogues, "group.a", "de") == "Main work");
    // No catalogue for pl: en's words, not the key.
    CHECK(morph::ui::catalogLabel(catalogues, "group.a", "pl") == "Main work");
    // A key only another locale holds.
    CHECK(morph::ui::catalogLabel(catalogues, "group.c", "pl") == "Nur deutsch");
    CHECK(morph::ui::catalogLabel(catalogues, "Plain heading", "de") == "Plain heading");
}
