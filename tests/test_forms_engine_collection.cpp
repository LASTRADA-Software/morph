// SPDX-License-Identifier: Apache-2.0
//
// Collections, headless: the claims of src/qt/forms/tests/tst_collectionview.qml, the collection half of
// tst_LargeIdPrecision.qml and the confirm gate of tst_ViewChrome.qml, re-expressed on CollectionSession. What
// those files assert about a drawn view (cells in a table, a visible dialog, chrome slots) waits for the view.

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <morph/forms/engine/collection.hpp>
#include <morph/forms/engine/form_session.hpp>
#include <morph/table/data_source.hpp>
#include <morph/table/engine.hpp>
#include <morph/util/rational.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "forms_engine_support.hpp"

namespace {

using morph::forms::CollectionModel;
using morph::forms::CollectionSession;
using morph::test::formsengine::Call;
using morph::test::formsengine::Harness;

// The view and schemas of tst_collectionview.qml.
constexpr std::string_view kView = R"({
  "v-kind": "collection", "v-title": "Rows", "v-query": "ListRows", "v-rowKey": "id",
  "v-columns": [
    {"field": "id", "label": "ID", "kind": "integer", "v-hidden": true},
    {"field": "name", "label": "Name", "kind": "text"},
    {"field": "amount", "label": "Amount", "kind": "quantity", "x-decimalPlaces": 1,
     "ExtUnits": {"unitAscii": "kg", "unitUnicode": "kg"}}],
  "v-rowAction": {"action": "EditRow", "bind": {"id": "id"}},
  "v-actions": [
    {"action": "DeleteRow", "label": "Delete", "scope": "row", "bind": {"id": "id"}, "confirm": true},
    {"action": "CreateRow", "label": "New", "scope": "collection"}]})";

constexpr std::string_view kEditRow =
    R"({"properties":{"id":{"type":"integer","x-order":0},"name":{"type":"string","x-order":1}},"required":["id","name"]})";

constexpr std::string_view kRows = R"({"rows":[{"id":1,"name":"First","amount":{"num":15,"den":10,"dp":1}},)"
                                   R"({"id":2,"name":"Second","amount":{"num":30,"den":10,"dp":1}}]})";

[[nodiscard]] CollectionModel viewModel(std::string_view json) {
    auto model = CollectionModel::fromSchema(json);
    INFO((model ? std::string{} : model.error().path + ": " + model.error().message));
    REQUIRE(model.has_value());
    return *std::move(model);
}

[[nodiscard]] morph::forms::SchemaLookup editRowSchema() {
    return [](std::string_view action) -> std::optional<std::string> {
        return action == "EditRow" ? std::optional<std::string>{std::string{kEditRow}} : std::nullopt;
    };
}

/// Lists kRows for ListRows, answers every other action `{"ok":true}`.
[[nodiscard]] std::optional<std::string> rowsServer(Call const& call) {
    if (call.action == "ListRows") {
        return std::string{kRows};
    }
    return std::string{R"({"ok":true})"};
}

struct Stack {
    Harness harness;
    std::unique_ptr<CollectionSession> view;

    explicit Stack(std::string_view json = kView) {
        harness.server.replyWith(rowsServer);
        view = std::make_unique<CollectionSession>(harness.rt, viewModel(json), editRowSchema(),
                                                   harness.server.submitter(), harness.server.fetcher());
        harness.settle();
    }

    [[nodiscard]] std::size_t count(std::string_view action) const {
        std::size_t total = 0;
        for (Call const& call : harness.server.submits()) {
            total += call.action == action ? 1U : 0U;
        }
        return total;
    }

    [[nodiscard]] std::optional<std::string> lastBody(std::string_view action) const {
        std::optional<std::string> body;
        for (Call const& call : harness.server.submits()) {
            if (call.action == action) {
                body = call.body;
            }
        }
        return body;
    }
};

}  // namespace

