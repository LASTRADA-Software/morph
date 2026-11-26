// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <span>
#include <vector>

/// @file
/// @brief How a table's stretch columns share the width left to them.
///
/// Specified in `docs/spec/ui/backend_contract.md`, "Table cells". The solver is backend-independent: the caller gives
/// each column's natural width, least width and floor in its backend's measure, and the room left after the fixed and
/// content columns.

namespace morph::ui {

/// @brief One stretch column's widths and weight, for `stretchWidths`.
struct StretchColumn {
    /// @brief The width the column asks for: the larger of its header and its widest shown cell.
    int natural = 0;
    /// @brief The narrowest it is laid out without losing content; `natural` for a column that does not wrap.
    int least = 0;
    /// @brief The narrowest it goes before the table is short of room.
    int floor = 0;
    /// @brief The share of the spare width, from 1.
    int weight = 1;
    /// @brief Whether it gives up width before the other stretch columns.
    bool shrink = false;
};

/// @brief The widths of a table's stretch columns in @p room.
///
/// Each column is clamped so that `floor <= least <= natural`. With `sum` over the columns:
///  1. `room >= sum(natural)`: each is its natural width plus its weighted share of the spare.
///  2. `sum(least) <= room < sum(natural)`: each is its least width plus the share
///     `(natural - least) * (room - sum(least)) / (sum(natural) - sum(least))` of the room between its widths.
///  3. `sum(floor) <= room < sum(least)`: each starts at its least width and the deficit is taken from the columns
///  that
///     shrink, last first, each down to its floor, then from the others in the same order.
///  4. `room < sum(floor)`: each is its floor.
///
/// In steps 1 to 3 the widths add up to @p room exactly: what a division leaves over goes one unit each to the first
/// columns that can take it.
/// @param columns The stretch columns, in layout order.
/// @param room    The width left after the columns that do not stretch; a negative room is 0.
/// @return One width per column.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): the steps share the columns' working vectors, and each reads against the sums the one before computed
[[nodiscard]] inline std::vector<int> stretchWidths(std::span<StretchColumn const> columns, int room) {
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): every index runs below `count`, the
    // size of `columns` and of each vector sized from it.
    using Wide = std::int64_t;
    std::size_t const count = columns.size();
    std::vector<int> natural(count);
    std::vector<int> least(count);
    std::vector<int> floor(count);
    std::vector<Wide> weight(count);
    for (std::size_t at = 0; at < count; ++at) {
        floor[at] = std::max(0, columns[at].floor);
        natural[at] = std::max(floor[at], columns[at].natural);
        least[at] = std::clamp(columns[at].least, floor[at], natural[at]);
        weight[at] = std::max(1, columns[at].weight);
    }
    Wide const sumNatural = std::accumulate(natural.begin(), natural.end(), Wide{0});
    Wide const sumLeast = std::accumulate(least.begin(), least.end(), Wide{0});
    Wide const sumFloor = std::accumulate(floor.begin(), floor.end(), Wide{0});
    Wide const weights = std::accumulate(weight.begin(), weight.end(), Wide{0});
    Wide const left = std::max(room, 0);
    std::vector<int> sizes(count, 0);
    if (count == 0) {
        return sizes;
    }
    if (left >= sumNatural) {
        Wide const spare = left - sumNatural;
        Wide given = 0;
        for (std::size_t at = 0; at < count; ++at) {
            Wide const share = spare * weight[at] / weights;
            sizes[at] = static_cast<int>(natural[at] + share);
            given += share;
        }
        for (std::size_t at = 0; at < count && given < spare; ++at, ++given) {
            ++sizes[at];
        }
    } else if (left >= sumLeast) {
        // Here `sumNatural > left >= sumLeast`, so the divisor is positive.
        Wide given = 0;
        for (std::size_t at = 0; at < count; ++at) {
            Wide const share = Wide{natural[at] - least[at]} * (left - sumLeast) / (sumNatural - sumLeast);
            sizes[at] = static_cast<int>(least[at] + share);
            given += share;
        }
        Wide remainder = left - sumLeast - given;
        for (std::size_t at = 0; at < count && remainder > 0; ++at) {
            if (natural[at] > least[at]) {
                ++sizes[at];
                --remainder;
            }
        }
    } else if (left >= sumFloor) {
        sizes = least;
        Wide deficit = sumLeast - left;
        for (bool const shrinking : {true, false}) {
            for (std::size_t at = count; at-- > 0 && deficit > 0;) {
                if (columns[at].shrink == shrinking) {
                    Wide const take = std::min<Wide>(sizes[at] - floor[at], deficit);
                    sizes[at] -= static_cast<int>(take);
                    deficit -= take;
                }
            }
        }
    } else {
        sizes = std::move(floor);
    }
    // NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    return sizes;
}

}  // namespace morph::ui
