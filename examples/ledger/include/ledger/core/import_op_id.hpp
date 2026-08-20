// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <compare>
#include <glaze/glaze.hpp>
#include <optional>
#include <string>
#include <utility>

/// @file
/// `ledger::ImportOpId` -- shape copied verbatim from
/// `bookmarks::ImportOpId` (`examples/bookmarks/include/bookmarks/core/
/// types.hpp`): string-payload, client-chosen, opaque idempotency key
/// (`IMPLEMENTATION.md` rule 3's protocol-scalars row: op-ids /
/// idempotency keys get a named opaque newtype).
///
/// Declared in this small shared header rather than inline in
/// `transaction_dto.hpp` (Task 11b, `StoreTransaction`'s own opId) or
/// `csv_import_dto.hpp` (Task 15, chunk-retry dedup -- this plan's design
/// spec §8) because both tasks need the identical type: declaring it once
/// here lets both include it, rather than duplicating the type or having
/// one task's DTO header reach into the other's.

namespace ledger {

/// @brief Idempotency key for an action that must be safely replayable
///        (`StoreTransaction`'s `opId`, Task 11b; `ImportBookmarks`-style
///        chunked imports, Task 15). Same shape as `bookmarks::ImportOpId`.
struct ImportOpId {
    /// @brief The payload; `std::nullopt` means "not entered" (no
    ///        idempotency requested).
    std::optional<std::string> value;

    /// @brief Constructs the empty state.
    constexpr ImportOpId() noexcept = default;

    /// @brief Engages with @p token.
    explicit ImportOpId(std::string token) noexcept : value{std::move(token)} {}

    /// @brief Adopts an optional payload as-is.
    /// @param payload The optional payload to adopt as-is.
    /// @return An `ImportOpId` wrapping @p payload directly.
    [[nodiscard]] static ImportOpId fromOptional(std::optional<std::string> payload) noexcept {
        ImportOpId result;
        result.value = std::move(payload);
        return result;
    }

    /// @brief Whether a value has been entered.
    /// @return `true` if the payload is engaged.
    [[nodiscard]] bool hasValue() const noexcept { return value.has_value(); }

    /// @brief Unchecked access to the engaged value (UB when empty, exactly
    ///        like `std::optional::operator*`).
    /// @return The engaged value.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    [[nodiscard]] const std::string& operator*() const noexcept { return *value; }

    /// @brief Equality/ordering on the payload; empty compares only equal to empty.
    [[nodiscard]] auto operator<=>(const ImportOpId&) const noexcept = default;
};

}  // namespace ledger

/// @brief On the wire an `ImportOpId` is its nullable underlying string.
template <>
struct glz::meta<ledger::ImportOpId> {
    static constexpr auto value = &ledger::ImportOpId::value;
    static constexpr std::string_view name = "ImportOpId";
};
