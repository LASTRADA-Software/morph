// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <string>
#include <vector>

#include "polls/core/types.hpp"

namespace polls {

/// @brief One row of `poll_events` -- the Zulip-pattern generic polling
///        payload. `kind` is a small closed set (`"vote"`, `"comment"`,
///        `"finalize"`) a client switches on to know how to apply the
///        increment without re-fetching `GetPollState`.
struct PollEvent {
    PollEventId id;
    std::string kind;
    std::string summary;  // human-readable, e.g. "alice voted", "poll finalized"
};

struct GetEventsSince {
    PollEventId lastEventId;  // {} (value 0) means "from the beginning"

    // A negative value static_cast<uint64_t>'s to a huge number in
    // execute(GetEventsSince)'s `id > lastEventId` comparison (poll_model.cpp),
    // silently matching zero rows instead of erroring -- indistinguishable
    // from a genuinely idle poll, so a poller with a corrupted cursor would
    // believe the poll is idle rather than desyncing loudly. PollEventId's
    // own wire encoding is its bare (signed) int64 payload
    // (glz::meta<polls::PollEventId>, core/types.hpp), so a negative value is
    // genuinely reachable from a malformed or malicious client, not merely a
    // local invariant this type already enforces.
    [[nodiscard]] bool validate() const noexcept { return lastEventId.value >= 0; }
};

struct GetEventsSinceResult {
    std::vector<PollEvent> events;  // oldest first, every id > lastEventId
};

}  // namespace polls
