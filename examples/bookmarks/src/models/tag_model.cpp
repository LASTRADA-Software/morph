// SPDX-License-Identifier: Apache-2.0
#include "bookmarks/models/tag_model.hpp"

#include "bookmarks/auth/bookmarks_authorizer.hpp"
#include "bookmarks/db/bookmark_tag_entity.hpp"
#include "bookmarks/db/outbox_entity.hpp"
#include "bookmarks/db/tag_entity.hpp"

#include "clock.hpp"

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <Lightweight/DataMapper/Pool.hpp>
#include <Lightweight/SqlError.hpp>
#include <Lightweight/SqlErrorDetection.hpp>
#include <Lightweight/SqlStatement.hpp>
#include <Lightweight/SqlTransaction.hpp>

#include <morph/core/registry.hpp>
#include <morph/session/session.hpp>

#include <atomic>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace bookmarks {

// See `bookmark_model.cpp`'s identical static_asserts for the full
// rationale this mirrors -- pinning `TagRecord`'s column capacities to the
// same DTO-level constants that already gate `RenameTag`/`MergeTags`
// input.
static_assert(decltype(db::TagRecord::ownerPrincipal)::ValueType{}.capacity() == auth::kMaxPrincipalBytes,
              "bookmarks::auth::kMaxPrincipalBytes must equal TagRecord::ownerPrincipal's SqlAnsiString capacity "
              "-- otherwise a principal the authorizer accepts could be silently truncated on the way into the "
              "row.");
static_assert(decltype(db::TagRecord::name)::ValueType{}.capacity() == kMaxTagNameBytes,
              "bookmarks::kMaxTagNameBytes must equal TagRecord::name's SqlAnsiString capacity -- otherwise "
              "RenameTag either rejects names that would have fit, or accepts ones that get silently truncated "
              "on the way into the row.");

namespace {

[[nodiscard]] std::int64_t nowMs() noexcept {
    return (*::morph::ladder::now().value).value.time_since_epoch().count();
}

/// @brief Process-wide monotonic counter, used only to disambiguate
///        `MergeTags`'s server-generated idempotency key (see its call
///        site) when two calls land in the same `nowMs()` millisecond --
///        `morph::ladder::now()` has millisecond resolution, so the
///        timestamp alone cannot be trusted to be unique across rapid
///        back-to-back calls from the same principal. Mirrors
///        `BookmarkModel`'s own `nextOutboxSeq()`
///        (`bookmark_model.cpp`) -- duplicated rather than shared across
///        translation units, this rung's established convention for small
///        internal details (see this task's own header comment).
///        `std::atomic` (not `thread_local`) because the model instance is
///        shared across whichever thread each dispatched call lands on.
[[nodiscard]] std::uint64_t nextOutboxSeq() noexcept {
    static std::atomic<std::uint64_t> counter{0};
    return counter.fetch_add(1, std::memory_order_relaxed);
}

/// @brief The authenticated caller's principal, or throws `Forbidden`. See
///        `BookmarkModel`'s identical helper (`bookmark_model.cpp`) for the
///        full rationale this mirrors.
[[nodiscard]] const std::string& requireOwner() {
    const auto* ctx = ::morph::session::current();
    if (ctx == nullptr || ctx->principal.empty()) {
        throw Forbidden{"no authenticated principal"};
    }
    return ctx->principal;
}

/// @brief Loads tag @p id, requiring it to exist and be owned by @p owner.
/// @throws NotFound if no such row exists at all.
/// @throws Forbidden if it exists but belongs to a different principal.
[[nodiscard]] db::TagRecord loadOwnedTag(::Lightweight::DataMapper& mapper, std::uint64_t id, const std::string& owner) {
    auto rows = mapper.Query<db::TagRecord>().Where(::Lightweight::FieldNameOf<&db::TagRecord::id>, "=", id).All();
    if (rows.empty()) {
        throw NotFound{"no such tag"};
    }
    if (rows.front().ownerPrincipal.Value() != owner) {
        throw Forbidden{"tag belongs to a different principal"};
    }
    return rows.front();
}

}  // namespace

Ack TagModel::execute(const RenameTag& action) {
    if (!action.validate()) {
        throw ValidationError{"RenameTag: id and a non-empty, bounded name are required"};
    }
    const auto& owner = requireOwner();
    auto mapper = ::Lightweight::GlobalDataMapperPool().Acquire();
    auto rec = loadOwnedTag(mapper.Get(), static_cast<std::uint64_t>(*action.id), owner);
    rec.name = action.name;
    try {
        mapper->Update(rec);
    } catch (const ::Lightweight::SqlException& error) {
        if (::Lightweight::IsUniqueConstraintViolation(error.info(), mapper->Connection().ServerType())) {
            throw Conflict{"RenameTag: a tag named '" + action.name + "' already exists"};
        }
        throw;
    }
    return Ack{};
}

