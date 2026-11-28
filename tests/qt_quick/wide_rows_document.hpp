// SPDX-License-Identifier: Apache-2.0
//
// The `ui/1` document of one `table` over a wide-table fixture's cells, as literal rows: what the native frontend
// baseline (F2 to F5) and the browser one (B5) both mount, so the two measure the same table. Its row writer is
// also what the interpreter's memory baseline (U4) mounts.

#pragma once

#include <cstddef>
#include <format>
#include <morph/table/data_source.hpp>
#include <string>
#include <type_traits>
#include <variant>

#include "table/wide_table_model.hpp"

namespace bench {

// Appends @p text as a JSON string.
inline void appendJsonText(std::string& out, std::string const& text) {
    out += '"';
    for (char const character : text) {
        if (character == '"' || character == '\\') {
            out += '\\';
        }
        out += character;
    }
    out += '"';
}

// Appends a cell as a document value: an exact number as {"num", "den", "dp"} (a document refuses a number with a
// fraction), a quantity with its unit, a text as a string, an empty cell as null.
inline void appendDocumentValue(std::string& out, morph::table::Cell const& cell, morph::table::ColumnKind kind) {
    auto const exact = [&out](morph::math::Rational const& value, char const* unit) {
        out += std::format(R"({{"num":{},"den":{},"dp":{})", value.numerator, value.denominator,
                           value.decimalPlaces.value);
        if (unit != nullptr) {
            out += std::format(R"(,"unit":"{}")", unit);
        }
        out += '}';
    };
    std::visit(
        [&]<typename T>(T const& value) {
            if constexpr (std::is_same_v<T, std::monostate>) {
                static_cast<void>(value);
                out += "null";
            } else if constexpr (std::is_same_v<T, std::string>) {
                appendJsonText(out, value);
            } else if constexpr (std::is_same_v<T, morph::math::Rational>) {
                exact(value, kind == morph::table::ColumnKind::Quantity ? "g" : nullptr);
            } else if constexpr (std::is_same_v<T, morph::table::QuantityCell>) {
                exact(value.amount, "kg");
            } else if constexpr (std::is_same_v<T, morph::table::Measure>) {
                exact(value.amount, nullptr);
            } else {
                static_cast<void>(value);
                out += morph::table::displayText(cell);
            }
        },
        cell);
}

// Every row of @p source as an object keyed by column id, comma-separated: the inside of a JSON list.
[[nodiscard]] inline std::string wideRowsJson(WideTableSource const& source) {
    auto const snapshot = source.snapshot();
    auto const columns = source.columns();
    std::string out;
    for (std::size_t row = 0; row < snapshot->rowCount(); ++row) {
        out += row == 0 ? "{" : ",{";
        for (std::size_t column = 0; column < columns.size(); ++column) {
            if (column > 0) {
                out += ',';
            }
            appendJsonText(out, columns[column].id);
            out += ':';
            appendDocumentValue(out, snapshot->cell(row, column), columns[column].kind);
        }
        out += '}';
    }
    return out;
}

// The source's column ids as a table's `columns`, each labelled with its id and sortable: the inside of a JSON list.
[[nodiscard]] inline std::string wideColumnsJson(WideTableSource const& source) {
    std::string out;
    bool first = true;
    for (auto const& column : source.columns()) {
        out += first ? R"({"id":)" : R"(,{"id":)";
        first = false;
        appendJsonText(out, column.id);
        out += R"(,"label":)";
        appendJsonText(out, column.id);
        out += R"(,"sortable":true})";
    }
    return out;
}

// A document whose one view is a `table` with testId "wide" over every row of @p source as literal rows, as tall as
// the window: a table given no height takes all its rows' height, and never scrolls inside itself.
[[nodiscard]] inline std::string wideRowsDocument(WideTableSource const& source) {
    return R"({ "vocab": "ui/1", "id": "wideRows", "view": { "kind": "table", "testId": "wide",)"
           R"( "sizing": { "lit": { "height": { "stretch": 1 } } }, "rows": { "lit": [)" +
           wideRowsJson(source) + R"(] }, "columns": [)" + wideColumnsJson(source) + "] } }";
}

}  // namespace bench
