// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <morph/ui/expression.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ui = morph::ui;
using Form = ui::Expr::Form;

namespace {

[[nodiscard]] ui::Json json(std::string_view text) {
    ui::Json out{};
    REQUIRE_FALSE(glz::read_json(out, text));
    return out;
}

[[nodiscard]] ui::Expr parsed(std::string_view text) {
    std::expected<ui::Expr, ui::Problem> tree = ui::parseExpression(json(text), "/e");
    INFO(text);
    if (!tree) {
        INFO(tree.error().path << ": " << tree.error().reason);
    }
    REQUIRE(tree.has_value());
    return *tree;
}

[[nodiscard]] ui::Problem refused(std::string_view text) {
    std::expected<ui::Expr, ui::Problem> tree = ui::parseExpression(json(text), "/e");
    INFO(text);
    REQUIRE_FALSE(tree.has_value());
    return tree.error();
}

// `{"list": [1, 1, ...]}` with @p elements constants: one node more than its elements.
[[nodiscard]] std::string listOf(std::size_t elements) {
    std::string out = R"({"list": [)";
    for (std::size_t index = 0; index < elements; ++index) {
        out += index == 0 ? "1" : ",1";
    }
    return out + "]}";
}

}  // namespace

// Mutation: map `pending` to the `Query` form (the table's row then fails), or keep `{"ref": "event"}` a `Ref`.
TEST_CASE("ui::parseExpression: every single-name form reads its name", "[ui][expr]") {
    struct Row {
        std::string_view text;
        Form form;
        std::string_view name;
    };
    for (Row const& row : {
             Row{.text = R"({"ref": "draft.title"})", .form = Form::Ref, .name = "draft.title"},
             Row{.text = R"({"ref": "app.theme"})", .form = Form::Ref, .name = "app.theme"},
             Row{.text = R"({"ref": "event"})", .form = Form::Event, .name = ""},
             Row{.text = R"({"ref": "event.text"})", .form = Form::Event, .name = "text"},
             Row{.text = R"({"ref": "eventCount"})", .form = Form::Ref, .name = "eventCount"},
             Row{.text = R"({"row": "sample.id"})", .form = Form::Row, .name = "sample.id"},
             Row{.text = R"({"query": "samples"})", .form = Form::Query, .name = "samples"},
             Row{.text = R"({"result": "save"})", .form = Form::Result, .name = "save"},
             Row{.text = R"({"pending": "save"})", .form = Form::Pending, .name = "save"},
             Row{.text = R"({"error": "samples"})", .form = Form::Error, .name = "samples"},
             Row{.text = R"({"errorKind": "save"})", .form = Form::ErrorKind, .name = "save"},
             Row{.text = R"({"can": "approve"})", .form = Form::Can, .name = "approve"},
             Row{.text = R"({"pref": "columns.order"})", .form = Form::Pref, .name = "columns.order"},
             Row{.text = R"({"env": "widthClass"})", .form = Form::Env, .name = "widthClass"},
             Row{.text = R"({"session": "displayName"})", .form = Form::Session, .name = "displayName"},
             Row{.text = R"({"host": "sync.needsYouCount"})", .form = Form::Host, .name = "sync.needsYouCount"},
         }) {
        INFO(row.text);
        ui::Expr const tree = parsed(row.text);
        CHECK(tree.form == row.form);
        CHECK(tree.name == row.name);
        CHECK(tree.path == "/e");
    }
}

