// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/core/bridge.hpp>
#include <morph/core/registry.hpp>

#include "bookmarks/core/errors.hpp"
#include "bookmarks/dto/shared_feed_dto.hpp"

namespace bookmarks {

/// @brief The one cross-principal read in this rung: every `Shared`,
///        non-archived bookmark, from every owner. Registered plain — see
///        this task's own header comment for why `AllowShared` is not used.
///
/// Holds no database state itself: `execute()` acquires a
/// `Lightweight::GlobalDataMapperPool()` connection for its own duration and
/// returns it before returning, rather than owning a connection for its own
/// lifetime.
class SharedFeedModel {
public:
    ListSharedFeedResult execute(const ListSharedFeed& action);
};

}  // namespace bookmarks

BRIDGE_REGISTER_MODEL(bookmarks::SharedFeedModel, "SharedFeedModel")
BRIDGE_REGISTER_ACTION(bookmarks::SharedFeedModel, bookmarks::ListSharedFeed, "ListSharedFeed",
                       ::morph::model::Loggable::No)
