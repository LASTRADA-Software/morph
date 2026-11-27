// SPDX-License-Identifier: Apache-2.0
//
// Reading a generic page back: numbers that cross the wire as integers or words, and measures that need the
// description's units.

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>
#include <morph/table/description.hpp>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace morph::table;
using morph::math::DecimalPlaces;
using morph::math::Rational;

namespace {

TableColumn columnOfKind(std::string const& id, ColumnKind kind) {
    return TableColumn{
        .info = {.id = id, .kind = kind, .comparator = {}, .format = {}, .level = {}}, .label = id, .group = {}};
}

UnitInfo kilogram() {
    return UnitInfo{
        .id = "kg", .symbol = "kg", .dimension = "mass", .toBase = Rational{1, DecimalPlaces{0}}, .decimals = 3};
}

TableDescription described(std::vector<TableColumn> columns, std::vector<UnitInfo> units = {}) {
    TableDescription description{.id = "t", .label = "T", .columns = std::move(columns), .key = "id"};
    description.units = std::move(units);
    return describe(std::move(description));
}

}  // namespace

TEST_CASE("a number column reads an integer token and the non-finite words as doubles", "[covc][table][pages]") {
    auto const description = described({columnOfKind("n", ColumnKind::Number)});
    auto const read = readGenericPage(
        R"({"columns":["n"],"keys":[1,2,3,4,5,6],"rows":[[7],["nan"],["inf"],["-inf"],["word"],[1.5]]})", description);
    REQUIRE(read);
    auto const& rows = read->rows;
    CHECK(std::get<double>(rows[0][0]) == 7.0);
    CHECK(std::isnan(std::get<double>(rows[1][0])));
    CHECK(std::get<double>(rows[2][0]) == std::numeric_limits<double>::infinity());
    CHECK(std::get<double>(rows[3][0]) == -std::numeric_limits<double>::infinity());
    CHECK(std::get<std::string>(rows[4][0]) == "word");
    CHECK(std::get<double>(rows[5][0]) == 1.5);
}

TEST_CASE("a measure column reads its unit id against the description's units", "[covc][table][pages]") {
    auto const description = described({columnOfKind("w", ColumnKind::Measure)}, {kilogram()});
    auto const read = readGenericPage(
        R"({"columns":["w"],"keys":[1,2,3,4,5],"rows":[[[15,10,"kg"]],[[1,10,null]],[[1,10,"zz"]],["word"],[7]]})",
        description);
    REQUIRE(read);
    auto const& rows = read->rows;
    auto const* const known = std::get_if<Measure>(&rows[0][0]);
    REQUIRE(known != nullptr);
    CHECK(known->amount.numerator == 3);
    CHECK(known->amount.denominator == 2);
    CHECK(read->units.at(known->unit) == "kg");
    auto const* const unitless = std::get_if<Measure>(&rows[1][0]);
    REQUIRE(unitless != nullptr);
    CHECK(unitless->unit == kUnknownUnit);
    auto const* const unknown = std::get_if<Measure>(&rows[2][0]);
    REQUIRE(unknown != nullptr);
    CHECK(unknown->unit == kUnknownUnit);
    CHECK(std::holds_alternative<std::string>(rows[3][0]));
    CHECK(std::holds_alternative<std::int64_t>(rows[4][0]));
}

TEST_CASE("a description whose units cannot be registered refuses a measure page", "[covc][table][pages]") {
    UnitInfo bad = kilogram();
    bad.id = "1kg";
    auto const description = described({columnOfKind("w", ColumnKind::Measure)}, {bad});
    auto const read = readGenericPage(R"({"columns":["w"],"keys":[1],"rows":[[[1,1,"kg"]]]})", description);
    REQUIRE_FALSE(read);
    CHECK(read.error().code == TableErrorCode::InvalidSpec);
    auto const registry = registryOf(description);
    REQUIRE_FALSE(registry);
    CHECK(registry.error().message.find("begins like a number") != std::string::npos);
}

