// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "bookmarks/core/types.hpp"
#include "bookmarks/dto/bookmark_dto.hpp"

#include <array>
#include <string_view>
#include <vector>

namespace bookmarks {

struct ListSharedFeed {
    Cursor cursor;  // empty = first page

    static constexpr std::array<std::string_view, 1> optionalFields{"cursor"};

    [[nodiscard]] bool validate() const noexcept { return true; }
};

/// @brief `BookmarkSummary` doubles as the shared feed's row shape — same
///        non-leak rule applies (no `notes`), and a shared bookmark's
///        `visibility` is always `Shared` by construction (the query that
///        builds this only ever selects `WHERE visibility = Shared`, Task
///        10), so there is nothing this result type needs beyond what
///        `BookmarkSummary` already carries.
struct ListSharedFeedResult {
    std::vector<BookmarkSummary> bookmarks;
    Cursor nextCursor;
};

}  // namespace bookmarks
