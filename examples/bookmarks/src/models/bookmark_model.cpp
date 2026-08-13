// SPDX-License-Identifier: Apache-2.0
#include "bookmarks/models/bookmark_model.hpp"

#include "bookmarks/auth/bookmarks_authorizer.hpp"
#include "bookmarks/db/bookmark_entity.hpp"
#include "bookmarks/db/bookmark_tag_entity.hpp"
#include "bookmarks/db/imported_op_entity.hpp"
#include "bookmarks/db/outbox_entity.hpp"
#include "bookmarks/db/tag_entity.hpp"
#include "bookmarks/import/netscape_bookmarks.hpp"

#include "clock.hpp"

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <Lightweight/SqlError.hpp>
#include <Lightweight/SqlErrorDetection.hpp>
#include <Lightweight/SqlStatement.hpp>
#include <Lightweight/SqlTransaction.hpp>

#include <morph/core/registry.hpp>
#include <morph/session/session.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace bookmarks {

namespace {

[[nodiscard]] std::int64_t nowMs() noexcept {
    return (*::morph::ladder::now().value).value.time_since_epoch().count();
}

/// @brief Process-wide monotonic counter, used only to disambiguate
///        `BulkEdit`'s server-generated idempotency key (see its call site)
///        when two calls land in the same `nowMs()` millisecond --
///        `morph::ladder::now()` has millisecond resolution (there is no
///        higher-resolution variant), so the timestamp alone cannot be
///        trusted to be unique across rapid back-to-back calls from the same
///        principal. `std::atomic` (not `thread_local`) because the model
///        instance is shared across whichever thread each dispatched call
///        lands on.
[[nodiscard]] std::uint64_t nextOutboxSeq() noexcept {
    static std::atomic<std::uint64_t> counter{0};
    return counter.fetch_add(1, std::memory_order_relaxed);
}

[[nodiscard]] ::morph::time::Timestamp fromEpochMs(std::int64_t epochMs) noexcept {
    return ::morph::time::Timestamp{::morph::time::DateTime{
        std::chrono::sys_time<std::chrono::milliseconds>{std::chrono::milliseconds{epochMs}}}};
}

/// @brief The authenticated caller's principal, or throws `Forbidden`.
///
/// `session::current()` is populated fresh on every dispatched action
/// (`session::detail::ScopedContext`, installed by `RemoteServer`/
/// `LocalBackend` around each `execute()`); reading it here rather than
/// once at construction is what lets a single plain-registered
/// `BookmarkModel` instance serve whichever principal's call actually
/// reaches it -- there is exactly one instance per registration, so in
/// practice this is stable across a registration's whole lifetime, but the
/// model never assumes that, matching rule 1's "models re-check their own
/// authorization" requirement. `nullptr`/empty is treated identically to an
/// unauthenticated caller: `Forbidden`, not a crash -- reachable from a
/// test that calls `execute()` directly with no session installed, and
/// (defensively) from a local backend, which installs a `Context` but
/// never verifies it.
[[nodiscard]] const std::string& requireOwner() {
    const auto* ctx = ::morph::session::current();
    if (ctx == nullptr || ctx->principal.empty()) {
        throw Forbidden{"no authenticated principal"};
    }
    return ctx->principal;
}

/// @brief Finds @p owner's tag named @p name, creating it if it does not
///        exist yet. Shared by `applyTagSet` (Task 6) and `BulkEdit`
///        (this task) — both run inside the caller's own transaction.
[[nodiscard]] std::uint64_t findOrCreateTagId(::Lightweight::DataMapper& mapper, const std::string& owner,
                                              const std::string& name) {
    auto existing = mapper.Query<db::TagRecord>()
                        .Where(::Lightweight::FieldNameOf<&db::TagRecord::ownerPrincipal>, "=", owner)
                        .Where(::Lightweight::FieldNameOf<&db::TagRecord::name>, "=", name)
                        .All();
    if (!existing.empty()) {
        return existing.front().id.Value();
    }
    db::TagRecord tag;
    tag.ownerPrincipal = owner;
    tag.name = name;
    mapper.Create(tag);
    return tag.id.Value();
}

/// @brief Adds a bookmark<->tag association if it does not already exist —
///        the junction table's unique index (`idx_bookmark_tags_pair`)
///        makes a duplicate a no-op to *detect*, but this checks first
///        rather than relying on catching the constraint violation, so a
///        `BulkEdit`'s per-item loop never has to distinguish "this item's
///        add was a genuine no-op" from "this item hit an unrelated store
///        error" via exception type alone.
void addTagAssociationIfAbsent(::Lightweight::DataMapper& mapper, std::uint64_t bookmarkId, std::uint64_t tagId) {
    auto existing = mapper.Query<db::BookmarkTagRecord>()
                        .Where(::Lightweight::FieldNameOf<&db::BookmarkTagRecord::bookmark>, "=", bookmarkId)
                        .Where(::Lightweight::FieldNameOf<&db::BookmarkTagRecord::tag>, "=", tagId)
                        .All();
    if (!existing.empty()) {
        return;
    }
    db::BookmarkTagRecord junction;
    junction.bookmark = bookmarkId;
    junction.tag = tagId;
    mapper.Create(junction);
}

/// @brief Writes one row into `bookmark_outbox`. Must run inside the
///        caller's own `SqlTransaction` — see this task's own doc comment.
template <typename Action, typename Result>
void writeOutboxEntry(::Lightweight::DataMapper& mapper, const std::string& owner, const Action& action,
                      const Result& result, std::string_view actionType, std::string_view idempotencyKey) {
    db::BookmarkOutboxRecord entry;
    entry.modelType = "BookmarkModel";
    entry.entityKey = owner;
    entry.actionType = std::string{actionType};
    entry.payload = ::morph::model::ActionTraits<Action>::toJson(action);
    entry.result = ::morph::model::ActionTraits<Action>::resultToJson(result);
    entry.principal = owner;
    entry.timestampMs = nowMs();
    entry.idempotencyKey = std::string{idempotencyKey};
    mapper.Create(entry);
}

}  // namespace

