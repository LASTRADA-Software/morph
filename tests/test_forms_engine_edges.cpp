// SPDX-License-Identifier: Apache-2.0
//
// The forms engine's edges: the malformed, out-of-range and rarely-taken inputs of the text helpers, the rule reader,
// the encoders and decoders, the schema reader and the session, each checked for what it answers rather than only
// reached.

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <limits>
#include <morph/forms/detail/schema_name.hpp>
#include <morph/forms/engine/detail/text.hpp>
#include <morph/forms/engine/field_model.hpp>
#include <morph/forms/engine/form_session.hpp>
#include <morph/forms/engine/rules.hpp>
#include <morph/forms/engine/values.hpp>
#include <morph/forms/forms.hpp>
#include <morph/forms/i18n.hpp>
#include <morph/forms/instance_constraints.hpp>
#include <morph/forms/layout.hpp>
#include <morph/util/rational.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "forms_engine_support.hpp"

namespace {

namespace engine = morph::forms::detail::engine;
using morph::forms::ChoiceSpec;
using morph::forms::FieldKind;
using morph::forms::FieldSpec;
using morph::forms::FormModel;
using morph::forms::RuleExpr;
using morph::forms::RuleOperand;
using morph::forms::Tri;
using morph::forms::UnitOption;
using morph::forms::ValueContext;

[[nodiscard]] engine::Json json(std::string_view text) {
    auto parsed = engine::parseJson(text);
    REQUIRE(parsed.has_value());
    return *std::move(parsed);
}

[[nodiscard]] std::optional<std::string> encoded(FieldSpec const& spec, std::string_view text, std::size_t unit = 0) {
    auto const result = morph::forms::encodeScalar(spec, text, unit, ValueContext{});
    if (!result.has_value() || !result->has_value()) {
        return std::nullopt;
    }
    return (*result)->json;
}

[[nodiscard]] std::string errorOf(FieldSpec const& spec, std::string_view text, std::size_t unit = 0) {
    auto const result = morph::forms::encodeScalar(spec, text, unit, ValueContext{});
    if (!result.has_value() || result->has_value()) {
        return {};
    }
    return result->error().code;
}

[[nodiscard]] FieldSpec specOf(FieldKind kind) {
    FieldSpec spec{};
    spec.name = "f";
    spec.path = "f";
    spec.kind = kind;
    return spec;
}

[[nodiscard]] std::string decoded(FieldSpec const& spec, std::string_view value) {
    return morph::forms::decodeScalar(spec, json(value), ValueContext{});
}

[[nodiscard]] std::expected<RuleExpr, morph::forms::SchemaError> rule(std::string_view text) {
    return RuleExpr::parse(json(text), "x-rules[0]");
}

[[nodiscard]] std::string ruleError(std::string_view text) {
    auto const parsed = rule(text);
    return parsed.has_value() ? std::string{} : parsed.error().message;
}

[[nodiscard]] std::expected<FormModel, morph::forms::SchemaError> model(std::string_view schema) {
    return FormModel::fromSchema("Edge", schema);
}

}  // namespace

TEST_CASE("engine edges: quoting, number text and display text", "[forms-engine][edges]") {
    CHECK(engine::quote("\b\f") == R"("\b\f")");
    engine::Json number{};
    number = std::numeric_limits<double>::infinity();
    REQUIRE(number.get_if<double>() != nullptr);
    CHECK_FALSE(engine::numberText(number).has_value());
    number = -0.0;
    CHECK(engine::numberText(number) == "0");
    CHECK_FALSE(engine::numberText(json(R"("text")")).has_value());
    CHECK(engine::displayText(json("true")) == "true");
    CHECK(engine::displayText(json("false")) == "false");
    CHECK(engine::displayText(json("null")).empty());
    CHECK(engine::displayText(json(R"({"a":1})")) == R"({"a":1})");
    CHECK_FALSE(engine::isCanonicalDecimal("1."));
    CHECK_FALSE(engine::isCanonicalDecimal("1.x"));
    CHECK(engine::compareRationalToDecimal(morph::math::Rational{}, "0") == std::strong_ordering::equal);
    CHECK(engine::compareRationalToDecimal(morph::math::Rational{}, "0.000") == std::strong_ordering::equal);
}

