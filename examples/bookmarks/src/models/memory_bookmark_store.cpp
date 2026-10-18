// SPDX-License-Identifier: Apache-2.0
#include "memory_bookmark_store.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <morph/core/profiler.hpp>
#include <morph/session/session.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "bookmarks/core/errors.hpp"
#include "clock.hpp"

namespace bookmarks::detail {

namespace {

[[nodiscard]] std::int64_t nowMs() noexcept {
    return (*::morph::ladder::now().value).value.time_since_epoch().count();
}

[[nodiscard]] ::morph::time::Timestamp fromEpochMs(std::int64_t epochMs) noexcept {
    return ::morph::time::Timestamp{
        ::morph::time::DateTime{std::chrono::sys_time<std::chrono::milliseconds>{std::chrono::milliseconds{epochMs}}}};
}

/// Ids are unique across every instance in the process, as database ids are,
/// so one client never sees the same id for two different bookmarks even
/// across its sessions.
[[nodiscard]] std::int64_t nextId() noexcept {
    static std::atomic<std::int64_t> counter{0};
    return counter.fetch_add(1, std::memory_order_relaxed) + 1;
}

/// The names in @p names, first occurrence kept: a bookmark's tag set has no
/// duplicates, whatever the client sent.
[[nodiscard]] std::vector<std::string> uniqueTags(const std::vector<std::string>& names) {
    std::vector<std::string> unique;
    unique.reserve(names.size());
    for (const auto& name : names) {
        if (std::ranges::find(unique, name) == unique.end()) {
            unique.push_back(name);
        }
    }
    return unique;
}

[[nodiscard]] BookmarkSummary toSummary(const BookmarkView& view) {
    BookmarkSummary summary;
    summary.id = view.id;
    summary.url = view.url;
    summary.title = view.title;
    summary.tags = view.tags;
    summary.createdAt = view.createdAt;
    summary.updatedAt = view.updatedAt;
    summary.readState = view.readState;
    summary.archiveState = view.archiveState;
    summary.visibility = view.visibility;
    return summary;
}

}  // namespace

void MemoryBookmarkStore::requireOwner() {
    const auto* ctx = ::morph::session::current();
    if (ctx == nullptr || ctx->principal.empty()) {
        throw Forbidden{"no authenticated principal"};
    }
    if (!_owner) {
        _owner = ctx->principal;
    } else if (*_owner != ctx->principal) {
        throw Forbidden{"bookmark belongs to a different principal"};
    }
}

MemoryBookmarkStore::Entry& MemoryBookmarkStore::find(const BookmarkId& id) {
    auto it = _bookmarks.find(*id);
    if (it == _bookmarks.end()) {
        throw NotFound{"no such bookmark"};
    }
    return it->second;
}

/// Stamps a bookmark as changed now, on the clock `GetChangesSince` reads.
static void touch(BookmarkView& view, std::int64_t& updatedAtMs) {
    updatedAtMs = nowMs();
    view.updatedAt = fromEpochMs(updatedAtMs);
}

CreateBookmarkResult MemoryBookmarkStore::execute(const CreateBookmark& action) {
    MORPH_ZONE("MemoryBookmarkStore::CreateBookmark");
    if (!action.validate()) {
        throw ValidationError{"CreateBookmark: a non-empty url within the length bound is required"};
    }
    requireOwner();
    const auto id = nextId();
    Entry entry;
    entry.updatedAtMs = nowMs();
    entry.view.id = BookmarkId{id};
    entry.view.url = action.url;
    entry.view.title = action.title;
    entry.view.description = action.description;
    entry.view.notes = action.notes;
    entry.view.tags = uniqueTags(action.tags);
    entry.view.createdAt = fromEpochMs(entry.updatedAtMs);
    entry.view.updatedAt = entry.view.createdAt;
    entry.view.visibility = action.visibility;
    _bookmarks.emplace(id, std::move(entry));
    return CreateBookmarkResult{.id = BookmarkId{id}};
}

BookmarkView MemoryBookmarkStore::execute(const EditBookmark& action) {
    MORPH_ZONE("MemoryBookmarkStore::EditBookmark");
    if (!action.validate()) {
        throw ValidationError{"EditBookmark: id and a non-empty url within the length bound are required"};
    }
    requireOwner();
    auto& entry = find(action.id);
    entry.view.url = action.url;
    entry.view.title = action.title;
    entry.view.description = action.description;
    entry.view.notes = action.notes;
    entry.view.tags = uniqueTags(action.tags);
    entry.view.visibility = action.visibility;
    touch(entry.view, entry.updatedAtMs);
    return entry.view;
}

Ack MemoryBookmarkStore::execute(const ArchiveBookmark& action) {
    MORPH_ZONE("MemoryBookmarkStore::ArchiveBookmark");
    if (!action.validate()) {
        throw ValidationError{"ArchiveBookmark: id is required"};
    }
    requireOwner();
    auto& entry = find(action.id);
    entry.view.archiveState = ArchiveState::Archived;
    touch(entry.view, entry.updatedAtMs);
    return Ack{};
}

Ack MemoryBookmarkStore::execute(const UnarchiveBookmark& action) {
    MORPH_ZONE("MemoryBookmarkStore::UnarchiveBookmark");
    if (!action.validate()) {
        throw ValidationError{"UnarchiveBookmark: id is required"};
    }
    requireOwner();
    auto& entry = find(action.id);
    entry.view.archiveState = ArchiveState::Active;
    touch(entry.view, entry.updatedAtMs);
    return Ack{};
}

Ack MemoryBookmarkStore::execute(const DeleteBookmark& action) {
    MORPH_ZONE("MemoryBookmarkStore::DeleteBookmark");
    if (!action.validate()) {
        throw ValidationError{"DeleteBookmark: id is required"};
    }
    requireOwner();
    (void)find(action.id);
    _bookmarks.erase(*action.id);
    return Ack{};
}

BookmarkView MemoryBookmarkStore::execute(const GetBookmark& action) {
    MORPH_ZONE("MemoryBookmarkStore::GetBookmark");
    if (!action.validate()) {
        throw ValidationError{"GetBookmark: id is required"};
    }
    requireOwner();
    return find(action.id).view;
}

ListBookmarksResult MemoryBookmarkStore::execute(const ListBookmarks& action) {
    MORPH_ZONE("MemoryBookmarkStore::ListBookmarks");
    requireOwner();
    // The same page as BookmarkModel's: up to 20 rows newest-first past the
    // cursor, selected by the archive and read filters; the tag and text
    // filters then run over that page, not over the whole collection.
    constexpr std::size_t kPageSize = 20;
    const auto matches = [&action](const BookmarkView& view) {
        if ((action.archiveFilter == ArchiveFilter::ActiveOnly && view.archiveState != ArchiveState::Active) ||
            (action.archiveFilter == ArchiveFilter::ArchivedOnly && view.archiveState != ArchiveState::Archived)) {
            return false;
        }
        return !((action.readFilter == ReadFilter::UnreadOnly && view.readState != ReadState::Unread) ||
                 (action.readFilter == ReadFilter::ReadOnly && view.readState != ReadState::Read));
    };
    auto& bookmarks = _bookmarks;
    auto it = action.cursor.hasValue() ? std::make_reverse_iterator(bookmarks.lower_bound(*action.cursor))
                                       : bookmarks.rbegin();
    std::vector<const BookmarkView*> page;
    bool hasMore = false;
    for (; it != bookmarks.rend(); ++it) {
        if (!matches(it->second.view)) {
            continue;
        }
        if (page.size() == kPageSize) {
            hasMore = true;
            break;
        }
        page.push_back(&it->second.view);
    }

    ListBookmarksResult result;
    for (const auto* view : page) {
        if (!action.tag.empty() && std::ranges::find(view->tags, action.tag) == view->tags.end()) {
            continue;
        }
        if (!action.searchText.empty() && view->title.find(action.searchText) == std::string::npos &&
            view->url.find(action.searchText) == std::string::npos) {
            continue;
        }
        result.bookmarks.push_back(toSummary(*view));
    }
    if (hasMore && !page.empty()) {
        result.nextCursor = Cursor{*page.back()->id};
    }
    return result;
}

GetChangesSinceResult MemoryBookmarkStore::execute(const GetChangesSince& action) {
    MORPH_ZONE("MemoryBookmarkStore::GetChangesSince");
    requireOwner();
    const auto asOf = nowMs();
    const std::int64_t sinceMs =
        action.since.timestampMs.hasValue() ? (*action.since.timestampMs).value.time_since_epoch().count() : 0;
    const std::int64_t sinceLastId = action.since.lastId.value_or(0);

    // Rows strictly after the cursor's instant, or at it with an id past the
    // last one delivered there, in (updatedAt, id) order: BookmarkModel's
    // cursor contract, so a client cannot tell the two models apart.
    std::vector<const Entry*> changed;
    for (const auto& [id, entry] : _bookmarks) {
        if (entry.updatedAtMs > sinceMs || (entry.updatedAtMs == sinceMs && id > sinceLastId)) {
            changed.push_back(&entry);
        }
    }
    std::ranges::sort(changed, [](const Entry* lhs, const Entry* rhs) {
        return std::pair{lhs->updatedAtMs, *lhs->view.id} < std::pair{rhs->updatedAtMs, *rhs->view.id};
    });

    GetChangesSinceResult result;
    result.asOf.timestampMs = fromEpochMs(asOf);
    for (auto it = changed.rbegin(); it != changed.rend(); ++it) {
        if ((*it)->updatedAtMs == asOf) {
            result.asOf.lastId = *(*it)->view.id;
            break;
        }
    }
    result.changed.reserve(changed.size());
    for (const auto* entry : changed) {
        result.changed.push_back(toSummary(entry->view));
    }
    return result;
}

}  // namespace bookmarks::detail