/// @brief Reads every tag name currently associated with @p bookmarkId.
///
/// Takes no owner and needs none: a tag row is always owned by the same
/// principal as every bookmark it is attached to, by construction --
/// `applyTagSet` below never creates a cross-owner association -- so the
/// junction rows for one bookmark are already owner-homogeneous, and the
/// caller has already established that the bookmark itself is readable.
[[nodiscard]] static std::vector<std::string> readTagNames(::Lightweight::DataMapper& mapper, std::uint64_t bookmarkId) {
    auto junctionRows = mapper.Query<db::BookmarkTagRecord>()
                             .Where(::Lightweight::FieldNameOf<&db::BookmarkTagRecord::bookmark>, "=", bookmarkId)
                             .All();
    std::vector<std::string> names;
    names.reserve(junctionRows.size());
    for (const auto& row : junctionRows) {
        auto tagRows = mapper.Query<db::TagRecord>()
                            .Where(::Lightweight::FieldNameOf<&db::TagRecord::id>, "=", row.tag.Value())
                            .All();
        if (!tagRows.empty()) {
            names.push_back(tagRows.front().name.Value());
        }
    }
    return names;
}

/// @brief Replaces @p bookmarkId's tag set with exactly @p desiredNames,
///        auto-creating any tag @p owner has never used before. Must run
///        inside the caller's own `SqlTransaction` -- this function opens
///        none of its own, so every write it makes commits or rolls back
///        with the surrounding action.
static void applyTagSet(::Lightweight::DataMapper& mapper, std::uint64_t bookmarkId, const std::string& owner,
                        const std::vector<std::string>& desiredNames) {
    const auto current = readTagNames(mapper, bookmarkId);
    std::vector<std::string> toAdd;
    for (const auto& name : desiredNames) {
        if (std::ranges::find(current, name) == current.end()) {
            toAdd.push_back(name);
        }
    }
    std::vector<std::string> toRemove;
    for (const auto& name : current) {
        if (std::ranges::find(desiredNames, name) == desiredNames.end()) {
            toRemove.push_back(name);
        }
    }

    for (const auto& name : toAdd) {
        const auto tagId = findOrCreateTagId(mapper, owner, name);
        addTagAssociationIfAbsent(mapper, bookmarkId, tagId);
    }

    for (const auto& name : toRemove) {
        auto tagRows = mapper.Query<db::TagRecord>()
                            .Where(::Lightweight::FieldNameOf<&db::TagRecord::ownerPrincipal>, "=", owner)
                            .Where(::Lightweight::FieldNameOf<&db::TagRecord::name>, "=", name)
                            .All();
        if (tagRows.empty()) {
            continue;
        }
        ::Lightweight::SqlStatement stmt{mapper.Connection()};
        stmt.Prepare("DELETE FROM bookmark_tags WHERE bookmark_id = ? AND tag_id = ?");
        (void) stmt.Execute(bookmarkId, tagRows.front().id.Value());
    }
}