TEST_CASE("engine edges: wall-clock text is refused component by component", "[forms-engine][edges]") {
    CHECK_FALSE(engine::digitsAt("12", 1, 2).has_value());
    CHECK_FALSE(engine::digitsAt("1a", 0, 2).has_value());
    CHECK_FALSE(engine::digitsAt("1/", 0, 2).has_value());
    for (std::string_view const text :
         {"2024/01-01T10:00", "2024-01/01T10:00", "2024-01-01 10:00", "2024-01-01T10-00", "2024-01-01T10:00-00",
          "20a4-01-01T10:00", "2024-a1-01T10:00", "2024-01-a1T10:00", "2024-01-01Ta0:00", "2024-01-01T10:a0",
          "2024-01-01T10:00:a0", "2024-01-01T24:00", "2024-01-01T10:60", "2024-01-01T10:00:60", "2024-02-30T10:00",
          "2024-01-01T10"}) {
        INFO(text);
        CHECK_FALSE(engine::parseWallClock(text).has_value());
    }
    auto const instant = [](std::string_view text) { return engine::parseInstant(text); };
    auto const at = [](std::string_view wall) { return engine::parseWallClock(wall).value(); };
    CHECK(instant("2024-01-01T10:00") == at("2024-01-01T10:00"));
    CHECK(instant("2024-01-01T10:00:05.250Z") == at("2024-01-01T10:00:05"));
    CHECK(instant("2024-01-01T10:00:00+01:30") == at("2024-01-01T08:30:00"));
    CHECK(instant("2024-01-01T10:00:00-0130") == at("2024-01-01T11:30:00"));
    for (std::string_view const text :
         {"2024-01-01T25:00", "2024-01-01T10:00:00X", "2024-01-01T10:00:00+1", "2024-01-01T10:00:00+ab:cd",
          "2024-01-01T10:00:00+01:3x", "2024-01-01T10:00:00+01-30"}) {
        INFO(text);
        CHECK_FALSE(instant(text).has_value());
    }
    for (std::string_view const text :
         {"2024-01-1", "2024/01-01", "2024-01/01", "aaaa-01-01", "2024-0a-01", "2024-01-0a", "2024-02-30"}) {
        INFO(text);
        CHECK_FALSE(engine::isIsoDate(text));
    }
}

TEST_CASE("engine edges: a malformed rule is refused with the reason", "[forms-engine][edges]") {
    CHECK(ruleError("1") == "a rule must be a JSON object");
    CHECK(ruleError(R"({"fields":["a"]})") == R"(a rule needs a string "kind")");
    CHECK(ruleError(R"({"kind":"engaged","fields":"a"})") == R"("fields" must be an array of strings)");
    CHECK(ruleError(R"({"kind":"engaged","fields":[1]})") == R"("fields" must be an array of strings)");
    CHECK(ruleError(R"({"kind":"not"})") == R"(not needs "condition")");
    CHECK(ruleError(R"({"kind":"not","condition":{"kind":"engaged"}})") == R"("engaged" needs at least 1 field(s))");
    CHECK(ruleError(R"({"kind":"and"})") == R"("and" needs a "conditions" array)");
    CHECK(ruleError(R"({"kind":"or","conditions":{}})") == R"("or" needs a "conditions" array)");
    CHECK(ruleError(R"({"kind":"and","conditions":[{"kind":"less","fields":["a"]}]})") ==
          R"("less" needs exactly 2 field(s))");
    CHECK(ruleError(R"({"kind":"visibleWhen","when":{"kind":"engaged","fields":["a"]}})") ==
          R"("visibleWhen" needs at least 1 field(s))");
    auto const unknown = rule(R"({"kind":"someday","fields":["a"]})");
    REQUIRE(unknown.has_value());
    CHECK(unknown->kind() == RuleExpr::Kind::Unknown);
    CHECK(morph::forms::triNot(Tri::Unknown) == Tri::Unknown);
}

TEST_CASE("engine edges: an equals literal is exact, or null when the language cannot hold it",
          "[forms-engine][edges]") {
    auto const literal = [](std::string_view text) {
        auto const parsed = rule(text);
        REQUIRE(parsed.has_value());
        return engine::toJson(parsed->expression());
    };
    CHECK(literal(R"({"kind":"equals","fields":["a"],"valueText":"99999999999999999999"})").contains("null"));
    CHECK(literal(R"({"kind":"equals","fields":["a"]})").contains("null"));
    CHECK(literal(R"({"kind":"equals","fields":["a"],"value":true})").contains("true"));
    CHECK(literal(R"({"kind":"equals","fields":["a"],"value":"x"})").contains(R"("x")"));
    CHECK(literal(R"({"kind":"equals","fields":["a"],"value":18446744073709551615})").contains("null"));
    CHECK(literal(R"({"kind":"equals","fields":["a"],"value":1.5})").contains(R"("num":3)"));
    CHECK(literal(R"({"kind":"equals","fields":["a"],"value":{"num":3,"den":2,"dp":1}})").contains(R"("num":3)"));
    CHECK(literal(R"({"kind":"equals","fields":["a"],"value":{"a":1}})").contains("null"));
    CHECK(literal(R"({"kind":"equals","fields":["a"],"value":[1]})").contains("null"));

    // An empty `and` holds and an empty `or` does not.
    auto const none = [](std::string_view) { return RuleOperand{}; };
    auto const empty = rule(R"({"kind":"and","conditions":[]})");
    REQUIRE(empty.has_value());
    CHECK(empty->evaluate(none) == Tri::True);
    auto const neither = rule(R"({"kind":"or","conditions":[]})");
    REQUIRE(neither.has_value());
    CHECK(neither->evaluate(none) == Tri::False);
    CHECK(neither->whenHolds(none) == Tri::Unknown);

    // Two presentation rules both undecided leave the field undecided.
    std::vector<RuleExpr> rules;
    for (std::string_view const text :
         {R"({"kind":"visibleWhen","fields":["x"],"when":{"kind":"engaged","fields":["a"]}})",
          R"({"kind":"visibleWhen","fields":["x"],"when":{"kind":"engaged","fields":["b"]}})"}) {
        auto parsed = rule(text);
        REQUIRE(parsed.has_value());
        rules.push_back(*std::move(parsed));
    }
    auto const undecided = [](std::string_view) {
        return RuleOperand{.state = RuleOperand::State::Invalid, .value = {}};
    };
    CHECK(morph::forms::visibleVerdict(rules, "x", undecided) == Tri::Unknown);
}