// Mutations: read `{"lit": ...}`'s object as a form; let a comprehension's body share its list's path.
TEST_CASE("ui::parseExpression: the composite forms keep their operands and each node its path", "[ui][expr]") {
    CHECK(parsed("7").literal == ui::Value{std::int64_t{7}});
    CHECK(parsed(R"("seven")").literal == ui::Value{"seven"});
    CHECK(parsed("null").literal.isNull());
    ui::Expr const constant = parsed(R"({"lit": {"fn": "not"}})");
    CHECK(constant.form == Form::Literal);
    REQUIRE(constant.literal.field("fn") != nullptr);
    CHECK(*constant.literal.field("fn") == ui::Value{"not"});

    ui::Expr const call = parsed(R"({"fn": "add", "args": [1, {"if": [{"ref": "on"}, 2, {"row": "n"}]}]})");
    CHECK(call.form == Form::Call);
    CHECK(call.name == "add");
    REQUIRE(call.args.size() == 2);
    CHECK(call.args[1].form == Form::If);
    CHECK(call.args[1].path == "/e/args/1");
    REQUIRE(call.args[1].args.size() == 3);
    CHECK(call.args[1].args[2].path == "/e/args/1/if/2");
    CHECK(call.size() == 6);

    ui::Expr const message =
        parsed(R"({"t": "samples.count", "fallback": "{$n} samples", "args": {"n/all": {"query": "total"}}})");
    CHECK(message.form == Form::Translate);
    CHECK(message.name == "samples.count");
    CHECK(message.fallback == "{$n} samples");
    REQUIRE(message.keys.size() == 1);
    REQUIRE(message.args.size() == 1);
    CHECK(message.keys[0] == "n/all");
    CHECK(message.args[0].path == "/e/args/n~1all");

    ui::Expr const body = parsed(R"({"obj": {"id": {"row": "id"}, "items": {"list": [1, 2]}}})");
    CHECK(body.form == Form::Object);
    CHECK(body.keys == std::vector<std::string>{"id", "items"});
    REQUIRE(body.args.size() == 2);
    CHECK(body.args[1].form == Form::List);
    CHECK(body.args[1].args.size() == 2);

    ui::Expr const map = parsed(R"({"map": {"query": "rows"}, "as": "r", "to": {"ref": "r.id"}})");
    CHECK(map.form == Form::Map);
    CHECK(map.name == "r");
    REQUIRE(map.args.size() == 2);
    CHECK(map.args[0].path == "/e/map");
    CHECK(map.args[1].path == "/e/to");
    ui::Expr const filter = parsed(R"({"filter": {"query": "rows"}, "as": "r", "where": {"ref": "r.open"}})");
    CHECK(filter.form == Form::Filter);
    CHECK(filter.args[1].path == "/e/where");
}

// Mutation: compare the count with `>=` instead of `>` (256 nodes is then refused).
TEST_CASE("ui::parseExpression: 256 nodes load and 257 are refused", "[ui][expr]") {
    CHECK(parsed(listOf(ui::kMaxExpressionNodes - 1)).size() == ui::kMaxExpressionNodes);
    ui::Problem const tooBig = refused(listOf(ui::kMaxExpressionNodes));
    CHECK(tooBig.path == "/e/list/255");
    CHECK(tooBig.reason == "the expression holds more than 256 nodes");
}