[[nodiscard]] static BookmarkView toView(const db::BookmarkRecord& rec, std::vector<std::string> tags) {
    BookmarkView view;
    view.id = BookmarkId{static_cast<std::int64_t>(rec.id.Value())};
    view.url = rec.url.Value();
    view.title = rec.title.Value();
    view.description = rec.description.Value();
    view.notes = rec.notes.Value();
    view.tags = std::move(tags);
    view.createdAt = fromEpochMs(rec.createdAtMs.Value());
    view.updatedAt = fromEpochMs(rec.updatedAtMs.Value());
    view.readState = rec.isUnread.Value() ? ReadState::Unread : ReadState::Read;
    view.archiveState = rec.isArchived.Value() ? ArchiveState::Archived : ArchiveState::Active;
    view.visibility = rec.isShared.Value() ? Visibility::Shared : Visibility::Private;
    return view;
}

/// @brief Loads @p id, requiring it to exist and be owned by @p owner.
/// @throws NotFound if no such row exists at all.
/// @throws Forbidden if it exists but belongs to a different principal --
///         distinguished on purpose (`bookmarks::Forbidden`'s own doc
///         comment) so the "local mode has no authorization at all" test
///         (Task 15) has something specific to assert against.
[[nodiscard]] static db::BookmarkRecord loadOwned(::Lightweight::DataMapper& mapper, std::uint64_t id,
                                                  const std::string& owner) {
    auto rows =
        mapper.Query<db::BookmarkRecord>().Where(::Lightweight::FieldNameOf<&db::BookmarkRecord::id>, "=", id).All();
    if (rows.empty()) {
        throw NotFound{"no such bookmark"};
    }
    if (rows.front().ownerPrincipal.Value() != owner) {
        throw Forbidden{"bookmark belongs to a different principal"};
    }
    return rows.front();
}

CreateBookmarkResult BookmarkModel::execute(const CreateBookmark& action) {
    if (!action.validate()) {
        throw ValidationError{"CreateBookmark: a non-empty url within the length bound is required"};
    }
    const auto& owner = requireOwner();

    db::BookmarkRecord rec;
    rec.ownerPrincipal = owner;
    rec.url = action.url;
    rec.title = action.title;
    rec.description = action.description;
    rec.notes = action.notes;
    rec.isShared = action.visibility == Visibility::Shared;
    const auto now = nowMs();
    rec.createdAtMs = now;
    rec.updatedAtMs = now;

    ::Lightweight::SqlTransaction transaction{mapper().Connection(), ::Lightweight::SqlTransactionMode::ROLLBACK};
    mapper().Create(rec);
    applyTagSet(mapper(), rec.id.Value(), owner, action.tags);
    transaction.Commit();

    return CreateBookmarkResult{.id = BookmarkId{static_cast<std::int64_t>(rec.id.Value())}};
}