TEST_CASE("CollectionModel: the v-* document reads into columns, the row action and the actions",
          "[forms-engine][collection]") {
    CollectionModel const model = viewModel(kView);
    CHECK(model.title() == "Rows");
    CHECK(model.query() == "ListRows");
    CHECK(model.rowKey() == "id");
    REQUIRE(model.columns().size() == 3);
    CHECK(model.columns()[0].hidden);
    CHECK(model.columns()[0].kind == morph::table::ColumnKind::Integer);
    CHECK(model.columns()[2].kind == morph::table::ColumnKind::Quantity);
    CHECK(model.columns()[2].unit == "kg");
    CHECK(model.columns()[2].decimalPlaces == 1U);
    REQUIRE(model.rowAction().has_value());
    CHECK(model.rowAction()->action == "EditRow");
    REQUIRE(model.actions().size() == 2);
    CHECK(model.actions()[0].confirm);
    CHECK(model.actions()[1].scope == morph::forms::CollectionActionScope::Collection);

    CHECK_FALSE(CollectionModel::fromSchema(R"({"v-kind":"collection"})").has_value());  // no v-query
    CHECK(CollectionModel::fromSchema(R"({"v-query":"Q","v-kind":"grid"})").error().path == "v-kind");
    CHECK(CollectionModel::fromSchema(R"({"v-query":"Q","v-columns":[{"field":"a","kind":"colour"}]})").error().path ==
          "v-columns.a");
    auto const defaults = CollectionModel::fromSchema(R"({"v-query":"Q"})");
    REQUIRE(defaults.has_value());
    CHECK(defaults->title() == "Q");
    CHECK(defaults->rowKey() == "id");
    CHECK(defaults->kind() == morph::forms::CollectionKind::Collection);
}

TEST_CASE("CollectionSession: the list loads on construction; cells are exact", "[forms-engine][collection]") {
    Stack stack;
    CHECK(stack.count("ListRows") == 1);
    CHECK(stack.lastBody("ListRows") == "{}");
    auto const& rows = stack.view->rows();
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].key == "1");
    CHECK(rows[0].cells == std::vector<std::string>{"First", "1.5 kg"});  // the hidden id is not a cell
    CHECK(rows[1].cells == std::vector<std::string>{"Second", "3.0 kg"});
    CHECK_FALSE(stack.view->loading());
}

TEST_CASE("CollectionSession: the rows are the table engine's source, keyed and typed by column",
          "[forms-engine][collection]") {
    Stack stack;
    auto const snapshot = stack.view->source()->tableSnapshot();
    REQUIRE(snapshot->rowCount() == 2);
    CHECK(stack.view->source()->columns().size() == 3);
    CHECK(std::get<std::int64_t>(snapshot->rowId(0)) == 1);
    CHECK(std::get<std::int64_t>(snapshot->cell(0, 0)) == 1);
    CHECK(std::get<std::string>(snapshot->cell(1, 1)) == "Second");
    CHECK(
        std::get<morph::math::Rational>(snapshot->cell(1, 2)) ==
        morph::math::Rational{morph::math::Numerator{3}, morph::math::Denominator{1}, morph::math::DecimalPlaces{1}});

    // The table engine sorts the source: descending by amount puts Second first.
    morph::table::Engine engine{stack.view->source()};
    REQUIRE(engine.setSort({{.column = "amount", .dir = morph::table::SortDirection::Descending}}).has_value());
    engine.settleNow();
    REQUIRE(engine.viewRowCount() == 2);
    CHECK(std::get<std::int64_t>(engine.rowIdAt(0)) == 2);

    // A refresh with new rows replaces the source.
    stack.harness.server.replyWith([](Call const& call) -> std::optional<std::string> {
        return call.action == "ListRows" ? std::optional<std::string>{R"([{"id":7,"name":"Only"}])"} : std::nullopt;
    });
    stack.view->refresh();
    stack.harness.settle();
    REQUIRE(stack.view->source()->tableSnapshot()->rowCount() == 1);
    CHECK(std::get<std::int64_t>(stack.view->source()->tableSnapshot()->rowId(0)) == 7);
}

