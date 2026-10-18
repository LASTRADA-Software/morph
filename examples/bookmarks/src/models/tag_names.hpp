// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <cstdint>
#include <morph/core/profiler.hpp>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "bookmarks/db/bookmark_tag_entity.hpp"
#include "bookmarks/db/tag_entity.hpp"

namespace bookmarks::detail {

/// @brief The tag names of every bookmark in @p bookmarkIds, in two queries
///        however many bookmarks and tags there are: one for the junction
///        rows of all of them, one for the names of every tag those rows
///        reference. A per-bookmark lookup costs a junction query plus a
///        query per tag, which on a 20-row listing page is about sixty.
///
/// Does no ownership check: the caller has already established that every
/// bookmark in @p bookmarkIds is readable, and junction rows for one bookmark
/// are owner-homogeneous.
/// @param mapper      The caller's data mapper.
/// @param bookmarkIds The bookmarks to look up; may be empty.
/// @return Each bookmark's tag names, keyed by bookmark id; a bookmark with no
///         tags has no entry.
[[nodiscard]] inline std::unordered_map<std::uint64_t, std::vector<std::string>> readTagNamesByBookmark(
    ::Lightweight::DataMapper& mapper, std::span<const std::uint64_t> bookmarkIds) {
    MORPH_ZONE("bookmarks::readTagNames");
    std::unordered_map<std::uint64_t, std::vector<std::string>> tagsByBookmarkId;
    if (bookmarkIds.empty()) {
        return tagsByBookmarkId;
    }
    const std::vector<std::uint64_t> ids{bookmarkIds.begin(), bookmarkIds.end()};
    auto junctionRows = mapper.Query<db::BookmarkTagRecord>()
                            .WhereIn(::Lightweight::FieldNameOf<&db::BookmarkTagRecord::bookmark>, ids)
                            .All();
    if (junctionRows.empty()) {
        return tagsByBookmarkId;
    }

    std::vector<std::uint64_t> tagIds;
    tagIds.reserve(junctionRows.size());
    for (const auto& row : junctionRows) {
        tagIds.push_back(row.tag.Value());
    }
    auto tagRows = mapper.Query<db::TagRecord>().WhereIn(::Lightweight::FieldNameOf<&db::TagRecord::id>, tagIds).All();

    std::unordered_map<std::uint64_t, std::string> tagNameById;
    tagNameById.reserve(tagRows.size());
    for (const auto& tagRow : tagRows) {
        // `TagRecord::name` is a bounded `SqlAnsiString`; tag names travel the
        // wire as `std::string`, so the conversion happens here, at the
        // model-layer boundary.
        tagNameById.emplace(tagRow.id.Value(), std::string{tagRow.name.Value()});
    }
    tagsByBookmarkId.reserve(ids.size());
    for (const auto& row : junctionRows) {
        if (const auto it = tagNameById.find(row.tag.Value()); it != tagNameById.end()) {
            tagsByBookmarkId[row.bookmark.Value()].push_back(it->second);
        }
    }
    return tagsByBookmarkId;
}

}  // namespace bookmarks::detail