BookmarkView BookmarkModel::execute(const EditBookmark& action) {
    if (!action.validate()) {
        throw ValidationError{"EditBookmark: id and a non-empty url within the length bound are required"};
    }
    const auto& owner = requireOwner();
    auto rec = loadOwned(mapper(), static_cast<std::uint64_t>(*action.id), owner);

    rec.url = action.url;
    rec.title = action.title;
    rec.description = action.description;
    rec.notes = action.notes;
    rec.isShared = action.visibility == Visibility::Shared;
    rec.updatedAtMs = nowMs();

    ::Lightweight::SqlTransaction transaction{mapper().Connection(), ::Lightweight::SqlTransactionMode::ROLLBACK};
    mapper().Update(rec);
    applyTagSet(mapper(), rec.id.Value(), owner, action.tags);
    transaction.Commit();

    return toView(rec, readTagNames(mapper(), rec.id.Value()));
}

Ack BookmarkModel::execute(const ArchiveBookmark& action) {
    if (!action.validate()) {
        throw ValidationError{"ArchiveBookmark: id is required"};
    }
    const auto& owner = requireOwner();
    auto rec = loadOwned(mapper(), static_cast<std::uint64_t>(*action.id), owner);
    rec.isArchived = true;
    rec.updatedAtMs = nowMs();
    mapper().Update(rec);
    return Ack{};
}

Ack BookmarkModel::execute(const UnarchiveBookmark& action) {
    if (!action.validate()) {
        throw ValidationError{"UnarchiveBookmark: id is required"};
    }
    const auto& owner = requireOwner();
    auto rec = loadOwned(mapper(), static_cast<std::uint64_t>(*action.id), owner);
    rec.isArchived = false;
    rec.updatedAtMs = nowMs();
    mapper().Update(rec);
    return Ack{};
}

Ack BookmarkModel::execute(const DeleteBookmark& action) {
    if (!action.validate()) {
        throw ValidationError{"DeleteBookmark: id is required"};
    }
    const auto& owner = requireOwner();
    const auto id = static_cast<std::uint64_t>(*action.id);
    (void) loadOwned(mapper(), id, owner);  // NotFound/Forbidden, same as every other action

    ::Lightweight::SqlTransaction transaction{mapper().Connection(), ::Lightweight::SqlTransactionMode::ROLLBACK};
    {
        ::Lightweight::SqlStatement stmt{mapper().Connection()};
        stmt.Prepare("DELETE FROM bookmark_tags WHERE bookmark_id = ?");
        (void) stmt.Execute(id);
    }
    {
        ::Lightweight::SqlStatement stmt{mapper().Connection()};
        stmt.Prepare("DELETE FROM bookmarks WHERE id = ?");
        (void) stmt.Execute(id);
    }
    transaction.Commit();
    return Ack{};
}

BookmarkView BookmarkModel::execute(const GetBookmark& action) {
    if (!action.validate()) {
        throw ValidationError{"GetBookmark: id is required"};
    }
    const auto& owner = requireOwner();
    const auto rec = loadOwned(mapper(), static_cast<std::uint64_t>(*action.id), owner);
    return toView(rec, readTagNames(mapper(), rec.id.Value()));
}

