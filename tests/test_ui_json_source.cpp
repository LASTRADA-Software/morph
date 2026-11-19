// SPDX-License-Identifier: Apache-2.0
//
// The source view's text (docs/spec/ui/custom_screens.md, "The client"): a document pretty-printed, and the place a
// refusal's JSON Pointer names in text a person may have edited, so the cursor can move there.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <morph/ui/json_source.hpp>
#include <morph/ui/value.hpp>
#include <optional>
#include <string>
#include <string_view>

namespace {

/// A document as a person might have left it: members on lines of their own, two names that need escaping.
constexpr std::string_view kEdited =
    "{\n"
    "  \"id\": \"a\",\n"
    "  \"view\": {\n"
    "    \"children\": [\n"
    "      {\"kind\": \"text\"},\n"
    "      {\"kind\": \"button\", \"a/b\": 1, \"t~n\": [2, {\"x\": \"\\\"q\\\"\"}]}\n"
    "    ]\n"
    "  }\n"
    "}";

/// The text from where @p pointer is found; fails the test when it is not found.
std::string_view at(std::string_view pointer) {
    std::optional<std::size_t> const found = morph::ui::pointerOffset(kEdited, pointer);
    REQUIRE(found.has_value());
    return kEdited.substr(*found);
}

}  // namespace

// Mutation: answer the text unchanged (no line breaks are added).
TEST_CASE("prettyJson writes one member per line in their order, and keeps what is not JSON", "[ui][json-source]") {
    std::string const pretty = morph::ui::prettyJson(R"({"vocab":"ui/1","id":"a","view":{"kind":"text","text":"x"}})");
    CHECK(std::ranges::count(pretty, '\n') >= 5);
    CHECK(pretty.find("\"vocab\"") < pretty.find("\"id\""));
    CHECK(pretty.find("\"id\"") < pretty.find("\"view\""));
    CHECK(morph::ui::parseJson(pretty).has_value());
    CHECK(morph::ui::prettyJson("{not json") == "{not json");
}

// Mutations: answer a member's value instead of its name (`/view` lands on `{`); drop the `~1` unescape (`a~1b` is
// not found); count array places from 1 (`/view/children/1` lands on the text element).
TEST_CASE("pointerOffset finds a member's name, an element, and escaped tokens in edited text", "[ui][json-source]") {
    CHECK(at("/view").starts_with("\"view\""));
    CHECK(at("/view/children/1").starts_with("{\"kind\": \"button\""));
    CHECK(at("/view/children/1/kind").starts_with("\"kind\": \"button\""));
    CHECK(at("/view/children/1/a~1b").starts_with("\"a/b\""));
    CHECK(at("/view/children/1/t~0n/1/x").starts_with("\"x\""));
    CHECK(morph::ui::pointerOffset(kEdited, "") == std::optional<std::size_t>{0});
    CHECK_FALSE(morph::ui::pointerOffset(kEdited, "/view/children/2").has_value());
    CHECK_FALSE(morph::ui::pointerOffset(kEdited, "/nope").has_value());
    CHECK_FALSE(morph::ui::pointerOffset(kEdited, "view").has_value());
    CHECK_FALSE(morph::ui::pointerOffset(kEdited, "/view/children/01").has_value());
    CHECK_FALSE(morph::ui::pointerOffset("{\"a\": [1, 2", "/a/5").has_value());
}

// Mutation: drop the depth bound (the 70-deep pointer is found).
TEST_CASE("pointerOffset stops at kMaxJsonDepth", "[ui][json-source]") {
    std::string const deep = std::string(70, '[') + "1" + std::string(70, ']');
    std::string shallow;
    std::string tooDeep;
    for (int level = 0; level < 70; ++level) {
        tooDeep += "/0";
        if (level < 10) {
            shallow += "/0";
        }
    }
    CHECK(morph::ui::pointerOffset(deep, shallow) == std::optional<std::size_t>{10});
    CHECK_FALSE(morph::ui::pointerOffset(deep, tooDeep).has_value());
}