Ack TagModel::execute(const MergeTags& action) {
    if (!action.validate()) {
        throw ValidationError{"MergeTags: sourceId and a distinct targetId are required"};
    }
    const auto& owner = requireOwner();
    const auto sourceId = static_cast<std::uint64_t>(*action.sourceId);
    const auto targetId = static_cast<std::uint64_t>(*action.targetId);
    auto mapper = ::Lightweight::GlobalDataMapperPool().Acquire();
    (void) loadOwnedTag(mapper.Get(), sourceId, owner);
    (void) loadOwnedTag(mapper.Get(), targetId, owner);

    ::Lightweight::SqlTransaction transaction{mapper->Connection(), ::Lightweight::SqlTransactionMode::ROLLBACK};

    auto sourceRows = mapper
                          ->Query<db::BookmarkTagRecord>()
                          .Where(::Lightweight::FieldNameOf<&db::BookmarkTagRecord::tag>, "=", sourceId)
                          .All();
    for (const auto& row : sourceRows) {
        const auto bookmarkId = row.bookmark.Value();
        auto clash = mapper
                         ->Query<db::BookmarkTagRecord>()
                         .Where(::Lightweight::FieldNameOf<&db::BookmarkTagRecord::bookmark>, "=", bookmarkId)
                         .Where(::Lightweight::FieldNameOf<&db::BookmarkTagRecord::tag>, "=", targetId)
                         .All();
        // Either way the source association must go -- delete it outright
        // rather than `mapper->Update()`-ing its `tag` field in place:
        // `BelongsTo::operator=(ValueType)` goes through the implicit
        // converting constructor + copy-assignment, which never sets the
        // field's `_modified` flag (only `operator=(ReferencedRecord&)`
        // does), so `Update()` would silently skip writing the column --
        // this is exactly why `bookmark_tag_entity.hpp`'s own doc comment
        // says tag (re)assignment is always a Create/delete of a whole row,
        // never an in-place Update.
        {
            ::Lightweight::SqlStatement stmt{mapper->Connection()};
            stmt.Prepare("DELETE FROM bookmark_tags WHERE bookmark_id = ? AND tag_id = ?");
            (void) stmt.Execute(bookmarkId, sourceId);
        }
        if (clash.empty()) {
            // No existing target association for this bookmark -- recreate
            // the row pointing at targetId instead of sourceId. When a
            // clash does exist, the target association already covers this
            // bookmark, so nothing further is needed (this is the
            // dedup case the unique index on (bookmark_id, tag_id) exists
            // to protect).
            db::BookmarkTagRecord junction;
            junction.bookmark = bookmarkId;
            junction.tag = targetId;
            mapper->Create(junction);
        }
    }
    {
        ::Lightweight::SqlStatement stmt{mapper->Connection()};
        stmt.Prepare("DELETE FROM tags WHERE id = ?");
        (void) stmt.Execute(sourceId);
    }

    Ack result{};
    db::BookmarkOutboxRecord entry;
    entry.modelType = "TagModel";
    entry.entityKey = owner;
    entry.actionType = "MergeTags";
    entry.payload = ::morph::model::ActionTraits<MergeTags>::toJson(action);
    entry.result = ::morph::model::ActionTraits<MergeTags>::resultToJson(result);
    entry.principal = owner;
    entry.timestampMs = nowMs();
    // idempotencyKey: nowMs() alone is only millisecond resolution, so two
    // MergeTags calls from the same owner landing in the same millisecond
    // would otherwise produce the identical key and collide against
    // `idx_bookmark_outbox_idempotency`'s unique index, spuriously failing
    // the second, legitimate call with a raw SQL constraint-violation
    // exception instead of succeeding -- the exact bug Task 8's review
    // caught in `BookmarkModel::execute(const BulkEdit&)`. `nextOutboxSeq()`
    // (a process-wide monotonic counter) makes the key collision-resistant
    // regardless of clock resolution.
    entry.idempotencyKey = owner + "-mergetags-" + std::to_string(nowMs()) + "-" + std::to_string(nextOutboxSeq());
    mapper->Create(entry);

    transaction.Commit();
    return result;
}

ListTagsResult TagModel::execute(const ListTags&) {
    const auto& owner = requireOwner();
    auto mapper = ::Lightweight::GlobalDataMapperPool().Acquire();
    auto rows =
        mapper->Query<db::TagRecord>().Where(::Lightweight::FieldNameOf<&db::TagRecord::ownerPrincipal>, "=", owner).All();

    // Batched, not per-tag: one query for every junction row across all of
    // this owner's tags, counted in-memory below -- instead of a `COUNT`
    // query *per tag* (N+1, and each one still pulls full rows just to
    // discard everything but `.size()`), this owner's whole tag list costs
    // exactly 1 extra query regardless of how many tags they have.
    std::vector<std::uint64_t> tagIds;
    tagIds.reserve(rows.size());
    for (const auto& rec : rows) {
        tagIds.push_back(rec.id.Value());
    }
    auto junctionRows = mapper->Query<db::BookmarkTagRecord>()
                            .WhereIn(::Lightweight::FieldNameOf<&db::BookmarkTagRecord::tag>, tagIds)
                            .All();
    std::unordered_map<std::uint64_t, std::uint64_t> countByTagId;
    countByTagId.reserve(tagIds.size());
    for (const auto& jrow : junctionRows) {
        ++countByTagId[jrow.tag.Value()];
    }

    ListTagsResult result;
    for (const auto& rec : rows) {
        TagSummary summary;
        summary.id = TagId{static_cast<std::int64_t>(rec.id.Value())};
        // `TagRecord::name` is `Light::SqlAnsiString<kMaxTagNameBytes>`;
        // `TagSummary::name` stays plain `std::string` on the wire.
        summary.name = std::string{rec.name.Value()};
        const auto it = countByTagId.find(rec.id.Value());
        const auto count = it != countByTagId.end() ? it->second : std::uint64_t{0};
        summary.bookmarkCount = Count::fromDouble(static_cast<double>(count));
        result.tags.push_back(std::move(summary));
    }
    return result;
}

}  // namespace bookmarks