ListBookmarksResult BookmarkModel::execute(const ListBookmarks& action) {
    const auto& owner = requireOwner();
    auto query = mapper().Query<db::BookmarkRecord>();
    (void) query.Where(::Lightweight::FieldNameOf<&db::BookmarkRecord::ownerPrincipal>, "=", owner);
    if (action.archiveFilter == ArchiveFilter::ActiveOnly) {
        (void) query.Where(::Lightweight::FieldNameOf<&db::BookmarkRecord::isArchived>, "=", false);
    } else if (action.archiveFilter == ArchiveFilter::ArchivedOnly) {
        (void) query.Where(::Lightweight::FieldNameOf<&db::BookmarkRecord::isArchived>, "=", true);
    }
    if (action.readFilter == ReadFilter::UnreadOnly) {
        (void) query.Where(::Lightweight::FieldNameOf<&db::BookmarkRecord::isUnread>, "=", true);
    } else if (action.readFilter == ReadFilter::ReadOnly) {
        (void) query.Where(::Lightweight::FieldNameOf<&db::BookmarkRecord::isUnread>, "=", false);
    }
    if (action.cursor.hasValue()) {
        (void) query.Where(::Lightweight::FieldNameOf<&db::BookmarkRecord::id>, "<",
                           static_cast<std::uint64_t>(*action.cursor));
    }
    // Text/tag filters run in C++ after the SQL page is fetched, not as a
    // LIKE/JOIN in the query above: this rung's scale (a demo bookmark
    // collection, not a production search index) does not warrant it, and
    // combining a tag filter with keyset pagination correctly needs the
    // junction table anyway, which the per-row loop below already touches.
    constexpr std::size_t kPageSize = 20;
    auto rows = query.OrderBy(::Lightweight::FieldNameOf<&db::BookmarkRecord::id>, ::Lightweight::SqlResultOrdering::DESCENDING)
                    .First(kPageSize + 1);
    const bool hasMore = rows.size() > kPageSize;
    if (hasMore) {
        rows.resize(kPageSize);
    }

    ListBookmarksResult result;
    for (const auto& rec : rows) {
        auto tags = readTagNames(mapper(), rec.id.Value());
        if (!action.tag.empty() && std::ranges::find(tags, action.tag) == tags.end()) {
            continue;
        }
        if (!action.searchText.empty() && rec.title.Value().find(action.searchText) == std::string::npos &&
            rec.url.Value().find(action.searchText) == std::string::npos) {
            continue;
        }
        BookmarkSummary summary;
        summary.id = BookmarkId{static_cast<std::int64_t>(rec.id.Value())};
        summary.url = rec.url.Value();
        summary.title = rec.title.Value();
        summary.tags = std::move(tags);
        summary.createdAt = fromEpochMs(rec.createdAtMs.Value());
        summary.updatedAt = fromEpochMs(rec.updatedAtMs.Value());
        summary.readState = rec.isUnread.Value() ? ReadState::Unread : ReadState::Read;
        summary.archiveState = rec.isArchived.Value() ? ArchiveState::Archived : ArchiveState::Active;
        summary.visibility = rec.isShared.Value() ? Visibility::Shared : Visibility::Private;
        result.bookmarks.push_back(std::move(summary));
    }
    if (hasMore) {
        // The cursor must be set whenever more raw rows exist, independent of
        // whether this page's *filtered* results happen to be empty: rows.back()
        // is the correct pagination boundary regardless of the tag/searchText
        // filters above. Gating this on !result.bookmarks.empty() would let a
        // page whose 20 raw rows are all filtered out (while a 21st still
        // proves hasMore) return an empty, cursor-less response -- a
        // tag/text-filtering client would then wrongly conclude the search is
        // exhausted and silently miss real matches further down the id space.
        result.nextCursor = Cursor{static_cast<std::int64_t>(rows.back().id.Value())};
    }
    return result;
}

