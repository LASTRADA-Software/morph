// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file table/view_change.hpp
/// @brief What changed between two view orders, as list-model operations.
///
/// A renderer's list model applies a `ViewChange` as keyed operations, so
/// selection, focus and scroll position survive a sort or a filter. The
/// change is computed from the old and new view orders and is minimal in
/// moves: rows on a longest increasing subsequence of the new order stay where
/// they are and only the rest move.
///
/// **Replay contract.** Apply the operations in order to the old list:
/// - `Removed{first, count}` erases `count` rows at `first`;
/// - `Moved{first, to}` erases the row at `first`, then inserts it at `to`
///   (an index into the list after the erase);
/// - `Inserted{first, count}` inserts the new view's rows `[first, first + count)`
///   at `first`;
/// - `Changed{first, count}` marks rows `[first, first + count)` of the new
///   view as having new content, and changes no order;
/// - `Reset` replaces the whole list with the new view.
///
/// All removals come first (back to front), then moves, then insertions
/// (front to back), then changes. The result is the new view.
///
/// See `docs/spec/table/engine.md`.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace morph::table {

/// @brief One list-model operation.
struct ViewOp {
    /// @brief The operation.
    enum class Kind : std::uint8_t {
        Removed,   ///< `count` rows at `first` were removed.
        Moved,     ///< The row at `first` moved to `to`.
        Inserted,  ///< `count` rows were inserted at `first`.
        Changed,   ///< `count` rows at `first` have new content.
        Reset,     ///< The whole view was replaced.
    };

    /// @brief The operation.
    Kind kind = Kind::Reset;
    /// @brief First row concerned.
    std::size_t first = 0;
    /// @brief Rows concerned (1 for `Moved`, 0 for `Reset`).
    std::size_t count = 0;
    /// @brief Destination of a `Moved` row, in the list after its removal.
    std::size_t to = 0;

    /// @brief Member-wise equality.
    /// @param other The operation to compare with.
    /// @return `true` when every member matches.
    [[nodiscard]] bool operator==(ViewOp const& other) const = default;
};

/// @brief The operations that turn one view order into another.
struct ViewChange {
    /// @brief The operations, in replay order.
    std::vector<ViewOp> ops;

    /// @brief Whether nothing changed.
    /// @return `true` when there are no operations.
    [[nodiscard]] bool empty() const noexcept { return ops.empty(); }

    /// @brief Whether the change is a whole-view reset.
    /// @return `true` when the only operation is `Reset`.
    [[nodiscard]] bool isReset() const noexcept { return ops.size() == 1 && ops.front().kind == ViewOp::Kind::Reset; }
};

namespace detail {

/// @brief A Fenwick tree of slot occupancy: how many occupied slots precede a slot.
class SlotCounter {
public:
    /// @brief An empty counter over @p size slots.
    /// @param size Slot count.
    explicit SlotCounter(std::size_t size) : _tree(size + 1, 0) {}

    /// @brief Changes a slot's occupancy.
    /// @param slot  The slot.
    /// @param delta +1 to occupy, -1 to free.
    void add(std::size_t slot, int delta) {
        for (auto at = slot + 1; at < _tree.size(); at += at & (~at + 1)) {
            _tree[at] += delta;
        }
    }
    /// @brief Occupied slots before @p slot.
    /// @param slot The slot.
    /// @return The count.
    [[nodiscard]] std::size_t before(std::size_t slot) const {
        int sum = 0;
        for (auto at = slot; at > 0; at -= at & (~at + 1)) {
            sum += _tree[at];
        }
        return static_cast<std::size_t>(sum);
    }

private:
    std::vector<int> _tree;
};

/// @brief Marks the elements of a longest strictly increasing subsequence.
/// @param values The sequence.
/// @return One flag per element; `true` on the subsequence.
[[nodiscard]] inline std::vector<bool> longestIncreasing(std::span<std::uint32_t const> values) {
    std::vector<std::size_t> tails;  // index into values of the smallest tail of each length
    std::vector<std::size_t> parent(values.size(), std::numeric_limits<std::size_t>::max());
    for (std::size_t i = 0; i < values.size(); ++i) {
        auto const at = std::ranges::lower_bound(tails, values[i], {}, [&](std::size_t j) { return values[j]; });
        auto const length = static_cast<std::size_t>(at - tails.begin());
        if (length > 0) {
            parent[i] = tails[length - 1];
        }
        if (at == tails.end()) {
            tails.push_back(i);
        } else {
            *at = i;
        }
    }
    std::vector<bool> marked(values.size(), false);
    if (!tails.empty()) {
        for (auto at = tails.back(); at != std::numeric_limits<std::size_t>::max(); at = parent[at]) {
            marked[at] = true;
        }
    }
    return marked;
}

}  // namespace detail

