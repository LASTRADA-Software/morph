// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <morph/forms/engine/field_model.hpp>
#include <morph/forms/engine/values.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using morph::forms::Encoding;
using morph::forms::FieldSpec;
using morph::forms::FormModel;
using morph::forms::ValueContext;
namespace engine = morph::forms::detail::engine;

/// One field described from a one-property schema.
[[nodiscard]] FieldSpec field(std::string_view propertyJson, std::string_view defs = "{}") {
    std::string const schema = R"({"$defs":)" + std::string{defs} + R"(,"properties":{"f":)" +
                               std::string{propertyJson} + R"(},"required":["f"]})";
    auto model = FormModel::fromSchema("T", schema);
    REQUIRE(model.has_value());
    return model->fields().front();
}

[[nodiscard]] std::string json(Encoding const& encoding) {
    REQUIRE(encoding.has_value());
    INFO((encoding->has_value() ? std::string{} : encoding->error().code));
    REQUIRE(encoding->has_value());
    return (*encoding)->json;
}

[[nodiscard]] std::string error(Encoding const& encoding) {
    REQUIRE(encoding.has_value());
    REQUIRE_FALSE(encoding->has_value());
    return encoding->error().code;
}

[[nodiscard]] ValueContext const& plainContext() {
    static ValueContext const value{};
    return value;
}
[[nodiscard]] ValueContext const& germanContext() {
    static ValueContext const value{.locale = {.decimalSeparator = ",", .groupSeparator = "."}};
    return value;
}

constexpr std::string_view kMass = R"({"$ref":"#/$defs/q","x-decimalPlaces":3,"x-unitAlternatives":[
  {"id":"g","display":"g","decimals":1,"num":1,"den":1000},{"id":"t","display":"t","decimals":4,"num":1000,"den":1}]})";
constexpr std::string_view kMassDefs =
    R"({"q":{"type":["object","null"],"ExtUnits":{"unitAscii":"kg","unitUnicode":"kg"}}})";
constexpr std::string_view kInt64Defs = R"({"int64_t":{"type":"integer","minimum":-9223372036854775808,
  "maximum":9223372036854775807,"x-exactMinimum":"-9223372036854775808","x-exactMaximum":"9223372036854775807"}})";

}  // namespace

TEST_CASE("values: a Quantity keeps the unreduced scaled digits the QML renderer sent", "[forms-engine][values]") {
    FieldSpec const mass = field(kMass, kMassDefs);
    CHECK(json(morph::forms::encodeScalar(mass, "2650.5", 0, plainContext())) ==
          R"({"num":2650500,"den":1000,"dp":3})");
    CHECK(json(morph::forms::encodeScalar(mass, "2650500.0", 1, plainContext())) ==
          R"({"num":26505000,"den":10000,"dp":3})");
    CHECK(json(morph::forms::encodeScalar(mass, "1.050,25", 0, germanContext())) ==
          R"({"num":1050250,"den":1000,"dp":3})");
    CHECK(error(morph::forms::encodeScalar(mass, "2650.5001", 0, plainContext())) == "too-precise");
    CHECK(error(morph::forms::encodeScalar(mass, "1.5", 0, germanContext())) == "malformed");
    FieldSpec const tenth = field(R"({"type":"object","x-decimalPlaces":1,"ExtUnits":{"unitAscii":"x"}})");
    CHECK(json(morph::forms::encodeScalar(tenth, "-0.5", 0, plainContext())) == R"({"num":-5,"den":10,"dp":1})");
    CHECK(json(morph::forms::encodeScalar(tenth, "123456789012345678.9", 0, plainContext())) ==
          R"({"num":1234567890123456789,"den":10,"dp":1})");
    CHECK(error(morph::forms::encodeScalar(tenth, "1234567890123456789.9", 0, plainContext())) == "out-of-range");
    FieldSpec const decimal = field(R"({"type":"object","x-decimalPlaces":2})");
    CHECK(json(morph::forms::encodeScalar(decimal, "3.5", 0, plainContext())) == R"({"num":350,"den":100,"dp":2})");
}