TEST_CASE("CollectionSession: opening a row prefills only its bound fields and fires nothing",
          "[forms-engine][collection]") {
    Stack stack;
    auto* const editor = stack.view->editor();
    REQUIRE(editor != nullptr);
    stack.view->open("1");
    stack.harness.settle();
    CHECK(stack.view->openRow() == "1");
    CHECK(editor->field("id").text().peek() == "1");
    CHECK(editor->field("name").text().peek().empty());  // not bound
    CHECK(stack.count("EditRow") == 0);
    CHECK(stack.count("ListRows") == 1);
}

TEST_CASE("CollectionSession: an edit after opening fires, closes the editor and reloads the list",
          "[forms-engine][collection]") {
    Stack stack;
    stack.view->open("1");
    stack.harness.settle();
    stack.harness.type(*stack.view->editor(), "name", "Renamed");
    CHECK(stack.lastBody("EditRow") == R"({"id":1,"name":"Renamed"})");
    CHECK_FALSE(stack.view->openRow().has_value());  // closed once the edit resolved
    CHECK(stack.count("ListRows") == 2);             // and the list reloaded
}

TEST_CASE("CollectionSession: a failed edit keeps the editor open and reloads nothing", "[forms-engine][collection]") {
    Stack stack;
    stack.harness.server.replyWith([](Call const& call) -> std::optional<std::string> {
        return call.action == "ListRows" ? std::optional<std::string>{std::string{kRows}} : std::nullopt;
    });
    stack.view->open("2");
    stack.harness.settle();
    stack.harness.type(*stack.view->editor(), "name", "Nope");
    REQUIRE(stack.count("EditRow") == 1);
    stack.harness.server.rejectSubmit(stack.harness.server.submits().size() - 1, "refused");
    stack.harness.settle();
    CHECK(stack.view->openRow() == "2");
    CHECK(stack.count("ListRows") == 1);
}

TEST_CASE("CollectionSession: a confirmed row action fires its bound body and reloads; a cancelled one fires nothing",
          "[forms-engine][collection]") {
    Stack stack;
    stack.view->fire(0, "1");
    stack.harness.settle();
    REQUIRE(stack.view->pendingConfirm().has_value());
    CHECK(stack.view->pendingConfirm()->rowKey == "1");
    CHECK(stack.count("DeleteRow") == 0);  // held until answered
    stack.view->cancelConfirm();
    stack.harness.settle();
    CHECK_FALSE(stack.view->pendingConfirm().has_value());
    CHECK(stack.count("DeleteRow") == 0);

    stack.view->fire(0, "1");
    stack.view->confirm();
    stack.harness.settle();
    CHECK(stack.lastBody("DeleteRow") == R"({"id":1})");
    CHECK(stack.count("ListRows") == 2);
    CHECK_FALSE(stack.view->actionPending());
}

TEST_CASE("CollectionSession: a collection action fires with an empty body", "[forms-engine][collection]") {
    Stack stack;
    stack.view->fire(1);
    stack.harness.settle();
    CHECK(stack.lastBody("CreateRow") == "{}");
    CHECK(stack.count("ListRows") == 2);

    // A collection action is on no row: its bind entries have nothing to read, even when a row key is passed.
    Stack bound{R"({"v-query":"ListRows","v-actions":[
      {"action":"Export","scope":"collection","bind":{"id":"id"}}]})"};
    bound.view->fire(0, "1");
    bound.harness.settle();
    CHECK(bound.lastBody("Export") == "{}");
}

TEST_CASE("CollectionSession: nothing typed into one row's editor carries over to the next opening",
          "[forms-engine][collection]") {
    Stack stack;
    stack.harness.server.replyWith([](Call const& call) -> std::optional<std::string> {
        return call.action == "ListRows" ? std::optional<std::string>{std::string{kRows}} : std::nullopt;
    });
    auto* const editor = stack.view->editor();
    stack.view->open("1");
    stack.harness.settle();
    stack.harness.type(*editor, "name", "Typed into row one");
    CHECK(stack.lastBody("EditRow") == R"({"id":1,"name":"Typed into row one"})");
    std::size_t const edits = stack.count("EditRow");

    stack.view->open("2");
    stack.harness.settle();
    CHECK(stack.view->openRow() == "2");
    CHECK(editor->field("name").text().peek().empty());
    CHECK(editor->field("id").text().peek() == "2");
    CHECK(stack.count("EditRow") == edits);  // opening fired nothing

    stack.view->open("2");  // reopening the same row starts clean as well
    stack.harness.settle();
    CHECK(editor->field("name").text().peek().empty());
    CHECK(stack.count("EditRow") == edits);
}

