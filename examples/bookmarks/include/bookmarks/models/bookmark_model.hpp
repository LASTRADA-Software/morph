// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/core/effects.hpp>
#include <morph/core/registry.hpp>

#include "bookmarks/core/errors.hpp"
#include "bookmarks/dto/bookmark_dto.hpp"
#include "bookmarks/dto/bulk_dto.hpp"
#include "bookmarks/dto/import_export_dto.hpp"
#include "bookmarks/dto/tag_dto.hpp"

/// @file
/// `BookmarkModel` — every action this rung's one entity-owning model
/// serves. Declared once, complete, here; Tasks 7/8 add bodies to
/// `bookmark_model.cpp` for `ListBookmarks`/`GetChangesSince`/`BulkEdit`/
/// `RecordMetadata` without touching this header again.

namespace bookmarks {

namespace detail {
class MemoryBookmarkStore;
}  // namespace detail

/// @brief Where `BookmarkModel` instances keep their bookmarks.
enum class Storage : std::uint8_t {
    /// The SQL database every instance shares (the default).
    Sql,
    /// Each instance's own memory, for as long as the instance lives: nothing
    /// is persisted and no instance sees another's bookmarks. For measuring
    /// the framework without a database under it.
    Memory,
};

/// @brief Chooses where `BookmarkModel` instances constructed from now on keep
///        their bookmarks. Process-wide; set once, before the server serves.
/// @param storage The storage.
void setStorage(Storage storage) noexcept;

/// @return Where `BookmarkModel` instances constructed now keep their bookmarks.
[[nodiscard]] Storage storage() noexcept;

/// @brief Create/read/edit/archive/delete/list/bulk-edit over the
///        `bookmarks`/`bookmark_tags` tables, scoped to the authenticated
///        caller's own collection.
///
/// Registered **plain** — no `BRIDGE_MODEL_KEY`, no `AllowShared`. Only
/// plain registration records a real instance owner (a *shared* instance is
/// recorded with an empty owner by design), so this is what makes
/// `BookmarksAuthorizer::authorizeInstance`'s per-instance ownership check
/// genuinely enforcing for this model: `register` envelopes now carry the
/// caller's authenticated session, so each client's own instance is recorded
/// under that client's real principal.
///
/// That instance-level check alone is not what keeps one user out of
/// another's bookmarks, though — `BridgeHandler<Model>` (this rung's only
/// shipped client) never names another connection's `modelId`, so a normal
/// client's cross-user access attempt (`GetBookmark{id}` naming another
/// user's row through the caller's *own* instance) never triggers
/// `authorizeInstance` at all; see that function's own doc comment for why.
/// What actually carries per-user, per-*row* ownership is this model
/// itself: every `execute()` reads `session::current()->principal` fresh
/// (`requireOwner()`) and uses it both as the query filter and, via
/// `loadOwned()`, as the authorization check on any row it touches.
/// `IMPLEMENTATION.md` rule 1 requires that re-check regardless (the local
/// backend enforces nothing at all), and it is the only layer that could
/// ever catch a row-level mismatch, on top of `SigningAuthorizer::
/// authorize()`'s per-`execute` token check and `authorizeInstance`'s
/// instance-level check. See `bookmarks/auth/bookmarks_authorizer.hpp` for
/// the full story.
///
/// Holds no database state itself: each `execute()` acquires a
/// `Lightweight::GlobalDataMapperPool()` connection for its own duration and
/// returns it before returning, rather than owning a connection for its own
/// lifetime.
class BookmarkModel {
public:
    BookmarkModel();
    ~BookmarkModel();
    BookmarkModel(BookmarkModel&&) noexcept;
    BookmarkModel& operator=(BookmarkModel&&) noexcept;
    BookmarkModel(const BookmarkModel&) = delete;
    BookmarkModel& operator=(const BookmarkModel&) = delete;

    CreateBookmarkResult execute(const CreateBookmark& action);
    BookmarkView execute(const EditBookmark& action);
    Ack execute(const ArchiveBookmark& action);
    Ack execute(const UnarchiveBookmark& action);
    Ack execute(const DeleteBookmark& action);
    BookmarkView execute(const GetBookmark& action);
    ListBookmarksResult execute(const ListBookmarks& action);      // Task 7
    GetChangesSinceResult execute(const GetChangesSince& action);  // Task 7
    BulkEditResult execute(const BulkEdit& action);                // Task 8
    Ack execute(const RecordMetadata& action);                     // Task 8, internal-only
    ImportBookmarksResult execute(const ImportBookmarks& action);  // Task 11
    ExportBookmarksResult execute(const ExportBookmarks& action);  // Task 11

private:
    /// Engaged under `Storage::Memory`: this instance's bookmarks, which the
    /// actions it supports are answered from instead of the database.
    std::unique_ptr<detail::MemoryBookmarkStore> _memory;
};

}  // namespace bookmarks