TEST_CASE("engine edges: schema readers take integers from every JSON number shape", "[forms-engine][edges]") {
    using Reader = engine::SchemaReader;
    engine::Json const negative = json("-3");
    engine::Json const integral = json("4.0");
    engine::Json const fraction = json("4.5");
    engine::Json const huge = json("1e30");
    engine::Json const text = json(R"("4")");
    engine::Json const big = json("18446744073709551615");
    CHECK(Reader::unsignedOf(nullptr) == std::nullopt);
    CHECK(Reader::unsignedOf(&negative) == std::nullopt);
    CHECK(Reader::unsignedOf(&integral) == 4U);
    CHECK(Reader::unsignedOf(&fraction) == std::nullopt);
    CHECK(Reader::unsignedOf(&huge) == std::nullopt);
    CHECK(Reader::unsignedOf(&text) == std::nullopt);
    CHECK(Reader::signedOf(&negative) == -3);
    CHECK(Reader::signedOf(&big) == std::nullopt);
    CHECK(Reader::signedOf(&fraction) == 4);
    CHECK(Reader::signedOf(&huge) == std::nullopt);
    CHECK(Reader::signedOf(&text) == std::nullopt);
}

TEST_CASE("engine edges: literals and digits become exact rule values", "[forms-engine][edges]") {
    CHECK(engine::valueOfLiteral(json("1.5")).asDecimal() != nullptr);
    CHECK(engine::valueOfLiteral(json(R"({"a":[1,{"b":2}]})")).isNull() == false);
    CHECK(engine::integerValue("99999999999999999999").isNull());
    CHECK(engine::decimalValue("1e5").isNull());
}

TEST_CASE("engine edges: encoders refuse what does not fit the field", "[forms-engine][edges]") {
    FieldSpec integer = specOf(FieldKind::Integer);
    integer.multipleOf = "5";
    CHECK(encoded(integer, "10") == "10");
    CHECK(errorOf(integer, "7") == "not-a-multiple");

    FieldSpec decimal = specOf(FieldKind::Decimal);
    decimal.decimalPlaces = 1;
    decimal.maximum = "10";
    decimal.multipleOf = "0.5";
    CHECK(encoded(decimal, "1.5", 7).has_value());  // a unit past the list is the field's own
    CHECK(errorOf(decimal, "10.5") == "above-maximum");
    CHECK(errorOf(decimal, "1.3") == "not-a-multiple");
    CHECK(errorOf(decimal, "abc") == "malformed");
    CHECK_FALSE(errorOf(decimal, "99999999999999999999999").empty());

    FieldSpec numbers = specOf(FieldKind::Array);
    numbers.itemType = "number";
    CHECK(encoded(numbers, "1.5, 2") == "[1.5,2]");
    CHECK(errorOf(numbers, "1.5, x") == "malformed");
    FieldSpec flags = specOf(FieldKind::Array);
    flags.itemType = "boolean";
    CHECK(encoded(flags, "true,false") == "[true,false]");
    CHECK(errorOf(flags, "maybe") == "malformed");

    CHECK(encoded(specOf(FieldKind::Multiline), "two\nlines") == R"("two\nlines")");
    // An Enum's draft is one of its option literals; a Choice's is whatever scalar its options carry.
    for (auto const& [kind, code] :
         {std::pair{FieldKind::Enum, "not-an-option"}, std::pair{FieldKind::Choice, "malformed"}}) {
        FieldSpec const choice = specOf(kind);
        CHECK(errorOf(choice, R"({"a":1})") == code);
        CHECK(errorOf(choice, "[1]") == code);
        CHECK(errorOf(choice, "null") == code);
        CHECK(errorOf(choice, "not json") == code);
    }
    CHECK(morph::forms::fieldError("no-such-code", ValueContext{}).message == "Not a valid value");
    FieldSpec slider = specOf(FieldKind::Slider);
    CHECK(encoded(slider, "4") == "4");
    slider.jsonType = "number";
    CHECK(encoded(slider, "4.5") == "4.5");
    CHECK_FALSE(encoded(specOf(FieldKind::Object), "{}").has_value());
    CHECK_FALSE(encoded(specOf(FieldKind::ObjectArray), "[]").has_value());
}

