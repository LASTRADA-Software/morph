// SPDX-License-Identifier: Apache-2.0
#pragma once

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

/// @brief On the wire an `ImportOpId` is its nullable underlying string.
template <>
struct glz::meta<bookmarks::ImportOpId> {
    static constexpr auto value = &bookmarks::ImportOpId::value;
    static constexpr std::string_view name = "ImportOpId";
};
