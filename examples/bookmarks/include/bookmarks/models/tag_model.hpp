// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/core/bridge.hpp>
#include <morph/core/registry.hpp>

#include "bookmarks/core/errors.hpp"
#include "bookmarks/db/db_model.hpp"
#include "bookmarks/dto/tag_dto.hpp"

namespace bookmarks {

/// @brief Rename/merge/list over the `tags` table, scoped to the caller.
///        Registered plain — same rationale as `BookmarkModel`.
class TagModel : private db::WithMapper {
public:
    Ack execute(const RenameTag& action);
    Ack execute(const MergeTags& action);
    ListTagsResult execute(const ListTags& action);
};

}  // namespace bookmarks

BRIDGE_REGISTER_MODEL(bookmarks::TagModel, "TagModel")
BRIDGE_REGISTER_ACTION(bookmarks::TagModel, bookmarks::RenameTag, "RenameTag")
// MergeTags is outbox-managed (this task) -- Loggable::No so the framework
// auto-append never double-logs alongside the model's own outbox write.
BRIDGE_REGISTER_ACTION(bookmarks::TagModel, bookmarks::MergeTags, "MergeTags", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(bookmarks::TagModel, bookmarks::ListTags, "ListTags", ::morph::model::Loggable::No)