TEST_CASE("values: Quantity bounds, instance bounds and multipleOf are checked exactly in the canonical unit",
          "[forms-engine][values]") {
    FieldSpec const budget =
        field(R"({"type":"object","x-decimalPlaces":1,"ExtUnits":{"unitAscii":"count"},"minimum":1,"multipleOf":1})");
    CHECK(error(morph::forms::encodeScalar(budget, "0", 0, plainContext())) == "below-minimum");
    CHECK(error(morph::forms::encodeScalar(budget, "2.5", 0, plainContext())) == "not-a-multiple");
    CHECK(json(morph::forms::encodeScalar(budget, "2", 0, plainContext())) == R"({"num":20,"den":10,"dp":1})");
    FieldSpec const step =
        field(R"({"type":"object","x-decimalPlaces":2,"multipleOf":0.5,"ExtUnits":{"unitAscii":"u"}})");
    CHECK((*morph::forms::encodeScalar(step, "1.5", 0, plainContext())).has_value());
    CHECK(error(morph::forms::encodeScalar(step, "0.75", 0, plainContext())) == "not-a-multiple");
    FieldSpec const decorated =
        field(R"({"type":["object","null"],"ExtUnits":{"unitAscii":"mg_per_L"},"x-decimalPlaces":1,
      "x-minimum":{"num":0,"den":1,"dp":1},"x-maximum":{"num":20,"den":1,"dp":1}})");
    CHECK((*morph::forms::encodeScalar(decorated, "12.5", 0, plainContext())).has_value());
    CHECK(error(morph::forms::encodeScalar(decorated, "25", 0, plainContext())) == "above-maximum");
    CHECK(error(morph::forms::encodeScalar(decorated, "-1", 0, plainContext())) == "below-minimum");
    CHECK(error(morph::forms::encodeScalar(decorated, "12.55", 0, plainContext())) == "too-precise");
    FieldSpec const capped = field(R"({"$ref":"#/$defs/q","x-decimalPlaces":3,"maximum":2000,
      "x-unitAlternatives":[{"id":"g","display":"g","decimals":1,"num":1,"den":1000}]})",
                                   kMassDefs);
    CHECK(error(morph::forms::encodeScalar(capped, "2000000.1", 1, plainContext())) ==
          "above-maximum");  // checked in kg
}

TEST_CASE("values: unit conversion is exact and rounds half away from zero", "[forms-engine][values]") {
    using morph::forms::convertDraft;
    using morph::forms::UnitOption;
    UnitOption const kilograms{.id = "kg", .display = "kg", .decimals = 3, .num = 1, .den = 1};
    UnitOption const gram{.id = "g", .display = "g", .decimals = 1, .num = 1, .den = 1000};
    UnitOption const tonne{.id = "t", .display = "t", .decimals = 4, .num = 1000, .den = 1};
    CHECK(convertDraft("2650.5", kilograms, gram, {}) == "2650500.0");
    CHECK(convertDraft("2650500.0", gram, kilograms, {}) == "2650.500");
    CHECK(convertDraft("2650.5", kilograms, tonne, {}) == "2.6505");
    CHECK(convertDraft("2.6505", tonne, gram, {}) == "2650500.0");
    CHECK(convertDraft("-0.001", kilograms, gram, {}) == "-1.0");
    CHECK(convertDraft("0.05", UnitOption{"a", "a", 2, 1, 1}, UnitOption{"b", "b", 1, 1, 1}, {}) == "0.1");
    CHECK(convertDraft("junk", kilograms, gram, {}) == std::nullopt);
    CHECK(convertDraft("2650,5", kilograms, gram, {.decimalSeparator = ",", .groupSeparator = "."}) == "2.650.500,0");
}

