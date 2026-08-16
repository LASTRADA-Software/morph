// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <glaze/glaze.hpp>
#include <string>

/// @file
/// Polls' strong id types and constants. `OptionId` and `PollEventId` wrap
/// auto-incrementing integers (SQLite row ids), following `BookmarkId`'s
/// pattern. `PollId` itself is not a strong type (see Global Constraints),
/// but `kTokenBytes` is shared by implementations and tests to ensure
/// consistency on generated token lengths.

namespace polls {

/// @brief Length in bytes of a generated `pollId`/admin-token/participant-token
///        string: 22 URL-safe base64 characters encoding 16 random bytes,
///        matching a nanoid-shaped unguessable identifier. Shared by
///        `CreatePoll`'s implementation (Task 5) and its tests so the two
///        never drift.
inline constexpr std::size_t kTokenBytes = 22;

/// @brief Strong identifier for one candidate date/time option within a poll.
///        Never the target of a `BRIDGE_MODEL_KEY`/`BRIDGE_KEY_FROM` macro —
///        `PollModel` is keyed by `pollId` alone (see `OpenPoll` in
///        `dto/poll_dto.hpp`), so this stays an ordinary strong type per
///        `IMPLEMENTATION.md` rule 3.
struct OptionId {
    /// @brief The payload; `0` means "not entered" (analogous to empty optional).
    std::int64_t value{0};

    /// @brief Whether a value has been entered.
    /// @return `true` if the payload is non-zero.
    [[nodiscard]] constexpr bool hasValue() const { return value != 0; }

    /// @brief Returns the payload as-is. Never UB, unlike
    ///        `std::optional::operator*` -- `value` is a plain
    ///        `std::int64_t` with `0` as its own "not entered" sentinel, not
    ///        a `std::optional` this wraps, so there is no engaged/empty
    ///        state distinction below the surface for this to violate. Check
    ///        `hasValue()` first when `0` vs. a real id matters to the
    ///        caller; this always returns whatever `value` holds either way.
    /// @return The payload, verbatim.
    [[nodiscard]] constexpr std::int64_t operator*() const { return value; }

    /// @brief Equality on the payload.
    [[nodiscard]] constexpr bool operator==(const OptionId&) const = default;
};

/// @brief Strong identifier for one row in the `poll_events` append-only log.
///        Table-wide monotonic (not per-poll), autoincrement — see this
///        plan's Global Constraints on why a sequence id, not a timestamp.
struct PollEventId {
    /// @brief The payload; `0` means "not entered" (analogous to empty optional).
    std::int64_t value{0};

    /// @brief Whether a value has been entered.
    /// @return `true` if the payload is non-zero.
    [[nodiscard]] constexpr bool hasValue() const { return value != 0; }

    /// @brief Returns the payload as-is. Never UB, unlike
    ///        `std::optional::operator*` -- `value` is a plain
    ///        `std::int64_t` with `0` as its own "not entered" sentinel, not
    ///        a `std::optional` this wraps, so there is no engaged/empty
    ///        state distinction below the surface for this to violate. Check
    ///        `hasValue()` first when `0` vs. a real id matters to the
    ///        caller; this always returns whatever `value` holds either way.
    /// @return The payload, verbatim.
    [[nodiscard]] constexpr std::int64_t operator*() const { return value; }

    /// @brief Equality on the payload.
    [[nodiscard]] constexpr bool operator==(const PollEventId&) const = default;
};

/// @brief One participant's answer for one option.
enum class VoteChoice { Yes, IfNeedBe, No };

}  // namespace polls

/// @brief On the wire an `OptionId` is its underlying integer.
template <>
struct glz::meta<polls::OptionId> {
    static constexpr auto value = &polls::OptionId::value;
    static constexpr std::string_view name = "OptionId";
};

/// @brief On the wire a `PollEventId` is its underlying integer.
template <>
struct glz::meta<polls::PollEventId> {
    static constexpr auto value = &polls::PollEventId::value;
    static constexpr std::string_view name = "PollEventId";
};

/// @brief Reflects `VoteChoice` as its enumerator names rather than a bare
///        ordinal -- same rationale and `glz::enumerate` shape as
///        `glz::meta<polls::Finalized>` (`dto/poll_dto.hpp`): a raw integer
///        both degrades the schema writer's `$defs` entry to an any-type
///        union and accepts any out-of-range value silently instead of
///        rejecting it during decode. Persistence is unaffected: `votes`
///        stores this as its own `choice` `std::uint8_t` column
///        (`db/poll_entity.hpp`), never as this JSON form.
template <>
struct glz::meta<polls::VoteChoice> {
    using enum polls::VoteChoice;
    static constexpr auto value = glz::enumerate(Yes, IfNeedBe, No);
};
