// SPDX-License-Identifier: Apache-2.0
//
// The source view's text (docs/spec/ui/custom_screens.md, "The client"): a document pretty-printed, and the place a
// refusal's JSON Pointer names in text a person may have edited, so the cursor can move there.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <glaze/glaze.hpp>
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

namespace {

/// A document of @p rows button nodes, each with an action list too wide for a line once the whole row is on one.
std::string wideDocument(int rows) {
    std::string document = R"({"vocab":"ui/1","id":"a","title":"A","view":{"kind":"column","children":[)";
    for (int row = 0; row < rows; ++row) {
        document += std::string{row == 0 ? "" : ","} + R"({"kind":"button","label":"Button number )" +
                    std::to_string(row) + R"(","testId":"b)" + std::to_string(row) +
                    R"(","onClick":[{"run":"save"},{"host":"print"},{"set":{"path":"x","value":1.50}}]})";
    }
    return document + "]}}";
}

}  // namespace

// Mutation: answer the text unchanged (no line breaks are added); put every value on a line of its own; keep a
// container over 100 columns on one line.
TEST_CASE("prettyJson indents by 2, keeps a container that fits on one line, and keeps what is not JSON",
          "[ui][json-source]") {
    std::string const pretty = morph::ui::prettyJson(wideDocument(3));
    CHECK(pretty.find("\"vocab\"") < pretty.find("\"id\""));
    CHECK(pretty.find("\"id\"") < pretty.find("\"view\""));
    CHECK(pretty.contains("\n  \"vocab\": \"ui/1\",\n"));
    CHECK(pretty.contains("\n      {\n        \"kind\": \"button\",\n"));
    // What fits on a line stays on it, with a space after the colon and the comma.
    CHECK(pretty.contains("\"onClick\": [{\"run\": \"save\"}, {\"host\": \"print\"}, "));
    CHECK(morph::ui::prettyJson(R"({"kind":"text","text":"x"})") == R"({"kind": "text", "text": "x"})");
    CHECK(morph::ui::prettyJson(R"( { "a" : [ ] , "b" : { } } )") == R"({"a": [], "b": {}})");
    CHECK(morph::ui::prettyJson("{not json") == "{not json");
}

// Mutation: reformat a number or a string, drop or reorder a member, or break a container the line cannot hold.
TEST_CASE("prettyJson changes only the white space, and is much smaller than one value per line",
          "[ui][json-source]") {
    std::string const document = wideDocument(400);
    std::string const pretty = morph::ui::prettyJson(document);
    CHECK(morph::ui::compactJson(pretty) == document);
    CHECK(morph::ui::parseJson(pretty).has_value());
    std::string oneEach;
    glz::prettify_json(document, oneEach);
    INFO("compact " << document.size() << ", this " << pretty.size() << ", one per line " << oneEach.size());
    CHECK(pretty.size() * 10 < oneEach.size() * 7);
    // A line is a container that fits 100 columns, or a part of one that does not.
    for (std::size_t from = 0; from < pretty.size();) {
        std::size_t const end = std::min(pretty.find('\n', from), pretty.size());
        std::string_view const line = std::string_view{pretty}.substr(from, end - from);
        bool const closes = line.ends_with("}") || line.ends_with("},") || line.ends_with("]") || line.ends_with("],");
        bool const opens = line.ends_with("{") || line.ends_with("[");
        if (closes && !opens && line.find_first_not_of(' ') != std::string_view::npos &&
            (line[line.find_first_not_of(' ')] == '{' || line.contains(": {") || line.contains(": ["))) {
            CHECK(line.size() <= 101);
        }
        from = end + 1;
    }
    // A string longer than a line is not cut.
    std::string const longText = std::string(300, 'w');
    CHECK(morph::ui::prettyJson("{\"t\":\"" + longText + "\",\"u\":1}").contains(longText));
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

// Mutation: drop spaces inside strings, or reorder members, or compact text that is not JSON.
TEST_CASE("compactJson removes white space between tokens only, in order, and leaves non-JSON alone",
          "[ui][json-source]") {
    std::string const spaced = "{ \"b\" : [ 1 , 2 ],\n  \"a\" : \"x  y \\\" z\\\\\" , \"c\":{ } }";
    CHECK(morph::ui::compactJson(spaced) == R"({"b":[1,2],"a":"x  y \" z\\","c":{}})");
    CHECK(morph::ui::compactJson(morph::ui::prettyJson(R"({"k":[1,{"a":" "}],"j":null})")) ==
          R"({"k":[1,{"a":" "}],"j":null})");
    CHECK(morph::ui::compactJson("{ not json") == "{ not json");
}