/// @brief Computes the operations that turn @p before into @p after.
///
/// Rows are identified by integers below @p identityLimit, the same in both
/// orders: a source row index when both views come from one snapshot, or a
/// mapping through `RowId` when they do not.
/// @param before        The old view order.
/// @param after         The new view order.
/// @param changed       Identities whose content changed; those in both views
///                      are reported as `Changed` in their new position.
/// @param identityLimit One more than the largest identity.
/// @param resetThreshold More operations than this give one `Reset`.
/// @return The change.
[[nodiscard]] inline ViewChange diffViews(std::span<std::uint32_t const> before, std::span<std::uint32_t const> after,
                                          std::span<std::uint32_t const> changed, std::size_t identityLimit,
                                          std::size_t resetThreshold) {
    constexpr auto kAbsent = std::numeric_limits<std::uint32_t>::max();
    ViewChange out;
    auto const reset = [&] {
        out.ops.assign(1, ViewOp{.kind = ViewOp::Kind::Reset, .first = 0, .count = 0, .to = 0});
        return out;
    };
    std::vector<std::uint32_t> posAfter(identityLimit, kAbsent);
    std::vector<std::uint32_t> posBefore(identityLimit, kAbsent);
    for (std::size_t i = 0; i < after.size(); ++i) {
        posAfter[after[i]] = static_cast<std::uint32_t>(i);
    }
    for (std::size_t i = 0; i < before.size(); ++i) {
        posBefore[before[i]] = static_cast<std::uint32_t>(i);
    }

    // Removals, back to front, in runs.
    for (std::size_t i = before.size(); i > 0;) {
        if (posAfter[before[i - 1]] != kAbsent) {
            --i;
            continue;
        }
        std::size_t start = i - 1;
        while (start > 0 && posAfter[before[start - 1]] == kAbsent) {
            --start;
        }
        out.ops.push_back(ViewOp{.kind = ViewOp::Kind::Removed, .first = start, .count = i - start, .to = 0});
        if (out.ops.size() > resetThreshold) {
            return reset();
        }
        i = start;
    }

    // The kept rows in old order, as ranks in the new order.
    std::vector<std::uint32_t> rankOf(identityLimit, kAbsent);
    std::uint32_t rank = 0;
    for (auto const id : after) {
        if (posBefore[id] != kAbsent) {
            rankOf[id] = rank++;
        }
    }
    std::vector<std::uint32_t> ranks;
    ranks.reserve(rank);
    for (auto const id : before) {
        if (rankOf[id] != kAbsent) {
            ranks.push_back(rankOf[id]);
        }
    }
    auto const stationary = detail::longestIncreasing(ranks);
    auto const stationaryCount = static_cast<std::size_t>(std::ranges::count(stationary, true));
    auto const moving = ranks.size() - stationaryCount;
    if (out.ops.size() + moving > resetThreshold) {
        return reset();
    }

    if (moving > 0) {
        // Gaps between stationary rows: a row's old gap is how many stationary
        // rows precede it in old order, its new gap how many precede it in new
        // order. Within a gap, rows already moved (by rank) sit before rows not
        // yet moved (by old position), and the stationary row closes the gap.
        std::vector<std::size_t> oldGap(ranks.size());
        std::vector<std::size_t> stationaryAtRank(rank, 0);
        std::size_t seen = 0;
        for (std::size_t i = 0; i < ranks.size(); ++i) {
            oldGap[i] = seen;
            if (stationary[i]) {
                stationaryAtRank[ranks[i]] = 1;
                ++seen;
            }
        }
        std::vector<std::size_t> stationaryBeforeRank(rank + 1, 0);
        for (std::size_t r = 0; r < rank; ++r) {
            stationaryBeforeRank[r + 1] = stationaryBeforeRank[r] + stationaryAtRank[r];
        }
        // Slot layout per gap: [moved slots][unmoved slots][stationary slot].
        std::vector<std::size_t> movedInGap(stationaryCount + 1, 0);
        std::vector<std::size_t> unmovedInGap(stationaryCount + 1, 0);
        for (std::size_t i = 0; i < ranks.size(); ++i) {
            if (!stationary[i]) {
                ++movedInGap[stationaryBeforeRank[ranks[i]]];
                ++unmovedInGap[oldGap[i]];
            }
        }
        std::vector<std::size_t> gapBase(stationaryCount + 2, 0);
        for (std::size_t g = 0; g <= stationaryCount; ++g) {
            gapBase[g + 1] = gapBase[g] + movedInGap[g] + unmovedInGap[g] + (g < stationaryCount ? 1 : 0);
        }
        detail::SlotCounter slots{gapBase[stationaryCount + 1]};
        std::vector<std::size_t> unmovedSlot(ranks.size(), 0);
        std::vector<std::size_t> nextUnmoved(stationaryCount + 1, 0);
        for (std::size_t i = 0; i < ranks.size(); ++i) {
            auto const g = oldGap[i];
            if (stationary[i]) {
                slots.add(gapBase[g] + movedInGap[g] + unmovedInGap[g], 1);
            } else {
                unmovedSlot[i] = gapBase[g] + movedInGap[g] + nextUnmoved[g]++;
                slots.add(unmovedSlot[i], 1);
            }
        }
        // Moving rows in new order.
        std::vector<std::size_t> indexOfRank(rank, 0);
        for (std::size_t i = 0; i < ranks.size(); ++i) {
            indexOfRank[ranks[i]] = i;
        }
        std::vector<std::size_t> nextMoved(stationaryCount + 1, 0);
        for (std::size_t r = 0; r < rank; ++r) {
            auto const i = indexOfRank[r];
            if (stationary[i]) {
                continue;
            }
            auto const from = slots.before(unmovedSlot[i]);
            slots.add(unmovedSlot[i], -1);
            auto const g = stationaryBeforeRank[r];
            auto const movedSlot = gapBase[g] + nextMoved[g]++;
            auto const to = slots.before(movedSlot);
            slots.add(movedSlot, 1);
            out.ops.push_back(ViewOp{.kind = ViewOp::Kind::Moved, .first = from, .count = 1, .to = to});
        }
    }

    // Insertions, front to back, in runs.
    for (std::size_t i = 0; i < after.size();) {
        if (posBefore[after[i]] != kAbsent) {
            ++i;
            continue;
        }
        std::size_t end = i + 1;
        while (end < after.size() && posBefore[after[end]] == kAbsent) {
            ++end;
        }
        out.ops.push_back(ViewOp{.kind = ViewOp::Kind::Inserted, .first = i, .count = end - i, .to = 0});
        if (out.ops.size() > resetThreshold) {
            return reset();
        }
        i = end;
    }

    // Changes, in new positions, in runs.
    std::vector<std::uint32_t> changedAt;
    for (auto const id : changed) {
        if (id < identityLimit && posAfter[id] != kAbsent && posBefore[id] != kAbsent) {
            changedAt.push_back(posAfter[id]);
        }
    }
    std::ranges::sort(changedAt);
    auto const [dupFirst, dupLast] = std::ranges::unique(changedAt);
    changedAt.erase(dupFirst, dupLast);
    for (std::size_t i = 0; i < changedAt.size();) {
        std::size_t end = i + 1;
        while (end < changedAt.size() && changedAt[end] == changedAt[end - 1] + 1) {
            ++end;
        }
        out.ops.push_back(ViewOp{.kind = ViewOp::Kind::Changed, .first = changedAt[i], .count = end - i, .to = 0});
        if (out.ops.size() > resetThreshold) {
            return reset();
        }
        i = end;
    }
    return out;
}

