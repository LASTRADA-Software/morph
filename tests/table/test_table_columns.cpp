// SPDX-License-Identifier: Apache-2.0
//
// Column metadata a table derives from a row type (spec 7 §4):
// morph::views::detail::deriveColumns emits each column's kind, title,
// unit alternatives and enum values, and leaves out x-hidden members.
//
// Mutations these tests were seen to fail on:
//  - deriveColumns not skipping x-hidden members: "derived columns carry
//    kind, title and metadata" fails (nine columns, not eight).
//  - inferColumnKind swapping quantity and decimal: the same test fails.
//  - ColumnLayout::apply ignoring `hidden`: "a stored layout is fitted to the
//    table" fails.
//  - ColumnLayout's fitting rule appending a new column last: the `apply` half of
//    "a new column takes its declared place" fails.
//  The column-group, typed-row group and other layout cases name their own mutations above them.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <glaze/glaze.hpp>
#include <map>
#include <memory>
#include <morph/forms/forms.hpp>
#include <morph/forms/views.hpp>
#include <morph/table/data_source.hpp>
#include <morph/table/engine.hpp>
#include <morph/table/query.hpp>
#include <morph/util/datetime.hpp>
#include <morph/util/quantity.hpp>
#include <morph/util/rational.hpp>
#include <optional>
#include <string>
#include <vector>

#include "table/table_fixtures.hpp"

namespace tcol {

enum class TcUnit : std::uint8_t { g, kg };
enum class TcStatus : std::uint8_t { Open, Closed };

}  // namespace tcol

template <>
struct morph::units::UnitTraits<tcol::TcUnit> {
    static constexpr morph::units::UnitMeta meta(tcol::TcUnit unit) noexcept {
        return unit == tcol::TcUnit::g ? morph::units::UnitMeta{.id = "g", .display = "g", .defaultDecimals = 3}
                                       : morph::units::UnitMeta{.id = "kg", .display = "kg", .defaultDecimals = 3};
    }
};

template <>
struct glz::meta<tcol::TcStatus> {
    using enum tcol::TcStatus;
    static constexpr auto value = glz::enumerate("open", Open, "closed", Closed);
};

namespace tcol {

struct TcRow {
    std::int64_t id = 0;
    std::string analysis;
    morph::math::Rational price;
    morph::units::Quantity<TcUnit::g> mass;
    double ph = 0;
    bool approved = false;
    morph::time::DateTime measuredOn;
    TcStatus status = TcStatus::Open;
    std::int64_t secret = 0;

    static constexpr auto fieldMetadata = std::array{
        morph::forms::FieldMeta{.field = "measuredOn", .label = "Measured on"},
        morph::forms::FieldMeta{.field = "secret", .hidden = true},
    };
};

struct NoOverride {};

}  // namespace tcol

namespace tcol {

// A row with a form layout and a column group: each writes its own schema key.
struct LayoutGroupRow {
    std::string companyA;
    std::string companyB;
    std::string name;

    static constexpr std::array<std::string_view, 2> kCompany{"companyA", "companyB"};
    static constexpr std::array<std::string_view, 3> kAll{"companyA", "companyB", "name"};
    static constexpr std::array formLayout{morph::forms::FieldGroup{.title = "Parties", .fields = kAll}};
    static constexpr std::array<morph::table::ColumnGroup, 1> columnGroups() noexcept {
        return {{{.id = "company", .columns = kCompany}}};
    }
};

struct EpochRow {
    std::int64_t day = 0;
    std::int64_t at = 0;
    std::string code;

    static constexpr auto fieldMetadata = std::array{
        morph::forms::FieldMeta{.field = "day", .widget = "epochDays"},
        morph::forms::FieldMeta{.field = "at", .widget = "epochSeconds"},
    };
};

struct HideNothing {};

struct OptionalRow {
    std::int64_t id = 0;
    std::optional<std::int64_t> count;
    std::optional<double> ph;
    std::optional<morph::math::Rational> price;
    std::optional<TcStatus> status;
    std::optional<morph::time::DateTime> at;
    std::optional<morph::units::Quantity<TcUnit::g>> mass;
};

struct NameSecret {
    static constexpr std::array columns{morph::views::ColumnOverride{.field = "secret"}};
};

}  // namespace tcol

