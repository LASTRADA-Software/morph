// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <morph/ui/value.hpp>
#include <string>

using morph::math::DecimalPlaces;
using morph::math::Denominator;
using morph::math::Numerator;
using morph::math::Rational;
using morph::ui::Decimal;
using morph::ui::Json;
using morph::ui::kMaxJsonDepth;
using morph::ui::parseJson;
using morph::ui::parseType;
using morph::ui::Quantity;
using morph::ui::Type;
using morph::ui::TypeKind;
using morph::ui::Value;
using morph::ui::valueFromJson;
using morph::ui::ValueKind;
using morph::ui::ValueList;
using morph::ui::ValueRecord;
using morph::ui::valueToJson;

namespace {

Value fromText(std::string_view text) {
    auto json = parseJson(text);
    REQUIRE(json.has_value());
    auto value = valueFromJson(*json);
    REQUIRE(value.has_value());
    return *value;
}

std::string reasonOf(std::string_view text) {
    auto json = parseJson(text);
    REQUIRE(json.has_value());
    auto value = valueFromJson(*json);
    REQUIRE_FALSE(value.has_value());
    return value.error();
}

std::string toText(Value const& value) { return glz::write_json(valueToJson(value)).value_or("?"); }

Decimal dec(std::int64_t num, std::int64_t den, std::uint32_t places) {
    return Decimal{Rational{Numerator{num}, Denominator{den}, DecimalPlaces{places}}};
}

}  // namespace

// Mutation: map a JSON array to null in the visitor -> the list case fails.
TEST_CASE("ui value: every kind round-trips through its JSON form", "[ui][value]") {
    for (std::string_view const text :
         {R"(null)", R"(true)", R"(42)", R"(-7)", R"("text")", R"({"num":1,"den":4,"dp":2})",
          R"({"num":3,"den":2,"dp":1,"unit":"mg"})", R"([1,"a",null])", R"({"a":1,"b":[true]})"}) {
        CAPTURE(text);
        Value const value = fromText(text);
        CHECK(fromText(toText(value)) == value);
    }
    CHECK(fromText("42").kind() == ValueKind::Int);
    CHECK(fromText(R"({"num":1,"den":4,"dp":2})").kind() == ValueKind::Decimal);
    CHECK(fromText(R"({"num":1,"den":4,"dp":2,"unit":"g"})").kind() == ValueKind::Quantity);
    CHECK(fromText(R"({"num":1,"den":4,"dp":2,"other":"g"})").kind() == ValueKind::Record);
    CHECK(fromText(R"({"num":1,"den":4,"xp":2})").kind() == ValueKind::Record);
    CHECK(fromText(R"({"num":1,"den":4})").kind() == ValueKind::Record);
    CHECK(fromText(R"({"num":1,"den":4,"dp":2,"unit":"g","more":1})").kind() == ValueKind::Record);
}

// Mutation: accept doubles as integers -> the fraction case fails.
TEST_CASE("ui value: inexact and malformed numbers are refused", "[ui][value]") {
    CHECK(reasonOf("1.5").contains("not exact"));
    CHECK(reasonOf("[1, 2.5]").contains("not exact"));
    CHECK(reasonOf(R"({"a": 2.5})").contains("not exact"));
    CHECK(reasonOf("18446744073709551615").contains("int64"));
    CHECK(reasonOf(R"({"num":1,"den":0,"dp":2})").contains("den"));
    CHECK(reasonOf(R"({"num":"1","den":2,"dp":2})").contains("integers"));
    CHECK(reasonOf(R"({"num":1,"den":2,"dp":19})").contains("between"));
    CHECK(reasonOf(R"({"num":1,"den":2,"dp":-1})").contains("between"));
    CHECK(reasonOf(R"({"num":1,"den":2,"dp":1,"unit":3})").contains("unit"));
    CHECK(fromText("-9223372036854775808") == Value{std::int64_t{-9223372036854775807} - 1});
    CHECK_FALSE(parseJson("{").has_value());
}

// Mutation: drop the places from Decimal's equality -> the first check fails.
TEST_CASE("ui value: equality is deep and counts a decimal's places", "[ui][value]") {
    CHECK_FALSE(Value{dec(1, 2, 1)} == Value{dec(1, 2, 2)});
    CHECK(Value{dec(2, 4, 1)} == Value{dec(1, 2, 1)});
    CHECK_FALSE(Value{Quantity{.value = dec(1, 2, 1).value, .unit = "g"}} ==
                Value{Quantity{.value = dec(1, 2, 1).value, .unit = "kg"}});
    CHECK(Value{ValueList{1, "a"}} == Value{ValueList{1, "a"}});
    CHECK_FALSE(Value{ValueList{1, "a"}} == Value{ValueList{1, "b"}});
    CHECK(Value{ValueRecord{{"a", 1}}} == Value{ValueRecord{{"a", 1}}});
    CHECK_FALSE(Value{ValueRecord{{"a", 1}}} == Value{ValueRecord{{"a", 2}}});
    CHECK_FALSE(Value{1} == Value{"1"});
    CHECK(Value{} == Value{nullptr});
    Value const record{ValueRecord{{"a", 1}}};
    REQUIRE(record.field("a") != nullptr);
    CHECK(*record.field("a") == Value{1});
    CHECK(record.field("b") == nullptr);
    CHECK(Value{1}.field("a") == nullptr);
    CHECK(Value{1}.asList() == nullptr);
    CHECK(Value{1}.asRecord() == nullptr);
}

