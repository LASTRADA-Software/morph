// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "bookmarks/core/types.hpp"
#include "bookmarks/units.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace bookmarks {

/// @brief Longest tag name, in bytes, this rung accepts — a `validate()`
///        sanity bound only, not a storage-column width. See this task's
///        own header comment for why `TagRecord::name` carries no
///        `SqlAnsiString` capacity to check against.
inline constexpr std::size_t kMaxTagNameBytes = 128;

struct RenameTag {
    TagId id;
    std::string name;

    [[nodiscard]] bool validate() const noexcept {
        return id.hasValue() && !name.empty() && name.size() <= kMaxTagNameBytes;
    }
};

/// @brief Reassigns every bookmark tagged `sourceId` to `targetId`
///        (deduplicating), then deletes `sourceId` — `TagModel::execute`
///        (Task 9) does the cascade; this DTO only carries the two ids.
struct MergeTags {
    TagId sourceId;
    TagId targetId;

    [[nodiscard]] bool validate() const noexcept {
        return sourceId.hasValue() && targetId.hasValue() && *sourceId != *targetId;
    }
};

struct ListTags {
    [[nodiscard]] bool validate() const noexcept { return true; }
};

struct TagSummary {
    TagId id;
    std::string name;
    Count bookmarkCount;
};

struct ListTagsResult {
    std::vector<TagSummary> tags;
};

}  // namespace bookmarks