TEST_CASE("table: derived columns carry kind, title and metadata", "[table][columns]") {
    auto const json = morph::views::detail::deriveColumns<tcol::NoOverride, tcol::TcRow>();
    INFO(json);
    glz::generic columns;
    REQUIRE_FALSE(glz::read_json(columns, json));
    REQUIRE(columns.is_array());
    auto const& list = columns.get_array();
    REQUIRE(list.size() == 8);  // `secret` is x-hidden

    auto const column = [&](std::string const& field) -> glz::generic const& {
        auto const found =
            std::ranges::find_if(list, [&](auto const& entry) { return entry["field"].get_string() == field; });
        INFO("column " << field);
        REQUIRE(found != list.end());
        return *found;
    };
    CHECK(column("id")["kind"].get_string() == "integer");
    CHECK(column("analysis")["kind"].get_string() == "text");
    CHECK(column("price")["kind"].get_string() == "decimal");
    CHECK(column("mass")["kind"].get_string() == "quantity");
    CHECK(column("ph")["kind"].get_string() == "number");
    CHECK(column("approved")["kind"].get_string() == "bool");
    CHECK(column("measuredOn")["kind"].get_string() == "dateTime");
    CHECK(column("status")["kind"].get_string() == "text");

    CHECK(column("measuredOn")["title"].get_string() == "Measured on");
    CHECK(column("measuredOn")["label"].get_string() == "measuredOn");
    auto const& values = column("status")["enum"].get_array();
    REQUIRE(values.size() == 2);
    CHECK(values[0].get_string() == "open");
    CHECK(values[1].get_string() == "closed");

    // The kinds read back as morph::table::ColumnKind.
    morph::table::ColumnKind kind{};
    REQUIRE_FALSE(glz::read_json(kind, R"("dateTime")"));
    CHECK(kind == morph::table::ColumnKind::DateTime);
}

TEST_CASE("table: integer columns with an epoch widget are dates", "[table][columns]") {
    auto const json = morph::views::detail::deriveColumns<tcol::HideNothing, tcol::EpochRow>();
    INFO(json);
    glz::generic columns;
    REQUIRE_FALSE(glz::read_json(columns, json));
    auto const& list = columns.get_array();
    REQUIRE(list.size() == 3);
    CHECK(list[0]["kind"].get_string() == "date");
    CHECK(list[1]["kind"].get_string() == "dateTime");
    CHECK(list[2]["kind"].get_string() == "text");
}

TEST_CASE("table: an override list may still name a hidden member", "[table][columns]") {
    auto const json = morph::views::detail::deriveColumns<tcol::NameSecret, tcol::TcRow>();
    glz::generic columns;
    REQUIRE_FALSE(glz::read_json(columns, json));
    REQUIRE(columns.get_array().size() == 1);
    CHECK(columns.get_array()[0]["kind"].get_string() == "integer");
}

// Mutation: resolveSchemaRef not looking inside `anyOf`, which is how glaze
// writes a std::optional member: every optional column is text, and an
// optional enum loses its values.
TEST_CASE("table: optional members keep their column kind", "[table][columns]") {
    auto const json = morph::views::detail::deriveColumns<tcol::HideNothing, tcol::OptionalRow>();
    INFO(json);
    glz::generic columns;
    REQUIRE_FALSE(glz::read_json(columns, json));
    auto const& list = columns.get_array();
    REQUIRE(list.size() == 7);
    auto const column = [&](std::string const& field) -> glz::generic const& {
        auto const found =
            std::ranges::find_if(list, [&](auto const& entry) { return entry["field"].get_string() == field; });
        INFO("column " << field);
        REQUIRE(found != list.end());
        return *found;
    };
    CHECK(column("count")["kind"].get_string() == "integer");
    CHECK(column("ph")["kind"].get_string() == "number");
    CHECK(column("price")["kind"].get_string() == "decimal");
    CHECK(column("status")["kind"].get_string() == "text");
    CHECK(column("status")["enum"].get_array().size() == 2);
    CHECK(column("at")["kind"].get_string() == "dateTime");
    CHECK(column("mass")["kind"].get_string() == "quantity");
    CHECK(column("mass").contains("ExtUnits"));
}

// Mutation: the copy list in deriveColumns without "x-column-group".
TEST_CASE("table: derived columns carry x-column-group", "[table][columns][groups]") {
    glz::generic_u64 schema{};
    REQUIRE_FALSE(glz::read_json(schema, morph::forms::schemaJson<tabletest::GroupRow>()));
    CHECK(schema["properties"]["companyB"]["x-column-group"].get_string() == "company");
    CHECK_FALSE(schema["properties"]["name"].contains("x-column-group"));
    auto const json = morph::views::detail::deriveColumns<tcol::HideNothing, tabletest::GroupRow>();
    INFO(json);
    glz::generic columns;
    REQUIRE_FALSE(glz::read_json(columns, json));
    auto const& list = columns.get_array();
    REQUIRE(list.size() == 4);
    CHECK(list[1]["field"].get_string() == "companyB");
    CHECK(list[1]["x-column-group"].get_string() == "company");
    CHECK_FALSE(list[3].contains("x-column-group"));
}

