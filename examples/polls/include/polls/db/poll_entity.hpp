// SPDX-License-Identifier: Apache-2.0
#pragma once

#ifndef __EMSCRIPTEN__
#include <Lightweight/DataMapper/DataMapper.hpp>
#endif

#include "polls/core/types.hpp"

#include <cstdint>
#include <string>
#include <string_view>

/// @file
/// Six ladder-rung-3 entities. Every child table (`OptionRecord`,
/// `VoteRecord`, `CommentRecord`, `VoteHistoryRecord`, `PollEventRecord`)
/// deliberately carries **zero** relation-typed members beyond `BelongsTo`
/// (no `HasMany`, no `HasManyThrough`) -- see
/// `bookmarks::db::BookmarkRecord`'s identical file comment
/// (`examples/bookmarks/include/bookmarks/db/bookmark_entity.hpp`) for the
/// verified reason: `DataMapper::Update()`'s non-reflection path calls
/// `field.IsModified()` on every member via `EnumerateRecordMembers` (which
/// does not filter by field kind), and neither relation type declares that
/// method, so a record embedding one fails to compile the instant `Update()`
/// is instantiated for it. Reads against a parent poll always go through a
/// plain `Query<T>().Where(FieldNameOf<&T::poll>, "=", pollDbId)` call in
/// the model (`poll_model.cpp`, Task 5+), never through an embedded
/// relation field on `PollRecord`.

namespace polls::db {

#ifndef __EMSCRIPTEN__

/// @brief One row of the `polls` table.
struct PollRecord {
    static constexpr std::string_view TableName = "polls";

    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    /// The shareable link id -- see this rung's Global Constraints. Fixed-width,
    /// ASCII, `kTokenBytes` long: the same ID/token-shaped case bank's `number`
    /// and pastebin's `id` are, so `SqlAnsiString`, not plain `std::string`
    /// (which this rung's own free-form Unicode text fields -- `title`,
    /// `participantName`, `body`, etc. -- correctly use instead, matching
    /// bookmarks' precedent for that different case).
    Light::Field<Light::SqlAnsiString<kTokenBytes>, Light::SqlRealName{"poll_id"}> pollId;  // 1
    /// Kept by the organizer only.
    Light::Field<Light::SqlAnsiString<kTokenBytes>, Light::SqlRealName{"admin_token"}> adminToken;  // 2
    /// Handed out with the shared link.
    Light::Field<Light::SqlAnsiString<kTokenBytes>, Light::SqlRealName{"participant_token"}> participantToken;  // 3
    Light::Field<std::string, Light::SqlRealName{"title"}> title;  // 4
    Light::Field<bool, Light::SqlRealName{"finalized"}> finalized{false};  // 5
    /// 0 = not finalized; FK-shaped but not FK-enforced (SQLite).
    Light::Field<std::int64_t, Light::SqlRealName{"finalized_option_id"}> finalizedOptionId{0};  // 6
    Light::Field<std::int64_t, Light::SqlRealName{"created_at_ms"}> createdAtMs{0};  // 7
};

/// @brief One row of the `poll_options` table.
struct OptionRecord {
    static constexpr std::string_view TableName = "poll_options";

    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::BelongsTo<&PollRecord::id, Light::SqlRealName{"poll_id"}> poll;  // 1
    Light::Field<std::string, Light::SqlRealName{"label"}> label;  // 2
    /// Preserves `CreatePoll`'s option order across storage/query.
    Light::Field<std::int64_t, Light::SqlRealName{"sort_order"}> sortOrder{0};  // 3
};

/// @brief One participant's current vote for one option. Unique on
///        (pollId, participantName, optionId) so a retried `SubmitVotes`
///        cannot double-count -- see Task 6's own doc comment on the exact
///        index this rung's DoD names.
struct VoteRecord {
    static constexpr std::string_view TableName = "votes";

    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::BelongsTo<&PollRecord::id, Light::SqlRealName{"poll_id"}> poll;  // 1
    Light::BelongsTo<&OptionRecord::id, Light::SqlRealName{"option_id"}> option;  // 2
    Light::Field<std::string, Light::SqlRealName{"participant_name"}> participantName;  // 3
    /// `VoteChoice`'s underlying value.
    Light::Field<std::uint8_t, Light::SqlRealName{"choice"}> choice{std::uint8_t{0}};  // 4
};

/// @brief One row of the `comments` table.
struct CommentRecord {
    static constexpr std::string_view TableName = "comments";

    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::BelongsTo<&PollRecord::id, Light::SqlRealName{"poll_id"}> poll;  // 1
    Light::Field<std::string, Light::SqlRealName{"participant_name"}> participantName;  // 2
    Light::Field<std::string, Light::SqlRealName{"body"}> body;  // 3
    Light::Field<std::int64_t, Light::SqlRealName{"created_at_ms"}> createdAtMs{0};  // 4
};

/// @brief Undo's own history, one row per vote-changing call
///        (`SubmitVotes`/`UpdateVotes`), storing the *previous* state so
///        `UndoLastVoteChange` can restore it. Never read by anything but
///        `UndoLastVoteChange` -- not the audit trail (the framework
///        journal covers that separately).
struct VoteHistoryRecord {
    static constexpr std::string_view TableName = "vote_history";

    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::BelongsTo<&PollRecord::id, Light::SqlRealName{"poll_id"}> poll;  // 1
    Light::Field<std::string, Light::SqlRealName{"participant_name"}> participantName;  // 2
    /// The pre-change vote set, JSON-encoded.
    Light::Field<std::string, Light::SqlRealName{"previous_votes_json"}> previousVotesJson;  // 3
    Light::Field<std::int64_t, Light::SqlRealName{"created_at_ms"}> createdAtMs{0};  // 4
};

/// @brief The event log. Table-wide autoincrement `id` is `PollEventId`'s
///        wire value directly -- see this plan's Global Constraints.
struct PollEventRecord {
    static constexpr std::string_view TableName = "poll_events";

    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::BelongsTo<&PollRecord::id, Light::SqlRealName{"poll_id"}> poll;  // 1
    Light::Field<std::string, Light::SqlRealName{"kind"}> kind;  // 2
    Light::Field<std::string, Light::SqlRealName{"summary"}> summary;  // 3
    Light::Field<std::int64_t, Light::SqlRealName{"created_at_ms"}> createdAtMs{0};  // 4
};

#else
// Client-only (WASM) build: entity shapes are never instantiated, only
// referenced by type in code that never runs there -- this stub pattern is
// what a WASM client falls back on since PollModel's own header (unlike a
// declaration-only facade) still pulls this file in transitively, purely to
// name these types (BridgeHandler<PollModel> is a template over the model
// type, and PollModel's execute() signatures still name db::PollRecord et
// al. even though poll_model.cpp -- the only place any of these types is
// ever instantiated -- is never compiled for Emscripten at all).
struct PollRecord {};
struct OptionRecord {};
struct VoteRecord {};
struct CommentRecord {};
struct VoteHistoryRecord {};
struct PollEventRecord {};
#endif

}  // namespace polls::db