// Mutations: return a null literal for an unknown form; skip the argument-count check; drop the extra-key check.
TEST_CASE("ui::parseExpression: a malformed form is refused with its path and the reason", "[ui][expr]") {
    struct Row {
        std::string_view text;
        std::string_view path;
        std::string_view reason;
    };
    for (Row const& row : {
             Row{.text = R"({"frobnicate": 1})", .path = "/e", .reason = "`frobnicate` is not an expression form"},
             Row{.text = "{}",
                 .path = "/e",
                 .reason = R"(an empty object is not an expression; a constant is {"lit": {}})"},
             Row{.text = "[1, 2]",
                 .path = "/e",
                 .reason = R"(a list is written {"list": [...]}, or {"lit": [...]} for a constant)"},
             Row{.text = "1.5",
                 .path = "/e",
                 .reason = "a number must be an int64; write a fraction as {num, den, dp}"},
             Row{.text = R"({"lit": 0.5})",
                 .path = "/e/lit",
                 .reason = "a number must be an int64; write a fraction as {num, den, dp}"},
             Row{.text = R"({"ref": "a", "row": "b"})",
                 .path = "/e/row",
                 .reason = "`row` is not a key of the `ref` form"},
             Row{.text = R"({"ref": ""})", .path = "/e/ref", .reason = "`ref` takes a non-empty name"},
             Row{.text = R"({"query": 3})", .path = "/e/query", .reason = "`query` takes a non-empty name"},
             Row{.text = R"({"fn": "launch", "args": []})",
                 .path = "/e/fn",
                 .reason = "`launch` is not a function of expr/1"},
             Row{.text = R"({"fn": "not", "args": [true, false]})",
                 .path = "/e",
                 .reason = "`not` takes 1 argument, not 2"},
             Row{.text = R"({"fn": "and"})", .path = "/e", .reason = "`and` takes at least 1 argument, not 0"},
             Row{.text = R"({"fn": "date", "args": [1, 2, 3]})",
                 .path = "/e",
                 .reason = "`date` takes 1 to 2 arguments, not 3"},
             Row{.text = R"({"fn": "add", "args": {"x": 1}})",
                 .path = "/e/args",
                 .reason = "`args` is a list of expressions"},
             Row{.text = R"({"fn": "add", "args": [1, {"bogus": 2}]})",
                 .path = "/e/args/1",
                 .reason = "`bogus` is not an expression form"},
             Row{.text = R"({"fn": "neg", "args": [1], "as": "x"})",
                 .path = "/e/as",
                 .reason = "`as` is not a key of the `fn` form"},
             Row{.text = R"({"t": "k", "args": [1]})",
                 .path = "/e/args",
                 .reason = "`args` is an object of named expressions"},
             Row{.text = R"({"t": "k", "fallback": 3})",
                 .path = "/e/fallback",
                 .reason = "`fallback` is the message's text"},
             Row{.text = R"({"if": [true, 1]})", .path = "/e/if", .reason = "`if` takes [condition, then, else]"},
             Row{.text = R"({"obj": [1]})", .path = "/e/obj", .reason = "`obj` takes an object of named expressions"},
             Row{.text = R"({"list": {"a": 1}})", .path = "/e/list", .reason = "`list` takes a list of expressions"},
             Row{.text = R"({"map": {"query": "q"}, "to": 1})", .path = "/e", .reason = "`map` takes `as` and `to`"},
             Row{.text = R"({"filter": {"query": "q"}, "as": "r", "to": 1})",
                 .path = "/e/to",
                 .reason = "`to` is not a key of the `filter` form"},
             Row{.text = R"({"lit": 1, "x": 2})", .path = "/e/x", .reason = "`x` is not a key of the `lit` form"},
             Row{.text = R"({"fn": 3})", .path = "/e/fn", .reason = "`fn` takes a non-empty name"},
             Row{.text = R"({"fn": "add", "args": [1, [2]]})",
                 .path = "/e/args/1",
                 .reason = R"(a list is written {"list": [...]}, or {"lit": [...]} for a constant)"},
             Row{.text = R"({"t": "k", "x": 1})", .path = "/e/x", .reason = "`x` is not a key of the `t` form"},
             Row{.text = R"({"t": 3})", .path = "/e/t", .reason = "`t` takes a non-empty name"},
             Row{.text = R"({"t": "k", "args": {"n": {"bogus": 1}}})",
                 .path = "/e/args/n",
                 .reason = "`bogus` is not an expression form"},
             Row{.text = R"({"if": [true, 1, 2], "x": 1})",
                 .path = "/e/x",
                 .reason = "`x` is not a key of the `if` form"},
             Row{.text = R"({"if": 3})", .path = "/e/if", .reason = "`if` takes [condition, then, else]"},
             Row{.text = R"({"if": [true, 1, {"bogus": 2}]})",
                 .path = "/e/if/2",
                 .reason = "`bogus` is not an expression form"},
             Row{.text = R"({"obj": {}, "x": 1})", .path = "/e/x", .reason = "`x` is not a key of the `obj` form"},
             Row{.text = R"({"obj": {"a/b~c": {"bogus": 1}}})",
                 .path = "/e/obj/a~1b~0c",
                 .reason = "`bogus` is not an expression form"},
             Row{.text = R"({"list": [], "x": 1})", .path = "/e/x", .reason = "`x` is not a key of the `list` form"},
             Row{.text = R"({"map": [1], "as": "x"})", .path = "/e", .reason = "`map` takes `as` and `to`"},
             Row{.text = R"({"map": {"lit": [1]}, "as": 3, "to": 1})",
                 .path = "/e/as",
                 .reason = "`as` takes a non-empty name"},
             Row{.text = R"({"map": {"bogus": 1}, "as": "x", "to": 1})",
                 .path = "/e/map",
                 .reason = "`bogus` is not an expression form"},
             Row{.text = R"({"filter": {"lit": [1]}, "as": "x", "where": {"bogus": 1}})",
                 .path = "/e/where",
                 .reason = "`bogus` is not an expression form"},
         }) {
        INFO(row.text);
        ui::Problem const problem = refused(row.text);
        CHECK(problem.path == row.path);
        CHECK(problem.reason == row.reason);
    }
}

// Mutation: let findFunction answer for any name.
TEST_CASE("ui::findFunction: the expr/1 library and only it", "[ui][expr]") {
    REQUIRE(ui::findFunction("round") != nullptr);
    CHECK(ui::findFunction("round")->minArgs == 2);
    CHECK(ui::findFunction("coalesce")->maxArgs == ui::kVariadic);
    CHECK(ui::findFunction("eval") == nullptr);
}