// Mutation: RowsSource<Row>::groups() returning nothing: the typed apply is UnknownColumn.
TEST_CASE("typed rows declare their groups to the source", "[table][columns][groups]") {
    using morph::table::ColumnGroupInfo;
    using morph::table::FilterEntry;
    using morph::table::GroupMode;
    using morph::table::RowId;
    auto const rows = std::make_shared<std::vector<tabletest::GroupRow> const>(
        std::vector<tabletest::GroupRow>{{.companyA = "F", .companyB = "S", .companyC = "X", .name = "a"},
                                         {.companyA = "F", .companyB = "F", .companyC = "X", .name = "b"},
                                         {.companyA = "S", .companyB = "F", .companyC = "F", .name = "c"}});
    morph::table::RowKey<tabletest::GroupRow> const key = [](tabletest::GroupRow const& row) {
        return RowId{row.name};
    };
    morph::table::RowsSource<tabletest::GroupRow> const source{rows, key};
    REQUIRE(source.groups().size() == 1);
    CHECK(source.groups()[0] ==
          ColumnGroupInfo{.id = "company", .label = "company", .columns = {"companyA", "companyB", "companyC"}});
    morph::table::TableQuery q;
    q.filters.groups.push_back({.group = "company",
                                .mode = GroupMode::Together,
                                .include = {FilterEntry{.eq = "F"}, FilterEntry{.eq = "S"}}});
    auto const page = morph::table::apply(*rows, key, q);
    REQUIRE(page);
    REQUIRE(page->rows.size() == 2);
    CHECK(page->rows[0].name == "a");
    CHECK(page->rows[1].name == "c");
}

// Mutation: annotateColumnGroups writing `x-group`, the layout's key: the layout pass overwrites the column group's
// id, and no column carries x-column-group.
TEST_CASE("table: a form layout's group and a column group keep their own keys", "[table][columns][groups]") {
    glz::generic_u64 schema{};
    REQUIRE_FALSE(glz::read_json(schema, morph::forms::schemaJson<tcol::LayoutGroupRow>()));
    CHECK(schema["properties"]["companyA"]["x-group"].get_string() == "Parties");
    CHECK(schema["properties"]["companyA"]["x-column-group"].get_string() == "company");
    CHECK(schema["properties"]["name"]["x-group"].get_string() == "Parties");
    CHECK_FALSE(schema["properties"]["name"].contains("x-column-group"));
    auto const json = morph::views::detail::deriveColumns<tcol::HideNothing, tcol::LayoutGroupRow>();
    INFO(json);
    glz::generic columns;
    REQUIRE_FALSE(glz::read_json(columns, json));
    auto const& list = columns.get_array();
    REQUIRE(list.size() == 3);
    CHECK(list[0]["x-column-group"].get_string() == "company");
    CHECK_FALSE(list[0].contains("x-group"));  // a layout title is not a column group
    CHECK_FALSE(list[2].contains("x-column-group"));
}

using morph::table::ColumnLayout;
using morph::table::ColumnLayoutValue;

namespace {

std::vector<std::string> orderOf(ColumnLayout const& layout) { return {layout.order().begin(), layout.order().end()}; }

// A layout that has been fitted: what the round-trip case stores.
ColumnLayout fittedLayout() {
    ColumnLayout layout{{"id", "name", "a", "b"}};
    layout.apply(ColumnLayoutValue{.order = {"b", "name", "id", "a"},
                                   .hidden = {"a"},
                                   .pinned = 2,
                                   .widths = {{"name", 24}},
                                   .groups = {{.id = "g1", .label = "Company", .columns = {"a", "b"}}},
                                   .units = {{"a", {{"mass", "g"}}}}});
    return layout;
}

}  // namespace

