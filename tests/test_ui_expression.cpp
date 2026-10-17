// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <format>
#include <morph/ui/expression.hpp>
#include <string>

#include "ui_corpus.hpp"

using morph::ui::checkExpression;
using morph::ui::evaluate;
using morph::ui::ExprPtr;
using morph::ui::Json;
using morph::ui::parseExpression;
using morph::ui::parseJson;
using morph::ui::PermissiveResolver;
using morph::ui::StaticEnvironment;
using morph::ui::UnitInfo;
using morph::ui::Value;
using morph::ui::ValueRecord;
using morph::ui::testing::member;
using morph::ui::testing::optionalMember;

namespace {

Json const& expressionsCorpus() {
    static Json const corpus = morph::ui::testing::loadCorpus(MORPH_UI_EXPRESSIONS_CORPUS);
    return corpus;
}

Value valueOf(Json const& json) {
    auto value = morph::ui::valueFromJson(json);
    REQUIRE(value.has_value());
    return *value;
}

ValueRecord recordOf(Json const& json) {
    Value const value = valueOf(json);
    REQUIRE(value.asRecord() != nullptr);
    return *value.asRecord();
}

void addUnits(morph::ui::UnitTable& units, Json const& json) {
    for (auto const& [id, entry] : std::get<Json::object_t>(json.data)) {
        auto const num = std::get<std::uint64_t>(member(entry, "num").data);
        auto const den = std::get<std::uint64_t>(member(entry, "den").data);
        UnitInfo info;
        info.canonical = std::get<std::string>(member(entry, "canonical").data);
        info.toCanonical = morph::math::Rational{morph::math::Numerator{static_cast<std::int64_t>(num)},
                                                 morph::math::Denominator{static_cast<std::int64_t>(den)},
                                                 morph::math::DecimalPlaces{0}};
        if (Json const* const display = optionalMember(entry, "display")) {
            info.display = std::get<std::string>(display->data);
        }
        units.insert_or_assign(id, info);
    }
}

std::set<std::string, std::less<>> namesOf(Json const& json) {
    std::set<std::string, std::less<>> names;
    for (Json const& name : std::get<Json::array_t>(json.data)) {
        names.insert(std::get<std::string>(name.data));
    }
    return names;
}

/// Builds the environment a corpus case describes.
void fill(StaticEnvironment& env, Json const* spec, ValueRecord& translations) {
    addUnits(env.format.units, member(expressionsCorpus(), "units"));
    if (spec == nullptr) {
        return;
    }
    auto record = [&](std::string_view key, ValueRecord& into) {
        if (Json const* const found = optionalMember(*spec, key)) {
            into = recordOf(*found);
        }
    };
    record("names", env.names);
    record("queries", env.queries);
    record("results", env.results);
    record("errors", env.errors);
    record("errorKinds", env.errorKinds);
    record("prefs", env.prefs);
    record("environment", env.environment);
    record("session", env.session);
    record("host", env.host);
    record("translations", translations);
    if (Json const* const row = optionalMember(*spec, "row")) {
        env.rowValue = valueOf(*row);
    }
    if (Json const* const pending = optionalMember(*spec, "pending")) {
        env.pendingNames = namesOf(*pending);
    }
    if (Json const* const permitted = optionalMember(*spec, "permitted")) {
        env.permitted = namesOf(*permitted);
    }
    if (Json const* const locale = optionalMember(*spec, "locale")) {
        env.format.locale = std::get<std::string>(locale->data);
    }
    if (Json const* const offset = optionalMember(*spec, "offset")) {
        env.format.utcOffsetMinutes = static_cast<std::int32_t>(std::get<std::uint64_t>(offset->data));
    }
    if (Json const* const units = optionalMember(*spec, "units")) {
        addUnits(env.format.units, *units);
    }
    if (Json const* const stamps = optionalMember(*spec, "timestamps")) {
        for (auto const& [name, text] : std::get<Json::object_t>(stamps->data)) {
            auto const instant = morph::time::DateTime::fromIso8601(std::get<std::string>(text.data));
            REQUIRE(instant.has_value());
            env.names.insert_or_assign(name, Value{*instant});
        }
    }
    if (!translations.empty()) {
        env.format.translations = [&translations](std::string_view key,
                                                  std::string_view locale) -> std::optional<std::string> {
            static_cast<void>(locale);
            auto const found = translations.find(key);
            if (found == translations.end()) {
                return std::nullopt;
            }
            return *found->second.asString();
        };
    }
}

}  // namespace

