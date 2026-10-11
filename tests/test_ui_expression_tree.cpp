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
             Row{R"({"ref": "draft.title"})", Form::Ref, "draft.title"},
             Row{R"({"ref": "app.theme"})", Form::Ref, "app.theme"},
             Row{R"({"ref": "event"})", Form::Event, ""},
             Row{R"({"ref": "event.text"})", Form::Event, "text"},
             Row{R"({"ref": "eventCount"})", Form::Ref, "eventCount"},
             Row{R"({"row": "sample.id"})", Form::Row, "sample.id"},
             Row{R"({"query": "samples"})", Form::Query, "samples"},
             Row{R"({"result": "save"})", Form::Result, "save"},
             Row{R"({"pending": "save"})", Form::Pending, "save"},
             Row{R"({"error": "samples"})", Form::Error, "samples"},
             Row{R"({"errorKind": "save"})", Form::ErrorKind, "save"},
             Row{R"({"can": "approve"})", Form::Can, "approve"},
             Row{R"({"pref": "columns.order"})", Form::Pref, "columns.order"},
             Row{R"({"env": "widthClass"})", Form::Env, "widthClass"},
             Row{R"({"session": "displayName"})", Form::Session, "displayName"},
             Row{R"({"host": "sync.needsYouCount"})", Form::Host, "sync.needsYouCount"},
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
             Row{R"({"frobnicate": 1})", "/e", "`frobnicate` is not an expression form"},
             Row{"{}", "/e", R"(an empty object is not an expression; a constant is {"lit": {}})"},
             Row{"[1, 2]", "/e", R"(a list is written {"list": [...]}, or {"lit": [...]} for a constant)"},
             Row{"1.5", "/e", "a number must be an int64; write a fraction as {num, den, dp}"},
             Row{R"({"lit": 0.5})", "/e/lit", "a number must be an int64; write a fraction as {num, den, dp}"},
             Row{R"({"ref": "a", "row": "b"})", "/e/row", "`row` is not a key of the `ref` form"},
             Row{R"({"ref": ""})", "/e/ref", "`ref` takes a non-empty name"},
             Row{R"({"query": 3})", "/e/query", "`query` takes a non-empty name"},
             Row{R"({"fn": "launch", "args": []})", "/e/fn", "`launch` is not a function of expr/1"},
             Row{R"({"fn": "not", "args": [true, false]})", "/e", "`not` takes 1 argument, not 2"},
             Row{R"({"fn": "and"})", "/e", "`and` takes at least 1 argument, not 0"},
             Row{R"({"fn": "date", "args": [1, 2, 3]})", "/e", "`date` takes 1 to 2 arguments, not 3"},
             Row{R"({"fn": "add", "args": {"x": 1}})", "/e/args", "`args` is a list of expressions"},
             Row{R"({"fn": "add", "args": [1, {"bogus": 2}]})", "/e/args/1", "`bogus` is not an expression form"},
             Row{R"({"fn": "neg", "args": [1], "as": "x"})", "/e/as", "`as` is not a key of the `fn` form"},
             Row{R"({"t": "k", "args": [1]})", "/e/args", "`args` is an object of named expressions"},
             Row{R"({"t": "k", "fallback": 3})", "/e/fallback", "`fallback` is the message's text"},
             Row{R"({"if": [true, 1]})", "/e/if", "`if` takes [condition, then, else]"},
             Row{R"({"obj": [1]})", "/e/obj", "`obj` takes an object of named expressions"},
             Row{R"({"list": {"a": 1}})", "/e/list", "`list` takes a list of expressions"},
             Row{R"({"map": {"query": "q"}, "to": 1})", "/e", "`map` takes `as` and `to`"},
             Row{R"({"filter": {"query": "q"}, "as": "r", "to": 1})", "/e/to",
                 "`to` is not a key of the `filter` form"},
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
