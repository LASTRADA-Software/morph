// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <compare>
#include <glaze/glaze.hpp>
#include <optional>
#include <string>
#include <utility>

/// @file
/// PasteId: a hasValue()-capable strong id wrapping the animal-name paste
/// key. Modeled on morph::forms::Ranged's shape
/// (include/morph/forms/widget_hints.hpp) — the closest existing
/// hasValue()-capable newtype template — but wraps std::optional<std::string>,
/// not a bounded arithmetic value, so it carries its own glz::meta rather than
/// reusing Ranged's. First real consumer of the eventual Tagged<T,"Name">
/// gap (docs/findings/009); do not promote this into a generic helper here
/// — the promotion rule (examples/IMPLEMENTATION.md) triggers on a third
/// consumer, not the first.

namespace pastebin {

/// @brief Strong id for a paste (the animal-name key, e.g. "swift-otter").
///
/// Wire form: a plain JSON string (via the `glz::meta` specialisation below),
/// exactly like an unwrapped `std::string` member — see the `glz::meta`
/// specialisation for the exact convention this follows.
struct PasteId {
    /// @brief The payload; `std::nullopt` means "not entered".
    std::optional<std::string> value;

    /// @brief Constructs the empty state.
    constexpr PasteId() noexcept = default;

    /// @brief Engages with @p id.
    explicit PasteId(std::string id) noexcept : value{std::move(id)} {}

    /// @brief Adopts an optional payload as-is.
    ///
    /// A named factory rather than a second same-arity constructor: a
    /// `std::string`-taking constructor and an
    /// `std::optional<std::string>`-taking constructor are both viable,
    /// equal-rank user-defined-conversion candidates for a string literal
    /// (`const char*`) argument, so `PasteId{"swift-otter"}` would be
    /// ambiguous if both were constructors. Keeping only the `std::string`
    /// overload as a constructor avoids that entirely.
    /// @param payload The optional payload to adopt as-is.
    /// @return A `PasteId` wrapping @p payload directly.
    [[nodiscard]] static PasteId fromOptional(std::optional<std::string> payload) noexcept {
        PasteId result;
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
    [[nodiscard]] auto operator<=>(const PasteId&) const noexcept = default;
};

/// @brief Opaque pagination cursor for `ListPastes`.
///
/// Same `hasValue()`-capable opaque-string shape as `PasteId` — a distinct
/// concrete type following the identical pattern (`IMPLEMENTATION.md` rule
/// 3's protocol-scalars row: pagination cursors get a named opaque newtype
/// per role, never a loose `std::string`), not the same helper reused a
/// third time, so the promotion rule does not apply here.
struct PasteCursor {
    /// @brief The payload; `std::nullopt` means "not entered".
    std::optional<std::string> value;

    /// @brief Constructs the empty state.
    constexpr PasteCursor() noexcept = default;

    /// @brief Engages with @p token.
    explicit PasteCursor(std::string token) noexcept : value{std::move(token)} {}

    /// @brief Adopts an optional payload as-is.
    ///
    /// A named factory rather than a second same-arity constructor — see
    /// `PasteId::fromOptional` for why: a `std::string`-taking constructor
    /// and an `std::optional<std::string>`-taking constructor would be
    /// equal-rank candidates for a string literal argument, making
    /// `PasteCursor{"..."}` ambiguous.
    /// @param payload The optional payload to adopt as-is.
    /// @return A `PasteCursor` wrapping @p payload directly.
    [[nodiscard]] static PasteCursor fromOptional(std::optional<std::string> payload) noexcept {
        PasteCursor result;
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
    [[nodiscard]] auto operator<=>(const PasteCursor&) const noexcept = default;
};

/// @brief Trivial, fieldless acknowledgement result for actions with nothing
///        else to return (`DeletePaste`, `ExpirePaste`).
struct Ack {};

}  // namespace pastebin

/// @brief On the wire a PasteId is its nullable underlying string — the
///        strong-typing lives in the C++ type only.
template <>
struct glz::meta<pastebin::PasteId> {
    static constexpr auto value = &pastebin::PasteId::value;
    static constexpr std::string_view name = "PasteId";
};

/// @brief On the wire a PasteCursor is its nullable underlying string — the
///        strong-typing lives in the C++ type only.
template <>
struct glz::meta<pastebin::PasteCursor> {
    static constexpr auto value = &pastebin::PasteCursor::value;
    static constexpr std::string_view name = "PasteCursor";
};