// Mutation: let a member's name push its value past the line width without breaking the value, or leave a tab, a
// carriage return or a line feed between tokens.
TEST_CASE("prettyJson breaks a container that starts past the line width, and compactJson drops every kind of space",
          "[ui][json-source]") {
    std::string const name(120, 'k');
    std::string const document = R"({")" + name + R"(":{"a":1},"list":[[1],[2,[3]],"x"]})";
    std::string const pretty = morph::ui::prettyJson(document);
    CHECK(pretty.contains("\"" + name + "\": {\n    \"a\": 1\n  }"));
    CHECK(pretty.contains("\"list\": [[1], [2, [3]], \"x\"]"));
    CHECK(morph::ui::compactJson(pretty) == document);
    CHECK(morph::ui::compactJson("{\t\"a\"\r\n:\t[ 1,\r2 ]\n}") == R"({"a":[1,2]})");
}

namespace {

struct PointerCase {
    std::string_view text;
    std::string_view pointer;
    std::string_view marker;  // where the pointer is found: the first occurrence of this text; empty for not found
    bool last = false;        // the last occurrence instead
};

}  // namespace

// Mutations: an escape in a member's name not decoded; the pair of surrogates decoded as two; a name's `~1` read as
// `~0`; the first of equal names winning; a malformed tail after the match discarding the match.
TEST_CASE("pointerOffset finds a pointer's place in text of every shape a person may leave", "[ui][json-source][r3]") {
    using namespace std::string_view_literals;
    for (PointerCase const& item : {
             // The root, and the members and elements below it.
             PointerCase{.text = "  {\"a\":1}", .pointer = "", .marker = "{"},
             PointerCase{.text = "{\"a\":1}", .pointer = "/a", .marker = "\"a\""},
             PointerCase{.text = R"({"a":{"b":[10,20,30]}})", .pointer = "/a/b/1", .marker = "20"},
             PointerCase{.text = R"({"a":{"b":[10,20,30]}})", .pointer = "/a/b/2", .marker = "30"},
             PointerCase{.text = "[ 7 , [8] ]", .pointer = "/1/0", .marker = "8"},
             PointerCase{.text = "{ \r\n\t\"a\" \t: \n 1 }", .pointer = "/a", .marker = "\"a\""},
             // The last of equal names wins.
             PointerCase{.text = R"({"a":1,"a":2})", .pointer = "/a", .marker = "\"a\"", .last = true},
             // Names with escapes of their own, and pointers escaping `~` and `/`.
             PointerCase{.text = R"({"a/b":1,"c~d":2})", .pointer = "/a~1b", .marker = "\"a/b\""},
             PointerCase{.text = R"({"a/b":1,"c~d":2})", .pointer = "/c~0d", .marker = "\"c~d\""},
             PointerCase{.text = R"({"x\ty":1,"\r":2,"\b":3,"\f":4,"\n":5,"\"":6,"\\":7,"\/":8})",
                         .pointer = "/x\ty",
                         .marker = R"("x\ty")"},
             PointerCase{.text = R"({"x\ty":1,"\r":2,"\b":3,"\f":4,"\n":5})", .pointer = "/\r", .marker = R"("\r")"},
             PointerCase{.text = R"({"x\ty":1,"\r":2,"\b":3,"\f":4,"\n":5})", .pointer = "/\b", .marker = R"("\b")"},
             PointerCase{.text = R"({"x\ty":1,"\r":2,"\b":3,"\f":4,"\n":5})", .pointer = "/\f", .marker = R"("\f")"},
             PointerCase{.text = R"({"x\ty":1,"\r":2,"\b":3,"\f":4,"\n":5})", .pointer = "/\n", .marker = R"("\n")"},
             PointerCase{.text = R"({"q\"r":1,"s\\t":2,"u\/v":3})", .pointer = "/q\"r", .marker = "\"q"},
             PointerCase{.text = R"({"q\"r":1,"s\\t":2,"u\/v":3})", .pointer = "/s\\t", .marker = "\"s"},
             PointerCase{.text = R"({"q\"r":1,"s\\t":2,"u\/v":3})", .pointer = "/u~1v", .marker = "\"u"},
             // `\u` escapes of one, two and three bytes, a pair of surrogates, and a lone one.
             PointerCase{.text = R"({"\u0041":1})", .pointer = "/A", .marker = R"("\u0041")"},
             PointerCase{.text = R"({"\u00e9":1})", .pointer = "/\xC3\xA9", .marker = R"("\u00e9")"},
             PointerCase{.text = R"({"\u20ac":1})", .pointer = "/\xE2\x82\xAC", .marker = R"("\u20ac")"},
             PointerCase{.text = R"({"\ud83d\ude00":1})", .pointer = "/\xF0\x9F\x98\x80", .marker = "\"\\ud83d"},
             PointerCase{.text = R"({"\ud83dx":1})", .pointer = "/\xED\xA0\xBDx", .marker = "\"\\ud83d"},
             PointerCase{.text = R"({"\ud83d\u0041":1})",
                         .pointer = "/\xED\xA0\xBD"
                                    "A",
                         .marker = "\"\\ud83d"},
             // An escape in a value skipped on the way to a later member.
             PointerCase{.text = R"({"a":"\u00e9\n\"","b":2})", .pointer = "/b", .marker = "\"b\""},
             // What is not a pointer, and what is not there.
             PointerCase{.text = "{\"a\":1}", .pointer = "a", .marker = ""},
             PointerCase{.text = "{\"a\":1}", .pointer = "/a~", .marker = ""},
             PointerCase{.text = "{\"a\":1}", .pointer = "/a~2", .marker = ""},
             PointerCase{.text = "{\"a\":1}", .pointer = "/b", .marker = ""},
             PointerCase{.text = "{}", .pointer = "/a", .marker = ""},
             PointerCase{.text = "[]", .pointer = "/0", .marker = ""},
             PointerCase{.text = "[1,2]", .pointer = "/2", .marker = ""},
             PointerCase{.text = "[1,2]", .pointer = "/01", .marker = ""},
             PointerCase{.text = "[1,2]", .pointer = "/-1", .marker = ""},
             PointerCase{.text = "[1,2]", .pointer = "/1x", .marker = ""},
             PointerCase{.text = "[1,2]", .pointer = "/", .marker = ""},
             PointerCase{.text = "[1,2]", .pointer = "/a", .marker = ""},
             PointerCase{.text = "{\"a\":1}", .pointer = "/a/b", .marker = ""},
             PointerCase{.text = "5", .pointer = "/a", .marker = ""},
             PointerCase{.text = "\"s\"", .pointer = "/a", .marker = ""},
             PointerCase{.text = "", .pointer = "/a", .marker = ""},
             PointerCase{.text = "   ", .pointer = "/a", .marker = ""},
             // Text cut short or malformed, around a match or before one.
             PointerCase{.text = "{\"a\":1,", .pointer = "/a", .marker = "\"a\""},
             PointerCase{.text = R"({"a":1,"b":)", .pointer = "/a", .marker = "\"a\""},
             PointerCase{.text = R"({"a":1 "b":2})", .pointer = "/a", .marker = "\"a\""},
             PointerCase{.text = R"({"a":1,"b":"\u12)", .pointer = "/a", .marker = "\"a\""},
             PointerCase{.text = R"({"a":1,"b":"\u12G4"})", .pointer = "/a", .marker = "\"a\""},
             PointerCase{.text = R"({"a":1,"b" 2})", .pointer = "/a", .marker = "\"a\""},
             PointerCase{.text = R"({"a":1,"b":"x)", .pointer = "/a", .marker = "\"a\""},
             PointerCase{.text = R"({"a":1,"b":"x\)", .pointer = "/a", .marker = "\"a\""},
             PointerCase{.text = R"({"a":1,"b":[1,2)", .pointer = "/a", .marker = "\"a\""},
             PointerCase{.text = "[1,2", .pointer = "/1", .marker = "2"},
             PointerCase{.text = "[1,2,", .pointer = "/1", .marker = "2"},
             PointerCase{.text = "{\"a\" 1}", .pointer = "/a", .marker = ""},
             PointerCase{.text = "{\"a\"", .pointer = "/a", .marker = ""},
             PointerCase{.text = "{\"a\":}", .pointer = "/a", .marker = ""},
             PointerCase{.text = R"({"a":,"b":1})", .pointer = "/a", .marker = ""},
             PointerCase{.text = "{\"a\":]", .pointer = "/a", .marker = ""},
             PointerCase{.text = "{a:1}", .pointer = "/a", .marker = ""},
             PointerCase{.text = R"({"\u12G4":1})", .pointer = "/a", .marker = ""},
             PointerCase{.text = "{\"\\u12", .pointer = "/a", .marker = ""},
             PointerCase{.text = R"({"b":1,"a)", .pointer = "/a", .marker = ""},
             PointerCase{.text = "[1,]", .pointer = "/1", .marker = ""},
             PointerCase{.text = "[1 2]", .pointer = "/1", .marker = ""},
             PointerCase{.text = R"({"b":"x\)", .pointer = "/a", .marker = ""},
             PointerCase{.text = R"({"b":,"a":1})", .pointer = "/a", .marker = ""},
         }) {
        INFO(item.text << " at " << item.pointer);
        std::optional<std::size_t> const found = morph::ui::pointerOffset(item.text, item.pointer);
        if (item.marker.empty()) {
            CHECK_FALSE(found.has_value());
            continue;
        }
        std::size_t const expected = item.last ? item.text.rfind(item.marker) : item.text.find(item.marker);
        REQUIRE(expected != std::string_view::npos);
        CHECK(found == std::optional<std::size_t>{expected});
    }
}

