// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "bookmarks/core/types.hpp"

#include <morph/util/datetime.hpp>

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

/// @file
/// Bookmark wire DTOs. `RecordMetadata` is the one action a GUI client never
/// sends — it is dispatched exclusively by the app-layer metadata-fetch
/// worker's internal client (Task 12), the same "internal-only" shape
/// `pastebin::ExpirePaste` established.

namespace bookmarks {

/// @brief Whether a bookmark is visible only to its owner or to the shared feed.
enum class Visibility { Private, Shared };

/// @brief Whether a bookmark has been read.
enum class ReadState { Unread, Read };

/// @brief Whether a bookmark is archived (hidden from the default list, not deleted).
enum class ArchiveState { Active, Archived };

/// @brief `ListBookmarks`' read-state filter.
enum class ReadFilter { Any, UnreadOnly, ReadOnly };

/// @brief `ListBookmarks`' archive-state filter.
enum class ArchiveFilter { Any, ActiveOnly, ArchivedOnly };

/// @brief Longest `url`, in bytes, this rung accepts (a sanity bound, not a
///        storage-column width — url/title are variable-length `TEXT`
///        columns with no fixed capacity to overflow, per
///        `IMPLEMENTATION.md` rule 4's "content needs no equivalent bound"
///        clause).
inline constexpr std::size_t kMaxUrlBytes = 2048;
/// @brief Longest `title`, in bytes, this rung accepts.
inline constexpr std::size_t kMaxTitleBytes = 512;

struct CreateBookmark {
    std::string url;
    std::string title;        // empty = not yet known; the metadata worker fills it in
    std::string description;
    std::string notes;
    std::vector<std::string> tags;  // tag names; auto-created on first use (Task 6)
    Visibility visibility = Visibility::Private;

    /// @brief Every member but `url` may be omitted from a schema-driven
    ///        submission — see `pastebin::CreatePaste::optionalFields`'s
    ///        doc comment for why this list exists at all.
    ///
    /// `title` belongs here for a reason the rest do not: this member's own
    /// comment above says "empty = not yet known; the metadata worker fills
    /// it in", and `validate()` accepts an empty one. Omitting it from this
    /// list made `schemaJson<CreateBookmark>()` emit `title` as *required*,
    /// so the generated create form refused to submit without one — which
    /// meant the shipped GUI could not create the very title-less bookmark
    /// the background metadata fetch exists to complete. Caught by driving
    /// the desktop client against a real server (task 18).
    static constexpr std::array<std::string_view, 5> optionalFields{"title", "description", "notes", "tags",
                                                                     "visibility"};

    [[nodiscard]] bool validate() const noexcept {
        return !url.empty() && url.size() <= kMaxUrlBytes && title.size() <= kMaxTitleBytes;
    }
};

struct CreateBookmarkResult {
    BookmarkId id;
};

/// @brief Full replace-set edit: `tags` is the *desired final* tag set, not
///        a delta — `BookmarkModel::execute(const EditBookmark&)` (Task 6)
///        diffs it against the current junction rows.
struct EditBookmark {
    BookmarkId id;
    std::string url;
    std::string title;
    std::string description;
    std::string notes;
    std::vector<std::string> tags;
    Visibility visibility = Visibility::Private;

    /// @brief Same set as `CreateBookmark::optionalFields`, and `title` is in
    ///        it for the same reason — see that member's doc comment.
    static constexpr std::array<std::string_view, 5> optionalFields{"title", "description", "notes", "tags",
                                                                     "visibility"};