GetChangesSinceResult BookmarkModel::execute(const GetChangesSince& action) {
    const auto& owner = requireOwner();
    // Captured *before* the query -- see this task's own doc comment for
    // why a later capture would let a racing write be lost across two
    // consecutive polls instead of merely duplicated across them.
    const auto asOf = nowMs();
    const std::int64_t sinceMs =
        action.since.timestampMs.hasValue() ? (*action.since.timestampMs).value.time_since_epoch().count() : 0;
    const std::uint64_t sinceLastId = static_cast<std::uint64_t>(action.since.lastId.value_or(0));

    // See ChangesCursor's doc comment (issue #43): a strict `updatedAtMs >
    // sinceMs` alone drops a write landing in the exact same millisecond as
    // `sinceMs`. The id tie-break recovers it without over-including: any
    // row strictly after sinceMs qualifies outright; a row *at* sinceMs
    // qualifies only if its id is past the last one already delivered at
    // that same instant.
    auto rows = mapper()
                    .Query<db::BookmarkRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::BookmarkRecord::ownerPrincipal>, "=", owner)
                    .Where([&](auto& q) {
                        return q.Where(::Lightweight::FieldNameOf<&db::BookmarkRecord::updatedAtMs>, ">", sinceMs)
                            .OrWhere([&](auto& q2) {
                                return q2.Where(::Lightweight::FieldNameOf<&db::BookmarkRecord::updatedAtMs>, "=",
                                                sinceMs)
                                    .Where(::Lightweight::FieldNameOf<&db::BookmarkRecord::id>, ">", sinceLastId);
                            });
                    })
                    .OrderBy(::Lightweight::FieldNameOf<&db::BookmarkRecord::updatedAtMs>)
                    .OrderBy(::Lightweight::FieldNameOf<&db::BookmarkRecord::id>)
                    .All();

    GetChangesSinceResult result;
    result.asOf.timestampMs = fromEpochMs(asOf);
    // The next cursor's tie-break is the highest id delivered *at exactly
    // asOf* -- a row strictly before asOf needs no tie-break (already
    // excluded outright by the next poll's `>` on its own), and no row can
    // exist strictly after asOf, since asOf was captured before this query
    // ran. Rows are ordered (updatedAtMs, id) ascending above, so the last
    // row sharing asOf's timestamp, if any, is found from the back.
    for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
        if (static_cast<std::int64_t>(it->updatedAtMs.Value()) == asOf) {
            result.asOf.lastId = static_cast<std::int64_t>(it->id.Value());
            break;
        }
    }
    for (const auto& rec : rows) {
        BookmarkSummary summary;
        summary.id = BookmarkId{static_cast<std::int64_t>(rec.id.Value())};
        summary.url = rec.url.Value();
        summary.title = rec.title.Value();
        summary.tags = readTagNames(mapper(), rec.id.Value());
        summary.createdAt = fromEpochMs(rec.createdAtMs.Value());
        summary.updatedAt = fromEpochMs(rec.updatedAtMs.Value());
        summary.readState = rec.isUnread.Value() ? ReadState::Unread : ReadState::Read;
        summary.archiveState = rec.isArchived.Value() ? ArchiveState::Archived : ArchiveState::Active;
        summary.visibility = rec.isShared.Value() ? Visibility::Shared : Visibility::Private;
        result.changed.push_back(std::move(summary));
    }
    return result;
}

BulkEditResult BookmarkModel::execute(const BulkEdit& action) {
    if (!action.validate()) {
        throw ValidationError{"BulkEdit: at least one id is required"};
    }
    const auto& owner = requireOwner();

    ::Lightweight::SqlTransaction transaction{mapper().Connection(), ::Lightweight::SqlTransactionMode::ROLLBACK};

    // Ownership check first, for *every* id, before any write: one
    // violation rejects the whole batch (README's "all-or-nothing"
    // framing, this task's resolved design decision) rather than applying
    // a partial edit and reporting which ids failed.
    std::vector<std::uint64_t> ids;
    ids.reserve(action.ids.size());
    for (const auto& bookmarkId : action.ids) {
        if (!bookmarkId.hasValue()) {
            throw ValidationError{"BulkEdit: every id must be engaged"};
        }
        const auto id = static_cast<std::uint64_t>(*bookmarkId);
        (void) loadOwned(mapper(), id, owner);  // throws Forbidden/NotFound -> whole transaction rolls back
        ids.push_back(id);
    }

    for (const auto id : ids) {
        if (action.archive == BulkArchiveOp::Archive) {
            ::Lightweight::SqlStatement stmt{mapper().Connection()};
            stmt.Prepare("UPDATE bookmarks SET is_archived = 1, updated_at_ms = ? WHERE id = ?");
            (void) stmt.Execute(nowMs(), id);
        } else if (action.archive == BulkArchiveOp::Unarchive) {
            ::Lightweight::SqlStatement stmt{mapper().Connection()};
            stmt.Prepare("UPDATE bookmarks SET is_archived = 0, updated_at_ms = ? WHERE id = ?");
            (void) stmt.Execute(nowMs(), id);
        }
        for (const auto& name : action.addTags) {
            const auto tagId = findOrCreateTagId(mapper(), owner, name);
            addTagAssociationIfAbsent(mapper(), id, tagId);
        }
        for (const auto& name : action.removeTags) {
            auto tagRows = mapper()
                               .Query<db::TagRecord>()
                               .Where(::Lightweight::FieldNameOf<&db::TagRecord::ownerPrincipal>, "=", owner)
                               .Where(::Lightweight::FieldNameOf<&db::TagRecord::name>, "=", name)
                               .All();
            if (tagRows.empty()) {
                continue;
            }
            ::Lightweight::SqlStatement stmt{mapper().Connection()};
            stmt.Prepare("DELETE FROM bookmark_tags WHERE bookmark_id = ? AND tag_id = ?");
            (void) stmt.Execute(id, tagRows.front().id.Value());
        }
    }

    BulkEditResult result{.affected = Count::fromDouble(static_cast<double>(ids.size()))};
    // idempotencyKey: not a client-supplied op-id (BulkEdit carries none --
    // unlike ImportBookmarks, retried bulk edits are not expected to be
    // idempotent at this layer), so a fresh key per call is enough to keep
    // this row distinguishable from any other outbox row; the relay's
    // dedup only matters across relay *retries* of the same row, not
    // across separate BulkEdit calls. `nowMs()` alone is only millisecond
    // resolution, so two calls from the same owner landing in the same
    // millisecond (a script, a double-click, a retry) would otherwise
    // produce the identical key and collide against
    // `idx_bookmark_outbox_idempotency`'s unique index, spuriously failing
    // the second, legitimate call with a raw SQL constraint-violation
    // exception instead of succeeding; `nextOutboxSeq()` (a process-wide
    // monotonic counter) makes the key collision-resistant regardless of
    // clock resolution.
    writeOutboxEntry(mapper(), owner, action, result, "BulkEdit",
                     owner + "-bulkedit-" + std::to_string(nowMs()) + "-" + std::to_string(nextOutboxSeq()));
    transaction.Commit();
    return result;
}