// Mutation: let parseType accept trailing text -> the "int x" check fails.
TEST_CASE("ui value: types parse, print and admit values", "[ui][value]") {
    for (std::string_view const text : {"bool", "int?", "string", "decimal", "quantity?", "timestamp", "key", "record",
                                        "any", "list<int>", "list<list<key?>>?"}) {
        CAPTURE(text);
        auto type = parseType(text);
        REQUIRE(type.has_value());
        CHECK(type->name() == text);
    }
    CHECK(parseType("any?")->name() == "any");
    CHECK_FALSE(parseType("float").has_value());
    CHECK_FALSE(parseType("int x").has_value());
    CHECK_FALSE(parseType("list<int").has_value());
    CHECK_FALSE(parseType("list<float>").has_value());
    CHECK_FALSE(parseType("list<list<list<list<list<list<list<list<list<list<int>>>>>>>>>>").has_value());
    CHECK(Type::listOf(Type::of(TypeKind::Int)) == *parseType("list<int>"));
    CHECK_FALSE(Type::listOf(Type::of(TypeKind::Int)) == *parseType("list<string>"));
    CHECK_FALSE(Type::listOf(Type::of(TypeKind::Int)) == Type::of(TypeKind::List));
    CHECK_FALSE(Type::of(TypeKind::Int) == Type::of(TypeKind::Int, true));
    CHECK(Type::of(TypeKind::List).name() == "list<any>");

    CHECK(Type::of(TypeKind::Int).admits(1));
    CHECK_FALSE(Type::of(TypeKind::Int).admits(Value{}));
    CHECK(Type::of(TypeKind::Int, true).admits(Value{}));
    CHECK(Type::any().admits(Value{}));
    CHECK(Type::of(TypeKind::Decimal).admits(1));
    CHECK(Type::of(TypeKind::Decimal).admits(dec(1, 2, 1)));
    CHECK_FALSE(Type::of(TypeKind::Decimal).admits("1"));
    CHECK(Type::of(TypeKind::Key).admits("k"));
    CHECK(Type::of(TypeKind::Key).admits(3));
    CHECK_FALSE(Type::of(TypeKind::Key).admits(true));
    CHECK(Type::of(TypeKind::Bool).admits(true));
    CHECK(Type::of(TypeKind::String).admits("s"));
    CHECK(Type::of(TypeKind::Quantity).admits(Quantity{.value = {}, .unit = "g"}));
    CHECK(Type::of(TypeKind::Timestamp).admits(morph::time::DateTime{}));
    CHECK(Type::of(TypeKind::Record).admits(ValueRecord{}));
    CHECK(parseType("list<int>")->admits(ValueList{1, 2}));
    CHECK_FALSE(parseType("list<int>")->admits(ValueList{1, "x"}));
    CHECK_FALSE(parseType("list<int>")->admits(1));
    CHECK(Type::of(TypeKind::List).admits(ValueList{1, "x"}));
}

// Mutation: skip the ISO-8601 parse for timestamps -> the timestamp kind check fails.
TEST_CASE("ui value: typed reading converts timestamps and widens integers to decimals", "[ui][value]") {
    auto stamp = valueFromJson(*parseJson(R"("2026-10-11T08:30:00Z")"), Type::of(TypeKind::Timestamp));
    REQUIRE(stamp.has_value());
    CHECK(stamp->kind() == ValueKind::Timestamp);
    CHECK(toText(*stamp) == R"("2026-10-11T08:30:00.000Z")");
    CHECK_FALSE(valueFromJson(*parseJson(R"("yesterday")"), Type::of(TypeKind::Timestamp)).has_value());
    CHECK_FALSE(valueFromJson(*parseJson("5"), Type::of(TypeKind::Timestamp)).has_value());

    auto widened = valueFromJson(*parseJson("5"), Type::of(TypeKind::Decimal));
    REQUIRE(widened.has_value());
    CHECK(widened->kind() == ValueKind::Decimal);

    auto stamps = valueFromJson(*parseJson(R"(["2026-10-11T08:30:00Z"])"), *parseType("list<timestamp>"));
    REQUIRE(stamps.has_value());
    CHECK(stamps->asList()->front().kind() == ValueKind::Timestamp);
    CHECK_FALSE(valueFromJson(*parseJson(R"(["no"])"), *parseType("list<timestamp>")).has_value());

    auto mismatch = valueFromJson(*parseJson(R"("x")"), Type::of(TypeKind::Int));
    REQUIRE_FALSE(mismatch.has_value());
    CHECK(mismatch.error() == "expected int, got string");
    CHECK_FALSE(valueFromJson(*parseJson("1.5"), Type::of(TypeKind::Int)).has_value());
    CHECK(valueFromJson(*parseJson("null"), Type::of(TypeKind::Int, true)).has_value());
}