// Mutation: let `and` return null when any operand is null (no Kleene dominance) -> "and false null" fails.
// Mutation: drop the overflow check in `add` (checkedAdd -> operator+) -> "add overflows" fails.
TEST_CASE("ui expressions: the evaluation corpus", "[ui][expr]") {
    std::size_t cases = 0;
    for (Json const& entry : std::get<Json::array_t>(member(expressionsCorpus(), "evaluate").data)) {
        std::string const name = std::get<std::string>(member(entry, "name").data);
        CAPTURE(name);
        auto parsed = parseExpression(member(entry, "expr"));
        if (!parsed) {
            FAIL("refused: " << parsed.error().message());
        }
        StaticEnvironment env;
        ValueRecord translations;
        fill(env, optionalMember(entry, "env"), translations);
        Value const result = evaluate(**parsed, env);
        Value const expected = valueOf(member(entry, "expect"));
        CAPTURE(glz::write_json(morph::ui::valueToJson(result)).value_or("?"));
        CHECK(result == expected);
        std::size_t reports = 0;
        if (Json const* const count = optionalMember(entry, "reports")) {
            reports = static_cast<std::size_t>(std::get<std::uint64_t>(count->data));
        }
        for (auto const& diagnostic : env.diagnostics) {
            UNSCOPED_INFO(diagnostic.message());
        }
        CHECK(env.diagnostics.size() == reports);
        ++cases;
    }
    CHECK(cases > 200);
}

// Mutation: skip the companion-key check in the parser -> "a stray key" fails.
TEST_CASE("ui expressions: the refusal corpus", "[ui][expr]") {
    for (Json const& entry : std::get<Json::array_t>(member(expressionsCorpus(), "refused").data)) {
        std::string const name = std::get<std::string>(member(entry, "name").data);
        CAPTURE(name);
        auto parsed = parseExpression(member(entry, "expr"));
        REQUIRE_FALSE(parsed.has_value());
        CAPTURE(parsed.error().message());
        CHECK(parsed.error().reason.contains(std::get<std::string>(member(entry, "reason").data)));
        CHECK(parsed.error().path == std::get<std::string>(member(entry, "at").data));
    }
}

// Mutation: accept any kind for every function argument -> "an argument of the wrong kind" fails.
TEST_CASE("ui expressions: the static check corpus", "[ui][expr]") {
    PermissiveResolver const resolver;
    for (Json const& entry : std::get<Json::array_t>(member(expressionsCorpus(), "check").data)) {
        std::string const name = std::get<std::string>(member(entry, "name").data);
        CAPTURE(name);
        auto parsed = parseExpression(member(entry, "expr"));
        REQUIRE(parsed.has_value());
        auto checked = checkExpression(**parsed, resolver);
        if (Json const* const reason = optionalMember(entry, "reason")) {
            REQUIRE_FALSE(checked.has_value());
            CAPTURE(checked.error().message());
            CHECK(checked.error().reason.contains(std::get<std::string>(reason->data)));
            CHECK(checked.error().path == std::get<std::string>(member(entry, "at").data));
            continue;
        }
        if (!checked) {
            FAIL("refused: " << checked.error().message());
        }
        CHECK(checked->name() == std::get<std::string>(member(entry, "type").data));
    }
}

// Mutation: count only the outermost node against the bound -> the 257-node list is accepted.
TEST_CASE("ui expressions: the size bound is 256 nodes", "[ui][expr]") {
    auto listOf = [](std::size_t items) {
        std::string text = R"({"list": [)";
        for (std::size_t index = 0; index < items; ++index) {
            text += index == 0 ? "1" : ",1";
        }
        return text + "]}";
    };
    CHECK(parseExpression(*parseJson(listOf(255)), "/x").has_value());
    auto refused = parseExpression(*parseJson(listOf(256)), "/x");
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().reason.contains("more than 256 nodes"));

    std::string deep = "1";
    for (int level = 0; level < 300; ++level) {
        deep = std::format(R"({{"fn": "neg", "args": [{}]}})", deep);
    }
    CHECK_FALSE(parseJson(deep).has_value());
}

// Mutation: drop the once-per-path check in StaticEnvironment::report -> the second evaluation adds a report.
TEST_CASE("ui expressions: a runtime error is reported once per path", "[ui][expr]") {
    auto parsed = parseExpression(*parseJson(R"({"fn": "div", "args": [1, 0]})"), "/view/text");
    REQUIRE(parsed.has_value());
    StaticEnvironment env;
    CHECK(evaluate(**parsed, env).isNull());
    CHECK(evaluate(**parsed, env).isNull());
    REQUIRE(env.diagnostics.size() == 1);
    CHECK(env.diagnostics.front().path == "/view/text");
    CHECK(env.diagnostics.front().reason == "division by zero");
}

// Mutation: ignore FormatContext::dates -> the custom formatter's text is not returned.
TEST_CASE("ui expressions: the display functions use the client's formatting", "[ui][expr]") {
    StaticEnvironment env;
    env.format.numeric = morph::render::NumericLocale{.decimalSeparator = ",", .groupSeparator = "."};
    env.format.dates = [](morph::time::DateTime local, std::string_view style, bool withTime) {
        return std::string{style} + (withTime ? "+time " : " ") + local.toIso8601().substr(0, 4);
    };
    env.names.insert_or_assign("when", Value{*morph::time::DateTime::fromIso8601("2026-10-11T08:30:00Z")});
    auto run = [&env](std::string_view text) {
        auto parsed = parseExpression(*parseJson(text));
        REQUIRE(parsed.has_value());
        return evaluate(**parsed, env);
    };
    CHECK(run(R"({"fn": "number", "args": [1234567]})") == Value{"1.234.567"});
    CHECK(run(R"({"fn": "decimal", "args": [{"lit": {"num": 3, "den": 2, "dp": 1}}, 2]})") == Value{"1,50"});
    CHECK(run(R"({"fn": "date", "args": [{"ref": "when"}, "long"]})") == Value{"long 2026"});
    CHECK(run(R"({"fn": "dateTime", "args": [{"ref": "when"}]})") == Value{"medium+time 2026"});
    CHECK(morph::ui::functionNames().size() == 41);
    CHECK(morph::ui::isName("a_1"));
    CHECK_FALSE(morph::ui::isName("1a"));
    CHECK_FALSE(morph::ui::isName(""));
    CHECK_FALSE(morph::ui::isName("a-b"));
    CHECK_FALSE(morph::ui::isName("a~"));
}

