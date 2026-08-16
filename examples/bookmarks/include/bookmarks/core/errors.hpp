// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdexcept>
#include <string>

/// @file
/// Domain exceptions. A model's `execute(...)` throws one of these; morph
/// captures it as a `std::exception_ptr` and delivers it to the caller's
/// `.onError(...)` callback. See `pastebin/core/errors.hpp` for the
/// identical shape and rationale this mirrors.

namespace bookmarks {

/// @brief Base of every bookmarks-specific error a model throws.
struct BookmarksError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/// @brief No bookmark/tag exists at the given id — it never existed, or it
///        was deleted. Ownership does *not* come into it: a row that exists
///        but belongs to another principal is `Forbidden`, which is what
///        `BookmarkModel::loadOwned()` actually throws for that case.
struct NotFound : BookmarksError {
    using BookmarksError::BookmarksError;
};

/// @brief An action's `validate()` rejected its input.
struct ValidationError : BookmarksError {
    using BookmarksError::BookmarksError;
};

/// @brief A write lost a race: the target row changed between this
///        client's read and its write (the compare-and-swap conflict shape
///        `pastebin::Conflict` established this session for `EditPaste`),
///        or a `MergeTags`/rename would collide with an existing tag name.
struct Conflict : BookmarksError {
    using BookmarksError::BookmarksError;
};

/// @brief The caller is authenticated, but the target row exists and is
///        owned by a different principal. Distinguished from `NotFound`
///        deliberately: `docs/spec/security.md`'s registration/instance
///        hooks already keep a foreign id from being *reached* in most
///        cases (Task 14), but a model's own re-check (rule 1 — the local
///        backend enforces nothing) needs its own typed signal, and the
///        expected-strain-points test for "local mode has no authorization
///        at all" (Task 15) specifically wants to see this thrown, not a
///        NotFound that would quietly look like the row never existed.
struct Forbidden : BookmarksError {
    using BookmarksError::BookmarksError;
};

/// @brief An import chunk (or other bounded payload) exceeded this rung's
///        own size bound, distinct from the transport's own message-size
///        limit (`docs/spec/security.md`) which rejects the call before a
///        model ever sees it.
struct TooLarge : BookmarksError {
    using BookmarksError::BookmarksError;
};

}  // namespace bookmarks