TEST_CASE("column layout: a stored layout is fitted to the table", "[table][layout]") {
    ColumnLayout layout{{"id", "name", "a", "new", "b"}};
    layout.apply(ColumnLayoutValue{.order = {"b", "ghost", "name", "id", "a"},
                                   .hidden = {"ghost", "a"},
                                   .pinned = 9,
                                   .widths = {{"name", 24}, {"ghost", 10}},
                                   .groups = {{.id = "g1", .label = "Company", .columns = {"a", "ghost", "b"}},
                                              {.id = "g2", .label = "X", .columns = {"ghost", "name"}}},
                                   .units = {{"a", {{"mass", "g"}}}, {"ghost", {{"mass", "kg"}}}}});
    auto const v = layout.value();
    CHECK(v.order == std::vector<std::string>{"b", "name", "id", "a", "new"});  // "new" after its predecessor "a"
    CHECK(v.hidden == std::vector<std::string>{"a"});
    CHECK(layout.visible() == std::vector<std::string>{"b", "name", "id", "new"});
    CHECK(v.pinned == 4);
    CHECK(v.widths == std::map<std::string, int>{{"name", 24}});
    REQUIRE(v.groups.size() == 1);
    CHECK(v.groups[0].columns == std::vector<std::string>{"a", "b"});
    CHECK(v.units == std::map<std::string, std::map<std::string, std::string>>{{"a", {{"mass", "g"}}}});
}

// Mutation: the fitting rule not showing a column when every one is hidden.
TEST_CASE("column layout: every column hidden shows the first declared column", "[table][layout]") {
    ColumnLayout layout{{"id", "name"}};
    layout.apply(ColumnLayoutValue{.order = {"name", "id"}, .hidden = {"name", "id"}});
    CHECK(layout.visible() == std::vector<std::string>{"id"});
}

TEST_CASE("column layout: a new column takes its declared place", "[table][layout]") {
    ColumnLayout layout{{"new0", "new1", "id", "name", "new2"}};
    layout.apply(ColumnLayoutValue{.order = {"name", "id"}});
    // new0 first (no placed predecessor), new1 after new0, new2 after name
    CHECK(orderOf(layout) == std::vector<std::string>{"new0", "new1", "name", "new2", "id"});
    ColumnLayout grown{{"a", "b", "c"}};
    grown.move("c", 0);
    grown.rebuild({"a", "b", "x", "c"});
    CHECK(orderOf(grown) == std::vector<std::string>{"c", "a", "b", "x"});
}

// Mutation: the pinned count not clamped (setPinned keeping 5).
TEST_CASE("column layout: widths and pinning", "[table][layout]") {
    ColumnLayout layout{{"a", "b", "c"}, {"c"}};
    CHECK(layout.visible() == std::vector<std::string>{"a", "b"});  // declared hidden
    CHECK(layout.setWidth("a", 30));
    CHECK_FALSE(layout.setWidth("zzz", 30));
    CHECK_FALSE(layout.setWidth("b", 0));
    CHECK(layout.widths() == std::map<std::string, int>{{"a", 30}});
    CHECK(layout.setWidth("a", std::nullopt));
    CHECK(layout.widths().empty());
    layout.setPinned(5);
    CHECK(layout.pinned() == 2);
    CHECK(layout.setVisible("b", false));
    CHECK(layout.pinned() == 1);  // clamped as visibility changes
    CHECK(layout.setWidth("a", 12));
    layout.reset();
    CHECK(layout.visible() == std::vector<std::string>{"a", "b"});
    CHECK(layout.widths().empty());
    CHECK(layout.pinned() == 0);
}

// Mutation: value() leaving the widths out: the round trip loses them.
TEST_CASE("column layout: the stored shape round-trips as JSON", "[table][layout]") {
    auto const fitted = fittedLayout();
    REQUIRE_FALSE(fitted.value().widths.empty());
    std::string json;
    REQUIRE_FALSE(glz::write_json(fitted.value(), json));
    ColumnLayoutValue back;
    REQUIRE_FALSE(glz::read_json(back, json));
    CHECK(back == fitted.value());
    ColumnLayoutValue partial;  // groups and units absent
    REQUIRE_FALSE(glz::read_json(partial, std::string{R"({"order":["a"],"hidden":[],"pinned":1,"widths":{"a":24}})"}));
    CHECK(partial.widths == std::map<std::string, int>{{"a", 24}});
    CHECK(partial.groups.empty());
}

// Mutation: the fitting rule keeping a repeated id: the order holds "b" twice.
TEST_CASE("column layout: a stored layout that repeats itself", "[table][layout]") {
    ColumnLayout layout{{"a", "b", "c"}};
    layout.apply(ColumnLayoutValue{.order = {"b", "a", "b", "c"},
                                   .pinned = 40,
                                   .widths = {{"a", 0}, {"b", -5}, {"c", 8}},
                                   .groups = {{.id = "g", .label = "G", .columns = {"a", "a", "c"}}}});
    CHECK(layout.value().order == std::vector<std::string>{"b", "a", "c"});
    CHECK(layout.value().widths == std::map<std::string, int>{{"c", 8}});
    CHECK(layout.pinned() == 3);
    REQUIRE(layout.value().groups.size() == 1);
    CHECK(layout.value().groups[0].columns == std::vector<std::string>{"a", "c"});
}

