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
//  The x-group and typed-row group cases name their own mutations above them.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <glaze/glaze.hpp>
#include <morph/forms/forms.hpp>
#include <morph/forms/views.hpp>
#include <memory>
#include <morph/table/data_source.hpp>
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
    auto const rows = std::make_shared<std::vector<tabletest::GroupRow> const>(std::vector<tabletest::GroupRow>{
        {.companyA = "F", .companyB = "S", .companyC = "X", .name = "a"},
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
    q.filters.groups.push_back(
        {.group = "company", .mode = GroupMode::Together, .include = {FilterEntry{.eq = "F"}, FilterEntry{.eq = "S"}}});
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