// Mutation: drop the `~` escape -> the escaped pointer check fails.
TEST_CASE("ui value: refusals and JSON pointers", "[ui][value]") {
    CHECK(morph::ui::pointerAppend("", "a/b~c") == "/a~1b~0c");
    CHECK(morph::ui::pointerAppend("/x", std::size_t{3}) == "/x/3");
    CHECK(morph::ui::Refusal{.document = "d", .path = "/a", .reason = "r"}.message() == "document d at /a: r");
    CHECK(morph::ui::Refusal{.document = "", .path = "", .reason = "r"}.message() == "at /: r");
    CHECK(morph::ui::kindName(ValueKind::Timestamp) == "timestamp");
    for (auto const kind : {ValueKind::Null, ValueKind::Bool, ValueKind::Int, ValueKind::String, ValueKind::Decimal,
                            ValueKind::Quantity, ValueKind::List, ValueKind::Record}) {
        CHECK_FALSE(morph::ui::kindName(kind).empty());
    }
}

// Mutation: compare only numerators in Decimal's equality -> the 1/2 against 1/3 check fails.
TEST_CASE("ui value: each part of an exact value takes part in equality and decoding", "[ui][value]") {
    CHECK_FALSE(Value{dec(1, 2, 1)} == Value{dec(1, 3, 1)});
    CHECK_FALSE(Value{dec(1, 2, 1)} == Value{dec(3, 2, 1)});
    CHECK_FALSE(Value{Quantity{.value = dec(1, 2, 1).value, .unit = "g"}} ==
                Value{Quantity{.value = dec(3, 2, 1).value, .unit = "g"}});
    CHECK_FALSE(Type::of(TypeKind::List) == Type::listOf(Type::of(TypeKind::Int)));
    CHECK(parseType("a~").error() == "unknown type `a`");

    CHECK(reasonOf(R"({"num":18446744073709551615,"den":1,"dp":0})").contains("integers"));
    CHECK(reasonOf(R"({"num":1,"den":"2","dp":1})").contains("integers"));
    CHECK(reasonOf(R"({"num":1,"den":2,"dp":"1"})").contains("integers"));
    CHECK(fromText(R"({"den":1,"dp":1,"unit":"g"})").kind() == ValueKind::Record);
    CHECK(fromText(R"({"num":1,"dp":1,"unit":"g"})").kind() == ValueKind::Record);
    CHECK(fromText(R"({"num":1,"den":1,"unit":"g"})").kind() == ValueKind::Record);

    auto untypedList = valueFromJson(*parseJson("[1]"), Type::of(TypeKind::List));
    REQUIRE(untypedList.has_value());
    CHECK(*untypedList == Value{ValueList{1}});
    auto notAList = valueFromJson(*parseJson("5"), *parseType("list<int>"));
    REQUIRE_FALSE(notAList.has_value());
    CHECK(notAList.error() == "expected list<int>, got int");
    auto decimal = valueFromJson(*parseJson(R"({"num":3,"den":2,"dp":1})"), Type::of(TypeKind::Decimal));
    REQUIRE(decimal.has_value());
    CHECK(*decimal == Value{dec(3, 2, 1)});
}

// Mutation: drop the nesting check in parseJson -> the over-deep text is parsed, and on a 1 MB stack a few hundred
// levels overflow it.
TEST_CASE("ui value: JSON nested deeper than the bound is refused before it is parsed", "[ui][value]") {
    auto nested = [](std::size_t levels) { return std::string(levels, '[') + std::string(levels, ']'); };
    CHECK(parseJson(nested(kMaxJsonDepth)).has_value());
    auto refused = parseJson(nested(kMaxJsonDepth + 1));
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().contains("nests deeper than"));
    CHECK_FALSE(parseJson(nested(600)).has_value());

    // Brackets inside a string, escaped quotes included, are text rather than nesting.
    std::string const brackets(kMaxJsonDepth + 1, '[');
    CHECK(parseJson("\"" + brackets + "\"").has_value());
    CHECK(parseJson(R"("a \" )" + brackets + "\"").has_value());
    CHECK(parseJson(R"("a \\")").has_value());

    // A stray closing bracket is the parser's error to report, not a nesting one.
    auto stray = parseJson("]");
    REQUIRE_FALSE(stray.has_value());
    CHECK_FALSE(stray.error().contains("nests deeper than"));
}
