// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/core/bridge.hpp>
#include <morph/core/registry.hpp>

#include "bookmarks/core/errors.hpp"
#include "bookmarks/db/db_model.hpp"
#include "bookmarks/dto/bookmark_dto.hpp"
#include "bookmarks/dto/bulk_dto.hpp"
#include "bookmarks/dto/import_export_dto.hpp"

/// @file
/// `BookmarkModel` — every action this rung's one entity-owning model
/// serves. Declared once, complete, here; Tasks 7/8 add bodies to
/// `bookmark_model.cpp` for `ListBookmarks`/`GetChangesSince`/`BulkEdit`/
/// `RecordMetadata` without touching this header again.

namespace bookmarks {

/// @brief Create/read/edit/archive/delete/list/bulk-edit over the
///        `bookmarks`/`bookmark_tags` tables, scoped to the authenticated
///        caller's own collection.
///
/// Registered **plain** — no `BRIDGE_MODEL_KEY`, no `AllowShared`. The
/// original reason was that only plain registration records a real instance
/// owner (a *shared* instance is recorded with an empty owner, defeating
/// `authorizeInstance`'s per-instance ownership check). That reason no
/// longer carries any weight:
/// `docs/findings/027-register-envelope-carries-no-session.md` established
/// that a `register` envelope carries no session at all, so `RemoteServer`
/// records an empty owner for *every* instance, plain or shared, and
/// `authorizeInstance` therefore denies nothing in practice. Plain
/// registration is retained because it is the simpler shape and because the
/// hook is expected to become real once finding 027 is closed — not because
/// it is currently enforcing anything.
///
/// What actually carries per-user ownership is this model itself: every
/// `execute()` reads `session::current()->principal` fresh (`requireOwner()`)
/// and uses it both as the query filter and, via `loadOwned()`, as the
/// authorization check on any row it touches. `IMPLEMENTATION.md` rule 1
/// requires that re-check regardless (the local backend enforces nothing at
/// all); after finding 027 it is simply the only enforcement point there is,
/// on top of `SigningAuthorizer::authorize()`'s per-`execute` token check.
/// See `bookmarks/auth/bookmarks_authorizer.hpp` and the rung README's
/// "Corrected by finding 027" bullet for the full story.
class BookmarkModel : private db::WithMapper {
public:
    CreateBookmarkResult execute(const CreateBookmark& action);
    BookmarkView execute(const EditBookmark& action);
    Ack execute(const ArchiveBookmark& action);
    Ack execute(const UnarchiveBookmark& action);
    Ack execute(const DeleteBookmark& action);
    BookmarkView execute(const GetBookmark& action);
    ListBookmarksResult execute(const ListBookmarks& action);            // Task 7
    GetChangesSinceResult execute(const GetChangesSince& action);        // Task 7
    BulkEditResult execute(const BulkEdit& action);                      // Task 8
    Ack execute(const RecordMetadata& action);                          // Task 8, internal-only
    ImportBookmarksResult execute(const ImportBookmarks& action);        // Task 11
    ExportBookmarksResult execute(const ExportBookmarks& action);        // Task 11
};

}  // namespace bookmarks

BRIDGE_REGISTER_MODEL(bookmarks::BookmarkModel, "BookmarkModel")
BRIDGE_REGISTER_ACTION(bookmarks::BookmarkModel, bookmarks::CreateBookmark, "CreateBookmark")
BRIDGE_REGISTER_ACTION(bookmarks::BookmarkModel, bookmarks::EditBookmark, "EditBookmark")
BRIDGE_REGISTER_ACTION(bookmarks::BookmarkModel, bookmarks::ArchiveBookmark, "ArchiveBookmark")
BRIDGE_REGISTER_ACTION(bookmarks::BookmarkModel, bookmarks::UnarchiveBookmark, "UnarchiveBookmark")
BRIDGE_REGISTER_ACTION(bookmarks::BookmarkModel, bookmarks::DeleteBookmark, "DeleteBookmark")
BRIDGE_REGISTER_ACTION(bookmarks::BookmarkModel, bookmarks::GetBookmark, "GetBookmark")
BRIDGE_REGISTER_ACTION(bookmarks::BookmarkModel, bookmarks::ListBookmarks, "ListBookmarks",
                       ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(bookmarks::BookmarkModel, bookmarks::GetChangesSince, "GetChangesSince",
                       ::morph::model::Loggable::No)
// BulkEdit is outbox-managed (Task 8) -- Loggable::No here too, so the
// framework's own auto-append never double-logs alongside the model's own
// outbox write.
BRIDGE_REGISTER_ACTION(bookmarks::BookmarkModel, bookmarks::BulkEdit, "BulkEdit", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(bookmarks::BookmarkModel, bookmarks::RecordMetadata, "RecordMetadata")
BRIDGE_REGISTER_ACTION(bookmarks::BookmarkModel, bookmarks::ImportBookmarks, "ImportBookmarks")
BRIDGE_REGISTER_ACTION(bookmarks::BookmarkModel, bookmarks::ExportBookmarks, "ExportBookmarks",
                       ::morph::model::Loggable::No)
