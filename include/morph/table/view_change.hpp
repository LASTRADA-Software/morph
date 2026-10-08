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

// NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) -- row and column indices are bounded by the snapshot's own counts, and the sort and filter loops index once per comparison, where at() would check every access

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

    /// @brief Marks a slot occupied.
    /// @param slot The slot.
    void occupy(std::size_t slot) { change(slot, 1); }

    /// @brief Marks a slot free.
    /// @param slot The slot.
    void release(std::size_t slot) { change(slot, -1); }

    /// @brief Occupied slots before @p slot.
    /// @param slot The slot.
    /// @return The count.
    [[nodiscard]] std::size_t before(std::size_t slot) const {
        int sum = 0;
        for (auto index = slot; index > 0; index -= index & (~index + 1)) {
            sum += _tree[index];
        }
        return static_cast<std::size_t>(sum);
    }

private:
    void change(std::size_t slot, int delta) {  // NOLINT(bugprone-easily-swappable-parameters)
        for (auto index = slot + 1; index < _tree.size(); index += index & (~index + 1)) {
            _tree[index] += delta;
        }
    }

    std::vector<int> _tree;
};

/// @brief Marks the elements of a longest strictly increasing subsequence.
/// @param values The sequence.
/// @return One flag per element; `true` on the subsequence.
[[nodiscard]] inline std::vector<bool> longestIncreasing(std::span<std::uint32_t const> values) {
    constexpr auto kNone = std::numeric_limits<std::size_t>::max();
    std::vector<std::size_t> tails;  // index into values of the smallest tail of each length
    std::vector<std::size_t> parent(values.size(), kNone);
    for (std::size_t i = 0; i < values.size(); ++i) {
        auto const position =
            std::ranges::lower_bound(tails, values[i], {}, [&](std::size_t index) { return values[index]; });
        auto const length = static_cast<std::size_t>(position - tails.begin());
        if (length > 0) {
            parent[i] = tails[length - 1];
        }
        if (position == tails.end()) {
            tails.push_back(i);
        } else {
            *position = i;
        }
    }
    std::vector<bool> marked(values.size(), false);
    if (!tails.empty()) {
        for (auto index = tails.back(); index != kNone; index = parent[index]) {
            marked[index] = true;
        }
    }
    return marked;
}

/// @brief Marks an identity absent from a view.
inline constexpr auto kAbsent = std::numeric_limits<std::uint32_t>::max();

/// @brief Where each identity sits in each view.
struct ViewPositions {
    /// @brief Position in the old view, or `kAbsent`.
    std::vector<std::uint32_t> before;
    /// @brief Position in the new view, or `kAbsent`.
    std::vector<std::uint32_t> after;
};

/// @brief Appends the removals, back to front, coalesced into runs.
/// @param before    The old view.
/// @param positions Where identities sit.
/// @param out       Receives the operations.
inline void appendRemovals(std::span<std::uint32_t const> before, ViewPositions const& positions,
                           std::vector<ViewOp>& out) {
    for (std::size_t end = before.size(); end > 0;) {
        if (positions.after[before[end - 1]] != kAbsent) {
            --end;
            continue;
        }
        std::size_t start = end - 1;
        while (start > 0 && positions.after[before[start - 1]] == kAbsent) {
            --start;
        }
        out.push_back(ViewOp{.kind = ViewOp::Kind::Removed, .first = start, .count = end - start, .to = 0});
        end = start;
    }
}

/// @brief Appends the moves that put the kept rows in their new order.
///
/// @p ranks lists the kept rows in old order, as their rank in the new order.
/// Rows on a longest increasing subsequence stay. Between two stationary rows
/// is a gap; each moving row leaves a slot in the gap it was in and takes a
/// slot in the gap it belongs to. Within a gap, rows already moved (by rank)
/// sit before rows not yet moved (by old position), so every row's index is a
/// count of occupied slots before its own.
/// @param ranks The kept rows, in old order, as new-order ranks.
/// @param out   Receives the operations.
inline void appendMoves(std::span<std::uint32_t const> ranks, std::vector<ViewOp>& out) {
    auto const stationary = longestIncreasing(ranks);
    auto const stationaryCount = static_cast<std::size_t>(std::ranges::count(stationary, true));
    auto const kept = ranks.size();
    std::vector<std::size_t> oldGap(kept);
    std::vector<std::size_t> stationaryBeforeRank(kept + 1, 0);
    std::size_t seen = 0;
    for (std::size_t i = 0; i < kept; ++i) {
        oldGap[i] = seen;
        if (stationary[i]) {
            stationaryBeforeRank[ranks[i] + 1] = 1;
            ++seen;
        }
    }
    for (std::size_t rank = 0; rank < kept; ++rank) {
        stationaryBeforeRank[rank + 1] += stationaryBeforeRank[rank];
    }
    // Slot layout per gap: [moved slots][unmoved slots][stationary slot].
    std::vector<std::size_t> movedInGap(stationaryCount + 1, 0);
    std::vector<std::size_t> unmovedInGap(stationaryCount + 1, 0);
    for (std::size_t i = 0; i < kept; ++i) {
        if (!stationary[i]) {
            ++movedInGap[stationaryBeforeRank[ranks[i]]];
            ++unmovedInGap[oldGap[i]];
        }
    }
    std::vector<std::size_t> gapBase(stationaryCount + 2, 0);
    for (std::size_t gap = 0; gap <= stationaryCount; ++gap) {
        gapBase[gap + 1] = gapBase[gap] + movedInGap[gap] + unmovedInGap[gap] + (gap < stationaryCount ? 1 : 0);
    }
    SlotCounter slots{gapBase[stationaryCount + 1]};
    std::vector<std::size_t> unmovedSlot(kept, 0);
    std::vector<std::size_t> nextUnmoved(stationaryCount + 1, 0);
    std::vector<std::size_t> indexOfRank(kept, 0);
    for (std::size_t i = 0; i < kept; ++i) {
        auto const gap = oldGap[i];
        indexOfRank[ranks[i]] = i;
        if (stationary[i]) {
            slots.occupy(gapBase[gap] + movedInGap[gap] + unmovedInGap[gap]);
        } else {
            unmovedSlot[i] = gapBase[gap] + movedInGap[gap] + nextUnmoved[gap]++;
            slots.occupy(unmovedSlot[i]);
        }
    }
    std::vector<std::size_t> nextMoved(stationaryCount + 1, 0);
    for (std::size_t rank = 0; rank < kept; ++rank) {
        auto const i = indexOfRank[rank];
        if (stationary[i]) {
            continue;
        }
        auto const from = slots.before(unmovedSlot[i]);
        slots.release(unmovedSlot[i]);
        auto const gap = stationaryBeforeRank[rank];
        auto const movedSlot = gapBase[gap] + nextMoved[gap]++;
        auto const destination = slots.before(movedSlot);
        slots.occupy(movedSlot);
        out.push_back(ViewOp{.kind = ViewOp::Kind::Moved, .first = from, .count = 1, .to = destination});
    }
}