Ack BookmarkModel::execute(const RecordMetadata& action) {
    if (!action.validate()) {
        throw ValidationError{"RecordMetadata: id is required"};
    }
    // Dispatched only by the internal metadata-fetch worker's
    // "system:metadata-fetcher" service principal (Task 12) -- deliberately
    // skips the *row-owner* check every GUI-reachable action performs: the
    // worker acts on behalf of whichever principal owns the row, not on
    // behalf of itself, so filtering by owner here would make it able to
    // update nothing at all. Mirrors pastebin::ExpirePaste's internal-only
    // shape, including the deleted-before-processed no-op below (that
    // action's "already gone" tolerance).
    //
    // What replaces the owner check is a *caller* check, and it has to live
    // here rather than in the authorizer: `authorizeInstance`'s
    // owner-vs-principal comparison would not fit here even with a real
    // recorded owner (which register envelopes now carry). The worker
    // dispatches through its OWN plain-registered instance -- an instance it
    // legitimately owns -- to touch a *row* some other user owns.
    // authorizeInstance compares instance ownership, not row ownership, so
    // it has nothing to object to: the worker's own instance is exactly what
    // it is authorized to use. Without this line any authenticated user
    // could dispatch RecordMetadata against any other user's bookmark id and
    // overwrite its title and favicon, since this is the one action that
    // does not scope its query to the caller. Rule 1 ("models must re-check
    // their own authorization") is exactly the instruction being followed.
    if (requireOwner() != auth::kMetadataFetcherPrincipal) {
        throw Forbidden{"RecordMetadata is dispatched only by the metadata-fetch service principal"};
    }
    const auto id = static_cast<std::uint64_t>(*action.id);
    auto rows =
        mapper().Query<db::BookmarkRecord>().Where(::Lightweight::FieldNameOf<&db::BookmarkRecord::id>, "=", id).All();
    if (rows.empty()) {
        return Ack{};
    }
    auto rec = rows.front();
    if (!action.title.empty()) {
        rec.title = action.title;
    }
    if (!action.faviconPath.empty()) {
        rec.faviconPath = action.faviconPath;
    }
    rec.updatedAtMs = nowMs();
    mapper().Update(rec);
    return Ack{};
}

