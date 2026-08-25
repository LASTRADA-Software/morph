// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <glaze/glaze.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "bookmarks/core/types.hpp"
#include "bookmarks/units.hpp"

namespace bookmarks {

/// @brief `BulkEdit`'s archive-state instruction — a three-state enum
///        (`IMPLEMENTATION.md` rule 3: never a `bool` two-state flag, and
///        this action genuinely has a third "don't touch archive state at
///        all" option a bool cannot express).
enum class BulkArchiveOp { None, Archive, Unarchive };

/// @brief The rung's first multi-entity atomic action — all-or-nothing
///        against SQLite (README). `addTags`/`removeTags` are name-based
///        (auto-create-on-first-use for `addTags`, same as
///        `EditBookmark::tags`'s handling — Task 8's own doc comment has
///        the exact SQL). Every id must be owned by the caller or the
///        *whole* batch is rejected (Task 8's resolved "reject the whole
///        batch on one violation" design decision).
struct BulkEdit {
    std::vector<BookmarkId> ids;
    std::vector<std::string> addTags;
    std::vector<std::string> removeTags;
    BulkArchiveOp archive = BulkArchiveOp::None;

    static constexpr std::array<std::string_view, 3> optionalFields{"addTags", "removeTags", "archive"};

    [[nodiscard]] bool validate() const noexcept { return !ids.empty(); }
};

struct BulkEditResult {
    Count affected;
};

}  // namespace bookmarks

/// @brief Reflects `BulkArchiveOp` as readable strings — same rationale as
///        every other enum reflection in this rung.
template <>
struct glz::meta<bookmarks::BulkArchiveOp> {
    using enum bookmarks::BulkArchiveOp;
    static constexpr auto value = glz::enumerate(None, Archive, Unarchive);
};
