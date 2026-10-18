// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>

#include "bookmarks/core/types.hpp"
#include "bookmarks/dto/bookmark_dto.hpp"

namespace bookmarks::detail {

/// @brief One `BookmarkModel` instance's bookmarks, held in memory for as long
///        as the instance lives: the model's state under
///        `Storage::Memory`.
///
/// Answers the same actions with the same validation, errors and cursor
/// contracts as the SQL path, so a client cannot tell the two apart -- except
/// that nothing outlives the instance and no other instance sees it.
///
/// Reached only through the owning model's `execute`, which the framework runs
/// on that instance's strand one call at a time, so it needs no lock.
class MemoryBookmarkStore {
public:
    CreateBookmarkResult execute(const CreateBookmark& action);
    BookmarkView execute(const EditBookmark& action);
    Ack execute(const ArchiveBookmark& action);
    Ack execute(const UnarchiveBookmark& action);
    Ack execute(const DeleteBookmark& action);
    BookmarkView execute(const GetBookmark& action);
    ListBookmarksResult execute(const ListBookmarks& action);
    GetChangesSinceResult execute(const GetChangesSince& action);

private:
    struct Entry {
        BookmarkView view;
        std::int64_t updatedAtMs = 0;
    };

    /// @brief The caller's principal, which must be the one this store serves.
    /// @throws Forbidden with no authenticated principal, or under another one.
    void requireOwner();

    /// @brief The bookmark @p id. @throws NotFound when there is none.
    [[nodiscard]] Entry& find(const BookmarkId& id);

    /// Keyed by id; ordered, so a listing pages newest-first by walking it backwards.
    std::map<std::int64_t, Entry> _bookmarks;
    /// The principal this store serves, fixed by the first call: an instance
    /// is one client's, so a later call under another principal is someone
    /// else reaching for this one's rows.
    std::optional<std::string> _owner;
};

}  // namespace bookmarks::detail
