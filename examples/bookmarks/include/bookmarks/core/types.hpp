// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/util/datetime.hpp>

#include <compare>
#include <cstdint>
#include <glaze/glaze.hpp>
#include <optional>
#include <string>
#include <utility>

/// @file
/// Bookmarks' strong id/protocol-scalar types. `BookmarkId`/`TagId` are the
/// numeric-surrogate-key sibling of `pastebin::PasteId` (which wraps a
/// string, since a paste's id *is* its animal-name primary key) —
/// bookmarks' primary keys are ordinary auto-incrementing integers (bank's
/// convention, `Light::PrimaryKey::ServerSideAutoIncrement`), so the
/// wrapped payload is `std::int64_t`, not `std::string`. Same
/// `hasValue()`-capable shape and the same `fromOptional` factory
/// (`examples/pastebin/include/pastebin/core/types.hpp`'s own doc comment
/// explains why it exists as a named factory rather than a second
/// same-arity constructor).

namespace bookmarks {

/// @brief Strong id for a bookmark (a `bookmarks` table surrogate key).
///
/// Wire form: a plain nullable JSON integer (via the `glz::meta`
/// specialisation below) — exactly like an unwrapped `std::optional<std::int64_t>`.
struct BookmarkId {
    /// @brief The payload; `std::nullopt` means "not entered".
    std::optional<std::int64_t> value;

    /// @brief Constructs the empty state.
    constexpr BookmarkId() noexcept = default;

    /// @brief Engages with @p id.
    explicit BookmarkId(std::int64_t id) noexcept : value{id} {}

    /// @brief Adopts an optional payload as-is.
    /// @param payload The optional payload to adopt as-is.
    /// @return A `BookmarkId` wrapping @p payload directly.
    [[nodiscard]] static BookmarkId fromOptional(std::optional<std::int64_t> payload) noexcept {
        BookmarkId result;
        result.value = payload;
        return result;
    }

    /// @brief Whether a value has been entered.
    /// @return `true` if the payload is engaged.
    [[nodiscard]] bool hasValue() const noexcept { return value.has_value(); }

    /// @brief Unchecked access to the engaged value (UB when empty, exactly
    ///        like `std::optional::operator*`).
    /// @return The engaged value.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    [[nodiscard]] std::int64_t operator*() const noexcept { return *value; }

    /// @brief Equality/ordering on the payload; empty compares only equal to empty.
    [[nodiscard]] auto operator<=>(const BookmarkId&) const noexcept = default;
};

/// @brief Strong id for a tag (a `tags` table surrogate key). Same shape as
///        `BookmarkId` — see that type's doc comment.
struct TagId {
    std::optional<std::int64_t> value;

    constexpr TagId() noexcept = default;
    explicit TagId(std::int64_t id) noexcept : value{id} {}

    [[nodiscard]] static TagId fromOptional(std::optional<std::int64_t> payload) noexcept {
        TagId result;
        result.value = payload;
        return result;
    }

    [[nodiscard]] bool hasValue() const noexcept { return value.has_value(); }
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    [[nodiscard]] std::int64_t operator*() const noexcept { return *value; }
    [[nodiscard]] auto operator<=>(const TagId&) const noexcept = default;
};

/// @brief Opaque pagination cursor, shared by every list action in this
///        rung (`ListBookmarks`, `ListSharedFeed`) — each keyset-paginates
///        on a numeric surrogate primary key, so one cursor shape serves
///        all of them (`IMPLEMENTATION.md` rule 3's protocol-scalars row:
///        a named opaque newtype per *role*, and "pagination cursor" is one
///        role here, not one per entity).
struct Cursor {
    std::optional<std::int64_t> value;

    constexpr Cursor() noexcept = default;
    explicit Cursor(std::int64_t token) noexcept : value{token} {}

    [[nodiscard]] static Cursor fromOptional(std::optional<std::int64_t> payload) noexcept {
        Cursor result;
        result.value = payload;
        return result;
    }