TEST_CASE("CollectionSession: ids beyond 2^53 stay distinct in keys, cells, the source and bodies",
          "[forms-engine][collection]") {
    constexpr std::string_view kLargeView = R"({"v-kind":"collection","v-query":"ListRows","v-rowKey":"id",
      "v-columns":[{"field":"id","label":"ID","kind":"integer"},{"field":"name","label":"Name"}],
      "v-actions":[{"action":"DeleteRow","label":"Delete","scope":"row","bind":{"id":"id"}}]})";
    Harness harness;
    harness.server.replyWith([](Call const& call) -> std::optional<std::string> {
        if (call.action == "ListRows") {
            return std::string{
                R"({"rows":[{"id":9007199254740993,"name":"Alpha"},{"id":9007199254740992,"name":"Beta"},)"
                R"({"id":42,"name":"Plain"}]})"};
        }
        return std::string{"{}"};
    });
    CollectionSession view{harness.rt, viewModel(kLargeView), {}, harness.server.submitter(), {}};
    harness.settle();
    auto const& rows = view.rows();
    REQUIRE(rows.size() == 3);
    CHECK(rows[0].key == "9007199254740993");
    CHECK(rows[1].key == "9007199254740992");
    CHECK(rows[0].cells.front() == "9007199254740993");
    CHECK(rows[2].key == "42");
    CHECK(std::get<std::int64_t>(view.source()->tableSnapshot()->rowId(0)) == 9007199254740993);
    CHECK(view.editor() == nullptr);  // no row action

    view.fire(0, "9007199254740993");
    harness.settle();
    auto const& submits = harness.server.submits();
    REQUIRE(submits.size() == 3);  // the list, the delete, the reload the delete triggered
    CHECK(submits[1].action == "DeleteRow");
    CHECK(submits[1].body == R"({"id":9007199254740993})");
}

TEST_CASE("CollectionModel: a malformed view document names where", "[forms-engine][collection]") {
    auto const where = [](std::string_view json) {
        auto const model = CollectionModel::fromSchema(json);
        return model.has_value() ? std::string{"(read)"} : model.error().path;
    };
    CHECK(where("not json").empty());
    CHECK(where("[]").empty());
    CHECK(where(R"({"v-query":"Q","v-columns":{}})") == "v-columns");
    CHECK(where(R"({"v-query":"Q","v-columns":[{"label":"x"}]})") == "v-columns");
    CHECK(where(R"({"v-query":"Q","v-rowAction":{"bind":{}}})") == "v-rowAction");
    CHECK(where(R"({"v-query":"Q","v-actions":{}})") == "v-actions");
    CHECK(where(R"({"v-query":"Q","v-actions":[{"label":"x"}]})") == "v-actions");

    auto const lenient = CollectionModel::fromSchema(R"({"v-query":"Q","v-columns":[
      {"field":"a","v-hidden":"yes","ExtUnits":{"unitAscii":"kg","unitUnicode":""}},
      {"field":"b","ExtUnits":{}}],
      "v-actions":[{"action":"Go","confirm":"yes","bind":{"id":"id","n":3}}]})");
    REQUIRE(lenient.has_value());
    CHECK_FALSE(lenient->columns()[0].hidden);  // not a boolean: not hidden
    CHECK(lenient->columns()[0].unit == "kg");
    CHECK(lenient->columns()[1].unit.empty());
    CHECK_FALSE(lenient->actions()[0].confirm);
    CHECK(lenient->actions()[0].bind == std::vector<std::pair<std::string, std::string>>{{"id", "id"}});
}

