// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <expected>
#include <morph/ui/expression.hpp>
#include <morph/util/datetime.hpp>
#include <morph/util/rational.hpp>
#include <string>
#include <string_view>

namespace ui = morph::ui;
namespace math = morph::math;

namespace {

[[nodiscard]] ui::Json json(std::string_view text) {
    ui::Json out{};
    REQUIRE_FALSE(glz::read_json(out, text));
    return out;
}

[[nodiscard]] ui::Type type(std::string_view text) {
    std::expected<ui::Type, std::string> parsed = ui::parseType(text);
    REQUIRE(parsed.has_value());
    return *parsed;
}

[[nodiscard]] math::Rational decimal(std::int64_t num, std::int64_t den, std::uint32_t places) {
    return math::Rational{math::Numerator{num}, math::Denominator{den}, math::DecimalPlaces{places}};
}

[[nodiscard]] morph::time::DateTime instant(std::string_view iso) {
    std::optional<morph::time::DateTime> parsed = morph::time::DateTime::fromIso8601(iso);
    REQUIRE(parsed.has_value());
    return *parsed;
}

// Decodes @p text as @p typeText, encodes it back, and decodes that again: both reads give @p expected.
void roundTrips(std::string_view text, std::string_view typeText, ui::Value const& expected) {
    INFO(text << " as " << typeText);
    std::expected<ui::Value, std::string> const first = ui::decode(json(text), type(typeText));
    REQUIRE(first.has_value());
    CHECK(*first == expected);
    std::expected<ui::Value, std::string> const second = ui::decode(ui::encode(*first), type(typeText));
    REQUIRE(second.has_value());
    CHECK(*second == expected);
}

}  // namespace

// Mutations: write a decimal without `dp` in encode; write a timestamp as its epoch; leave the unit off a quantity.
TEST_CASE("ui::decode: every type reads its JSON form and reads back what encode writes", "[ui][expr]") {
    roundTrips("true", "bool", ui::Value{true});
    roundTrips("-42", "int", ui::Value{std::int64_t{-42}});
    roundTrips(R"("Zoë")", "string", ui::Value{"Zoë"});
    roundTrips(R"({"num":-5,"den":4,"dp":2})", "decimal", ui::Value{decimal(-5, 4, 2)});
    roundTrips(R"({"num":3,"den":2,"dp":1,"unit":"mg_per_l"})", "quantity",
               ui::Value{ui::QuantityValue{.amount = decimal(3, 2, 1), .unit = "mg_per_l"}});
    roundTrips(R"("2026-10-11T07:30:00Z")", "timestamp", ui::Value{instant("2026-10-11T07:30:00Z")});
    roundTrips("7", "key", ui::Value{std::int64_t{7}});
    roundTrips(R"("sample-7")", "key", ui::Value{"sample-7"});
    roundTrips("[1,2,3]", "list<int>", ui::Value{ui::ValueList{std::int64_t{1}, std::int64_t{2}, std::int64_t{3}}});
    roundTrips(R"({"id":1,"note":null})", "{id: int, note: string?}",
               ui::Value{ui::ValueRecord{{"id", std::int64_t{1}}, {"note", ui::Value{}}}});
    roundTrips("null", "int?", ui::Value{});
}

// Mutation: read a `timestamp` as a string (the case's first check then fails), or accept any string as one.
TEST_CASE("ui::decode: a timestamp is an ISO-8601 instant, and only its type tells it from a string", "[ui][expr]") {
    ui::Json const text = json(R"("2026-10-11T07:30:00Z")");
    CHECK(ui::decode(text, type("timestamp"))->kind() == ui::Value::Kind::Timestamp);
    CHECK(ui::decode(text, type("string"))->kind() == ui::Value::Kind::String);
    CHECK(ui::decodeUntyped(text)->kind() == ui::Value::Kind::String);
    std::expected<ui::Value, std::string> const notAnInstant = ui::decode(json(R"("yesterday")"), type("timestamp"));
    REQUIRE_FALSE(notAnInstant.has_value());
    CHECK(notAnInstant.error() == "not an ISO-8601 UTC timestamp");
}

// Mutation: read any object holding `num`, `den` and `dp` as a decimal, whatever else it holds.
TEST_CASE("ui::decodeUntyped: a decimal is exactly {num, den, dp}; a fourth field makes a record", "[ui][expr]") {
    CHECK(ui::decodeUntyped(json(R"({"num":1,"den":2,"dp":1})"))->kind() == ui::Value::Kind::Decimal);
    CHECK(ui::decodeUntyped(json(R"({"num":1,"den":2,"dp":1,"unit":"g"})"))->kind() == ui::Value::Kind::Quantity);
    std::expected<ui::Value, std::string> const record = ui::decodeUntyped(json(R"({"num":1,"den":2,"dpx":1})"));
    REQUIRE(record.has_value());
    CHECK(record->kind() == ui::Value::Kind::Record);
    std::expected<ui::Value, std::string> const extra =
        ui::decodeUntyped(json(R"({"num":1,"den":2,"dp":1,"label":"half"})"));
    REQUIRE(extra.has_value());
    CHECK(extra->kind() == ui::Value::Kind::Record);
    REQUIRE(extra->field("label") != nullptr);
    CHECK(*extra->field("label") == ui::Value{"half"});
}

