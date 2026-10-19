// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/core/bridge.hpp>
#include <morph/core/effects.hpp>
#include <morph/core/registry.hpp>

#include "bookmarks/core/errors.hpp"
#include "bookmarks/dto/bookmark_dto.hpp"
#include "bookmarks/dto/tag_dto.hpp"

namespace bookmarks {

/// @brief Rename/merge/list over the `tags` table, scoped to the caller.
///        Registered plain — same rationale as `BookmarkModel`.
///
/// Holds no database state itself: each `execute()` acquires a
/// `Lightweight::GlobalDataMapperPool()` connection for its own duration and
/// returns it before returning, rather than owning a connection for its own
/// lifetime.
class TagModel {
public:
    Ack execute(const RenameTag& action);
    Ack execute(const MergeTags& action);
    ListTagsResult execute(const ListTags& action);
};

}  // namespace bookmarks

BRIDGE_REGISTER_MODEL(bookmarks::TagModel, "TagModel")
// Renaming or merging a tag cascades to every bookmark carrying it, so the bookmarks change too.
BRIDGE_REGISTER_ACTION(bookmarks::TagModel, bookmarks::RenameTag, "RenameTag")
BRIDGE_REGISTER_EFFECTS(bookmarks::RenameTag, ::morph::effects::changes<bookmarks::TagSummary>,
                        ::morph::effects::changes<bookmarks::BookmarkSummary>,
                        ::morph::effects::changes<bookmarks::BookmarkView>)
// MergeTags is outbox-managed (this task) -- Loggable::No so the framework
// auto-append never double-logs alongside the model's own outbox write.
BRIDGE_REGISTER_ACTION(bookmarks::TagModel, bookmarks::MergeTags, "MergeTags", ::morph::model::Loggable::No)
BRIDGE_REGISTER_EFFECTS(bookmarks::MergeTags, ::morph::effects::changes<bookmarks::TagSummary>,
                        ::morph::effects::changes<bookmarks::BookmarkSummary>,
                        ::morph::effects::changes<bookmarks::BookmarkView>)
BRIDGE_REGISTER_ACTION(bookmarks::TagModel, bookmarks::ListTags, "ListTags", ::morph::model::Loggable::No)
