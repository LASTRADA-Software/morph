// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>
#include <vector>

#include "kanban/core/types.hpp"

namespace kanban {

struct BoardEvent {
    BoardEventId id;
    std::string kind;
    std::string summary;
};

/// @brief Lists every event after `lastEventId`, oldest first -- design
///        spec §1's "GetEventsSince is a real table" decision.
///        `lastEventId == BoardEventId{}` (its default) means "from the
///        beginning": `board_events.id` is a `ServerSideAutoIncrement`
///        primary key starting at 1, so `id > 0` already matches every row.
struct GetEventsSince {
    BoardEventId lastEventId;

    // A negative value static_cast<uint64_t>'s to a huge number in the
    // `id > lastEventId` comparison, silently matching zero rows instead of
    // erroring -- see polls::GetEventsSince's identical guard and comment.
    [[nodiscard]] bool validate() const noexcept { return lastEventId.value >= 0; }
};

struct GetEventsSinceResult {
    std::vector<BoardEvent> events;
};

}  // namespace kanban