TEST_CASE("values: an integer keeps every digit and prefers the exact bound", "[forms-engine][values]") {
    FieldSpec const wideId = field(R"({"$ref":"#/$defs/int64_t"})", kInt64Defs);
    CHECK(json(morph::forms::encodeScalar(wideId, "9223372036854775807", 0, plainContext())) == "9223372036854775807");
    CHECK(json(morph::forms::encodeScalar(wideId, "-9223372036854775808", 0, plainContext())) ==
          "-9223372036854775808");
    CHECK(error(morph::forms::encodeScalar(wideId, "9223372036854775808", 0, plainContext())) == "above-maximum");
    CHECK(error(morph::forms::encodeScalar(wideId, "-9223372036854775809", 0, plainContext())) == "below-minimum");
    CHECK(error(morph::forms::encodeScalar(wideId, "99999999999999999999", 0, plainContext())) == "above-maximum");
    CHECK(json(morph::forms::encodeScalar(wideId, "0009223372036854775807", 0, plainContext())) ==
          "9223372036854775807");
    CHECK(json(morph::forms::encodeScalar(wideId, "-42", 0, plainContext())) == "-42");
    CHECK(error(morph::forms::encodeScalar(wideId, "banana", 0, plainContext())) == "malformed");
    FieldSpec const small = field(R"({"type":"integer","minimum":-10,"maximum":10})");
    CHECK((*morph::forms::encodeScalar(small, "10", 0, plainContext())).has_value());
    CHECK(error(morph::forms::encodeScalar(small, "11", 0, plainContext())) == "above-maximum");
    CHECK(error(morph::forms::encodeScalar(small, "-11", 0, plainContext())) == "below-minimum");
    FieldSpec const unbounded = field(R"({"type":"integer"})");
    CHECK(json(morph::forms::encodeScalar(unbounded, "18446744073709551615", 0, plainContext())) ==
          "18446744073709551615");
    // What a rule compares is the exact int, not a double: 2^53 + 1 survives.
    auto const value = (*morph::forms::encodeScalar(wideId, "9007199254740993", 0, plainContext()))->value;
    REQUIRE(value.asInt() != nullptr);
    CHECK(*value.asInt() == 9007199254740993);
    // Beyond int64 the body keeps every digit; the expression language holds no such int, so rules read null.
    CHECK((*morph::forms::encodeScalar(unbounded, "18446744073709551615", 0, plainContext()))->value.isNull());
}

TEST_CASE("values: a plain number is a JSON number of the digits typed, gated by its bounds",
          "[forms-engine][values]") {
    constexpr std::string_view kDefs =
        R"({"double":{"type":"number","minimum":-1.7976931348623157e+308,"maximum":1.7976931348623157e+308},
      "float":{"type":"number","minimum":-3.4028234663852886e+38,"maximum":3.4028234663852886e+38}})";
    FieldSpec const ratio = field(R"({"$ref":"#/$defs/double"})", kDefs);
    FieldSpec const score = field(R"({"$ref":"#/$defs/float"})", kDefs);
    CHECK(json(morph::forms::encodeScalar(ratio, "3.5", 0, plainContext())) == "3.5");
    CHECK(json(morph::forms::encodeScalar(ratio, "4", 0, plainContext())) == "4");
    CHECK(json(morph::forms::encodeScalar(ratio, "-0.25", 0, plainContext())) == "-0.25");
    CHECK(json(morph::forms::encodeScalar(ratio, "007.50", 0, plainContext())) == "7.50");
    for (std::string_view const text : {"abc", "3.", ".5", "1e5", "--1", "1 2", "0x10", "3,5"}) {
        INFO("accepted " << text);
        CHECK(error(morph::forms::encodeScalar(ratio, text, 0, plainContext())) == "malformed");
    }
    ValueContext const grouped{.locale = {.groupSeparator = ","}};
    CHECK(json(morph::forms::encodeScalar(ratio, "1,000.5", 0, grouped)) == "1000.5");
    std::string const huge = "100000000000000000000000000000000000000000";
    CHECK(error(morph::forms::encodeScalar(score, huge, 0, plainContext())) == "above-maximum");
    CHECK(json(morph::forms::encodeScalar(ratio, huge, 0, plainContext())) == huge);
    FieldSpec const temperature = field(R"({"type":"number","minimum":-40.5,"maximum":120,"multipleOf":0.5})");
    CHECK((*morph::forms::encodeScalar(temperature, "-40.5", 0, plainContext())).has_value());
    CHECK(error(morph::forms::encodeScalar(temperature, "-41", 0, plainContext())) == "below-minimum");
    CHECK(error(morph::forms::encodeScalar(temperature, "120.5", 0, plainContext())) == "above-maximum");
    CHECK(error(morph::forms::encodeScalar(temperature, "0.25", 0, plainContext())) == "not-a-multiple");
    CHECK(json(morph::forms::encodeScalar(temperature, "0.5", 0, plainContext())) == "0.5");
    FieldSpec const density = field(R"({"type":"number","x-displayDecimals":3})");
    CHECK(json(morph::forms::encodeScalar(density, "2.505", 0, plainContext())) == "2.505");
    CHECK(error(morph::forms::encodeScalar(density, "2.5051", 0, plainContext())) == "too-precise");
    FieldSpec const whole = field(R"({"type":"number","x-displayDecimals":0})");
    CHECK(json(morph::forms::encodeScalar(whole, "12", 0, plainContext())) == "12");
    CHECK(error(morph::forms::encodeScalar(whole, "12.5", 0, plainContext())) == "too-precise");
}

TEST_CASE("values: booleans, enums, choices, dates and arrays encode to their wire shapes", "[forms-engine][values]") {
    FieldSpec const flag = field(R"({"type":"boolean"})");
    CHECK(json(morph::forms::encodeScalar(flag, "true", 0, plainContext())) == "true");
    CHECK(json(morph::forms::encodeScalar(flag, "", 0, plainContext())) ==
          "false");  // required: an unchecked box answers
    FieldSpec optionalFlag = flag;
    optionalFlag.required = false;
    CHECK_FALSE(morph::forms::encodeScalar(optionalFlag, "", 0, plainContext()).has_value());
    CHECK(error(morph::forms::encodeScalar(flag, "banana", 0, plainContext())) == "malformed");

    FieldSpec const role = field(
        R"({"type":"string","oneOf":[{"title":"Viewer","const":"Viewer"},{"title":"Manager","const":"Manager"}]})");
    CHECK(json(morph::forms::encodeScalar(role, R"("Manager")", 0, plainContext())) == R"("Manager")");
    CHECK(error(morph::forms::encodeScalar(role, R"("Emperor")", 0, plainContext())) == "not-an-option");
    CHECK(error(morph::forms::encodeScalar(role, "Manager", 0, plainContext())) == "not-an-option");

    FieldSpec const slot = field(R"({"type":"integer","x-optionsAction":"ListSlots"})");
    CHECK(json(morph::forms::encodeScalar(slot, "9007199254740993", 0, plainContext())) == "9007199254740993");
    CHECK(error(morph::forms::encodeScalar(slot, "{not json", 0, plainContext())) == "malformed");

    FieldSpec const when = field(R"({"type":"string","format":"date-time"})");
    CHECK(json(morph::forms::encodeScalar(when, "2026-07-05T14:30", 0, plainContext())) ==
          R"("2026-07-05T14:30:00Z")");
    CHECK(json(morph::forms::encodeScalar(when, "2026-07-05T16:30:00", 0,
                                          ValueContext{.displayOffsetMinutes = 120})) == R"("2026-07-05T14:30:00Z")");
    CHECK(error(morph::forms::encodeScalar(when, "not-a-date", 0, plainContext())) == "malformed");
    FieldSpec const day = field(R"({"type":"string","format":"date"})");
    CHECK(json(morph::forms::encodeScalar(day, "2026-07-20", 0, plainContext())) == R"("2026-07-20")");
    CHECK(error(morph::forms::encodeScalar(day, "2026-07-32", 0, plainContext())) == "malformed");

    FieldSpec const tags = field(R"({"type":"array","items":{"type":"string"}})");
    CHECK(json(morph::forms::encodeScalar(tags, "red, green, blue", 0, plainContext())) ==
          R"(["red","green","blue"])");
    CHECK(json(morph::forms::encodeScalar(tags, "  red ,, green ,   ", 0, plainContext())) == R"(["red","green"])");
    CHECK(json(morph::forms::encodeScalar(tags, " , , ", 0, plainContext())) == "[]");
    FieldSpec const counts = field(R"({"type":"array","items":{"type":"integer"}})");
    CHECK(json(morph::forms::encodeScalar(counts, "1, 2, 30", 0, plainContext())) == "[1,2,30]");
    CHECK(error(morph::forms::encodeScalar(counts, "1, x", 0, plainContext())) == "malformed");

    FieldSpec const text = field(R"({"type":"string"})");
    CHECK(json(morph::forms::encodeScalar(text, "  say \"hi\"  ", 0, plainContext())) == R"("say \"hi\"")");
}

TEST_CASE("values: decoding is the inverse of encoding, per kind", "[forms-engine][values]") {
    auto const decode = [](FieldSpec const& spec, std::string_view value,
                           ValueContext const& context = plainContext()) {
        auto const dom = engine::parseJson(value);
        REQUIRE(dom.has_value());
        return morph::forms::decodeScalar(spec, *dom, context);
    };
    FieldSpec const density =
        field(R"({"type":["object","null"],"ExtUnits":{"unitAscii":"kg_per_m3"},"x-decimalPlaces":2})");
    CHECK(decode(density, R"({"num":5,"den":4,"dp":2})") == "1.25");
    CHECK(decode(density, R"({"num":-1,"den":3,"dp":2})") == "-0.33");
    CHECK(decode(density, R"({"num":245050,"den":100,"dp":2})") == "2450.50");
    CHECK(decode(density, R"({"num":245050,"den":100,"dp":2})", germanContext()) == "2450,50");
    CHECK(decode(density, R"("oops")").empty());
    FieldSpec const temperature = field(R"({"type":"number"})");
    CHECK(decode(temperature, "1e-7") == "0.0000001");
    CHECK(decode(temperature, "3") == "3");
    CHECK(decode(temperature, "21.5", germanContext()) == "21,5");
    FieldSpec const padded = field(R"({"type":"number","x-displayDecimals":2})");
    CHECK(decode(padded, "3") == "3.00");
    CHECK(decode(padded, "21.5") == "21.50");
    CHECK(decode(padded, "1.23456") == "1.23456");
    CHECK(decode(field(R"({"type":"boolean"})"), "false") == "false");
    CHECK(decode(field(R"({"type":"string","oneOf":[{"const":"A"},{"const":"B"}]})"), R"("A")") == R"("A")");
    FieldSpec const takenAt = field(R"({"type":"string","format":"date-time"})");
    CHECK(decode(takenAt, R"("2026-01-01T00:30:00+01:00")") == "2025-12-31T23:30:00");
    CHECK(decode(takenAt, R"("2026-07-20T09:00:00Z")", ValueContext{.displayOffsetMinutes = 120}) ==
          "2026-07-20T11:00:00");
    CHECK(decode(field(R"({"type":"integer"})"), "9007199254740993") == "9007199254740993");
    CHECK(decode(field(R"({"type":"array","items":{"type":"string"}})"), R"(["a","b"])") == "a, b");
    CHECK(decode(field(R"({"type":"string"})"), "null").empty());
}

TEST_CASE("values: option rows are the reply array or its first array member, ids exact", "[forms-engine][values]") {
    morph::forms::ChoiceSpec const spec{.optionsAction = "ListRows"};
    CHECK(morph::forms::parseOptions(R"([{"id":1,"name":"One"}])", spec).size() == 1);
    CHECK(morph::forms::parseOptions(R"({"rows":[{"id":1},{"id":2}]})", spec).size() == 2);
    CHECK(morph::forms::parseOptions(R"({"nothing":1})", spec).empty());
    CHECK(morph::forms::parseOptions("not json", spec).empty());
    auto const big = morph::forms::parseOptions(
        R"({"rows":[{"id":9007199254740993,"name":"Alpha"},{"id":9007199254740992,"name":"Beta"}]})", spec);
    REQUIRE(big.size() == 2);
    CHECK(big[0].valueJson == "9007199254740993");
    CHECK(big[1].valueJson == "9007199254740992");
    CHECK(big[0].label == "Alpha");
    morph::forms::ChoiceSpec const byId{.optionsAction = "ListRows", .labelField = "id"};
    CHECK(morph::forms::parseOptions(R"([{"id":9007199254740993}])", byId).front().label == "9007199254740993");
}

TEST_CASE("values: placeholders follow the kind and the declared precision", "[forms-engine][values]") {
    CHECK(morph::forms::defaultPlaceholder(field(R"({"type":"number","x-displayDecimals":3})")) == "0.000");
    CHECK(morph::forms::defaultPlaceholder(field(R"({"type":"number","x-displayDecimals":0})")) == "0");
    CHECK(morph::forms::defaultPlaceholder(field(R"({"type":"number"})")).empty());
    CHECK(morph::forms::defaultPlaceholder(field(R"({"type":"integer"})")) == "0");
    CHECK(morph::forms::defaultPlaceholder(field(kMass, kMassDefs)) == "0.000");
    CHECK(morph::forms::defaultPlaceholder(field(R"({"type":"string","x-placeholder":"e.g. 1050"})")) == "e.g. 1050");
    CHECK(morph::forms::defaultPlaceholder(field(R"({"type":"array","items":{"type":"string"}})")) ==
          "comma-separated (e.g. red, green, blue)");
}

TEST_CASE("values: an error message resolves through the translation provider", "[forms-engine][values]") {
    ValueContext const translated{
        .translations = [](std::string_view key, std::string_view locale) -> std::optional<std::string> {
            if (key == "morph.forms.error.malformed" && locale == "de") {
                return "Ungültiger Wert";
            }
            return std::nullopt;
        },
        .bcp47 = "de"};
    auto const encoding = morph::forms::encodeScalar(field(R"({"type":"integer"})"), "x", 0, translated);
    REQUIRE(encoding.has_value());
    CHECK(encoding->error().message == "Ungültiger Wert");
    CHECK(morph::forms::fieldError("too-precise", plainContext()).message == "Too many decimal places");
}

TEST_CASE("values: epoch widgets enter a wall clock or a date and send the count since the epoch",
          "[forms-engine][values]") {
    FieldSpec const seconds = field(R"({"type":"integer","x-widget":"epochSeconds"})");
    CHECK(seconds.kind == morph::forms::FieldKind::EpochDateTime);
    CHECK(json(morph::forms::encodeScalar(seconds, "1970-01-02T00:00", 0, plainContext())) == "86400");
    CHECK(json(morph::forms::encodeScalar(seconds, "2026-07-05T16:30:00", 0,
                                          ValueContext{.displayOffsetMinutes = 120})) == "1783261800");
    CHECK(error(morph::forms::encodeScalar(seconds, "86400", 0, plainContext())) == "malformed");
    FieldSpec const days = field(R"({"type":"integer","x-widget":"epochDays"})");
    CHECK(days.kind == morph::forms::FieldKind::EpochDate);
    CHECK(json(morph::forms::encodeScalar(days, "1970-01-11", 0, plainContext())) == "10");
    CHECK(json(morph::forms::encodeScalar(days, "1969-12-31", 0, plainContext())) == "-1");
    CHECK(error(morph::forms::encodeScalar(days, "1970-02-30", 0, plainContext())) == "malformed");
    auto const value = (*morph::forms::encodeScalar(days, "1970-01-11", 0, plainContext()))->value;
    REQUIRE(value.asInt() != nullptr);
    CHECK(*value.asInt() == 10);

    auto const decode = [](FieldSpec const& spec, std::string_view text, ValueContext const& context) {
        auto const dom = engine::parseJson(text);
        REQUIRE(dom.has_value());
        return morph::forms::decodeScalar(spec, *dom, context);
    };
    CHECK(decode(seconds, "1783261800", ValueContext{.displayOffsetMinutes = 120}) == "2026-07-05T16:30:00");
    CHECK(decode(days, "-1", plainContext()) == "1969-12-31");
    CHECK(decode(days, "1.5", plainContext()).empty());
}