/// @brief Applies @p change to @p list, following the replay contract.
///
/// The reference implementation of what a renderer's list model does; the
/// tests replay every change through it.
/// @tparam T Element type.
/// @param list   The old view's elements; becomes the new view's.
/// @param change The change.
/// @param after  The new view's elements, where inserted and reset rows come from.
template <typename T>
void applyViewChange(std::vector<T>& list, ViewChange const& change, std::span<T const> after) {
    for (auto const& op : change.ops) {
        auto const first = static_cast<std::ptrdiff_t>(op.first);
        switch (op.kind) {
            case ViewOp::Kind::Removed:
                list.erase(list.begin() + first, list.begin() + first + static_cast<std::ptrdiff_t>(op.count));
                break;
            case ViewOp::Kind::Moved: {
                T moved = std::move(list[op.first]);
                list.erase(list.begin() + first);
                list.insert(list.begin() + static_cast<std::ptrdiff_t>(op.to), std::move(moved));
                break;
            }
            case ViewOp::Kind::Inserted:
                list.insert(list.begin() + first, after.begin() + first,
                            after.begin() + first + static_cast<std::ptrdiff_t>(op.count));
                break;
            case ViewOp::Kind::Reset:
                list.assign(after.begin(), after.end());
                break;
            case ViewOp::Kind::Changed:
            default:
                break;
        }
    }
}

}  // namespace morph::table