TEST_CASE("a registry skips a unit with no id and a page of a text column needs none", "[covc][table][pages]") {
    UnitInfo anonymous = kilogram();
    anonymous.id.clear();
    auto const description = described({columnOfKind("t", ColumnKind::Text)}, {anonymous, kilogram()});
    auto const registry = registryOf(description);
    REQUIRE(registry);
    CHECK((*registry)->find("kg"));
    auto const read = readGenericPage(R"({"columns":["t"],"keys":[1],"rows":[["x"]]})", description);
    REQUIRE(read);
}

TEST_CASE("the columns a query reads include their companions, once, unless the query overrides them",
          "[covc][table][pages]") {
    auto plain = columnOfKind("a", ColumnKind::Integer);
    plain.tone = "aTone";
    plain.tooltip = "aTip";
    auto other = columnOfKind("b", ColumnKind::Integer);
    other.tone = "aTone";
    auto description =
        described({plain, other, columnOfKind("aTone", ColumnKind::Text), columnOfKind("aTip", ColumnKind::Text),
                   columnOfKind("rowTone", ColumnKind::Text), columnOfKind("lock", ColumnKind::Text)});
    description.rowTone = "rowTone";
    description.rowLock = "lock";
    auto const asked = std::vector<std::string>{"a", "b", "a", "zz", ""};
    CHECK(columnsWithCompanions(description, asked, {}) ==
          std::vector<std::string>{"a", "aTone", "aTip", "b", "zz", "rowTone", "lock"});

    CompanionOverrides overrides;
    overrides.tone.insert("a");
    overrides.tooltip.insert("a");
    overrides.rowTone = true;
    CHECK(columnsWithCompanions(description, asked, overrides) ==
          std::vector<std::string>{"a", "b", "aTone", "zz", "lock"});
    CHECK(companionColumns(description) == std::vector<std::string>{"aTip", "aTone", "lock", "rowTone"});
    CHECK(companionColumns(described({columnOfKind("a", ColumnKind::Integer)})).empty());
}

TEST_CASE("a band names the columns it spans, or the description is refused", "[covc][table][pages]") {
    std::vector<TableColumn> const columns{columnOfKind("a", ColumnKind::Integer),
                                           columnOfKind("b", ColumnKind::Integer)};
    auto const band = [](std::string label, std::vector<std::string> ids, std::vector<BandInfo> inner = {}) {
        return BandInfo{.label = std::move(label), .unit = {}, .columns = std::move(ids), .bands = std::move(inner)};
    };
    auto const refusal = [&](std::vector<BandInfo> const& bands) {
        auto const checked = checkBands(bands, columns);
        REQUIRE_FALSE(checked);
        CHECK(checked.error().code == TableErrorCode::SourceContract);
        return checked.error().message;
    };
    CHECK(checkBands({}, columns));
    CHECK(checkBands(std::vector<BandInfo>{band("All", {"a", "b"}, {band("One", {"a"})})}, columns));
    CHECK(refusal({band("Empty", {})}).find("names no column") != std::string::npos);
    CHECK(refusal({band("Ghost", {"a", "nope"})}).find("which is not a column of the table") != std::string::npos);
    CHECK(refusal({band("Twice", {"a"}), band("Again", {"a"})}).find("another band of its level covers") !=
          std::string::npos);
    CHECK(refusal({band("Outer", {"a"}, {band("Inner", {"b"})})}).find("which its parent band does not") !=
          std::string::npos);
    auto const deep = band("L1", {"a"}, {band("L2", {"a"}, {band("L3", {"a"}, {band("L4", {"a"})})})});
    CHECK(refusal({deep}).find("nests past 3 levels") != std::string::npos);
    CHECK(bandRefusal(std::vector<BandInfo>{band("Free", {"anything"})}, {}) == std::nullopt);
}