// Mutation: count lines from 0 (it reports 5 where the button's `kind` is on line 6).
TEST_CASE("lineAt counts lines from 1", "[ui][json-source]") {
    std::optional<std::size_t> const kind = morph::ui::pointerOffset(kEdited, "/view/children/1/kind");
    REQUIRE(kind.has_value());
    CHECK(morph::ui::lineAt(kEdited, *kind) == 6);
    CHECK(morph::ui::lineAt(kEdited, 0) == 1);
    CHECK(morph::ui::lineAt(kEdited, 100000) == 9);
}

// Mutation: stop at the first match (the first `view` is found instead of the second).
TEST_CASE("pointerOffset answers the last of equal member names, and the match so far when the rest is cut short",
          "[ui][json-source]") {
    std::string_view const twice = R"({"view": 1, "view": {"a": 2}, "view": 3})";
    std::optional<std::size_t> const last = morph::ui::pointerOffset(twice, "/view");
    REQUIRE(last.has_value());
    CHECK(*last == twice.rfind("\"view\""));
    CHECK(morph::ui::pointerOffset(twice, "/view/a") == std::nullopt);
    std::string_view const inner = R"({"view": {"a": 1}, "view": {"a": 2}})";
    CHECK(morph::ui::pointerOffset(inner, "/view/a") == std::optional<std::size_t>{inner.rfind("\"a\"")});
    std::string_view const cut = "{\n\"view\": 1,\n\"view\": 2, \"x";
    CHECK(morph::ui::pointerOffset(cut, "/view") == std::optional<std::size_t>{cut.rfind("\"view\"")});
}

// Mutation: scanString stops at an escaped quote.
TEST_CASE("pointerOffset reads past escaped quotes in values and names", "[ui][json-source]") {
    std::string_view const afterValue = R"({"x": "\"q\"", "y": 1})";
    CHECK(morph::ui::pointerOffset(afterValue, "/y") == std::optional<std::size_t>{afterValue.find("\"y\"")});
    std::string_view const quoted = R"({"a\"b": 1})";
    CHECK(morph::ui::pointerOffset(quoted, "/a\"b") == std::optional<std::size_t>{1});
}

// Mutation: keep `\u` escapes as written.
TEST_CASE("pointerOffset compares member names with their \\u escapes decoded", "[ui][json-source]") {
    std::string_view const text = R"({"\u006bind": 1, "\u0041": 2, "\ud83d\ude00": 3})";
    CHECK(morph::ui::pointerOffset(text, "/kind") == std::optional<std::size_t>{1});
    CHECK(morph::ui::pointerOffset(text, "/A") == std::optional<std::size_t>{text.find("\"\\u0041\"")});
    CHECK_FALSE(morph::ui::pointerOffset(text, "/\\u0041").has_value());
    CHECK(morph::ui::pointerOffset(text, "/\xF0\x9F\x98\x80") == std::optional<std::size_t>{text.find("\"\\ud83d")});
}

// Mutation: accept anything between members and elements.
TEST_CASE("pointerOffset refuses members and elements that are not separated", "[ui][json-source]") {
    CHECK_FALSE(morph::ui::pointerOffset("[1 2]", "/1").has_value());
    CHECK_FALSE(morph::ui::pointerOffset("[1,,2]", "/1").has_value());
    CHECK_FALSE(morph::ui::pointerOffset("[1,,2]", "/2").has_value());
    CHECK_FALSE(morph::ui::pointerOffset("[1,]", "/1").has_value());
    CHECK_FALSE(morph::ui::pointerOffset(R"({"a": 1 "b": 2})", "/b").has_value());
    CHECK(morph::ui::pointerOffset("[1, 2]", "/1") == std::optional<std::size_t>{4});
}

// Mutation: count `\r` as a line break too (the line becomes 5, not 3).
TEST_CASE("pointerOffset and lineAt agree on CRLF text and non-ASCII values", "[ui][json-source]") {
    std::string_view const text = "{\r\n  \"name\": \"h\xC3\xA9llo \xE2\x82\xAC\",\r\n  \"target\": 1\r\n}";
    std::optional<std::size_t> const target = morph::ui::pointerOffset(text, "/target");
    REQUIRE(target.has_value());
    CHECK(*target == text.find("\"target\""));
    CHECK(morph::ui::lineAt(text, *target) == 3);
}