    [[nodiscard]] bool validate() const noexcept {
        return id.hasValue() && !url.empty() && url.size() <= kMaxUrlBytes && title.size() <= kMaxTitleBytes;
    }
};

struct ArchiveBookmark {
    BookmarkId id;
    [[nodiscard]] bool validate() const noexcept { return id.hasValue(); }
};

struct UnarchiveBookmark {
    BookmarkId id;
    [[nodiscard]] bool validate() const noexcept { return id.hasValue(); }
};

struct DeleteBookmark {
    BookmarkId id;
    [[nodiscard]] bool validate() const noexcept { return id.hasValue(); }
};

struct GetBookmark {
    BookmarkId id;
    [[nodiscard]] bool validate() const noexcept { return id.hasValue(); }
};

/// @brief The full, owner-only view of one bookmark.
struct BookmarkView {
    BookmarkId id;
    std::string url;
    std::string title;
    std::string description;
    std::string notes;
    std::vector<std::string> tags;
    ::morph::time::Timestamp createdAt;
    ::morph::time::Timestamp updatedAt;
    ReadState readState = ReadState::Unread;
    ArchiveState archiveState = ArchiveState::Active;
    Visibility visibility = Visibility::Private;
};

/// @brief One row of `ListBookmarks`'/`GetChangesSince`'s result —
///        deliberately narrower than `BookmarkView`: a listing must not
///        leak `notes` (mirrors `pastebin::PasteSummary`'s non-leak rule).
struct BookmarkSummary {
    BookmarkId id;
    std::string url;
    std::string title;
    std::vector<std::string> tags;
    ::morph::time::Timestamp createdAt;
    ::morph::time::Timestamp updatedAt;
    ReadState readState = ReadState::Unread;
    ArchiveState archiveState = ArchiveState::Active;
    Visibility visibility = Visibility::Private;
};

struct ListBookmarks {
    Cursor cursor;             // empty = first page
    ReadFilter readFilter = ReadFilter::Any;
    ArchiveFilter archiveFilter = ArchiveFilter::ActiveOnly;  // archived hidden by default, linkding's own convention
    std::string tag;           // empty = no tag filter
    std::string searchText;    // empty = no text filter

    static constexpr std::array<std::string_view, 5> optionalFields{"cursor", "readFilter", "archiveFilter", "tag",
                                                                     "searchText"};

    [[nodiscard]] bool validate() const noexcept { return true; }  // every field is optional
};

struct ListBookmarksResult {
    std::vector<BookmarkSummary> bookmarks;
    Cursor nextCursor;  // empty = no further page
};

/// @brief Minimal changes-since poll (README's rung-3 event-pattern
///        preview): every bookmark this owner touched (created, edited,
///        archived/unarchived, or metadata-recorded) since @p since.
struct GetChangesSince {
    ChangesCursor since;  // empty = every bookmark ever (first poll)

    static constexpr std::array<std::string_view, 1> optionalFields{"since"};

    [[nodiscard]] bool validate() const noexcept { return true; }
};

struct GetChangesSinceResult {
    std::vector<BookmarkSummary> changed;
    /// @brief The boundary this poll ran to, captured *before* the query
    ///        itself (`BookmarkModel::execute`'s own doc comment, Task 7,
    ///        has the full argument for why) — the next poll's `since`.
    ///        `ChangesCursor` (issue #43), not a bare `Timestamp`: a
    ///        millisecond-resolution timestamp alone cannot distinguish a
    ///        write that lands in the exact same millisecond as this
    ///        instant from one that happened strictly before it.
    ChangesCursor asOf;
};

/// @brief Internal-only: the metadata-fetch worker's write-back
///        (`app::MetadataFetchWorker`, Task 12). Never dispatched by a GUI
///        client — mirrors `pastebin::ExpirePaste`'s "internal-only"
///        convention exactly.
struct RecordMetadata {
    BookmarkId id;
    std::string title;        // empty = the fetch found no <title>
    std::string faviconPath;  // empty = no favicon fetched

    static constexpr std::array<std::string_view, 2> optionalFields{"title", "faviconPath"};

    [[nodiscard]] bool validate() const noexcept { return id.hasValue(); }
};

}  // namespace bookmarks

/// @brief Reflects `Visibility` as readable strings — same rationale and
///        `glz::enumerate` shape as `pastebin`'s enum reflections
///        (`glz::meta<pastebin::Visibility>`'s doc comment has the full
///        argument: a bare ordinal degrades the schema writer's `$defs`
///        entry to an any-type union).
template <>
struct glz::meta<bookmarks::Visibility> {
    using enum bookmarks::Visibility;
    static constexpr auto value = glz::enumerate(Private, Shared);
};

template <>
struct glz::meta<bookmarks::ReadState> {
    using enum bookmarks::ReadState;
    static constexpr auto value = glz::enumerate(Unread, Read);
};

template <>
struct glz::meta<bookmarks::ArchiveState> {
    using enum bookmarks::ArchiveState;
    static constexpr auto value = glz::enumerate(Active, Archived);
};

template <>
struct glz::meta<bookmarks::ReadFilter> {
    using enum bookmarks::ReadFilter;
    static constexpr auto value = glz::enumerate(Any, UnreadOnly, ReadOnly);
};

template <>
struct glz::meta<bookmarks::ArchiveFilter> {
    using enum bookmarks::ArchiveFilter;
    static constexpr auto value = glz::enumerate(Any, ActiveOnly, ArchivedOnly);
};