// Mutation: parse with a default NumericLocale -> "12,5" does not parse and the first check fails.
TEST_CASE("ui expressions: parseDecimal reads typed text as an exact decimal in the client's locale", "[ui][expr]") {
    StaticEnvironment env;
    env.format.numeric = morph::render::NumericLocale{.decimalSeparator = ",", .groupSeparator = "."};
    auto const run = [&env](std::string_view text) {
        auto parsed = parseExpression(*parseJson(text));
        REQUIRE(parsed.has_value());
        return evaluate(**parsed, env);
    };
    auto const decimal = [](std::int64_t num, std::int64_t den, std::uint32_t places) {
        return Value{morph::ui::Decimal{morph::math::Rational{
            morph::math::Numerator{num}, morph::math::Denominator{den}, morph::math::DecimalPlaces{places}}}};
    };
    CHECK(run(R"({"fn": "parseDecimal", "args": ["12,5"]})") == decimal(25, 2, 1));
    CHECK(run(R"({"fn": "parseDecimal", "args": ["1.234,5", 3]})") == decimal(2469, 2, 3));
    // Text that is not a number yet, or finer than the places allow, is null and not an error.
    CHECK(run(R"({"fn": "parseDecimal", "args": [""]})").isNull());
    CHECK(run(R"({"fn": "parseDecimal", "args": ["12,"]})").isNull());
    CHECK(run(R"({"fn": "parseDecimal", "args": ["1,25", 1]})").isNull());
    CHECK(run(R"({"fn": "parseDecimal", "args": [null]})").isNull());
    CHECK(env.diagnostics.empty());
    // A wrong kind is a mismatch, reported.
    CHECK(run(R"({"fn": "parseDecimal", "args": ["1", 19]})").isNull());
    CHECK(env.diagnostics.size() == 1);
    StaticEnvironment fresh;
    auto parsed = parseExpression(*parseJson(R"({"fn": "parseDecimal", "args": [{"lit": 5}, 2]})"));
    REQUIRE(parsed.has_value());
    CHECK(evaluate(**parsed, fresh).isNull());
    CHECK(fresh.diagnostics.size() == 1);
}

// Mutation: make PermissiveResolver refuse refs -> the comprehension check of a plain ref fails.
TEST_CASE("ui expressions: a resolver decides what names mean", "[ui][expr]") {
    struct Refusing final : morph::ui::NameResolver {
        [[nodiscard]] std::expected<morph::ui::Type, std::string> ref(
            std::string_view head, std::span<std::string const> path) const override {
            static_cast<void>(path);
            return std::unexpected("no `" + std::string{head} + "`");
        }
        [[nodiscard]] std::expected<morph::ui::Type, std::string> row(
            std::span<std::string const> path) const override {
            static_cast<void>(path);
            return std::unexpected(std::string{"no row"});
        }
        [[nodiscard]] std::expected<morph::ui::Type, std::string> control(morph::ui::ExprKind form,
                                                                          std::string_view name) const override {
            static_cast<void>(form);
            return std::unexpected("no `" + std::string{name} + "`");
        }
        [[nodiscard]] std::expected<morph::ui::Type, std::string> pref(
            std::string_view point, std::span<std::string const> path) const override {
            static_cast<void>(path);
            return std::unexpected("no point `" + std::string{point} + "`");
        }
    };
    Refusing const resolver;
    auto refusal = [&resolver](std::string_view text) {
        auto parsed = parseExpression(*parseJson(text), "/e");
        REQUIRE(parsed.has_value());
        auto checked = checkExpression(**parsed, resolver);
        REQUIRE_FALSE(checked.has_value());
        return checked.error();
    };
    CHECK(refusal(R"({"ref": "x"})").reason == "no `x`");
    CHECK(refusal(R"({"row": "x"})").reason == "no row");
    CHECK(refusal(R"({"pending": "q"})").reason == "no `q`");
    CHECK(refusal(R"({"pref": "p"})").reason == "no point `p`");
    // A comprehension variable resolves before the resolver is asked.
    auto local = parseExpression(*parseJson(R"({"map": {"lit": [1]}, "as": "x", "to": {"ref": "x"}})"));
    REQUIRE(local.has_value());
    CHECK(checkExpression(**local, resolver).has_value());
    CHECK(checkExpression(**local, PermissiveResolver{}).has_value());
}