TEST_CASE("CollectionSession: cells and table cells for every member shape", "[forms-engine][collection]") {
    namespace engine = morph::forms::detail::engine;
    auto const row = engine::parseJson(R"({"flag":true,"ratio":0.5,"big":18446744073709551615,"n":null,
      "plain":{"num":5,"den":2},"odd":{"num":1,"den":0},"other":{"x":1},"list":[1]})");
    REQUIRE(row.has_value());
    morph::forms::CollectionColumn column{};
    auto const cell = [&](std::string_view field, morph::table::ColumnKind kind = morph::table::ColumnKind::Text) {
        column.field = std::string{field};
        column.kind = kind;
        return engine::tableCell(*row, column);
    };
    CHECK(std::get<bool>(cell("flag")));
    CHECK(std::get<double>(cell("ratio", morph::table::ColumnKind::Number)) == 0.5);
    CHECK(std::get<std::string>(cell("ratio")) == "0.5");
    CHECK(std::get<std::string>(cell("big")) == "18446744073709551615");
    CHECK(std::holds_alternative<std::monostate>(cell("n")));
    CHECK(std::holds_alternative<morph::math::Rational>(cell("plain")));
    CHECK(std::get<std::string>(cell("odd")) == R"({"num":1,"den":0})");
    CHECK(std::get<std::string>(cell("other")) == R"({"x":1})");
    CHECK(std::get<std::string>(cell("list")) == "[1]");

    column.field = "plain";
    column.unit.clear();
    column.decimalPlaces = 2;
    CHECK(engine::cellText(*row, column) == "2.50");  // no dp of its own: the column's places, no unit
    column.field = "odd";
    CHECK(engine::cellText(*row, column) == R"({"num":1,"den":0})");
    column.field = "n";
    CHECK(engine::cellText(*row, column).empty());
}

TEST_CASE("CollectionSession: a reply's rows, keys and an editor that cannot be built", "[forms-engine][collection]") {
    Harness harness;
    std::string reply = R"([{"name":"no key"},{"id":1.5,"name":"fraction"}])";
    harness.server.replyWith([&reply](Call const& call) -> std::optional<std::string> {
        if (call.action == "ListRows") {
            return reply;
        }
        return std::string{"{}"};
    });
    auto const schemas = [](std::string_view action) -> std::optional<std::string> {
        return action == "EditRow" ? std::optional<std::string>{"not a schema"} : std::nullopt;
    };
    CHECK_THROWS_AS(CollectionSession(harness.rt, viewModel(kView), schemas, {}, {}), std::invalid_argument);
    CollectionSession view{harness.rt, viewModel(kView), schemas, harness.server.submitter(), {}};
    harness.settle();
    CHECK(view.editor() == nullptr);  // the row action's schema does not read
    view.open("0");                   // no editor: nothing opens
    CHECK_FALSE(view.openRow().has_value());
    REQUIRE(view.rows().size() == 2);
    CHECK(view.rows()[0].key == "0");  // no key: its position
    CHECK(view.rows()[1].key == "1.5");
    CHECK(std::get<std::string>(view.rows()[1].id) == "1.5");  // a fractional key is text
    view.confirm();                                            // nothing pending: nothing sent
    view.fire(0, "missing");
    view.confirm();
    harness.settle();
    std::optional<std::string> deleted;
    for (Call const& call : harness.server.submits()) {
        if (call.action == "DeleteRow") {
            deleted = call.body;
        }
    }
    CHECK(deleted == R"({"id":null})");  // an unknown row binds null

    reply = R"({"total":0})";
    view.refresh();
    harness.settle();
    CHECK(view.rows().empty());
    reply = "not json";
    view.refresh();
    harness.settle();
    CHECK(view.rows().empty());
    reply = R"({"total":1,"items":[{"id":3}]})";
    view.refresh();
    harness.settle();
    REQUIRE(view.rows().size() == 1);
    CHECK(view.rows()[0].key == "3");
}
