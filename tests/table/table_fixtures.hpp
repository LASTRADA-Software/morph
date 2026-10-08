// SPDX-License-Identifier: Apache-2.0
//
// Shared helpers for the morph::table tests: building sources tersely, and a
// collator that folds accents so accent-insensitive rules can be pinned
// without a toolkit.

#pragma once

#include <cstdint>
#include <memory>
#include <morph/table/data_source.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tabletest {

using morph::table::Cell;
using morph::table::ColumnInfo;
using morph::table::ColumnKind;
using morph::table::RowId;

// A Rational from decimal text; zero on bad input, which the tests never pass.
inline morph::math::Rational dec(std::string_view text) {
    return morph::table::parseDecimal(text).value_or(morph::math::Rational{});
}

inline ColumnInfo col(std::string id, ColumnKind kind, std::string comparator = {}) {
    return ColumnInfo{.id = std::move(id), .kind = kind, .comparator = std::move(comparator), .format = {}};
}

// A source whose row keys are 0..n-1 (as int64) and whose cells are given row by row.
inline std::shared_ptr<morph::table::VectorSource> makeSource(std::vector<ColumnInfo> columns,
                                                              std::vector<std::vector<Cell>> rows) {
    auto source = std::make_shared<morph::table::VectorSource>(std::move(columns));
    std::vector<RowId> ids;
    ids.reserve(rows.size());
    for (std::size_t i = 0; i < rows.size(); ++i) {
        ids.emplace_back(static_cast<std::int64_t>(i));
    }
    source->setRows(std::move(ids), std::move(rows));
    return source;
}

// A one-column source.
inline std::shared_ptr<morph::table::VectorSource> column(ColumnKind kind, std::vector<Cell> cells,
                                                          std::string id = "c") {
    std::vector<std::vector<Cell>> rows;
    rows.reserve(cells.size());
    for (auto& cell : cells) {
        rows.push_back({std::move(cell)});
    }
    return makeSource({col(std::move(id), kind)}, std::move(rows));
}

// Folds the accented Latin-1 vowels the fixtures use to their base letter,
// then lowers ASCII: a stand-in for a locale collator at primary strength.
class AccentFoldingCollator final : public morph::table::TextCollator {
public:
    [[nodiscard]] std::string sortKey(std::string_view text) const override { return fold(text); }
    [[nodiscard]] std::string fold(std::string_view text) const override {
        static constexpr std::pair<std::string_view, char> kMap[] = {
            {"\xC3\xA9", 'e'}, {"\xC3\xA8", 'e'}, {"\xC3\x89", 'e'}, {"\xC3\xA1", 'a'}, {"\xC3\xA0", 'a'},
            {"\xC3\x81", 'a'}, {"\xC3\xB6", 'o'}, {"\xC3\x96", 'o'}, {"\xC3\xBC", 'u'}, {"\xC3\x9C", 'u'},
        };
        std::string out;
        for (std::size_t i = 0; i < text.size();) {
            bool mapped = false;
            for (auto const& [from, to] : kMap) {
                if (text.substr(i, from.size()) == from) {
                    out.push_back(to);
                    i += from.size();
                    mapped = true;
                    break;
                }
            }
            if (mapped) {
                continue;
            }
            char ch = text[i++];
            if (ch >= 'A' && ch <= 'Z') {
                ch = static_cast<char>(ch - 'A' + 'a');
            }
            out.push_back(ch);
        }
        return out;
    }
};

}  // namespace tabletest
