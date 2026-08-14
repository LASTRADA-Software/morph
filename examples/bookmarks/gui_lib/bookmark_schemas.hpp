// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/forms/forms.hpp>

#include <string>

#include "bookmarks/dto/auth_dto.hpp"
#include "bookmarks/dto/bookmark_dto.hpp"
#include "bookmarks/dto/import_export_dto.hpp"
#include "bookmarks/dto/tag_dto.hpp"

/// @file
/// The one schema document every bookmarks form renders from, assembled in
/// one place so every shell that builds a `BookmarkFormsController` — the
/// desktop client (`gui/main.cpp`), a future WASM client, and the tests —
/// builds the *identical* map instead of each assembling its own
/// (`examples/TESTING.md`'s "same client code" requirement). Same split
/// `pastebin::gui::pasteSchemasJson()` uses, and for the same reason:
/// `BookmarkFormsController` takes the document as a constructor argument by
/// design, so whatever composes it decides which actions it serves.

namespace bookmarks::gui {

/// @brief The `{actionType: schema}` document this rung's forms render from.
///
/// Exactly the six actions a user *enters* — everything else is
/// parameterised by an id picked from a list, never typed, and therefore
/// routes through a presenter rather than a form:
///
/// * `Login` — the one action an unauthenticated caller can reach, and the
///   whole of this rung's login UI (`bookmarks/dto/auth_dto.hpp`'s `@file`
///   comment states plainly what "dev-mode login" does and does not mean).
///   Rendering it from its own schema rather than hand-building a username
///   field is what keeps `examples/IMPLEMENTATION.md` rule 2 true of the
///   login screen too.
/// * `CreateBookmark` / `EditBookmark` / `ImportBookmarks` — `BookmarkModel`.
/// * `RenameTag` / `MergeTags` — `TagModel`.
///
/// `BulkEdit` is deliberately absent, and its absence is a renderer
/// limitation rather than a design choice: its one required member is
/// `std::vector<BookmarkId>`, and the shipped `DynamicForm` has no control
/// for a JSON `array` field (see `BookmarkFormsController`'s class comment
/// and `examples/bookmarks/README.md`'s known-gaps entry). The GUI therefore
/// drives `BulkEdit` from the list's own multi-selection through
/// `BookmarkBridge`, where no typing is involved at all.
///
/// @return `{"Login": …, "CreateBookmark": …, "EditBookmark": …,
///          "ImportBookmarks": …, "RenameTag": …, "MergeTags": …}`.
[[nodiscard]] inline std::string bookmarkSchemasJson() {
    return std::string{"{\"Login\":"} + ::morph::forms::schemaJson<bookmarks::Login>() +
           ",\"CreateBookmark\":" + ::morph::forms::schemaJson<bookmarks::CreateBookmark>() +
           ",\"EditBookmark\":" + ::morph::forms::schemaJson<bookmarks::EditBookmark>() +
           ",\"ImportBookmarks\":" + ::morph::forms::schemaJson<bookmarks::ImportBookmarks>() +
           ",\"RenameTag\":" + ::morph::forms::schemaJson<bookmarks::RenameTag>() +
           ",\"MergeTags\":" + ::morph::forms::schemaJson<bookmarks::MergeTags>() + "}";
}

}  // namespace bookmarks::gui
