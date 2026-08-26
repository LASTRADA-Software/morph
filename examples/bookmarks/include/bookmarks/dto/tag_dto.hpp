// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "bookmarks/core/types.hpp"
#include "bookmarks/units.hpp"

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

    /// @brief Same opt-out, same reason as `CreateBookmark::explicitSubmit` —
    ///        a rename mutates a row, so it must not fire per keystroke. Here
    ///        every intermediate name a user types through would be applied
    ///        (and the last one that happened to collide would be reported as
    ///        the `Conflict`).
    static constexpr bool explicitSubmit = true;

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

    /// @brief Same opt-out, same reason as `CreateBookmark::explicitSubmit`.
    ///        A merge is destructive — it reassigns every junction row and
    ///        then deletes `sourceId` — so firing it on a digit typed midway
    ///        through an id would destroy the wrong tag.
    static constexpr bool explicitSubmit = true;

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