// An int64 beyond 2^53 survives exactly: a double would round 2^53 + 1 to 2^53. Mutations: decode untyped
// numbers through a double; compare decimals as doubles in Value's equality.
TEST_CASE("ui::decode: an int64 beyond 2^53 and a decimal over it stay exact", "[ui][expr]") {
    constexpr std::int64_t kBeyond = (std::int64_t{1} << 53) + 1;
    std::expected<ui::Value, std::string> const untyped = ui::decodeUntyped(json("9007199254740993"));
    REQUIRE(untyped.has_value());
    REQUIRE(untyped->asInt() != nullptr);
    CHECK(*untyped->asInt() == kBeyond);
    CHECK(ui::encode(*untyped).get<std::int64_t>() == kBeyond);
    ui::Value const above{decimal(kBeyond, 1, 0)};
    ui::Value const below{decimal(kBeyond - 1, 1, 0)};
    CHECK_FALSE(above == below);
    CHECK(ui::decode(json(R"({"num":9007199254740993,"den":1,"dp":0})"), type("decimal")).value() == above);
}

// Mutation: drop the `den` or `dp` bound from decimalFrom.
TEST_CASE("ui::decode: a malformed decimal and a fractional number are refused with the reason", "[ui][expr]") {
    for (std::string_view const text : {R"({"num":1,"den":0,"dp":1})", R"({"num":1,"den":-2,"dp":1})",
                                        R"({"num":1,"den":2,"dp":19})", R"({"num":1.5,"den":2,"dp":1})"}) {
        INFO(text);
        std::expected<ui::Value, std::string> const read = ui::decode(json(text), type("decimal"));
        REQUIRE_FALSE(read.has_value());
        CHECK(read.error() == "a decimal needs an int64 `num`, a positive `den` and `dp` from 0 to 18");
    }
    std::expected<ui::Value, std::string> const fraction = ui::decodeUntyped(json("0.1"));
    REQUIRE_FALSE(fraction.has_value());
    CHECK(fraction.error() == "a number must be an int64; write a fraction as {num, den, dp}");
    CHECK(ui::decode(json(R"({"id":"x"})"), type("{id: int}")).error() == "field `id`: not an int");
    CHECK(ui::decode(json("{}"), type("{id: int}")).error() == "missing field `id`");
    CHECK(ui::decode(json("null"), type("int")).error() == "null where an int is required");
}

// Mutation: leave a record's keys in declaration order in canonicalJson.
TEST_CASE("ui::canonicalJson: the same content in any field order gives the same text", "[ui][expr]") {
    ui::Value const one{ui::ValueRecord{{"b", std::int64_t{1}}, {"a", ui::ValueRecord{{"z", true}, {"y", "t"}}}}};
    ui::Value const two{ui::ValueRecord{{"a", ui::ValueRecord{{"y", "t"}, {"z", true}}}, {"b", std::int64_t{1}}}};
    CHECK(ui::canonicalJson(one) == R"({"a":{"y":"t","z":true},"b":1})");
    CHECK(ui::canonicalJson(two) == ui::canonicalJson(one));
    CHECK(ui::canonicalJson(ui::Value{ui::QuantityValue{.amount = decimal(1, 4, 2), .unit = "g"}}) ==
          R"({"den":4,"dp":2,"num":1,"unit":"g"})");
}

// Mutations: let `null` fit a type that is not optional; let a list fit whatever its element type.
TEST_CASE("ui::parseType and fits: written types read back, and a value fits only where it belongs", "[ui][expr]") {
    for (std::string_view const text : {"int", "decimal?", "list<{id: key, at: timestamp?}>", "{}", "any"}) {
        CHECK(type(text).text() == text);
    }
    CHECK(ui::parseType("list<int").error() == "`list<T>` must end with `>`");
    CHECK(ui::parseType("money").error() == "unknown type `money`");
    CHECK(ui::fits(type("int"), type("key")));
    CHECK(ui::fits(type("int"), type("decimal?")));
    CHECK_FALSE(ui::fits(type("int?"), type("int")));
    CHECK_FALSE(ui::fits(type("list<string>"), type("list<int>")));
    CHECK(ui::fits(type("{id: int, note: string}"), type("{id: key}")));
    CHECK_FALSE(ui::fits(type("{id: int}"), type("{id: int, note: string}")));
}

// Mutations: ignore `x-decimalPlaces`; read `[T, "null"]` as T without the optional mark; skip `$ref`.
TEST_CASE("ui::typeFromSchema: a served reply schema becomes the type of the reply", "[ui][expr]") {
    ui::Json const schema = json(R"({
        "$defs": {"Entry": {"type": "object", "required": ["id", "amount", "at"], "properties": {
            "id": {"type": "integer"},
            "amount": {"type": "object", "x-decimalPlaces": 2,
                       "properties": {"num": {"type": "integer"}, "den": {"type": "integer"},
                                      "dp": {"type": "integer"}}},
            "at": {"type": ["string", "null"], "format": "date-time"}}}},
        "type": "array", "items": {"$ref": "#/$defs/Entry"}})");
    ui::Type const reply = ui::typeFromSchema(schema, schema);
    CHECK(reply.text() == "list<{id: int, amount: decimal, at: timestamp?}>");
    REQUIRE(reply.element().field("amount").has_value());
    CHECK(reply.element().field("amount")->places() == 2U);
}