    [[nodiscard]] bool hasValue() const noexcept { return value.has_value(); }
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    [[nodiscard]] std::int64_t operator*() const noexcept { return *value; }
    [[nodiscard]] auto operator<=>(const Cursor&) const noexcept = default;
};

/// @brief `GetChangesSince`'s cursor (issue #43): a millisecond timestamp
///        alone cannot be a correct "since" boundary, because a strict `>`
///        comparison on `updated_at_ms` silently drops a write that lands in
///        the *same millisecond* as the previous poll's cursor -- plausible
///        whenever poll -> write -> poll executes within one clock tick (a
///        fast machine, or a loaded CI runner). Neither `>` (under-inclusive,
///        the bug) nor `>=` (over-inclusive: would re-deliver the exact row
///        that established the cursor on every later poll at the same
///        instant) is correct alone. Pairing the timestamp with the id of
///        the last row already delivered *at that exact timestamp* makes
///        the boundary strictly orderable: a query filters on
///        `updated_at_ms > timestampMs OR (updated_at_ms = timestampMs AND
///        id > lastId)`, so a same-millisecond write with a higher id is
///        included, and the row that produced `lastId` itself is not
///        re-delivered.
///
///        `lastId` is meaningful only relative to its own `timestampMs`; it
///        does not on its own establish a global row ordering the way
///        `Cursor` (this file, `ListBookmarks`' keyset pagination) does --
///        `BookmarkRecord.id` and `updated_at_ms` do not necessarily
///        co-vary, since a row's id is assigned at creation but
///        `updated_at_ms` bumps on every later edit. `lastId.hasValue() ==
///        false` (the default) means "no tie-break needed": correct both
///        for the empty "first poll ever" cursor and for an `asOf` whose
///        instant had no row landing at exactly that millisecond.
///
/// Deliberately a plain aggregate with no user-declared special members
/// (matching `BookmarkSummary`/`GetChangesSince`/`GetChangesSinceResult`,
/// not `Cursor`/`BookmarkId`'s explicit-constructor-plus-`glz::meta` shape):
/// glaze's automatic reflection needs it that way, and no call site needs
/// direct `ChangesCursor` equality/ordering -- see this file's `glz::meta`
/// section for why `ChangesCursor` itself has none.
struct ChangesCursor {
    /// @brief The boundary instant. Empty means "the beginning of time"
    ///        (`GetChangesSince`'s first-ever poll).
    ::morph::time::Timestamp timestampMs;
    /// @brief The highest id already delivered at exactly `timestampMs`.
    ///        Empty means no tie-break is needed at this boundary.
    std::optional<std::int64_t> lastId;
};

/// @brief Idempotency key for one chunk of an `ImportBookmarks` call
///        (`IMPLEMENTATION.md` rule 3's protocol-scalars row: op-ids /
///        idempotency keys get a named opaque newtype). String-payload,
///        client-chosen, opaque — same shape as `pastebin::PasteId`.
struct ImportOpId {
    std::optional<std::string> value;

    constexpr ImportOpId() noexcept = default;
    explicit ImportOpId(std::string token) noexcept : value{std::move(token)} {}

    [[nodiscard]] static ImportOpId fromOptional(std::optional<std::string> payload) noexcept {
        ImportOpId result;
        result.value = std::move(payload);
        return result;
    }

    [[nodiscard]] bool hasValue() const noexcept { return value.has_value(); }
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    [[nodiscard]] const std::string& operator*() const noexcept { return *value; }
    [[nodiscard]] auto operator<=>(const ImportOpId&) const noexcept = default;
};

/// @brief Trivial, fieldless acknowledgement result for actions with
///        nothing else to return. Mirrors `pastebin::Ack`.
struct Ack {};

}  // namespace bookmarks

/// @brief On the wire a `BookmarkId` is its nullable underlying integer.
template <>
struct glz::meta<bookmarks::BookmarkId> {
    static constexpr auto value = &bookmarks::BookmarkId::value;
    static constexpr std::string_view name = "BookmarkId";
};

/// @brief On the wire a `TagId` is its nullable underlying integer.
template <>
struct glz::meta<bookmarks::TagId> {
    static constexpr auto value = &bookmarks::TagId::value;
    static constexpr std::string_view name = "TagId";
};

/// @brief On the wire a `Cursor` is its nullable underlying integer.
template <>
struct glz::meta<bookmarks::Cursor> {
    static constexpr auto value = &bookmarks::Cursor::value;
    static constexpr std::string_view name = "Cursor";
};

// `ChangesCursor` needs no `glz::meta` specialisation: it is a plain
// aggregate with public named fields (`timestampMs`, `lastId`), so glaze's
// automatic reflection already maps it to a small wire object with those
// same field names -- the same reason `BookmarkSummary` and
// `GetChangesSince`/`GetChangesSinceResult` (bookmark_dto.hpp) have none
// either. `glz::meta` here is reserved for the single-scalar newtypes above
// (which must be *unwrapped* to their payload on the wire) and the enums
// below (which need a string mapping).

/// @brief On the wire an `ImportOpId` is its nullable underlying string.
template <>
struct glz::meta<bookmarks::ImportOpId> {
    static constexpr auto value = &bookmarks::ImportOpId::value;
    static constexpr std::string_view name = "ImportOpId";
};
