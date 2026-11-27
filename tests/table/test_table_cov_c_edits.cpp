// SPDX-License-Identifier: Apache-2.0
//
// Which form field kinds may edit which column kinds, and the names those field kinds print.

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <morph/forms/forms.hpp>
#include <morph/table/edits.hpp>
#include <string_view>
#include <utility>
#include <vector>

using morph::forms::FieldKind;
using morph::table::ColumnKind;

namespace {

constexpr std::array kFieldKinds{
    FieldKind::Text,     FieldKind::Multiline,     FieldKind::Integer,   FieldKind::Number, FieldKind::Decimal,
    FieldKind::Quantity, FieldKind::Boolean,       FieldKind::Enum,      FieldKind::Choice, FieldKind::DateTime,
    FieldKind::Date,     FieldKind::EpochDateTime, FieldKind::EpochDate, FieldKind::Slider, FieldKind::Array,
    FieldKind::Object,   FieldKind::ObjectArray};

}  // namespace

TEST_CASE("a column kind is edited by exactly the field kinds that hold its values", "[covc][table][edits]") {
    using Fits = std::vector<FieldKind>;
    std::vector<std::pair<ColumnKind, Fits>> const table{
        {ColumnKind::Integer, {FieldKind::Integer, FieldKind::Slider, FieldKind::Enum, FieldKind::Choice}},
        {ColumnKind::Key, {FieldKind::Integer, FieldKind::Choice}},
        {ColumnKind::Decimal, {FieldKind::Decimal}},
        {ColumnKind::Quantity, {FieldKind::Quantity}},
        {ColumnKind::Measure, {FieldKind::Quantity}},
        {ColumnKind::Number, {FieldKind::Number, FieldKind::Slider}},
        {ColumnKind::Text, {FieldKind::Text, FieldKind::Multiline, FieldKind::Enum, FieldKind::Choice}},
        {ColumnKind::Date, {FieldKind::Date, FieldKind::EpochDate}},
        {ColumnKind::DateTime, {FieldKind::DateTime, FieldKind::EpochDateTime}},
        {ColumnKind::Bool, {FieldKind::Boolean}},
        {ColumnKind::Custom, {FieldKind::Text, FieldKind::Enum, FieldKind::Choice}},
    };
    for (auto const& [column, fits] : table) {
        for (FieldKind const field : kFieldKinds) {
            CAPTURE(static_cast<int>(column), morph::forms::fieldKindName(field));
            CHECK(morph::table::detail::editFits(column, field) == std::ranges::contains(fits, field));
        }
    }
    CHECK_FALSE(morph::table::detail::editFits(static_cast<ColumnKind>(200), FieldKind::Text));
}

TEST_CASE("every field kind prints its own name, and an unknown one prints as text", "[covc][table][edits]") {
    using Names = std::vector<std::string_view>;
    Names names;
    for (FieldKind const field : kFieldKinds) {
        names.push_back(morph::forms::fieldKindName(field));
    }
    CHECK(names == Names{"text", "multiline", "integer", "number", "decimal", "quantity", "boolean", "enum", "choice",
                         "datetime", "date", "epochDateTime", "epochDate", "slider", "array", "object",
                         "objectArray"});
    CHECK(morph::forms::fieldKindName(static_cast<FieldKind>(250)) == "text");
}