TEST_CASE("engine edges: decoders read a mismatched wire value as blank", "[forms-engine][edges]") {
    CHECK(decoded(specOf(FieldKind::Multiline), R"("a\nb")") == "a\nb");
    CHECK(decoded(specOf(FieldKind::Date), "5").empty());
    CHECK(decoded(specOf(FieldKind::DateTime), "5").empty());
    CHECK(decoded(specOf(FieldKind::DateTime), R"("not a time")").empty());
    CHECK(decoded(specOf(FieldKind::EpochDateTime), "1.5").empty());
    CHECK(decoded(specOf(FieldKind::EpochDate), R"("x")").empty());
    CHECK(decoded(specOf(FieldKind::Boolean), R"("yes")").empty());
    CHECK(decoded(specOf(FieldKind::Slider), "7") == "7");
    CHECK(decoded(specOf(FieldKind::Number), R"("x")").empty());
    CHECK(decoded(specOf(FieldKind::Decimal), R"({"num":1,"den":0})").empty());
    CHECK(decoded(specOf(FieldKind::Decimal), R"({"num":1})").empty());
    CHECK(decoded(specOf(FieldKind::Array), R"("a")").empty());
    CHECK(decoded(specOf(FieldKind::Object), R"({"a":1})").empty());
    CHECK(decoded(specOf(FieldKind::ObjectArray), "[]").empty());
}

TEST_CASE("engine edges: unit conversion refuses a degenerate unit or a draft that is not a number",
          "[forms-engine][edges]") {
    UnitOption const unit{.id = "g", .display = "g", .decimals = 1, .num = 1, .den = 1000};
    UnitOption const zeroDen{.id = "x", .display = "x", .decimals = 0, .num = 1, .den = 0};
    UnitOption const zeroNum{.id = "y", .display = "y", .decimals = 0, .num = 0, .den = 1};
    morph::render::NumericLocale const locale{};
    CHECK_FALSE(morph::forms::convertDraft("abc", unit, unit, locale).has_value());
    CHECK_FALSE(morph::forms::convertDraft("1", zeroDen, unit, locale).has_value());
    CHECK_FALSE(morph::forms::convertDraft("1", unit, zeroNum, locale).has_value());
}

TEST_CASE("engine edges: options come from the first array of a reply, skipping rows without a value",
          "[forms-engine][edges]") {
    ChoiceSpec const spec{};
    CHECK(morph::forms::parseOptions("not json", spec).empty());
    CHECK(morph::forms::parseOptions(R"({"total":2})", spec).empty());
    CHECK(morph::forms::parseOptions("5", spec).empty());
    auto const options = morph::forms::parseOptions(R"({"total":2,"rows":[{"name":"x"},{"id":7}]})", spec);
    REQUIRE(options.size() == 1);
    CHECK(options.front().valueJson == "7");
    CHECK(options.front().label == "7");  // no label member: the value is the label
}

TEST_CASE("engine edges: every kind has its default placeholder", "[forms-engine][edges]") {
    CHECK(morph::forms::defaultPlaceholder(specOf(FieldKind::Slider)) == "0");
    FieldSpec number = specOf(FieldKind::Number);
    CHECK(morph::forms::defaultPlaceholder(number).empty());
    number.displayDecimals = 0;
    CHECK(morph::forms::defaultPlaceholder(number) == "0");
    for (FieldKind const kind : {FieldKind::Multiline, FieldKind::EpochDateTime, FieldKind::EpochDate,
                                 FieldKind::Object, FieldKind::ObjectArray}) {
        CHECK(morph::forms::defaultPlaceholder(specOf(kind)).empty());
    }
    CHECK(morph::forms::fieldKindName(FieldKind::Multiline) == "multiline");
    CHECK(morph::forms::fieldKindName(FieldKind::EpochDateTime) == "epochDateTime");
    CHECK(morph::forms::fieldKindName(FieldKind::EpochDate) == "epochDate");
    CHECK(morph::forms::fieldKindName(FieldKind::Slider) == "slider");
}

