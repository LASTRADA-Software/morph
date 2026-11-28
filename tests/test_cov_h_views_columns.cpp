// SPDX-License-Identifier: Apache-2.0

// The column a row type's schema gives a table: its kind from the property's type, format and widget, and what it
// carries of the schema's definitions, read from schemas written by hand so that each shape is the only one present.

#include <catch2/catch_test_macros.hpp>
#include <glaze/glaze.hpp>
#include <morph/forms/views.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace views = morph::views::detail;

glz::generic_u64 domOf(std::string_view json) {
    glz::generic_u64 dom{};
    REQUIRE_FALSE(glz::read_json(dom, std::string{json}));
    return dom;
}

// The entry of the one property `p` of @p schema, as compact JSON.
std::string entryOf(std::string_view schema, std::string label = "P", bool hidden = false) {
    auto const dom = domOf(schema);
    auto const entry = views::buildColumnEntry(dom, "p", label, hidden);
    return glz::write_json(entry).value_or("");
}

}  // namespace

TEST_CASE("cov h views: a column's kind follows the property's measure, type, widget and format", "[covh][views]") {
    struct Case {
        char const* property;
        char const* kind;
    };
    for (Case const& item : std::vector<Case>{
             {R"({"x-measure":true})", "measure"},
             {R"({"x-measure":false,"type":"string"})", "text"},
             {R"({"x-measure":"yes","type":"string"})", "text"},
             {R"({"x-comparator":"natural","x-measure":true})", "custom"},
             {R"({"type":"integer"})", "integer"},
             {R"({"type":"integer","x-widget":"epochDays"})", "date"},
             {R"({"type":"integer","x-widget":"epochSeconds"})", "dateTime"},
             {R"({"type":"number"})", "number"},
             {R"({"type":"boolean"})", "bool"},
             {R"({"type":"string","format":"date"})", "date"},
             {R"({"type":"string","format":"date-time"})", "dateTime"},
             {R"({"type":"string"})", "text"},
             {R"({})", "text"},
             {R"({"type":"object","properties":{"num":{"type":"integer"},"den":{"type":"integer"}}})", "decimal"},
             {R"({"type":"object","ExtUnits":{"unitAscii":"kg"},"properties":{"num":{},"den":{}}})", "quantity"},
             {R"({"type":"object","properties":{"num":{"type":"integer"}}})", "text"},
         }) {
        std::string const schema = std::string{R"({"properties":{"p":)"} + item.property + "}}";
        INFO(schema);
        CHECK(entryOf(schema).contains(std::string{R"("kind":")"} + item.kind + "\""));
    }
}

TEST_CASE(
    "cov h views: a nullable property reads through its non-null alternative and a reference through its definition",
    "[covh][views]") {
    struct Case {
        char const* schema;
        char const* kind;
    };
    for (
        Case const& item : std::vector<Case>{
            {R"({"properties":{"p":{"anyOf":[{"type":"null"},{"type":"integer"}]}}})", "integer"},
            {R"({"properties":{"p":{"anyOf":[{"type":"integer"},{"type":"null"}]}}})", "integer"},
            {R"({"properties":{"p":{"anyOf":[{"type":"null"}]}}})", "text"},
            {R"({"properties":{"p":{"anyOf":[{"type":7},{"type":"boolean"}]}}})", "text"},
            {R"({"properties":{"p":{"anyOf":[{"$ref":"#/$defs/n"},{"type":"null"}]}},"$defs":{"n":{"type":"number"}}})",
             "number"},
            {R"({"properties":{"p":{"$ref":"#/$defs/n"}},"$defs":{"n":{"type":"boolean"}}})", "bool"},
            {R"({"properties":{"p":{"$ref":"#/$defs/missing"}},"$defs":{"n":{"type":"boolean"}}})", "text"},
            {R"({"properties":{"p":{"$ref":"#/$defs/n"}}})", "text"},
            {R"({"properties":{"p":{"$ref":5,"type":"integer"}}})", "integer"},
        }) {
        INFO(item.schema);
        CHECK(entryOf(item.schema).contains(std::string{R"("kind":")"} + item.kind + "\""));
    }
}

TEST_CASE("cov h views: a column carries the closed set and the units its schema declares", "[covh][views]") {
    CHECK(entryOf(R"({"properties":{"p":{"enum":["a","b"]}}})").contains(R"("enum":["a","b"])"));
    CHECK(entryOf(R"({"properties":{"p":{"oneOf":[{"const":"a"},{"const":"b"},{"title":"no const"}]}}})")
              .contains(R"("enum":["a","b"])"));
    CHECK_FALSE(entryOf(R"({"properties":{"p":{"oneOf":[{"title":"x"}]}}})").contains("enum"));
    CHECK_FALSE(entryOf(R"({"properties":{"p":{"oneOf":3}}})").contains("enum"));
    CHECK(entryOf(R"({"properties":{"p":{"$ref":"#/$defs/e"}},"$defs":{"e":{"enum":[1,2]}}})")
              .contains(R"("enum":[1,2])"));
    CHECK(entryOf(R"({"properties":{"p":{"ExtUnits":{"unitAscii":"kg"}}}})")
              .contains(R"("ExtUnits":{"unitAscii":"kg"})"));
    CHECK(entryOf(R"({"properties":{"p":{"$ref":"#/$defs/q"}},"$defs":{"q":{"ExtUnits":{"unitAscii":"g"}}}})")
              .contains(R"("ExtUnits":{"unitAscii":"g"})"));
    CHECK(entryOf(R"({"properties":{"p":{"x-decimalPlaces":3,"title":"Mass","x-level":"detail"}}})")
              .contains(R"("x-decimalPlaces":3)"));
    auto const hidden = entryOf(R"({"properties":{"p":{"type":"string"}}})", "Label", true);
    CHECK(hidden.contains(R"("v-hidden":true)"));
    CHECK(hidden.contains(R"("label":"Label")"));
}

TEST_CASE("cov h views: a field the row type does not declare is a bare column", "[covh][views]") {
    auto const dom = domOf(R"({"properties":{"q":{"type":"integer"}}})");
    auto const entry = glz::write_json(views::buildColumnEntry(dom, "p", "P", false)).value_or("");
    CHECK(entry.contains(R"("field":"p")"));
    CHECK_FALSE(entry.contains("kind"));
}