BRIDGE_REGISTER_MODEL(bookmarks::BookmarkModel, "BookmarkModel")
// A bookmark's tags are counted by the tag list, so whatever adds, removes or retags a bookmark changes the tags too.
BRIDGE_REGISTER_ACTION(bookmarks::BookmarkModel, bookmarks::CreateBookmark, "CreateBookmark")
BRIDGE_REGISTER_EFFECTS(bookmarks::CreateBookmark, ::morph::effects::changes<bookmarks::BookmarkSummary>,
                        ::morph::effects::changes<bookmarks::TagSummary>)
// The reply is the edited bookmark's full view; its summary in a list differs in shape, so the lists refetch.
BRIDGE_REGISTER_ACTION(bookmarks::BookmarkModel, bookmarks::EditBookmark, "EditBookmark")
BRIDGE_REGISTER_EFFECTS(bookmarks::EditBookmark, ::morph::effects::updates<bookmarks::BookmarkView>,
                        ::morph::effects::changes<bookmarks::BookmarkSummary>,
                        ::morph::effects::changes<bookmarks::TagSummary>)
BRIDGE_REGISTER_ACTION(bookmarks::BookmarkModel, bookmarks::ArchiveBookmark, "ArchiveBookmark")
BRIDGE_REGISTER_EFFECTS(bookmarks::ArchiveBookmark, ::morph::effects::changes<bookmarks::BookmarkSummary>,
                        ::morph::effects::changes<bookmarks::BookmarkView>,
                        ::morph::effects::changes<bookmarks::TagSummary>)
BRIDGE_REGISTER_ACTION(bookmarks::BookmarkModel, bookmarks::UnarchiveBookmark, "UnarchiveBookmark")
BRIDGE_REGISTER_EFFECTS(bookmarks::UnarchiveBookmark, ::morph::effects::changes<bookmarks::BookmarkSummary>,
                        ::morph::effects::changes<bookmarks::BookmarkView>,
                        ::morph::effects::changes<bookmarks::TagSummary>)
BRIDGE_REGISTER_ACTION(bookmarks::BookmarkModel, bookmarks::DeleteBookmark, "DeleteBookmark")
BRIDGE_REGISTER_EFFECTS(bookmarks::DeleteBookmark, ::morph::effects::changes<bookmarks::BookmarkSummary>,
                        ::morph::effects::changes<bookmarks::BookmarkView>,
                        ::morph::effects::changes<bookmarks::TagSummary>)
BRIDGE_REGISTER_ACTION(bookmarks::BookmarkModel, bookmarks::GetBookmark, "GetBookmark")
BRIDGE_REGISTER_ACTION(bookmarks::BookmarkModel, bookmarks::ListBookmarks, "ListBookmarks",
                       ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(bookmarks::BookmarkModel, bookmarks::GetChangesSince, "GetChangesSince",
                       ::morph::model::Loggable::No)
// BulkEdit is outbox-managed (Task 8) -- Loggable::No here too, so the
// framework's own auto-append never double-logs alongside the model's own
// outbox write.
BRIDGE_REGISTER_ACTION(bookmarks::BookmarkModel, bookmarks::BulkEdit, "BulkEdit", ::morph::model::Loggable::No)
BRIDGE_REGISTER_EFFECTS(bookmarks::BulkEdit, ::morph::effects::changes<bookmarks::BookmarkSummary>,
                        ::morph::effects::changes<bookmarks::BookmarkView>,
                        ::morph::effects::changes<bookmarks::TagSummary>)
// The metadata worker fills in a fetched title, which every list showing the bookmark then shows.
BRIDGE_REGISTER_ACTION(bookmarks::BookmarkModel, bookmarks::RecordMetadata, "RecordMetadata")
BRIDGE_REGISTER_EFFECTS(bookmarks::RecordMetadata, ::morph::effects::changes<bookmarks::BookmarkSummary>,
                        ::morph::effects::changes<bookmarks::BookmarkView>)
BRIDGE_REGISTER_ACTION(bookmarks::BookmarkModel, bookmarks::ImportBookmarks, "ImportBookmarks")
BRIDGE_REGISTER_EFFECTS(bookmarks::ImportBookmarks, ::morph::effects::changes<bookmarks::BookmarkSummary>,
                        ::morph::effects::changes<bookmarks::TagSummary>)
// Export reads, but a screen runs it as a mutation, on a button, rather than fetching the file on every visit.
BRIDGE_REGISTER_ACTION(bookmarks::BookmarkModel, bookmarks::ExportBookmarks, "ExportBookmarks",
                       ::morph::model::Loggable::No)
BRIDGE_REGISTER_EFFECTS(bookmarks::ExportBookmarks, ::morph::effects::none)