TEST_CASE("engine edges: the schema reader's lenient readings", "[forms-engine][edges]") {
    auto const read = model(R"({
      "$defs": {"Code": {"type": "string", "title": "From the definition"}, "Odd": 5},
      "properties": {
        "code": {"$ref": "#/$defs/Code", "x-order": 0},
        "missing": {"$ref": "#/$defs/Nowhere", "type": "string", "x-order": 1},
        "odd": {"$ref": "#/$defs/Odd", "type": "string", "x-order": 2},
        "picked": {"anyOf": [{"type": "null"}, {"type": "integer", "title": "Picked"}], "x-order": 3},
        "one": {"oneOf": [{"type": "string"}], "x-order": 4},
        "level": {"enum": ["a", null, "b"], "x-order": 5},
        "shade": {"oneOf": [{"const": 1, "title": "One"}, {"const": 2}, {"type": "null"}], "x-order": 6},
        "mixed": {"oneOf": [{"const": 1}, {"type": "string"}], "x-order": 7},
        "total": {"type": "integer", "x-computed": {"inputs": ["code", 3]}, "x-order": 8},
        "lazy": {"type": "integer", "x-computed": {"inputs": "code"}, "x-order": 9},
        "noStep": {"type": "number", "multipleOf": 0, "x-order": 10},
        "exact": {"type": "integer", "x-exactMinimum": "1.5", "x-exactMaximum": "x", "x-order": 11},
        "bounded": {"type": "integer", "x-minimum": {"num": 1, "den": 0}, "x-maximum": {"num": 1}, "x-order": 12},
        "ascii": {"type": ["object", "null"], "x-decimalPlaces": 1, "ExtUnits": {"unitUnicode": ""}, "x-order": 13},
        "mass": {"type": "object", "x-decimalPlaces": 1, "ExtUnits": {"unitAscii": "kg"}, "x-order": 14,
                 "x-unitAlternatives": [{"id": "g", "num": 1, "den": 1000}, {"id": "bad", "num": 0}]},
        "money": {"type": "object", "x-decimalPlaces": 2, "x-unitAlternatives": [{"id": "x"}], "x-order": 15},
        "dial": {"type": "number", "x-widget": "slider", "x-min": 0, "x-max": 10, "x-order": 16},
        "unbounded": {"type": "integer", "x-widget": "slider", "x-order": 17},
        "list": {"type": "array", "x-order": 18},
        "words": {"type": "array", "items": {"type": ["string", 5]}, "x-order": 19},
        "rows": {"type": "array", "items": {"type": "array"}, "x-order": 20},
        "picks": {"type": "integer", "x-optionsAction": "Opts", "x-optionsDependsOn": ["code", 2], "x-order": 21},
        "notes": {"type": "string", "x-widget": "textarea", "x-blankAs": "empty", "x-order": 22}},
      "required": ["code", 7]})");
    INFO((read ? std::string{} : read.error().path + ": " + read.error().message));
    REQUIRE(read.has_value());
    auto const field = [&](std::string_view name) {
        FieldSpec const* const spec = read->find(name);
        REQUIRE(spec != nullptr);
        return *spec;
    };
    CHECK(field("code").title == "From the definition");
    CHECK(field("code").required);
    CHECK(field("missing").kind == FieldKind::Text);
    CHECK(field("odd").kind == FieldKind::Text);
    CHECK(field("picked").kind == FieldKind::Integer);
    CHECK(field("one").kind == FieldKind::Text);
    CHECK(field("level").enumOptions.size() == 2);
    REQUIRE(field("shade").enumOptions.size() == 2);
    CHECK(field("shade").enumOptions.front().label == "One");
    CHECK(field("shade").enumOptions.back().label == "2");
    CHECK(field("mixed").kind != FieldKind::Enum);  // a branch that is not a constant: not an enumeration
    CHECK(field("total").computedInputs == std::vector<std::string>{"code"});
    CHECK(field("lazy").computedInputs.empty());
    CHECK_FALSE(field("noStep").multipleOf.has_value());
    CHECK_FALSE(field("exact").exactMinimum.has_value());
    CHECK_FALSE(field("exact").exactMaximum.has_value());
    CHECK_FALSE(field("bounded").instanceMinimum.has_value());
    CHECK_FALSE(field("bounded").instanceMaximum.has_value());
    CHECK(field("ascii").unit.empty());
    CHECK(field("mass").kind == FieldKind::Quantity);
    REQUIRE(field("mass").units.size() == 2);
    CHECK(field("mass").units.back().display == "g");  // no display: the id
    CHECK(field("money").kind == FieldKind::Decimal);
    CHECK(field("money").units.size() == 1);  // alternatives are a Quantity's
    CHECK(field("dial").kind == FieldKind::Slider);
    CHECK(field("unbounded").kind == FieldKind::Integer);
    CHECK(field("list").itemType == "string");
    CHECK(field("words").itemType == "string");
    CHECK(field("rows").kind == FieldKind::ObjectArray);
    CHECK(field("rows").truncated);
    REQUIRE(field("picks").choice.has_value());
    CHECK(field("picks").choice->dependsOn == std::vector<std::string>{"code"});
    CHECK(field("notes").kind == FieldKind::Multiline);
    CHECK(read->find("nothing") == nullptr);
}

TEST_CASE("engine edges: a field's presentation from the schema and the translations", "[forms-engine][edges]") {
    morph::test::formsengine::Harness harness;
    morph::forms::FormSessionOptions options{};
    options.translations = [](std::string_view key, std::string_view) -> std::optional<std::string> {
        if (key == "custom.street.label") {
            return "Strasse";
        }
        return std::nullopt;
    };
    auto const session = harness.session("Edge", R"({"properties":{
      "secret":{"type":"string","x-hidden":true,"x-order":0},
      "fixed":{"type":"string","x-readonly":true,"x-placeholder":"as given","x-order":1},
      "home":{"type":"object","x-readonly":true,"x-order":2,"properties":{
        "street":{"type":"string","x-i18nKey":"custom.street"},
        "city":{"type":"string","x-i18nKey":"custom.city"}}}}})",
                                         std::move(options));
    harness.settle();
    CHECK_FALSE(session->field("secret").visible().get());
    CHECK(session->field("fixed").readonly().get());
    CHECK(session->field("fixed").placeholder() == "as given");
    CHECK(session->field("home.street").readonly().get());  // inside a read-only object
    CHECK(session->field("home.street").label() == "Strasse");
    CHECK(session->field("home.city").label() == "city");  // a key the catalog lacks: the name
}

