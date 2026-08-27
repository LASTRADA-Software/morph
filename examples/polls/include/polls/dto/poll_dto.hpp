// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <compare>
#include <cstdint>
#include <glaze/glaze.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "polls/core/types.hpp"
#include "polls/units.hpp"

namespace polls {

constexpr std::size_t kMaxTitleBytes = 200;
constexpr std::size_t kMaxOptionLabelBytes = 100;
constexpr std::size_t kMinOptions = 2;
constexpr std::size_t kMaxOptions = 20;

/// @brief One candidate date/time, as free text (Rallly stores these as
///        ISO-ish date strings; this rung follows suit rather than parsing
///        into `morph::time::Timestamp`, since `morph::time` is UTC-only
///        and per-participant local rendering is explicitly GUI logic per
///        the README's "Expected strain points").
struct CreatePollOption {
    std::string label;
};

struct CreatePoll {
    std::string title;
    std::vector<CreatePollOption> options;

    [[nodiscard]] bool validate() const noexcept {
        if (title.empty() || title.size() > kMaxTitleBytes) {
            return false;
        }
        if (options.size() < kMinOptions || options.size() > kMaxOptions) {
            return false;
        }
        for (const auto& opt : options) {
            if (opt.label.empty() || opt.label.size() > kMaxOptionLabelBytes) {
                return false;
            }
        }
        return true;
    }
};

/// @brief Opaque capability-token newtype for the organizer's secret
///        (`examples/IMPLEMENTATION.md` rule 3's protocol-scalars row:
///        capability/confirmation tokens get a named opaque wrapper per
///        role, never a loose `std::string`). Same shape and rationale as
///        `bookmarks::AuthToken` — read that type's doc comment for the
///        `fromOptional`/`hasValue()` factory argument, which applies here
///        verbatim. Distinct from `ParticipantToken` below *by type*, not
///        merely by field name: the two are never interchangeable, and only
///        this one satisfies `PollModel::requireAdmin()`.
struct AdminToken {
    /// @brief The payload; `std::nullopt` means "no token".
    std::optional<std::string> value;

    /// @brief Constructs the empty state.
    constexpr AdminToken() noexcept = default;

    /// @brief Engages with @p token.
    explicit AdminToken(std::string token) noexcept : value{std::move(token)} {}

    /// @brief Adopts an optional payload as-is.
    /// @param payload The optional payload to adopt as-is.
    /// @return An `AdminToken` wrapping @p payload directly.
    [[nodiscard]] static AdminToken fromOptional(std::optional<std::string> payload) noexcept {
        AdminToken result;
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
    [[nodiscard]] auto operator<=>(const AdminToken&) const noexcept = default;
};

/// @brief Opaque capability-token newtype for the secret handed out with the
///        shared link. Same shape as `AdminToken` above and, deliberately, a
///        *different type* from it.
///
/// @warning Generated, stored and returned, but **verified by nothing** in
/// the shipped rung — see `polls/models/poll_model.hpp`'s `@file` comment and
/// the rung README's resolved design decision 1. `pollId` is itself the
/// 128-bit shared secret that gates reaching a poll at all; this token is
/// reserved for a later rung that wants a second, revocable capability level.
struct ParticipantToken {
    /// @brief The payload; `std::nullopt` means "no token".
    std::optional<std::string> value;

    /// @brief Constructs the empty state.
    constexpr ParticipantToken() noexcept = default;

    /// @brief Engages with @p token.
    explicit ParticipantToken(std::string token) noexcept : value{std::move(token)} {}

    /// @brief Adopts an optional payload as-is.
    /// @param payload The optional payload to adopt as-is.
    /// @return A `ParticipantToken` wrapping @p payload directly.
    [[nodiscard]] static ParticipantToken fromOptional(std::optional<std::string> payload) noexcept {
        ParticipantToken result;
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
    [[nodiscard]] auto operator<=>(const ParticipantToken&) const noexcept = default;
};

/// @brief Whether a poll has been finalized. A two-enumerator `enum class`,
///        never a bare `bool`, per `examples/IMPLEMENTATION.md` rule 3 —
///        same convention as `pastebin::Visibility`/`bookmarks::ReadState`
///        on the wire and `PollModel::WriteHistory` internally.
enum class Finalized : std::uint8_t { No, Yes };

struct CreatePollResult {
    std::string pollId;                 // the shareable link id -- see README design decision 7
    AdminToken adminToken;              // kept by the organizer only
    ParticipantToken participantToken;  // handed out with the shared link; verified by nothing today
};

/// @brief The keyed attach action -- `BRIDGE_MODEL_KEY(PollModel, OpenPoll, &OpenPoll::pollId)`.
struct OpenPoll {
    std::string pollId;

    [[nodiscard]] bool validate() const noexcept { return !pollId.empty(); }
};

struct GetPollState {
    [[nodiscard]] bool validate() const noexcept { return true; }
};

struct PollOptionView {
    OptionId id;
    std::string label;
    // Default-initialized to an engaged zero, not Quantity's default empty
    // state -- Quantity arithmetic is empty-propagating (empty + anything =
    // empty forever), which silently broke buildState()'s incremental vote
    // tally until Task 6 caught it. These initializers close that footgun
    // at the type itself, not just at buildState()'s one call site, so a
    // future construction site can't reintroduce the same bug silently.
    Count yesCount = Count::fromDouble(0.0);
    Count ifNeedBeCount = Count::fromDouble(0.0);
    Count noCount = Count::fromDouble(0.0);
};

struct ParticipantVoteView {
    std::string participantName;
    OptionId optionId;
    VoteChoice choice;
};

struct CommentView {
    std::string participantName;
    std::string body;
};

struct GetPollStateResult {
    std::string pollId;
    std::string title;
    Finalized finalized{Finalized::No};
    OptionId finalizedOptionId;  // hasValue() == false unless finalized == Finalized::Yes
    std::vector<PollOptionView> options;
    std::vector<ParticipantVoteView> votes;
    std::vector<CommentView> comments;
    PollEventId lastEventId;  // GetEventsSince's starting cursor for a fresh client
};

}  // namespace polls

/// @brief Reflects `AdminToken` as its bare payload — same rationale and
///        shape as `glz::meta<bookmarks::AuthToken>`: the wire form of an
///        opaque scalar newtype is the scalar, not an object with a `value`
///        member.
template <>
struct glz::meta<polls::AdminToken> {
    static constexpr auto value = &polls::AdminToken::value;
    static constexpr std::string_view name = "AdminToken";
};

/// @brief Reflects `ParticipantToken` as its bare payload — see
///        `glz::meta<polls::AdminToken>` above.
template <>
struct glz::meta<polls::ParticipantToken> {
    static constexpr auto value = &polls::ParticipantToken::value;
    static constexpr std::string_view name = "ParticipantToken";
};

/// @brief Reflects `Finalized` as the strings `"No"`/`"Yes"` rather than its
///        underlying `0`/`1` — same rationale and `glz::enumerate` shape as
///        `glz::meta<pastebin::Visibility>` (a bare ordinal also degrades the
///        schema writer's `$defs` entry to an any-type union). Persistence is
///        unaffected: the `polls` table stores this as its own `finalized`
///        boolean column (`db/poll_entity.hpp`), never as this JSON form.
template <>
struct glz::meta<polls::Finalized> {
    using enum polls::Finalized;
    static constexpr auto value = glz::enumerate(No, Yes);
};
