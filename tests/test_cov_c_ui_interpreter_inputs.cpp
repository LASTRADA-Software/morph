// SPDX-License-Identifier: Apache-2.0
//
// The table inputs an action's schema declares: which members take a selection, a row or a cell edit.

#include <catch2/catch_test_macros.hpp>
#include <morph/table/query.hpp>
#include <morph/ui/document.hpp>
#include <morph/ui/interpreter.hpp>
#include <string>
#include <vector>

using morph::ui::TableInput;
using morph::ui::TableInputKind;
using morph::ui::tableInputsOf;

TEST_CASE("an action's schema names its table inputs by the keywords it carries", "[covc][ui][interpreter]") {
    auto const inputs = [](std::string const& schema) { return tableInputsOf(schema); };
    auto const member = [](TableInput const& input) { return input.member + ":" + input.record; };

    CHECK(inputs("not json").empty());
    CHECK(inputs("[1]").empty());
    CHECK(inputs("5").empty());
    CHECK(inputs(R"({})").empty());
    CHECK(inputs(R"({"properties":5})").empty());
    CHECK(inputs(R"({"properties":{"a":5,"b":"text","c":{}}})").empty());
    CHECK(inputs(R"({"properties":{"a":{"x-selectionOf":5,"x-rowOf":[1]}}})").empty());

    auto const picked =
        inputs(R"({"properties":{"rows":{"x-selectionOf":"Lims.rows"},"row":{"x-rowOf":"Lims.rows"}}})");
    REQUIRE(picked.size() == 2);
    CHECK(member(picked[0]) == "rows:Lims.rows");
    CHECK(picked[0].kind == TableInputKind::Selection);
    CHECK(member(picked[1]) == "row:Lims.rows");
    CHECK(picked[1].kind == TableInputKind::Row);

    auto const cell = inputs(R"({"x-cellEditOf":"Lims.rows","properties":{"key":{}}})");
    REQUIRE(cell.size() == 1);
    CHECK(member(cell[0]) == "key:Lims.rows");
    CHECK(cell[0].kind == TableInputKind::CellEdit);
    auto const row = inputs(R"({"x-rowEditOf":"Lims.rows"})");
    REQUIRE(row.size() == 1);
    CHECK(row[0].kind == TableInputKind::CellEdit);
    CHECK(inputs(R"({"x-cellEditOf":3,"x-rowEditOf":null})").empty());
    CHECK(inputs(R"({"x-cellEditOf":"A","x-rowEditOf":"B","properties":{"s":{"x-selectionOf":"C"}}})").size() == 3);
}

TEST_CASE("a list action's schema names the members that carry the table's query", "[covc][ui][document]") {
    using morph::ui::tableQueryMembers;
    auto const marker = std::string{morph::table::TableQuery::schemaMarker()};
    CHECK(tableQueryMembers("not json").empty());
    CHECK(tableQueryMembers("[1]").empty());
    CHECK(tableQueryMembers("{}").empty());
    CHECK(tableQueryMembers(R"({"properties":5})").empty());
    CHECK(tableQueryMembers(R"({"properties":{"a":5,"b":{},"c":{")" + marker + R"(":false},"d":{")" + marker +
                            R"(":"yes"}}})")
              .empty());
    CHECK(tableQueryMembers(R"({"properties":{"a":{")" + marker + R"(":true},"b":{},"c":{")" + marker +
                            R"(":true}}})") == std::vector<std::string>{"a", "c"});
}