// Mutation: the fitting rule showing every column the stored layout does not mention.
TEST_CASE("column layout: a declared-hidden column a rebuild adds stays hidden without a reset", "[table][layout]") {
    ColumnLayout layout{{"a"}};
    layout.rebuild({"a", "x"}, {"x"});
    CHECK(layout.visible() == std::vector<std::string>{"a"});
    CHECK(layout.order().size() == 2);
    layout.apply(ColumnLayoutValue{.order = {"a"}});
    CHECK(layout.visible() == std::vector<std::string>{"a"});
    layout.apply(ColumnLayoutValue{.order = {"x", "a"}});
    CHECK(layout.visible() == std::vector<std::string>{"x", "a"});  // a layout that names it decides
}

// Mutation: reset clearing the groups the user formed.
TEST_CASE("column layout: reset keeps groups and units", "[table][layout]") {
    ColumnLayout layout{{"a", "b"}};
    layout.apply(ColumnLayoutValue{.order = {"b", "a"},
                                   .groups = {{.id = "g1", .label = "G", .columns = {"a", "b"}}},
                                   .units = {{"a", {{"mass", "g"}}}}});
    layout.reset();
    CHECK(layout.value().order == std::vector<std::string>{"a", "b"});
    CHECK(layout.value().groups.size() == 1);
    CHECK(layout.value().units.size() == 1);
}

// Mutation: the fitting rule keeping a group of 17 columns.
TEST_CASE("column layout: user-formed groups are fitted to the group rules", "[table][layout]") {
    std::vector<std::string> ids;
    for (int at = 0; at < 18; ++at) {
        // NOLINTNEXTLINE(performance-inefficient-vector-operation): `ids` is a fixture list built once; sizing it first would not change what the case checks
        ids.push_back("c" + std::to_string(at));
    }
    ColumnLayout layout{ids};
    std::vector<std::string> const wide(ids.begin(), ids.begin() + 17);
    layout.apply(ColumnLayoutValue{.groups = {{.id = "g1", .label = "Wide", .columns = wide},
                                              {.id = "g2", .label = "A", .columns = {"c0", "c1", "c2"}},
                                              {.id = "g2", .label = "Again", .columns = {"c5", "c6"}},
                                              {.id = "g3", .label = "B", .columns = {"c1", "c3", "c4"}},
                                              {.id = "g4", .label = "C", .columns = {"c2", "c17"}}}});
    auto const groups = layout.value().groups;
    REQUIRE(groups.size() == 2);
    CHECK(groups[0] == morph::table::ColumnGroupInfo{.id = "g2", .label = "A", .columns = {"c0", "c1", "c2"}});
    CHECK(groups[1].columns == std::vector<std::string>{"c3", "c4"});  // c1 is g2's
}

// Mutation: formGroup accepting a column already in a user-formed group.
TEST_CASE("column layout: the user forms and removes groups and sets display units", "[table][columns]") {
    ColumnLayout layout{{"companyA", "companyB", "companyC", "weight"}};
    auto const id = layout.formGroup("Company", {"companyA", "companyB"});
    REQUIRE(id == "g1");
    CHECK_FALSE(layout.formGroup("Again", {"companyB", "companyC"}));
    CHECK_FALSE(layout.formGroup("One", {"companyC"}));
    CHECK_FALSE(layout.formGroup("Ghost", {"companyC", "ghost"}));
    CHECK(layout.formGroup("Pair", {"companyC", "weight"}) == "g2");
    CHECK(layout.removeGroup("g2"));
    CHECK_FALSE(layout.removeGroup("g2"));
    CHECK(layout.groups().size() == 1);
    CHECK(layout.setUnit("weight", "mass", "g"));
    CHECK_FALSE(layout.setUnit("ghost", "mass", "g"));
    ColumnLayoutValue const value = layout.value();
    REQUIRE(value.groups.size() == 1);
    CHECK(value.groups.at(0).columns == std::vector<std::string>{"companyA", "companyB"});
    CHECK(value.units == std::map<std::string, std::map<std::string, std::string>>{{"weight", {{"mass", "g"}}}});
    CHECK(layout.setUnit("weight", "mass", std::nullopt));
    CHECK(layout.units().empty());
    // Mutation: the first unused id skipped for the next after the highest.
    CHECK(layout.formGroup("Back", {"companyC", "weight"}) == "g2");
    CHECK(layout.removeGroup("g1"));
    CHECK(layout.formGroup("Front", {"companyA", "companyB"}) == "g1");
}