ImportBookmarksResult BookmarkModel::execute(const ImportBookmarks& action) {
    // Checked ahead of the general `validate()` so the size bound gets the
    // typed signal `TooLarge`'s own doc comment promises. `validate()` folds
    // three conditions into one bool, and a caller that chunked its file too
    // coarsely needs to tell "make the chunks smaller" apart from "this
    // request was malformed" — which is the entire reason `TooLarge` exists
    // as a distinct type.
    if (action.chunk.size() > kMaxImportChunkBytes) {
        throw TooLarge{"ImportBookmarks: chunk exceeds kMaxImportChunkBytes"};
    }
    if (!action.validate()) {
        throw ValidationError{"ImportBookmarks: a non-empty chunk and an opId are required"};
    }
    const auto& owner = requireOwner();
    const auto& opIdStr = *action.opId;

    auto existingOp = mapper()
                          .Query<db::ImportedOpRecord>()
                          .Where(::Lightweight::FieldNameOf<&db::ImportedOpRecord::ownerPrincipal>, "=", owner)
                          .Where(::Lightweight::FieldNameOf<&db::ImportedOpRecord::opId>, "=", opIdStr)
                          .All();
    if (!existingOp.empty()) {
        // Already applied -- a retried chunk after a dropped connection is
        // a safe no-op, per this task's idempotency requirement. Reports
        // zero: the caller's own first, successful attempt already learned
        // the real counts, and a retry's purpose is confirming "did this
        // land," not re-reporting them.
        return ImportBookmarksResult{.imported = Count::fromDouble(0.0), .skipped = Count::fromDouble(0.0)};
    }

    const auto entries = ::bookmarks::import::parseNetscapeChunk(action.chunk);
    std::size_t imported = 0;
    std::size_t skipped = 0;

    ::Lightweight::SqlTransaction transaction{mapper().Connection(), ::Lightweight::SqlTransactionMode::ROLLBACK};
    for (const auto& entry : entries) {
        // The parser is a *file* parser, not a DTO: nothing upstream of it
        // applies this rung's own field bounds. Writing an over-long url or
        // title anyway would create a row that `EditBookmark::validate()`
        // (and `CreateBookmark::validate()`) then refuse to accept — an
        // imported bookmark the owner can see but can never edit, which is a
        // worse outcome than not importing it. Truncating instead would be
        // worse still: a silently mangled url is not the bookmark the user
        // saved. So such an entry is skipped and counted, exactly like a
        // malformed one.
        if (entry.url.empty() || entry.url.size() > kMaxUrlBytes || entry.title.size() > kMaxTitleBytes) {
            ++skipped;
            continue;
        }
        db::BookmarkRecord rec;
        rec.ownerPrincipal = owner;
        rec.url = entry.url;
        rec.title = entry.title;
        const auto now = nowMs();
        rec.createdAtMs = now;
        rec.updatedAtMs = now;
        mapper().Create(rec);
        ++imported;
    }
    db::ImportedOpRecord op;
    op.ownerPrincipal = owner;
    op.opId = opIdStr;
    op.appliedAtMs = nowMs();
    mapper().Create(op);
    transaction.Commit();

    return ImportBookmarksResult{.imported = Count::fromDouble(static_cast<double>(imported)),
                                 .skipped = Count::fromDouble(static_cast<double>(skipped))};
}

ExportBookmarksResult BookmarkModel::execute(const ExportBookmarks&) {
    const auto& owner = requireOwner();
    auto rows = mapper()
                    .Query<db::BookmarkRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::BookmarkRecord::ownerPrincipal>, "=", owner)
                    .All();
    std::string html = "<!DOCTYPE NETSCAPE-Bookmark-file-1>\n<TITLE>Bookmarks</TITLE>\n<H1>Bookmarks</H1>\n<DL><p>\n";
    for (const auto& rec : rows) {
        html += "    <DT><A HREF=\"" + ::bookmarks::import::escapeHtml(rec.url.Value()) + "\">" +
               ::bookmarks::import::escapeHtml(rec.title.Value()) + "</A>\n";
    }
    html += "</DL><p>\n";
    return ExportBookmarksResult{.html = std::move(html)};
}

}  // namespace bookmarks
