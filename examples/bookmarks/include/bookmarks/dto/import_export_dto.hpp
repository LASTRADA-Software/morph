// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "bookmarks/core/types.hpp"
#include "bookmarks/units.hpp"

#include <cstddef>
#include <string>

namespace bookmarks {

/// @brief Longest one `ImportBookmarks` chunk this rung accepts, in bytes —
///        well under the transport's own message-size bound
///        (`docs/spec/security.md`), so a client that respects this limit
///        never has to distinguish "this rung refused it" from "the
///        transport refused it".
///
/// A chunk over this bound is refused by `BookmarkModel::execute` with
/// `TooLarge`, not `ValidationError`, precisely so those two answers stay
/// distinguishable. The transport's own bound is *not* separately measured
/// by this rung — see the README's known-gaps section.
inline constexpr std::size_t kMaxImportChunkBytes = 65536;

/// @brief One chunk of a Netscape Bookmark HTML import. Idempotent per
///        `opId` (Task 5's `ImportedOpRecord`/Task 11's dedup check): a
///        retried chunk after a dropped connection is a safe no-op, never
///        a duplicate import.
struct ImportBookmarks {
    std::string chunk;
    ImportOpId opId;

    // Deliberately does NOT bound `chunk.size()` here: `validate()` is what
    // the framework's `ActionValidator`/`Bridge::executeVia` consult before
    // `Model::execute` is ever reached (`include/morph/core/bridge.hpp`,
    // `include/morph/core/remote.hpp`), so a size check here would fail the
    // request as `ValidationError` before `BookmarkModel::execute` gets a
    // chance to throw the more specific `TooLarge` -- exactly the
    // "make the chunks smaller" vs. "this request was malformed" distinction
    // `kMaxImportChunkBytes`'s own doc comment promises. The bound is
    // enforced once, in `BookmarkModel::execute(const ImportBookmarks&)`.
    [[nodiscard]] bool validate() const noexcept { return !chunk.empty() && opId.hasValue(); }
};

struct ImportBookmarksResult {
    Count imported;
    /// @brief Entries the chunk contained but this import did not write: a
    ///        malformed `<A>` entry with no href, or one whose url/title
    ///        exceeds `kMaxUrlBytes`/`kMaxTitleBytes` (writing those would
    ///        create a row `EditBookmark::validate()` would then refuse).
    Count skipped;
};

struct ExportBookmarks {
    [[nodiscard]] bool validate() const noexcept { return true; }
};

struct ExportBookmarksResult {
    std::string html;  // a complete Netscape Bookmark File
};

}  // namespace bookmarks