TEST_CASE("engine edges: a collection nested too deeply to edit carries only what it was given",
          "[forms-engine][edges]") {
    morph::test::formsengine::Harness harness;
    constexpr std::string_view kSchema = R"({"properties":{
      "grid":{"type":"array","items":{"type":"array"},"x-order":0}},"required":["grid"]})";
    auto const session = harness.session("Edge", kSchema);
    harness.settle();
    CHECK(session->body() == R"({"grid":[]})");  // required and untouched: empty
    session->prefill(R"({"grid":[]})");
    harness.settle();
    CHECK(session->body() == R"({"grid":[]})");
    session->prefill(R"({"grid":[[1]]})");
    harness.settle();
    REQUIRE(session->field("grid").error().get().has_value());
    CHECK(session->field("grid").error().get()->code == "unrepresentable");
    session->prefill(R"({"grid":null})");
    harness.settle();
    CHECK(session->body() == R"({"grid":[]})");
}

TEST_CASE("engine edges: paths, assignments and unit switches that address nothing change nothing",
          "[forms-engine][edges]") {
    morph::test::formsengine::Harness harness;
    auto const session = harness.session("Edge", R"({"properties":{
      "name":{"type":"string","x-order":0},
      "mass":{"type":"object","x-decimalPlaces":1,"ExtUnits":{"unitAscii":"kg"},"x-order":1,
              "x-unitAlternatives":[{"id":"g","num":1,"den":1000}]},
      "rows":{"type":"array","x-order":2,"items":{"type":"object","properties":{"a":{"type":"string"}}}}}})");
    harness.settle();
    for (std::string_view const path : {"rows[0", "rows[x].a", "rows[].a", "rows[0].a", "name.x", "nothing"}) {
        INFO(path);
        CHECK_THROWS_AS(static_cast<void>(session->field(path)), std::out_of_range);
    }
    session->prefill(R"({"rows":5})");  // not a list: no rows
    harness.settle();
    CHECK(session->field("rows").rows().empty());
    session->assign("name", "not json");
    harness.settle();
    CHECK(session->field("name").text().get().empty());
    harness.type(*session, "mass", "1.5");
    session->field("mass").switchUnit(0);  // already the unit
    session->field("mass").switchUnit(9);  // no such unit
    CHECK(session->field("mass").text().get() == "1.5");
    CHECK(session->field("mass").unit().get() == 0U);
}

TEST_CASE("engine edges: a dependent Choice waits for parents that are known and encode", "[forms-engine][edges]") {
    morph::test::formsengine::Harness harness;
    auto const session = harness.session("Edge", R"({"properties":{
      "site":{"type":"integer","x-order":0},
      "room":{"type":"integer","x-optionsAction":"Rooms","x-optionsDependsOn":["site"],"x-order":1},
      "desk":{"type":"integer","x-optionsAction":"Desks","x-optionsDependsOn":["nowhere"],"x-order":2}}})");
    harness.settle();
    harness.type(*session, "site", "abc");
    std::size_t rooms = 0;
    std::size_t desks = 0;
    for (auto const& call : harness.server.fetches()) {
        rooms += call.action == "Rooms" ? 1U : 0U;
        desks += call.action == "Desks" ? 1U : 0U;
    }
    CHECK(rooms == 0);
    CHECK(desks == 0);
    harness.type(*session, "site", "4");
    REQUIRE_FALSE(harness.server.fetches().empty());
    CHECK(harness.server.fetches().back().body == R"({"site":4})");
}

TEST_CASE("engine edges: a malformed schema names where", "[forms-engine][edges]") {
    auto const where = [](std::string_view schema) {
        auto const read = model(schema);
        return read.has_value() ? std::string{"(read)"} : read.error().path;
    };
    CHECK(where(R"({"properties":{"a":{"type":"string"}},"x-rules":{}})") == "x-rules");
    CHECK(where(R"({"properties":{"a":{"type":"string"}},"x-rules":[{"kind":"not"}]})") == "x-rules[0]");
    CHECK(where(R"({"properties":{"o":{"type":"object","properties":{"bad":5}}}})") == "properties.o.bad");
    CHECK(where(R"({"properties":{"r":{"type":"array","items":{"type":"object","properties":{"bad":5}}}}})") ==
          "properties.r[].bad");
    auto const explicitMode = model(R"({"properties":{},"x-submitMode":"explicit"})");
    REQUIRE(explicitMode.has_value());
    CHECK(explicitMode->submitMode() == morph::forms::SubmitMode::Explicit);
    auto const grouped = model(R"({"properties":{"a":{"type":"string","x-section":9}},
      "x-layout":{"groups":[{"kind":"tab"},{"kind":"accordion","title":"More"},{}]}})");
    REQUIRE(grouped.has_value());
    REQUIRE(grouped->groups().size() == 4);
    CHECK(grouped->groups()[0].kind == morph::forms::GroupKind::Tab);
    CHECK(grouped->groups()[1].kind == morph::forms::GroupKind::Accordion);
    CHECK(grouped->groups()[2].kind == morph::forms::GroupKind::Section);
    CHECK(grouped->groups()[3].fields == std::vector<std::string>{"a"});  // a section past the list: implicit
}

