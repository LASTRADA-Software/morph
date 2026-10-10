// SPDX-License-Identifier: Apache-2.0
//
// morph::table ViewChange (spec 7 §7): minimal list operations that replay
// the old view into the new one.
//
// Mutations these tests were seen to fail on:
//  - diffViews marking no row stationary (every kept row moves): "moving the
//    first row to the end is one move" fails.
//  - diffViews computing a move's destination before removing the row from
//    its slot: "random reorders replay exactly" fails.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <morph/table/view_change.hpp>
#include <numeric>
#include <random>
#include <span>
#include <vector>

using namespace morph::table;

namespace {

using Ids = std::vector<std::uint32_t>;
using Kind = ViewOp::Kind;

ViewChange diff(Ids const& before, Ids const& after, Ids const& changed = {}, std::size_t threshold = 5000) {
    std::uint32_t limit = 0;
    for (auto const id : before) {
        limit = std::max(limit, id + 1);
    }
    for (auto const id : after) {
        limit = std::max(limit, id + 1);
    }
    return diffViews(before, after, changed, limit, threshold);
}

Ids replay(Ids list, ViewChange const& change, Ids const& after) {
    applyViewChange(list, change, std::span<std::uint32_t const>{after});
    return list;
}

std::size_t count(ViewChange const& change, Kind kind) {
    return static_cast<std::size_t>(
        std::ranges::count_if(change.ops, [kind](ViewOp const& op) { return op.kind == kind; }));
}

}  // namespace

TEST_CASE("table: an unchanged view gives no operations", "[table][viewchange]") {
    CHECK(diff({1, 2, 3}, {1, 2, 3}).empty());
    CHECK(diff({}, {}).empty());
}

TEST_CASE("table: a single insert, remove and update are one operation each", "[table][viewchange]") {
    auto const inserted = diff({1, 2, 3}, {1, 9, 2, 3});
    REQUIRE(inserted.ops.size() == 1);
    CHECK(inserted.ops[0] == ViewOp{.kind = Kind::Inserted, .first = 1, .count = 1, .to = 0});

    auto const removed = diff({1, 2, 3, 4}, {1, 4});
    REQUIRE(removed.ops.size() == 1);
    CHECK(removed.ops[0] == ViewOp{.kind = Kind::Removed, .first = 1, .count = 2, .to = 0});

    auto const updated = diff({1, 2, 3}, {1, 2, 3}, {2});
    REQUIRE(updated.ops.size() == 1);
    CHECK(updated.ops[0] == ViewOp{.kind = Kind::Changed, .first = 1, .count = 1, .to = 0});
}

TEST_CASE("table: moving the first row to the end is one move", "[table][viewchange]") {
    Ids const original{0, 1, 2, 3, 4, 5};
    Ids const rotated{1, 2, 3, 4, 5, 0};
    auto const change = diff(original, rotated);
    REQUIRE(change.ops.size() == 1);
    CHECK(change.ops[0] == ViewOp{.kind = Kind::Moved, .first = 0, .count = 1, .to = 5});
    CHECK(replay(original, change, rotated) == rotated);

    auto const back = diff(rotated, original);
    REQUIRE(back.ops.size() == 1);
    CHECK(back.ops[0] == ViewOp{.kind = Kind::Moved, .first = 5, .count = 1, .to = 0});
}

TEST_CASE("table: an updated row that moves is one move and one change", "[table][viewchange]") {
    Ids const before{0, 1, 2, 3};
    Ids const after{0, 2, 3, 1};
    auto const change = diff(before, after, {1});
    REQUIRE(change.ops.size() == 2);
    CHECK(change.ops[0].kind == Kind::Moved);
    CHECK(change.ops[1] == ViewOp{.kind = Kind::Changed, .first = 3, .count = 1, .to = 0});
    CHECK(replay(before, change, after) == after);
}

TEST_CASE("table: a reversal moves all but one row", "[table][viewchange]") {
    Ids before(50);
    std::ranges::iota(before, 0U);
    Ids after(before.rbegin(), before.rend());
    auto const change = diff(before, after);
    CHECK(count(change, Kind::Moved) == 49);
    CHECK(replay(before, change, after) == after);
}

TEST_CASE("table: random reorders with inserts and removes replay exactly", "[table][viewchange]") {
    // NOLINTNEXTLINE(bugprone-random-generator-seed,cert-msc32-c,cert-msc51-cpp) -- a fixed seed keeps the case reproducible
    std::mt19937 rng{889};
    for (int round = 0; round < 300; ++round) {
        auto const universe = static_cast<std::uint32_t>(1 + (rng() % 60));
        Ids pool(universe);
        std::ranges::iota(pool, 0U);
        std::ranges::shuffle(pool, rng);
        Ids before(pool.begin(), pool.begin() + static_cast<std::ptrdiff_t>(rng() % (universe + 1)));
        std::ranges::shuffle(pool, rng);
        Ids after(pool.begin(), pool.begin() + static_cast<std::ptrdiff_t>(rng() % (universe + 1)));
        Ids changed;
        for (auto const id : before) {
            if (rng() % 4 == 0) {
                changed.push_back(id);
            }
        }
        auto const change = diff(before, after, changed);
        INFO("round " << round);
        REQUIRE(replay(before, change, after) == after);
        for (auto const& op : change.ops) {
            if (op.kind == Kind::Changed) {
                for (std::size_t i = op.first; i < op.first + op.count; ++i) {
                    REQUIRE(std::ranges::find(changed, after[i]) != changed.end());
                    REQUIRE(std::ranges::find(before, after[i]) != before.end());
                }
            }
        }
    }
}

TEST_CASE("table: a change over the threshold is one reset", "[table][viewchange]") {
    Ids before(100);
    std::ranges::iota(before, 0U);
    Ids after(before.rbegin(), before.rend());
    auto const change = diff(before, after, {}, 10);
    CHECK(change.isReset());
    CHECK(replay(before, change, after) == after);

    Ids full(100);
    std::ranges::iota(full, 0U);
    Ids sparse;
    sparse.reserve(50);
    for (std::uint32_t i = 0; i < 100; i += 2) {
        sparse.push_back(i);
    }
    CHECK(diff(full, sparse, {}, 10).isReset());
    CHECK(diff(sparse, full, {}, 10).isReset());
}
