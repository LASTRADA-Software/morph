// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace kanban {

/// @brief One journal-derived activity entry for `GetActivity` (design spec
///        §4). Mapped from a `morph::journal::LogEntry`, not a parallel
///        `board_events`-style table row.
struct ActivityEvent {
    /// @brief Its position in the board's journal, from 0: the journal is append-only and read oldest first, so this
    ///        names the event for as long as the journal lasts.
    std::int64_t sequence = 0;
    std::string actionType;
    std::string principal;
    std::int64_t timestampMs = 0;
    std::string summary;
};

/// @brief Lists journal entries recorded for this handler's attached board.
struct GetActivity {
    [[nodiscard]] bool validate() const noexcept { return true; }
};

/// @brief `GetActivity`'s result: every collapsed activity entry, oldest first.
struct GetActivityResult {
    std::vector<ActivityEvent> events;
};

}  // namespace kanban
