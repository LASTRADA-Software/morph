// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <cstdint>
#include <morph/offline/replay_ledger.hpp>
#include <optional>
#include <string>
#include <string_view>

#include "bookmarks/db/imported_op_entity.hpp"

namespace bookmarks::offline {

/// @brief `morph::offline::IReplayLedger` over `db::ImportedOpRecord`
///        (morph#226) — the first rung migrated onto the promoted interface.
///
/// Skip-only: `record()` is always called with an empty payload here, and
/// `ImportBookmarks::execute` only ever asks `lookup()` "was this opId
/// already applied", never reads the (always-empty) string back. This mirrors
/// `db::ImportedOpRecord`'s own shape from before this migration — it never
/// stored a result — so the on-disk table and its query pattern are
/// unchanged; only the seam a model reaches them through moved onto the
/// framework's interface.
///
/// @par Atomicity
/// Constructed fresh, per call, over the model's *already-open* `DataMapper&`
/// (see `IReplayLedger::record()`'s own doc comment on why this matters) —
/// never opens a connection or transaction of its own. `record()`'s
/// `mapper.Create(op)` therefore commits inside whichever `SqlTransaction`
/// the caller already has open around it, exactly as the hand-written
/// check-then-insert this replaces did.
class BookmarksReplayLedger : public morph::offline::IReplayLedger {
public:
    /// @param mapper      The caller's already-acquired data mapper. Borrowed:
    ///        must outlive this ledger, which is expected to be a short-lived
    ///        local constructed around one `execute()` call.
    /// @param appliedAtMs Timestamp stamped on a row this instance records.
    ///        Taken from the caller rather than read here so this class does
    ///        not need its own opinion of "now" (`ImportBookmarks::execute`
    ///        already reads one, via `morph::ladder::now()`, for its own use).
    BookmarksReplayLedger(::Lightweight::DataMapper& mapper, std::int64_t appliedAtMs)
        : _mapper{mapper}, _appliedAtMs{appliedAtMs} {}

protected:
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters) -- scope/opId, not interchangeable
    [[nodiscard]] std::optional<std::string> doLookup(std::string_view scope, std::string_view opId) const override {
        auto const existing =
            _mapper.Query<db::ImportedOpRecord>()
                .Where(::Lightweight::FieldNameOf<&db::ImportedOpRecord::ownerPrincipal>, "=", std::string{scope})
                .Where(::Lightweight::FieldNameOf<&db::ImportedOpRecord::opId>, "=", std::string{opId})
                .All();
        if (existing.empty()) {
            return std::nullopt;
        }
        // Skip-only: nothing was ever stored beyond the fact of the row's
        // existence (db::ImportedOpRecord carries no result column), so the
        // payload is always empty on a hit -- see this class's own doc
        // comment.
        return std::string{};
    }

    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters) -- scope/opId, not interchangeable
    void doRecord(std::string_view scope, std::string_view opId, std::string /*payload*/) override {
        db::ImportedOpRecord op;
        op.ownerPrincipal = std::string{scope};
        op.opId = std::string{opId};
        op.appliedAtMs = _appliedAtMs;
        _mapper.Create(op);
    }

private:
    ::Lightweight::DataMapper& _mapper;
    std::int64_t _appliedAtMs;
};

}  // namespace bookmarks::offline
