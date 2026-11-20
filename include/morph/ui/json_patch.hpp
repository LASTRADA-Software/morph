// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <expected>
#include <morph/export.hpp>
#include <morph/ui/value.hpp>
#include <string>

/// @file
/// @brief RFC 6902 JSON Patch over a JSON value: applying one, and computing the one that turns a document into
///        another.
///
/// Specified in `docs/spec/ui/document.md`, "JSON Patch and structural diff".

namespace morph::ui {

/// @brief @p document with the RFC 6902 operations of @p patch applied in order.
///
/// Either every operation applies and the patched document is returned, or none does and the refusal names the first
/// one that failed. Each operation is one of `add`, `remove`, `replace`, `move`, `copy` and `test`; `add` and
/// `replace` and `test` carry a `value`, `move` and `copy` a `from`, and every operation a `path`. The path `""` is
/// the whole document. A path that does not exist is refused, and no intermediate container is made for it.
/// @param document The document to patch, taken by value so the caller's copy is untouched.
/// @param patch    A JSON array of operation objects; an empty array changes nothing.
/// @return The patched document, or a refusal: `op N is not a JSON object` or `op N: ...` for an operation that is
///         malformed, and `op N (name path): ...` for one that cannot be applied, with N counted from 0. A @p patch
///         that is not an array is refused with a message of its own.
[[nodiscard]] MORPH_API std::expected<Json, std::string> applyJsonPatch(Json document, Json const& patch);

/// @brief An RFC 6902 patch that turns @p before into @p after.
///
/// Objects are compared member by member and arrays element by element, so a change deep in a document is one
/// operation at its own pointer: a member only in @p before is a `remove`, one only in @p after an `add`, a value of
/// another kind or another scalar a `replace`. An array that grew gets an `add` for each element past the old end, in
/// order; one that shrank gets a `remove` for each element past the new end, last first, so every index named is the
/// one the operation finds. An element moved within an array or a member moved between parents is reported as the
/// removal and the addition it amounts to; no `move` or `copy` is produced. Pointer tokens escape `~` as `~0` and `/`
/// as `~1`.
/// @param before The document the patch starts from.
/// @param after  The document the patch ends at.
/// @return A JSON array of operations: empty when the documents are equal, and otherwise one that
///         `applyJsonPatch(before, patch)` turns back into @p after.
[[nodiscard]] MORPH_API Json diffJson(Json const& before, Json const& after);

}  // namespace morph::ui