// Mutations: accept a value of the wrong JSON kind for any one type (its row then decodes).
TEST_CASE("ui::decode: each type refuses the JSON of another kind", "[ui][expr]") {
    struct Row {
        std::string_view json;
        std::string_view type;
        std::string_view reason;
    };
    for (Row const& row : {
             Row{.json = "1", .type = "bool", .reason = "not a bool"},
             Row{.json = "true", .type = "int", .reason = "not an int"},
             Row{.json = "1", .type = "string", .reason = "not a string"},
             Row{.json = "true", .type = "key", .reason = "not a key"},
             Row{.json = "1", .type = "timestamp", .reason = "not a timestamp"},
             Row{.json = R"({"num":1,"den":1,"dp":0})",
                 .type = "quantity",
                 .reason = "a quantity is written {num, den, dp, unit}"},
             Row{.json = R"("x")", .type = "decimal", .reason = "a decimal is written {num, den, dp}"},
             Row{.json = "{}", .type = "list<int>", .reason = "not a list<int>"},
             Row{.json = "[1, true]", .type = "list<int>", .reason = "not an int"},
             Row{.json = "[]", .type = "{id: int}", .reason = "not a {id: int}"},
             Row{.json = R"({"num":1,"den":1,"dp":1,"unit":3})",
                 .type = "quantity",
                 .reason = "a quantity is written {num, den, dp, unit}"},
         }) {
        INFO(row.json << " as " << row.type);
        std::expected<ui::Value, std::string> const read = ui::decode(json(row.json), type(row.type));
        REQUIRE_FALSE(read.has_value());
        CHECK(read.error() == row.reason);
    }
    CHECK(ui::decode(json("3"), type("decimal")).value() == ui::Value{math::Rational{3, math::DecimalPlaces{0}}});
    CHECK(ui::decode(json("[1, 2]"), type("any")).value() ==
          ui::Value{ui::ValueList{std::int64_t{1}, std::int64_t{2}}});
    CHECK(ui::decodeUntyped(json(R"([1, 0.5])")).error() ==
          "a number must be an int64; write a fraction as {num, den, dp}");
    CHECK(ui::decodeUntyped(json(R"({"a": 0.5})")).error() ==
          "a number must be an int64; write a fraction as {num, den, dp}");
    CHECK(ui::decodeUntyped(json("18446744073709551615")).error() ==
          "a number must be an int64; write a fraction as {num, den, dp}");
}

// Mutation: let a written type end early (drop the trailing-text check in parseType).
TEST_CASE("ui::parseType: malformed written types are refused with the reason", "[ui][expr]") {
    struct Row {
        std::string_view text;
        std::string_view reason;
    };
    for (Row const& row : {
             Row{.text = "list", .reason = "`list` must be written `list<T>`"},
             Row{.text = "list<money>", .reason = "unknown type `money`"},
             Row{.text = "{id int}", .reason = "a record field must be written `name: type`"},
             Row{.text = "{id: money}", .reason = "unknown type `money`"},
             Row{.text = "{id: int", .reason = "a record type must end with `}`"},
             Row{.text = "int int", .reason = "unexpected `int` after the type"},
             Row{.text = "", .reason = "unknown type ``"},
         }) {
        INFO(row.text);
        std::expected<ui::Type, std::string> const parsed = ui::parseType(row.text);
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error() == row.reason);
    }
    CHECK(type("{a: {b: list<key?>}}?").text() == "{a: {b: list<key?>}}?");
    CHECK(type("{ }").text() == "{}");
}

// Mutations: read an unknown `type` as a string; follow a `$ref` the document does not define.
TEST_CASE("ui::typeFromSchema: shapes it cannot read are `any`", "[ui][expr]") {
    ui::Json const root = json(R"({"$defs": {}})");
    for (std::string_view const schema :
         {R"(3)", R"({"$ref": "#/$defs/Missing"})", R"({"type": "number"})", R"({"type": "object"})", R"({})"}) {
        INFO(schema);
        CHECK(ui::typeFromSchema(json(schema), root).kind() == ui::Type::Kind::Any);
    }
    CHECK(ui::typeFromSchema(json(R"({"type": "boolean"})"), root).text() == "bool");
    CHECK(ui::typeFromSchema(json(R"({"type": "array"})"), root).text() == "list<any>");
    CHECK(ui::typeFromSchema(json(R"({"type": ["object", "null"]})"), root).text() == "any");
    CHECK(ui::typeFromSchema(json(R"({"type": "object", "properties": {"num": {}, "den": {}, "dp": {}, "unit": {}}})"),
                             root)
              .text() == "quantity");
    CHECK(ui::typeFromSchema(json(R"({"type": "string", "x-decimalPlaces": "two"})"), root).text() == "string");
}
