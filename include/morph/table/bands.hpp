// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file table/bands.hpp
/// @brief `BandInfo`: a header band over a table's columns, light enough for the view tree to hold.
///
/// See `docs/spec/table/editing.md`, "Header bands".

#include <algorithm>
#include <cstddef>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace morph::table {

/// @brief A heading over columns, drawn above their headers, with the bands one level down beneath it.
// NOLINTNEXTLINE(misc-no-recursion): a band holds its child bands.
struct BandInfo {
    /// @brief The band's text.
    std::string label;
    /// @brief The unit shown after the label, as a column's is ("Mass retained [g]"); empty for none. A column under
    ///        the band whose unit is the same does not repeat it.
    std::string unit;
    /// @brief The ids of the columns under the band.
    std::vector<std::string> columns;
    /// @brief The bands one level down, each over a subset of `columns`.
    std::vector<BandInfo> bands;

    /// @brief Member-wise equality.
    /// @param other The band to compare with.
    /// @return `true` when every member matches.
    [[nodiscard]] bool operator==(BandInfo const& other) const = default;
};

/// @brief How many levels of bands a table may nest: a band, its children and theirs.
inline constexpr std::size_t kMaxBandLevels = 3;

namespace detail {

/// @brief Why bands of one level cannot be drawn over the columns their parent covers, recursing into each band's own.
/// @param bands  The bands of one level.
/// @param known  Every column id of the table; null leaves the ids unchecked.
/// @param parent The columns the enclosing band covers; null at the top level.
/// @param level  The level of @p bands, 1 for the top.
/// @return The refusal, naming the band's label; nothing when the bands can be drawn.
// NOLINTNEXTLINE(misc-no-recursion): a band's children are bands one level down.
[[nodiscard]] inline std::optional<std::string> bandLevelRefusal(std::span<BandInfo const> bands,
                                                                 std::span<std::string const> const* known,
                                                                 std::vector<std::string> const* parent,
                                                                 std::size_t level) {
    std::vector<std::string> claimed;
    for (BandInfo const& band : bands) {
        std::string const named = "band `" + band.label + "` ";
        if (level > kMaxBandLevels) {
            return named + "nests past " + std::to_string(kMaxBandLevels) + " levels";
        }
        if (band.columns.empty()) {
            return named + "names no column";
        }
        for (std::string const& columnId : band.columns) {
            if (known != nullptr && !std::ranges::contains(*known, columnId)) {
                return std::format("{}names `{}`, which is not a column of the table", named, columnId);
            }
            if (parent != nullptr && !std::ranges::contains(*parent, columnId)) {
                return std::format("{}covers `{}`, which its parent band does not", named, columnId);
            }
            if (std::ranges::contains(claimed, columnId)) {
                return std::format("{}covers `{}`, which another band of its level covers", named, columnId);
            }
            claimed.push_back(columnId);
        }
        if (auto refused = bandLevelRefusal(band.bands, known, &band.columns, level + 1)) {
            return refused;
        }
    }
    return std::nullopt;
}

}  // namespace detail

/// @brief Why header bands cannot be drawn: a band nested past `kMaxBandLevels` levels, a band naming no column, a
///        band naming a column id the table does not have, a child band over a column its parent lacks, or a column in
///        two bands of one level.
/// @param bands The bands, outermost first.
/// @param known Every column id of the table; empty leaves the ids unchecked (a document's bands over a model's
///              table, whose columns come with its description).
/// @return The refusal, naming the band's label; nothing when the bands can be drawn.
[[nodiscard]] inline std::optional<std::string> bandRefusal(std::span<BandInfo const> bands,
                                                            std::span<std::string const> known) {
    return detail::bandLevelRefusal(bands, known.empty() ? nullptr : &known, nullptr, 1);
}

}  // namespace morph::table
