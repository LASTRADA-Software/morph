// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "polls/core/types.hpp"

#include <cstdint>
#include <glaze/glaze.hpp>
#include <string>
#include <vector>

namespace polls {

constexpr std::size_t kMaxParticipantNameBytes = 80;
constexpr std::size_t kMaxCommentBytes = 500;

struct OneVote {
    OptionId optionId;
    VoteChoice choice;
};

/// @brief Whether @p votes names the same `optionId` more than once.
///
/// Without this check, two entries for the same option collide with
/// `idx_votes_poll_participant_option`'s unique index and throw a raw,
/// unhandled SQL constraint-violation exception instead of the typed
/// `ValidationError` every other bad-input path in this model produces.
/// @param votes The vote list to check.
/// @return `true` if any `optionId` repeats.
[[nodiscard]] inline bool hasDuplicateOptionId(const std::vector<OneVote>& votes) {
    for (std::size_t i = 0; i < votes.size(); ++i) {
        for (std::size_t j = i + 1; j < votes.size(); ++j) {
            if (votes[i].optionId == votes[j].optionId) {
                return true;
            }
        }
    }
    return false;
}

/// @brief First-time vote submission for one participant. Idempotent on
///        retry: a duplicate submission with the same participantName is
///        rejected by the option-uniqueness invariant (Task 6), never
///        double-counted.
struct SubmitVotes {
    std::string participantName;
    std::vector<OneVote> votes;

    [[nodiscard]] bool validate() const noexcept {
        return !participantName.empty() && participantName.size() <= kMaxParticipantNameBytes && !votes.empty() &&
               !hasDuplicateOptionId(votes);
    }
};

/// @brief Replaces an existing participant's votes wholesale.
struct UpdateVotes {
    std::string participantName;
    std::vector<OneVote> votes;

    [[nodiscard]] bool validate() const noexcept {
        return !participantName.empty() && participantName.size() <= kMaxParticipantNameBytes && !votes.empty() &&
               !hasDuplicateOptionId(votes);
    }
};

struct AddComment {
    std::string participantName;
    std::string body;

    [[nodiscard]] bool validate() const noexcept {
        return !participantName.empty() && participantName.size() <= kMaxParticipantNameBytes && !body.empty() &&
               body.size() <= kMaxCommentBytes;
    }
};

/// @brief Admin-token-gated: the poll becomes read-only.
struct FinalizePoll {
    OptionId optionId;

    [[nodiscard]] bool validate() const noexcept { return optionId.hasValue(); }
};

/// @brief Reverses the calling participant's own most recent vote change --
///        a compensating action against `vote_history`, never
///        `SessionLog::undoLast()`. See the README's resolved design
///        decision 3.
struct UndoLastVoteChange {
    std::string participantName;

    [[nodiscard]] bool validate() const noexcept {
        return !participantName.empty() && participantName.size() <= kMaxParticipantNameBytes;
    }
};

/// @brief Whether an undo actually put a prior vote set back. A
///        two-enumerator `enum class`, never a bare `bool`, per
///        `examples/IMPLEMENTATION.md` rule 3 — same convention as
///        `polls::Finalized` (`dto/poll_dto.hpp`) and
///        `PollModel::WriteHistory`.
enum class Restored : std::uint8_t { No, Yes };

struct UndoLastVoteChangeResult {
    // Restored::No is unreachable in practice: there being nothing to undo
    // throws Conflict instead of returning it (see Task 8). It exists so the
    // field has a meaningful default rather than a fabricated success value.
    Restored restored{Restored::No};
};

}  // namespace polls

/// @brief Reflects `Restored` as the strings `"No"`/`"Yes"` rather than its
///        underlying `0`/`1` — see `glz::meta<polls::Finalized>`
///        (`dto/poll_dto.hpp`) for the full rationale.
template <>
struct glz::meta<polls::Restored> {
    using enum polls::Restored;
    static constexpr auto value = glz::enumerate(No, Yes);
};
