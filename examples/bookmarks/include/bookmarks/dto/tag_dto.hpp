// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "bookmarks/core/types.hpp"
#include "bookmarks/units.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace bookmarks {

/// @brief Longest tag name, in bytes, this rung accepts — both a
///        `validate()` sanity bound and the storage-column width:
///        `TagRecord::name` (`tag_entity.hpp`) is `Light::SqlAnsiString<128>`,
///        and `tag_model.cpp`'s static_assert pins that capacity to this
///        same constant, so the two can never drift apart silently.
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