TEST_CASE("pointerOffset refuses a pointer deeper than the depth the source view reads", "[ui][json-source][r3]") {
    std::string text;
    std::string pointer;
    for (std::size_t level = 0; level < 200; ++level) {
        text += "[";
        pointer += "/0";
    }
    text += "1";
    text.append(200, ']');
    CHECK_FALSE(morph::ui::pointerOffset(text, pointer).has_value());
    CHECK(morph::ui::pointerOffset(text, "/0/0").has_value());
}

// Mutations: a container cut short while it is skipped taking the member after it as found; a trailing comma inside a
// skipped container accepted.
TEST_CASE("pointerOffset skips containers that are empty, cut short or malformed on the way to a member",
          "[ui][json-source][r3]") {
    for (PointerCase const& item : {
             PointerCase{.text = R"({"b":{},"a":2})", .pointer = "/a", .marker = "\"a\""},
             PointerCase{.text = R"({"b":[],"a":2})", .pointer = "/a", .marker = "\"a\""},
             PointerCase{.text = R"({"b":[1,2],"a":3})", .pointer = "/a", .marker = "\"a\""},
             PointerCase{.text = R"({"b":{"x":[1,{"y":2}]},"a":3})", .pointer = "/a", .marker = "\"a\""},
             PointerCase{.text = R"({"b":{ "x" : 1 , "y" : 2 } , "a":3})", .pointer = "/a", .marker = "\"a\""},
             PointerCase{.text = R"({"a":1,"b":{)", .pointer = "/a", .marker = "\"a\""},
             PointerCase{.text = R"({"a":{"x":1,"y")", .pointer = "/a", .marker = "\"a\""},
             PointerCase{.text = "{\"a\":[1,2", .pointer = "/a", .marker = "\"a\""},
             PointerCase{.text = "{\"a\":[", .pointer = "/a", .marker = "\"a\""},
             PointerCase{.text = R"({"b":{"x")", .pointer = "/a", .marker = ""},
             PointerCase{.text = "{\"b\":[1,2", .pointer = "/a", .marker = ""},
             PointerCase{.text = "{\"b\":[", .pointer = "/a", .marker = ""},
             PointerCase{.text = "{\"b\":{", .pointer = "/a", .marker = ""},
             PointerCase{.text = R"({"b":{"x":1 )", .pointer = "/a", .marker = ""},
             PointerCase{.text = R"({"b":{"x" 1},"a":2})", .pointer = "/a", .marker = ""},
             PointerCase{.text = R"({"b":{"x":1,},"a":2})", .pointer = "/a", .marker = ""},
             PointerCase{.text = R"({"b":{1:2},"a":2})", .pointer = "/a", .marker = ""},
             PointerCase{.text = R"({"b":[1 2],"a":2})", .pointer = "/a", .marker = ""},
             PointerCase{.text = R"({"b":[,],"a":2})", .pointer = "/a", .marker = ""},
             PointerCase{.text = R"({"\ud83d\ude0":1})", .pointer = "/a", .marker = ""},
             PointerCase{.text = R"({"\ud83d\\":1,"a":2})", .pointer = "/a", .marker = "\"a\""},
             PointerCase{.text = R"({"\ud800":1})", .pointer = "/\xED\xA0\x80", .marker = R"("\ud800")"},
             PointerCase{
                 .text = R"({"\udc00\ud800":1})", .pointer = "/\xED\xB0\x80\xED\xA0\x80", .marker = "\"\\udc00"},
             PointerCase{.text = R"({"b":"\ud83d\ude00\ud83d","a":2})", .pointer = "/a", .marker = "\"a\""},
         }) {
        INFO(item.text << " at " << item.pointer);
        std::optional<std::size_t> const found = morph::ui::pointerOffset(item.text, item.pointer);
        if (item.marker.empty()) {
            CHECK_FALSE(found.has_value());
            continue;
        }
        std::size_t const expected = item.last ? item.text.rfind(item.marker) : item.text.find(item.marker);
        REQUIRE(expected != std::string_view::npos);
        CHECK(found == std::optional<std::size_t>{expected});
    }
}