TEST_CASE("engine edges: instants with a fraction and no zone, or a fraction before the zone",
          "[forms-engine][edges]") {
    auto const at = [](std::string_view wall) { return engine::parseWallClock(wall).value(); };
    CHECK(engine::parseInstant("2024-01-01T10:00:00.5") == at("2024-01-01T10:00:00"));
    CHECK(engine::parseInstant("2024-01-01T10:00:00.+01:00") == at("2024-01-01T09:00:00"));
    CHECK(engine::parseInstant("2024-01-01T10:00+01:00") == at("2024-01-01T09:00"));
    engine::Json const negative = json("-1.0");
    engine::Json const enormous = json("2e19");
    engine::Json const fraction = json("-2.5");
    CHECK(engine::SchemaReader::unsignedOf(&negative) == std::nullopt);
    CHECK(engine::SchemaReader::unsignedOf(&enormous) == std::nullopt);
    CHECK(engine::SchemaReader::signedOf(&fraction) == -2);
}

TEST_CASE("engine edges: keywords of the wrong shape are passed over", "[forms-engine][edges]") {
    auto const read = model(R"({"properties":{
      "a":{"anyOf":5,"oneOf":5,"type":"string","x-order":0},
      "b":{"anyOf":[{"type":"null"}],"type":"string","x-order":1},
      "c":{"type":5,"x-readonly":false,"x-order":2},
      "d":{"enum":"x","type":"string","x-order":3},
      "e":{"type":"integer","x-computed":{},"multipleOf":"x","x-order":4},
      "f":{"type":"integer","x-minimum":{"num":1},"x-order":5},
      "g":{"type":"integer","x-optionsAction":"G","x-optionsDependsOn":"a","x-order":6},
      "h":{"type":"object","x-decimalPlaces":1,"ExtUnits":{"unitAscii":"kg"},"x-unitAlternatives":5,"x-order":7},
      "i":{"type":"object","x-decimalPlaces":1,"ExtUnits":{"unitAscii":"kg"},"x-order":8,
           "x-unitAlternatives":[{"id":"z","num":1,"den":0},{"id":"y","num":1,"den":-1}]},
      "j":{"type":"string","x-widget":"slider","x-min":0,"x-max":5,"x-order":9},
      "k":{"type":"object","x-order":10},
      "l":{"oneOf":[{"oneOf":[]}],"x-order":11}},
      "required":"a","x-submitMode":"auto"})");
    INFO((read ? std::string{} : read.error().path + ": " + read.error().message));
    REQUIRE(read.has_value());
    CHECK(read->find("a")->kind == FieldKind::Text);
    CHECK_FALSE(read->find("a")->required);  // "required" is not a list
    CHECK(read->find("b")->kind == FieldKind::Text);
    CHECK(read->find("c")->kind == FieldKind::Text);  // a type that is neither a name nor a list: text
    CHECK_FALSE(read->find("c")->readOnly);
    CHECK(read->find("d")->enumOptions.empty());
    CHECK(read->find("e")->computedInputs.empty());
    CHECK_FALSE(read->find("e")->multipleOf.has_value());
    CHECK_FALSE(read->find("f")->instanceMinimum.has_value());
    CHECK(read->find("g")->choice->dependsOn.empty());
    CHECK(read->find("h")->units.size() == 1);
    CHECK(read->find("i")->units.size() == 1);        // neither alternative has a positive ratio
    CHECK(read->find("j")->kind == FieldKind::Text);  // a slider is an integer's or a number's
    CHECK(read->find("k")->truncated);                // an object without properties cannot be edited here
    CHECK(read->find("l")->enumOptions.empty());
    CHECK(read->submitMode() == morph::forms::SubmitMode::Automatic);
}

TEST_CASE("engine edges: a session's nested members, collections and dependent options", "[forms-engine][edges]") {
    morph::test::formsengine::Harness harness;
    morph::forms::FormSessionOptions options{};
    options.translations = [](std::string_view, std::string_view) { return std::optional<std::string>{}; };
    auto const session = harness.session("Edge", R"({"properties":{
      "home":{"type":"object","x-order":0,"properties":{
        "street":{"type":"string","x-i18nKey":"custom.street"}}},
      "deep":{"type":"object","x-order":1},
      "list":{"type":"array","items":{"type":"array"},"x-order":2},
      "name":{"type":"string","x-blankAs":"empty","x-order":3},
      "mass":{"type":"object","x-decimalPlaces":1,"ExtUnits":{"unitAscii":"kg"},"x-order":4,
              "x-unitAlternatives":[{"id":"g","num":1,"den":1000}]},
      "a":{"type":"integer","x-order":5},
      "b":{"type":"integer","x-order":6},
      "pick":{"type":"integer","x-optionsAction":"Picks","x-optionsDependsOn":["a","b"],"x-order":7}},
      "x-rules":[{"kind":"engaged","fields":["ghost"]}]})",
                                         std::move(options));
    harness.settle();
    CHECK(session->field("home.street").visible().get());
    CHECK_FALSE(session->field("home.street").readonly().get());
    CHECK(session->field("home.street").label() == "street");
    CHECK(session->field("deep").addRow() == 0);  // not a collection
    CHECK(session->field("list").addRow() == 0);  // a collection too deep to edit
    session->prefill(R"({"deep":{"x":1},"list":[[1]],"name":""})");
    harness.settle();
    CHECK(session->field("name").error().get() == std::nullopt);
    CHECK_FALSE(session->field("deep").blank());
    CHECK_FALSE(session->field("list").blank());
    session->field("mass").unit().set(5);  // a unit index past the list
    session->field("mass").switchUnit(1);
    CHECK(session->field("mass").unit().get() == 5U);
    CHECK_THROWS_AS(static_cast<void>(session->field("list[01x]")), std::out_of_range);
    harness.type(*session, "a", "1");
    harness.type(*session, "b", "2");
    REQUIRE_FALSE(harness.server.fetches().empty());
    CHECK(harness.server.fetches().back().body == R"({"a":1,"b":2})");
    CHECK_FALSE(session->ready());  // the rule names a field the form does not have: it never holds
    CHECK_THROWS_AS(morph::forms::FormSession(
                        harness.rt, morph::test::formsengine::modelOf("Edge", R"({"properties":{}})"), {}, {}),
                    std::invalid_argument);
}

