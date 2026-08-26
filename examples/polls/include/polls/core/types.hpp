// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <glaze/glaze.hpp>
#include <string>

#include "errors.hpp"

/// @file
/// Polls' strong id types and constants. `OptionId` and `PollEventId` wrap
/// auto-incrementing integers (SQLite row ids). `PollId` itself is not a
/// strong type (see the rung README's resolved design decision 7, which
/// records that as this rung's unmigrated state rather than a framework
/// restriction), but `kTokenBytes` is shared by
/// implementations and tests to ensure consistency on generated token lengths.
///
/// These two use a **zero sentinel**, not `BookmarkId`'s
/// `std::optional`-backed shape: `value == 0` *is* the "not entered" state.
/// That is deliberate -- neither is ever handed a nullable payload to adopt,
/// and `PollEventId{}` is the natural spelling of "no cursor yet, start from
/// the beginning" for `GetEventsSince` -- but it carries a constraint the
/// type cannot enforce on its own: **an id of `0` is unrepresentable**.
/// Construct one and it reports `hasValue() == false` and behaves as absent
/// everywhere downstream, so a real record would read as "no record"
/// (morph#215).
///
/// The constraint holds because both ids come from SQLite row ids, which
/// start at 1. `fromRowId()` is the enforcement: every conversion from a
/// stored row id goes through it, and it rejects `0` loudly rather than
/// letting it collapse into the empty state. Use it instead of constructing
/// these ids directly from database values.

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

    /// @brief Wraps a stored row id, rejecting the one value this type cannot
    ///        represent.
    ///
    /// `0` is this type's "not entered" sentinel, so an id of `0` would arrive
    /// as *absent* and a real option would read as "no option selected"
    /// (morph#215). SQLite row ids start at 1, so this never fires in
    /// practice -- it exists so that a seeded row, a migrated dataset, an
    /// externally supplied key, or a sequence reset fails loudly at the
    /// boundary instead of collapsing silently one layer below the surface.
    /// @param rowId Stored row id; must be non-zero.
    /// @return An engaged `OptionId` wrapping @p rowId.
    /// @throws PollsError if @p rowId is `0`.
    [[nodiscard]] static OptionId fromRowId(std::int64_t rowId) {
        if (rowId == 0) {
            throw PollsError{
                "OptionId::fromRowId: an option row id of 0 is unrepresentable -- 0 is this "
                "type's \"not entered\" sentinel (morph#215)"};
        }
        return OptionId{.value = rowId};
    }
};

/// @brief Strong identifier for one row in the `poll_events` append-only log.
///        Table-wide monotonic (not per-poll), autoincrement — see the
///        rung README's resolved design decision 4 on why a sequence id,
///        not a timestamp.
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

    /// @brief Wraps a stored row id, rejecting the one value this type cannot
    ///        represent.
    ///
    /// `0` is this type's "not entered" sentinel -- the spelling
    /// `GetEventsSince` uses for "no cursor yet, replay from the beginning" --
    /// so an event row id of `0` would arrive as *absent* and the reader would
    /// silently rewind to the start of the log (morph#215). SQLite row ids
    /// start at 1, so this never fires in practice; it exists so that a
    /// seeded row, a migrated dataset, or a sequence reset fails loudly at the
    /// boundary rather than collapsing silently.
    /// @param rowId Stored row id; must be non-zero.
    /// @return An engaged `PollEventId` wrapping @p rowId.
    /// @throws PollsError if @p rowId is `0`.
    [[nodiscard]] static PollEventId fromRowId(std::int64_t rowId) {
        if (rowId == 0) {
            throw PollsError{
                "PollEventId::fromRowId: an event row id of 0 is unrepresentable -- 0 is this "
                "type's \"not entered\" sentinel (morph#215)"};
        }
        return PollEventId{.value = rowId};
    }
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