/// @brief Appends the insertions, front to back, coalesced into runs.
/// @param after     The new view.
/// @param positions Where identities sit.
/// @param out       Receives the operations.
inline void appendInsertions(std::span<std::uint32_t const> after, ViewPositions const& positions,
                             std::vector<ViewOp>& out) {
    for (std::size_t start = 0; start < after.size();) {
        if (positions.before[after[start]] != kAbsent) {
            ++start;
            continue;
        }
        std::size_t end = start + 1;
        while (end < after.size() && positions.before[after[end]] == kAbsent) {
            ++end;
        }
        out.push_back(ViewOp{.kind = ViewOp::Kind::Inserted, .first = start, .count = end - start, .to = 0});
        start = end;
    }
}

/// @brief Appends the changes of rows in both views, in new positions, coalesced into runs.
/// @param changed   Identities whose content changed.
/// @param positions Where identities sit.
/// @param out       Receives the operations.
inline void appendChanges(std::span<std::uint32_t const> changed, ViewPositions const& positions,
                          std::vector<ViewOp>& out) {
    std::vector<std::uint32_t> changedAt;
    for (auto const identity : changed) {
        if (identity < positions.after.size() && positions.after[identity] != kAbsent &&
            positions.before[identity] != kAbsent) {
            changedAt.push_back(positions.after[identity]);
        }
    }
    std::ranges::sort(changedAt);
    auto const [dupFirst, dupLast] = std::ranges::unique(changedAt);
    changedAt.erase(dupFirst, dupLast);
    for (std::size_t start = 0; start < changedAt.size();) {
        std::size_t end = start + 1;
        while (end < changedAt.size() && changedAt[end] == changedAt[end - 1] + 1) {
            ++end;
        }
        out.push_back(ViewOp{.kind = ViewOp::Kind::Changed, .first = changedAt[start], .count = end - start, .to = 0});
        start = end;
    }
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
// NOLINTBEGIN(bugprone-easily-swappable-parameters) -- old then new view, then the identity bound and the threshold, as documented above
[[nodiscard]] inline ViewChange diffViews(std::span<std::uint32_t const> before, std::span<std::uint32_t const> after,
                                          std::span<std::uint32_t const> changed, std::size_t identityLimit,
                                          std::size_t resetThreshold) {
    // NOLINTEND(bugprone-easily-swappable-parameters)
    ViewChange out;
    detail::ViewPositions positions{.before = std::vector<std::uint32_t>(identityLimit, detail::kAbsent),
                                    .after = std::vector<std::uint32_t>(identityLimit, detail::kAbsent)};
    for (std::size_t i = 0; i < after.size(); ++i) {
        positions.after[after[i]] = static_cast<std::uint32_t>(i);
    }
    for (std::size_t i = 0; i < before.size(); ++i) {
        positions.before[before[i]] = static_cast<std::uint32_t>(i);
    }
    detail::appendRemovals(before, positions, out.ops);

    // The kept rows in old order, as ranks in the new order.
    std::vector<std::uint32_t> rankOf(identityLimit, detail::kAbsent);
    std::uint32_t rank = 0;
    for (auto const identity : after) {
        if (positions.before[identity] != detail::kAbsent) {
            rankOf[identity] = rank++;
        }
    }
    std::vector<std::uint32_t> ranks;
    ranks.reserve(rank);
    for (auto const identity : before) {
        if (rankOf[identity] != detail::kAbsent) {
            ranks.push_back(rankOf[identity]);
        }
    }
    if (out.ops.size() <= resetThreshold) {
        detail::appendMoves(ranks, out.ops);
    }
    if (out.ops.size() <= resetThreshold) {
        detail::appendInsertions(after, positions, out.ops);
    }
    if (out.ops.size() <= resetThreshold) {
        detail::appendChanges(changed, positions, out.ops);
    }
    if (out.ops.size() > resetThreshold) {
        out.ops.assign(1, ViewOp{.kind = ViewOp::Kind::Reset, .first = 0, .count = 0, .to = 0});
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

// NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)

}  // namespace morph::table