TEST_CASE("engine edges: equals literals given as digits that are not an integer", "[forms-engine][edges]") {
    auto const literal = [](std::string_view text) {
        auto const parsed = rule(text);
        REQUIRE(parsed.has_value());
        return engine::toJson(parsed->expression());
    };
    CHECK(literal(R"({"kind":"equals","fields":["a"],"valueText":"1.5","value":1.5})").contains(R"("num":3)"));
    CHECK(literal(R"({"kind":"equals","fields":["a"],"valueText":"x","value":"x"})").contains(R"("x")"));
    CHECK(literal(R"({"kind":"equals","fields":["a"],"value":{"num":1}})").contains("null"));
}

// Mutation: write a `FieldMeta` builder's argument to another member (the expected member stays at its default).
TEST_CASE("forms: FieldMeta's unit, decimals and blankAs builders set their member at run time", "[forms][meta]") {
    morph::forms::FieldMeta const base{.field = "mass"};
    morph::forms::FieldMeta const withUnit = base.withUnit("mg");
    CHECK(withUnit.unit == "mg");
    CHECK(withUnit.field == "mass");
    morph::forms::FieldMeta const withDecimals = base.withDecimals(morph::math::DecimalPlaces{3});
    REQUIRE(withDecimals.decimals.has_value());
    CHECK(withDecimals.decimals->value == 3);
    CHECK(base.withBlankAs(morph::forms::BlankAs::Empty).blankAs == morph::forms::BlankAs::Empty);
    CHECK(base.unit.empty());
}

// Mutation: drop appendDecimal's zero case (a zero width renders as nothing).
TEST_CASE("forms: a schema shape tag appends text and decimal digits", "[forms][meta]") {
    morph::forms::detail::ShapeTag tag;
    tag.append("f");
    tag.appendDecimal(128);
    CHECK(tag.view() == "f128");
    morph::forms::detail::ShapeTag zero;
    zero.appendDecimal(0);
    CHECK(zero.view() == "0");
}

// Mutation: let an out-of-range enum value name something other than its documented fallback.
TEST_CASE("forms: a kind name outside its enum falls back to the documented name", "[forms][meta]") {
    CHECK(morph::forms::i18n::fieldSlotName(static_cast<morph::forms::i18n::FieldSlot>(200)) == "label");
    CHECK(morph::forms::i18n::fieldSlotName(morph::forms::i18n::FieldSlot::Placeholder) == "placeholder");
    CHECK(morph::forms::groupKindName(static_cast<morph::forms::GroupKind>(200)) == "section");
    CHECK(morph::forms::groupKindName(morph::forms::GroupKind::Accordion) == "accordion");
    CHECK(morph::forms::violationKindName(static_cast<morph::forms::ConstraintViolationKind>(200)) == "unknown");
    CHECK(morph::forms::violationKindName(morph::forms::ConstraintViolationKind::PrecisionExceeded) ==
          "precisionExceeded");
    CHECK(morph::forms::fieldKindName(static_cast<morph::forms::FieldKind>(200)) == "text");
    CHECK(morph::forms::fieldKindName(morph::forms::FieldKind::ObjectArray) == "objectArray");
}

// Mutation: stop at a node that is not an object, has no kind or no fields (a later contradiction goes unreported).
TEST_CASE("forms: an unsatisfiable conjunct is found past nodes that do not name fields", "[forms][rules]") {
    glz::generic_u64 nodes{};
    REQUIRE_FALSE(glz::read_json(nodes, R"([
      7, {"fields":["a","b"]}, {"kind":"exactlyOneOf"}, {"kind":"engaged","fields":["a","b"]},
      {"kind":"and"}, {"kind":"and","conditions":[{"kind":"mutuallyExclusive","fields":["a","b"]}]}])"));
    std::vector<std::string_view> const required{"a", "b"};
    auto const offender =
        morph::forms::detail::findUnsatisfiableConjunct(nodes.get<glz::generic_u64::array_t>(), required);
    REQUIRE(offender.has_value());
    CHECK(offender->first == "mutuallyExclusive");
    CHECK(offender->second == "a, b");
}
