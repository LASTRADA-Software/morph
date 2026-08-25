// SPDX-License-Identifier: Apache-2.0
#include "bookmarks/models/shared_feed_model.hpp"

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <Lightweight/DataMapper/Pool.hpp>
#include <cstdint>
#include <morph/session/session.hpp>
#include <string>
#include <unordered_map>
#include <vector>

#include "bookmarks/db/bookmark_entity.hpp"
#include "bookmarks/db/bookmark_tag_entity.hpp"
#include "bookmarks/db/tag_entity.hpp"
#include "clock.hpp"

namespace bookmarks {

namespace {

[[nodiscard]] ::morph::time::Timestamp fromEpochMs(std::int64_t epochMs) noexcept {
    return ::morph::time::Timestamp{
        ::morph::time::DateTime{std::chrono::sys_time<std::chrono::milliseconds>{std::chrono::milliseconds{epochMs}}}};
}

/// @brief Requires *some* authenticated principal, but never filters on it
///        — this model's whole point is a cross-principal read. See this
///        task's own doc comment for why the check still exists.
void requireAnyPrincipal() {
    const auto* ctx = ::morph::session::current();
    if (ctx == nullptr || ctx->principal.empty()) {
        throw Forbidden{"no authenticated principal"};
    }
}

}  // namespace

ListSharedFeedResult SharedFeedModel::execute(const ListSharedFeed& action) {
    requireAnyPrincipal();
    auto mapper = ::Lightweight::GlobalDataMapperPool().Acquire();
    auto query = mapper->Query<db::BookmarkRecord>();
    (void)query.Where(::Lightweight::FieldNameOf<&db::BookmarkRecord::isShared>, "=", true);
    (void)query.Where(::Lightweight::FieldNameOf<&db::BookmarkRecord::isArchived>, "=", false);
    if (action.cursor.hasValue()) {
        (void)query.Where(::Lightweight::FieldNameOf<&db::BookmarkRecord::id>, "<",
                          static_cast<std::uint64_t>(*action.cursor));
    }
    constexpr std::size_t kPageSize = 20;
    auto rows =
        query
            .OrderBy(::Lightweight::FieldNameOf<&db::BookmarkRecord::id>, ::Lightweight::SqlResultOrdering::DESCENDING)
            .First(kPageSize + 1);
    const bool hasMore = rows.size() > kPageSize;
    if (hasMore) {
        rows.resize(kPageSize);
    }

    // Batched, not per-row: one query for every page bookmark's junction
    // rows, one for every referenced tag's name, grouped in-memory below --
    // instead of a junction query plus one tag query *per junction row*
    // (N+1+M), this page's tag names cost exactly 2 queries regardless of
    // how many bookmarks or tags-per-bookmark it holds.
    std::vector<std::uint64_t> pageIds;
    pageIds.reserve(rows.size());
    for (const auto& rec : rows) {
        pageIds.push_back(rec.id.Value());
    }
    auto junctionRows = mapper->Query<db::BookmarkTagRecord>()
                            .WhereIn(::Lightweight::FieldNameOf<&db::BookmarkTagRecord::bookmark>, pageIds)
                            .All();

    std::vector<std::uint64_t> tagIds;
    tagIds.reserve(junctionRows.size());
    for (const auto& jrow : junctionRows) {
        tagIds.push_back(jrow.tag.Value());
    }
    auto tagRows =
        mapper->Query<db::TagRecord>().WhereIn(::Lightweight::FieldNameOf<&db::TagRecord::id>, tagIds).All();

    std::unordered_map<std::uint64_t, std::string> tagNameById;
    tagNameById.reserve(tagRows.size());
    for (const auto& tagRow : tagRows) {
        tagNameById.emplace(tagRow.id.Value(), std::string{tagRow.name.Value()});
    }

    std::unordered_map<std::uint64_t, std::vector<std::string>> tagsByBookmarkId;
    tagsByBookmarkId.reserve(pageIds.size());
    for (const auto& jrow : junctionRows) {
        if (const auto it = tagNameById.find(jrow.tag.Value()); it != tagNameById.end()) {
            tagsByBookmarkId[jrow.bookmark.Value()].push_back(it->second);
        }
    }

    ListSharedFeedResult result;
    for (const auto& rec : rows) {
        BookmarkSummary summary;
        summary.id = BookmarkId{static_cast<std::int64_t>(rec.id.Value())};
        summary.url = std::string{rec.url.Value()};
        summary.title = std::string{rec.title.Value()};
        summary.tags = std::move(tagsByBookmarkId[rec.id.Value()]);
        summary.createdAt = fromEpochMs(rec.createdAtMs.Value());
        summary.updatedAt = fromEpochMs(rec.updatedAtMs.Value());
        summary.readState = rec.isUnread.Value() ? ReadState::Unread : ReadState::Read;
        summary.archiveState = ArchiveState::Active;  // the query already excludes archived rows
        summary.visibility = Visibility::Shared;      // the query already excludes non-shared rows
        result.bookmarks.push_back(std::move(summary));
    }
    if (hasMore) {
        // Gated on `hasMore` alone, matching `BookmarkModel::execute(const
        // ListBookmarks&)` — see that call site's comment for the argument.
        // The extra `!result.bookmarks.empty()` conjunct this used to carry
        // is redundant here (this loop filters nothing, so `hasMore` already
        // implies a non-empty page) but it is the exact predicate
        // shape that *was* a real bug in the sibling model, and two sibling
        // paginators disagreeing invites re-introducing it.
        result.nextCursor = Cursor{static_cast<std::int64_t>(rows.back().id.Value())};
    }
    return result;
}

}  // namespace bookmarks
